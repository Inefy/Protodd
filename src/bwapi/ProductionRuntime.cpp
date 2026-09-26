#include "ProductionRuntime.hpp"
#include <algorithm>
#include <chrono>
#include "protodd/MacroPlanner.hpp"

namespace protodd::bwapi {
namespace {
int actionFor(const BWAPI::UnitCommand& c) {
    using namespace BWAPI;
    if (c.getType()!=UnitCommandTypes::Train && c.getType()!=UnitCommandTypes::Build) return -1;
    const std::array<UnitType,8> types{UnitTypes::Protoss_Probe,UnitTypes::Protoss_Pylon,
        UnitTypes::Protoss_Gateway,UnitTypes::Protoss_Assimilator,UnitTypes::Protoss_Cybernetics_Core,
        UnitTypes::Protoss_Zealot,UnitTypes::Protoss_Dragoon,UnitTypes::Protoss_Nexus};
    for(int i=0;i<8;++i) if(c.extra==types[i].getID() &&
        types[i].isBuilding()==(c.getType()==UnitCommandTypes::Build)) return i;
    return -1;
}
int queued(BWAPI::Unit u, int type) {
    const auto queue=u->getTrainingQueue();
    return static_cast<int>(std::count(queue.begin(),queue.end(),BWAPI::UnitType(type)));
}
BWAPI::Position buildCenter(BWAPI::TilePosition tile,int type) {
    const BWAPI::UnitType unit(type);
    const auto p=productionBuildCenter(tile.x,tile.y,unit.tileWidth(),unit.tileHeight());
    return {p.x,p.y};
}
}
void ProductionRuntime::start(std::ostream& log) {
    enabled_=observing_=false; history_.reset(); observations_.reset(); pending_.clear(); features_.clear();
    feedback_=ProductionFeedback{};
    control_=false;quota_=ProductionQuota{};
    if(inputs_.is_open()) inputs_.close();
    std::ifstream modeFile("bwapi-data/read/ProductionDemand-mode.txt");
    std::string mode; std::getline(modeFile,mode);
    if(!mode.empty()&&mode.back()=='\r')mode.pop_back();
    if(mode.empty()||mode=="off")return;
    if(mode=="local-train-units") {
#ifdef PROTODD_PRODUCTION_LOCAL_EVALUATION
        control_=true;
#else
        log<<"PRODUCTION,status=disabled,reason=local-evaluation-build-required\n";return;
#endif
    } else if(mode!="shadow"){log<<"PRODUCTION,status=disabled,reason=unsupported-mode\n";return;}
    std::ifstream input("bwapi-data/read/ProductionDemand.bin",std::ios::binary);
    std::string error;
    if(!model_.load(input,error)){log<<"PRODUCTION,status=disabled,reason="<<error<<'\n';return;}
    inputs_.open("bwapi-data/write/production-inputs.bin",std::ios::binary|std::ios::trunc);
    if(!inputs_){log<<"PRODUCTION,status=disabled,reason=input-audit-open\n";return;}
    inputs_.write("PTDLIVE1",8);
    enabled_=observing_=true;
    log<<"PRODUCTION,status="<<(control_?"local-train-units":"shadow")
        <<",history=observed-transition,feedback_scope=probe-zealot-dragoon,inputs="<<model_.inputCount()<<'\n';
}
void ProductionRuntime::fail(std::string_view reason,std::ostream& log) {
    log<<"PRODUCTION,status=disabled,reason="<<reason<<'\n'; enabled_=observing_=false;
}
bool ProductionRuntime::command(const BWAPI::UnitCommand& c,bool before,bool accepted,std::ostream& log) {
    if(!enabled_)return true;
    const int action=actionFor(c); const auto u=c.getUnit();
    if(action<0||!u||u->getPlayer()!=BWAPI::Broodwar->self())return true;
    const int frame=BWAPI::Broodwar->getFrameCount(), id=u->getID();
    if(before) {
        reconcile(frame,log);
        if(!enabled_)return !control_;
        if(pending_.contains(id)){fail("ambiguous-overlapping-command",log);return !control_;}
        pending_[id]={action,c.extra,frame,queued(u,c.extra),u->getOrder().getID(),u->getBuildType().getID(),
            u->getOrderTargetPosition(),c.getTargetTilePosition(),false};
        pending_[id].ticket=feedback_.reserve(c,action,log);
        if(control_&&(action==0||action==5||action==6)&&pending_[id].ticket<0){pending_.erase(id);return false;}
    } else {
        const auto it=pending_.find(id); if(it==pending_.end())return true;
        log<<"PRODUCTION_ISSUE,"<<frame<<",actor="<<id<<",action="<<action<<",api_accepted="<<accepted<<'\n';
        if(!accepted){feedback_.outcome(it->second.ticket,false,log);pending_.erase(it);return true;}
        const auto& p=it->second;
        if(BWAPI::UnitType(p.type).isBuilding() && p.buildBefore==p.type &&
            p.orderPositionBefore==buildCenter(p.tile,p.type)) {
            log<<"PRODUCTION_REPEAT,"<<frame<<",actor="<<id<<",action="<<action<<'\n';
            feedback_.outcome(p.ticket,false,log);
            pending_.erase(it);return true;
        }
        feedback_.outcome(p.ticket,true,log);
        it->second.accepted=true; reconcile(frame,log);
    }
    return true;
}
bool ProductionRuntime::allows(const BWAPI::UnitCommand& c,std::string_view source) const {
    if(!enabled_||!control_||BWAPI::Broodwar->getFrameCount()>=7200||source=="production-demand")return true;
    const int a=actionFor(c);return a!=0&&a!=5&&a!=6;
}
void ProductionRuntime::act(Frame frame,const std::function<bool(const BWAPI::UnitCommand&)>& dispatch,std::ostream& log) {
    if(!enabled_||!control_)return;
    const std::array<std::pair<int,BWAPI::UnitType>,3> types{{{0,BWAPI::UnitTypes::Protoss_Probe},
        {5,BWAPI::UnitTypes::Protoss_Zealot},{6,BWAPI::UnitTypes::Protoss_Dragoon}}};
    for(const auto& [action,type]:types) {
        if(!quota_.wants(frame,action))continue;
        BWAPI::Unit selected=nullptr;
        for(auto unit:BWAPI::Broodwar->self()->getUnits()) {
            if(!unit->exists()||!unit->isCompleted()||!unit->isPowered()||unit->getType()!=type.whatBuilds().first)continue;
            const bool recent=unit->getLastCommand().getType()==BWAPI::UnitCommandTypes::Train&&
                unit->getLastCommandFrame()+std::max(1,BWAPI::Broodwar->getLatencyFrames())>=frame;
            if(!trainingSlotAvailable(unit->isTraining()||unit->getRemainingTrainTime()>0,
                static_cast<int>(unit->getTrainingQueue().size()),unit->getRemainingTrainTime(),
                BWAPI::Broodwar->getRemainingLatencyFrames(),recent)||!unit->canTrain(type))continue;
            if(!selected||unit->getID()<selected->getID())selected=unit;
        }
        if(!selected)continue;
        const bool accepted=dispatch(BWAPI::UnitCommand::train(selected,type));
        quota_.result(frame,action,accepted);
        log<<"PRODUCTION_CONTROL,"<<frame<<",action="<<action<<",actor="<<selected->getID()<<",accepted="<<accepted<<'\n';
        if(!enabled_)return;
    }
}
void ProductionRuntime::reconcile(Frame frame,std::ostream& log) {
    for(auto it=pending_.begin();it!=pending_.end();) {
        const auto& p=it->second;const auto u=BWAPI::Broodwar->getUnit(it->first);
        if(!p.accepted){++it;continue;}
        const bool exists=u&&u->exists()&&u->getPlayer()==BWAPI::Broodwar->self();
        bool confirmed=false;
        if(exists) {
            if(!BWAPI::UnitType(p.type).isBuilding()) confirmed=queued(u,p.type)>p.queueBefore;
            else {
                const bool changed=u->getOrder().getID()!=p.orderBefore || u->getBuildType().getID()!=p.buildBefore ||
                    u->getOrderTargetPosition()!=p.orderPositionBefore;
                // Check matching construction intent, not just a Move or last command.
                confirmed=changed && u->getBuildType().getID()==p.type &&
                    u->getOrderTargetPosition()==buildCenter(p.tile,p.type);
                const auto building=u->getBuildUnit();
                confirmed=confirmed||(building&&building->exists()&&building->getType().getID()==p.type&&
                    building->getTilePosition()==p.tile&&!building->isCompleted());
            }
        }
        if(confirmed) {
            history_.accepted(frame,p.action);
            log<<"PRODUCTION_ACCEPT,"<<frame<<",issued="<<p.issued<<",actor="<<it->first<<",action="<<p.action
                <<",delay="<<frame-p.issued<<'\n';
            it=pending_.erase(it);
        } else if(!exists || frame-p.issued>BWAPI::Broodwar->getLatencyFrames()+48) {
            log<<"PRODUCTION_UNRESOLVED,"<<frame<<",issued="<<p.issued<<",actor="<<it->first<<",action="<<p.action<<'\n';
            fail("unresolved-acceptance",log);return;
        } else ++it;
    }
}
void ProductionRuntime::observe(const GameState& state,std::ostream& log) {
    if(!observing_)return;
    feedback_.observe(state.frame,log);
    if(!enabled_)return;
    if(state.frame>=7200){log<<"PRODUCTION,status=scope-complete,frame="<<state.frame<<'\n';enabled_=false;return;}
    reconcile(state.frame,log);if(!enabled_)return;
    std::vector<UnitId> removals;
    for(const auto& known:observations_.rememberedEnemies())
        if(!std::ranges::binary_search(state.enemy.units,known.id,{},&UnitSnapshot::id))removals.push_back(known.id);
    const auto previous=observations_.sampleFrame();
    observations_.observe(state,removals,[](Position p){
        const BWAPI::TilePosition tile(p.x/32,p.y/32);
        return tile.isValid()&&BWAPI::Broodwar->isVisible(tile);
    });
    if(observations_.sampleFrame()!=previous) {
        features_.assign(observations_.features().begin(),observations_.features().end());
        const auto history=history_.sample(state.frame);features_.insert(features_.end(),history.begin(),history.end());
    }
}
void ProductionRuntime::infer(Frame frame,const FrameBudget& budget,std::ostream& log) {
    if(!enabled_||!observations_.inferenceDue(frame))return;
    if(!observations_.beginInference(frame,budget)) {
        log<<"PRODUCTION_SKIP,"<<frame<<",observation="<<observations_.sampleFrame()<<'\n';
        if(control_)fail("runtime-load-fallback",log);return;
    }
    const auto prediction=model_.predict(features_);
    if(!prediction){fail("invalid-input",log);return;}
    if(control_)quota_.set(observations_.sampleFrame(),prediction->quantities);
    const std::int32_t sample=observations_.sampleFrame();
    inputs_.write(reinterpret_cast<const char*>(&sample),4);
    inputs_.write(reinterpret_cast<const char*>(features_.data()),static_cast<std::streamsize>(features_.size()*4));
    inputs_.write(reinterpret_cast<const char*>(prediction->quantities.data()),32);
    if(!inputs_){fail("input-audit-write",log);return;}
    log<<"PRODUCTION_SHADOW,"<<frame<<",observation="<<sample;
    for(int i=0;i<8;++i)log<<','<<productionDemandActions[i]<<'='<<prediction->quantities[i];
    log<<'\n';
}
void ProductionRuntime::end(std::ostream& log){if(inputs_.is_open()){feedback_.end(log);inputs_.close();}observing_=false;}
}
