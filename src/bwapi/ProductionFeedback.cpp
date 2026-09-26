#include "ProductionFeedback.hpp"
#include "protodd/MacroPlanner.hpp"
namespace protodd::bwapi {
void ProductionFeedback::observe(int frame,std::ostream& log) {
    if(frame==frame_)return;
    auto self=BWAPI::Broodwar->self();
    ProductionCommitments::Snapshot snapshot{frame,self->minerals(),self->gas()};
    for(auto unit:self->getUnits())if(unit->exists())snapshot.live.insert(unit->getID());
    for(auto& [key,w]:work_) {
        const auto& ticket=ledger_.ticket(key);
        if(ticket.terminal()||ticket.phase==ProductionCommitments::Phase::pending)continue;
        if(w.product<0) {
            BWAPI::Unit product=nullptr;
            if(BWAPI::UnitType(w.type).isBuilding()) {
                for(auto unit:self->getUnits())if(unit->exists()&&!unit->isCompleted()&&unit->getType().getID()==w.type&&unit->getTilePosition()==w.tile){product=unit;break;}
            } else {
                const auto producer=BWAPI::Broodwar->getUnit(w.actor);
                if(producer&&producer->exists())product=producer->getBuildUnit();
            }
            bool alreadyBound=false;
            for(const auto& [otherKey,other]:work_)if(otherKey!=key&&product&&other.product==product->getID())alreadyBound=true;
            if(product&&product->exists()&&!product->isCompleted()&&product->getID()!=w.priorProduct&&!alreadyBound&&
                product->getPlayer()==self&&product->getType().getID()==w.type) {
                w.product=product->getID();snapshot.spent.insert(key);snapshot.started.insert(key);
                log<<"PRODUCTION_SPENT,"<<frame<<",ticket="<<key<<",actor="<<w.actor<<",product="<<w.product
                    <<",type="<<w.type<<",minerals="<<snapshot.minerals<<",gas="<<snapshot.gas<<'\n';
            }
        }
        if(w.product>=0) {
            const auto product=BWAPI::Broodwar->getUnit(w.product);
            if(!product||!product->exists()||product->getPlayer()!=self||product->getType().getID()!=w.type) {
                snapshot.failed.insert(key);log<<"PRODUCTION_FAILED,"<<frame<<",ticket="<<key<<",product="<<w.product<<'\n';
            } else if(product->isCompleted()) {
                snapshot.completed.insert(key);log<<"PRODUCTION_COMPLETED,"<<frame<<",ticket="<<key<<",product="<<w.product<<'\n';
            }
        }
    }
    ledger_.observe(snapshot);frame_=frame;
}
int ProductionFeedback::reserve(const BWAPI::UnitCommand& command,int action,std::ostream& log) {
    // Builders can be reassigned by the current controller before spending.
    // That requires a separate cancellation/ownership adapter. Qualify only
    // the three training actions for this first executable scope.
    if(action!=0&&action!=5&&action!=6)return -1;
    const auto actor=command.getUnit();const BWAPI::UnitType type(command.extra);
    const bool recent=actor->getLastCommand().getType()==BWAPI::UnitCommandTypes::Train&&
        actor->getLastCommandFrame()+std::max(1,BWAPI::Broodwar->getLatencyFrames())>=frame_;
    const bool available=trainingSlotAvailable(actor->isTraining()||actor->getRemainingTrainTime()>0,
        static_cast<int>(actor->getTrainingQueue().size()),actor->getRemainingTrainTime(),
        BWAPI::Broodwar->getRemainingLatencyFrames(),recent);
    const int proposal=++proposal_;
    ledger_.submit({proposal,action,1,frame_+240});
    const auto ids=ledger_.reserve({{action,actor->getID(),type.mineralPrice(),type.gasPrice(),actor->canTrain(type),available,true}});
    // Each externally selected baseline command is one proposal. Cancellation
    // prevents a second dispatch while retaining every already-owned ticket.
    ledger_.cancel(proposal);
    if(ids.empty()) {
        log<<"PRODUCTION_RESERVE_MISS,"<<frame_<<",actor="<<actor->getID()<<",action="<<action<<'\n';return -1;
    }
    const auto prior=actor->getBuildUnit();
    const int key=ids.front();work_[key]={type.getID(),actor->getID(),-1,prior?prior->getID():-1,command.getTargetTilePosition()};
    const auto held=ledger_.reservations();
    log<<"PRODUCTION_RESERVED,"<<frame_<<",ticket="<<key<<",actor="<<actor->getID()<<",action="<<action
        <<",minerals="<<held[0]<<",gas="<<held[1]<<'\n';
    return key;
}
void ProductionFeedback::outcome(int key,bool accepted,std::ostream& log) {
    if(key<0)return;
    if(accepted)ledger_.accepted(key);else ledger_.rejected(key);
    log<<"PRODUCTION_DISPATCH,"<<BWAPI::Broodwar->getFrameCount()<<",ticket="<<key<<",api_accepted="<<accepted<<'\n';
}
void ProductionFeedback::end(std::ostream& log) const {
    int complete=0,failed=0,pending=0;
    for(const auto& [key,w]:work_) {
        const auto& t=ledger_.ticket(key);
        if(t.phase==ProductionCommitments::Phase::completed)++complete;
        else if(t.terminal())++failed;else ++pending;
    }
    const auto held=ledger_.reservations();
    log<<"PRODUCTION_FEEDBACK_SUMMARY,completed="<<complete<<",failed_or_rejected="<<failed<<",pending="<<pending
        <<",reserved_minerals="<<held[0]<<",reserved_gas="<<held[1]<<'\n';
}
}
