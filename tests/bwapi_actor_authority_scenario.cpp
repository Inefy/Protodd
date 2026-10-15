// Isolated stock-engine T012 proof. Never packaged as the playing bot.
#include "ProtoddModule.hpp"
#include <BWAPI.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#if !defined(PROTODD_ACTOR_AUTHORITY_TESTS)
#error "Actor authority access is only enabled in the isolated native fixture."
#endif
namespace protodd::bwapi {
class ActorAuthorityFixture final : public BWAPI::AIModule {
    ProtoddModule module_;
    std::ofstream result_;
    std::string scenario_;
    struct Role { UnitId actor{-1}; Command oldOrder; Command emergency; Position acceptedPosition{-1,-1}; bool moved{}; };
    std::array<Role, 5> roles_{}; // builder, scout, worker, transport actor, fighter
    UnitId shuttle_{-1}, passenger_{-1};
    MacroAction build_;
    Frame seedFrame_{-1}, acceptedFrame_{-1}, stopFrame_{-1}, unloadFrame_{-1};
    int stage_{}, failures_{}, seedBuilds_{}, seedLoads_{}, stops_{}, unloads_{};
    int emergencyAccepted_{}, emergencyRejected_{}, initialMinerals_{};
    bool finished_{}, loadedCase_{}, nativeStop_{}, buildReleased_{}, badDuplicate_{};
    std::map<std::pair<Frame,UnitId>, int> acceptedPerActor_;
    void check(const char* name, bool passed) {
        result_ << "CHECK," << name << ',' << passed << '\n';result_.flush();
        if(!passed) ++failures_;
    }
    void finish(Frame frame) {
        check("engine-postcondition",failures_==0);
        result_ << "DONE," << frame << ',' << failures_ << '\n';result_.flush();
        finished_=true;BWAPI::Broodwar->leaveGame();
    }
    bool builderHeld() const {
        const auto ids=module_.bridge_.reservedBuilders();
        return std::ranges::find(ids,roles_[0].actor)!=ids.end();
    }
    bool scoutHeld() const {
        return std::ranges::find(module_.leasedScouts_,roles_[1].actor)!=module_.leasedScouts_.end() &&
            std::ranges::any_of(module_.pendingScoutOrders_,[this](const auto& o){return o.scout==roles_[1].actor;}) &&
            module_.scouts_.missionMetrics().cancelled==0;
    }
    Command move(UnitId actor, Position target, Frame frame, std::string source, int urgency=1000) const {
        Command c{actor,CommandType::move,-1,target,UnitKind::unknown,urgency,frame,std::move(source)};
        c.owner=CommandOwner::combat;c.urgency=urgency;c.deadlineFrame=frame;c.leaseGeneration=1;return c;
    }
    struct RawActor { UnitId actor{-1}; Position from{-1,-1}, goal{-1,-1}; Frame accepted{-1}; bool observed{}; };
    std::array<RawActor,5> rawActors_{};

    bool scoutMiningCase_{}, scoutMiningReject_{}, scoutMiningBounded_{true};
    UnitId scoutMiningActor_{-1}, scoutMiningEscapeTarget_{-1};
    int scoutMiningStage_{}, scoutMiningAccepted_{}, scoutMiningRejected_{}, scoutMiningFrames_{};
    int scoutMiningInitialBank_{};
    std::uint64_t scoutMiningWork_{};
    Frame scoutMiningMoveFrame_{-1}, scoutMiningEscapeFrame_{-1};
    Position scoutMiningOrigin_{-1,-1};
    BaseSnapshot scoutMiningHome_{}, scoutMiningFar_{};
    std::vector<ScoutOrder> scoutMiningOrders_;
    Command scoutMiningEscape_;
    GameState scoutMiningPlanningState() const {
        auto state=module_.state_;
        std::erase_if(state.bases,[this](const BaseSnapshot& base){
            return base.ownerId!=stateSelfId() && base.id!=scoutMiningFar_.id;
        });
        return state;
    }
    int stateSelfId() const {return module_.state_.self.id;}
    std::vector<ScoutOrder> scoutMiningAssign() {
        const auto state=scoutMiningPlanningState();
        const UnitId actor[]{scoutMiningActor_};
        ThreatAssessment threat;threat.uncertainty=1.0;
        return module_.scouts_.assign(state,actor,module_.influence_,threat,&module_.navigation_);
    }
    void scoutMiningStart() {
        module_.onStart();
        module_.bridge_.productionPermission=[this](const BWAPI::UnitCommand&,std::string_view){
            return !scoutMiningReject_;
        };
        const auto previous=module_.bridge_.actionDiagnostic;
        module_.bridge_.actionDiagnostic=[this,previous](const ActionDiagnostic& d){
            if(previous)previous(d);
            const auto frame=BWAPI::Broodwar->getFrameCount();
            if(d.accepted) {
                if(++acceptedPerActor_[{frame,d.actor}]>1)badDuplicate_=true;
                if(d.actor==scoutMiningActor_) {
                    if(d.type=="Gather")++scoutMiningAccepted_;
                    if(d.source=="scout-travel") {
                        scoutMiningMoveFrame_=frame;
                        const auto native=BWAPI::Broodwar->getUnit(d.actor);
                        if(native && native->exists())
                            scoutMiningOrigin_={native->getPosition().x,native->getPosition().y};
                    }
                }
            }
            if(d.actor==scoutMiningActor_ && d.outcome=="production-permission-rejected")
                ++scoutMiningRejected_;
            result_<<"SCOUT_PLANNING_ACTION,"<<frame<<','<<d.actor<<','<<d.source<<','<<d.type
                <<','<<d.attempted<<','<<d.accepted<<','<<d.target<<','<<d.outcome<<'\n';
            result_.flush();
        };
    }
    void scoutMiningFrame(Frame frame) {
        if(scoutMiningStage_==0) {
            const auto home=std::ranges::find_if(module_.state_.bases,[this](const BaseSnapshot& base){
                return base.ownerId==stateSelfId();
            });
            if(home==module_.state_.bases.end()){check("planner-native-owned-base-exposed",false);finish(frame);return;}
            scoutMiningHome_=*home;
            int farDistance=0;
            for(const auto& base:module_.state_.bases) {
                const auto range=distanceSquared(base.center,home->center);
                if(base.ownerId!=stateSelfId() && range>farDistance) {
                    scoutMiningFar_=base;farDistance=range;
                }
            }
            for(const auto& probe:module_.state_.self.units)
                if(probe.kind==UnitKind::probe && probe.completed && !probe.loaded &&
                   !probe.carryingResources && !probe.underAttack &&
                   distanceSquared(probe.position,home->center)<256*256 &&
                   (scoutMiningActor_<0 || probe.id<scoutMiningActor_))scoutMiningActor_=probe.id;
            for(const auto& enemy:module_.state_.enemy.units)
                if(enemy.visible && enemy.kind==UnitKind::pylon)scoutMiningEscapeTarget_=enemy.id;
            check("planner-real-native-actor-far-base-and-static-escape-target",
                scoutMiningActor_>=0 && farDistance>2000*2000 && scoutMiningEscapeTarget_>=0);
            if(failures_){finish(frame);return;}
            const auto native=BWAPI::Broodwar->getUnit(scoutMiningActor_);
            scoutMiningOrigin_={native->getPosition().x,native->getPosition().y};
            scoutMiningInitialBank_=BWAPI::Broodwar->self()->minerals();
            // These proposals come from the real typed adapter. The separate
            // buses inspect metadata only; they never dispatch native orders.
            for(const auto job:{WorkerJob::minerals,WorkerJob::transfer,WorkerJob::gas}) {
                WorkerAssignment assignment{scoutMiningActor_,job,home->id,-1,home->mineralLine,35};
                if(job==WorkerJob::gas)for(const auto unit:BWAPI::Broodwar->self()->getUnits())
                    if(unit && unit->exists() && unit->isCompleted() && unit->getType().isRefinery())
                        assignment.targetUnit=unit->getID();
                CommandBus metadata;metadata.beginFrame(frame,BWAPI::Broodwar->getLatencyFrames());
                const WorkerAssignment assignments[]{assignment};
                static_cast<void>(module_.bridge_.submitWorkerCommands(assignments,metadata));
                const auto proposed=metadata.finalize();
                check(job==WorkerJob::gas?"planner-real-gas-proposal-is-routine":
                    job==WorkerJob::transfer?"planner-real-transfer-proposal-is-routine":
                    "planner-real-mining-proposal-is-routine",
                    proposed.size()==1 && proposed.front().type==CommandType::gather &&
                    proposed.front().activity==CommandActivity::workerEconomy);
            }
            const auto orders=scoutMiningAssign();
            check("planner-real-native-terrain-exposes-unleased-pending-proof",
                orders.empty() && module_.scouts_.informationGap().reason==ScoutGapReason::navigationPending &&
                module_.scouts_.hasRouteWork(scoutMiningActor_) && module_.scouts_.activeMissionScouts().empty());
            if(failures_){finish(frame);return;}
            const auto before=module_.scouts_.navigationDiagnostics().frameWork;
            WorkerAssignment assignment{scoutMiningActor_,WorkerJob::minerals,home->id,-1,home->mineralLine,35};
            const WorkerAssignment assignments[]{assignment};
            static_cast<void>(module_.bridge_.submitWorkerCommands(assignments,module_.commands_));
            module_.dispatchFrameCommands();
            check("planner-native-accepted-mining-keeps-unleased-proof",
                scoutMiningAccepted_==1 && module_.scouts_.hasRouteWork(scoutMiningActor_) &&
                module_.scouts_.activeMissionScouts().empty() &&
                module_.scouts_.navigationDiagnostics().frameWork==before);
            if(failures_){finish(frame);return;}
            scoutMiningWork_+=before;++scoutMiningStage_;
        } else if(scoutMiningStage_==1) {
            const auto native=BWAPI::Broodwar->getUnit(scoutMiningActor_);
            if(native && native->exists() && native->isGatheringMinerals())++scoutMiningFrames_;
            if(frame%6==0) {
                module_.updateScoutMicro();
                if(frame%24==0)scoutMiningOrders_=scoutMiningAssign();
                const auto work=module_.scouts_.navigationDiagnostics().frameWork;
                scoutMiningWork_+=work;
                scoutMiningBounded_=scoutMiningBounded_ && work<=ScoutManager::navigationWorkPerFrame;
                result_<<"SCOUT_PLANNING_WORK,"<<frame<<",work="<<work<<",total="<<scoutMiningWork_
                    <<",queued="<<module_.scouts_.navigationDiagnostics().queued
                    <<",mining_frames="<<scoutMiningFrames_<<'\n';result_.flush();
                if(!scoutMiningOrders_.empty()) {
                    check("planner-eventual-safe-order-through-production-micro",
                        scoutMiningBounded_ && scoutMiningWork_>2048 && scoutMiningFrames_>0 &&
                        scoutMiningOrders_.front().target.valid() && scoutMiningOrders_.front().routeRisk<=1.0);
                    module_.pendingScoutOrders_=scoutMiningOrders_;
                    module_.leasedScouts_={scoutMiningActor_};
                    ++scoutMiningStage_;
                }
            }
        } else if(scoutMiningStage_==2) {
            static_cast<void>(module_.bridge_.submitScouts(scoutMiningOrders_,module_.commands_));
            module_.dispatchFrameCommands();
            if(scoutMiningMoveFrame_>=0) {
                check("planner-completed-proof-produces-native-scout-move",
                    module_.scouts_.activeMissionScouts()==std::vector<UnitId>{scoutMiningActor_});
                ++scoutMiningStage_;
            }
        } else if(scoutMiningStage_==3 && frame>scoutMiningMoveFrame_+24) {
            const auto native=BWAPI::Broodwar->getUnit(scoutMiningActor_);
            const Position now{native->getPosition().x,native->getPosition().y};
            check("planner-native-scout-movement-observed",
                native->getLastCommand().getType()==BWAPI::UnitCommandTypes::Move &&
                distanceSquared(now,scoutMiningOrigin_)>8*8);
            // A visible static enemy Pylon is a declared fixture escape anchor,
            // not evidence of organic combat. The real evacuation branch must
            // keep its Gather exclusive regardless of that controlled intent.
            WorkerAssignment assignment{scoutMiningActor_,WorkerJob::evacuate,scoutMiningHome_.id,
                scoutMiningEscapeTarget_,scoutMiningHome_.mineralLine,100};
            const WorkerAssignment assignments[]{assignment};
            static_cast<void>(module_.bridge_.submitWorkerCommands(assignments,module_.commands_));
            module_.dispatchFrameCommands();
            const auto& winners=module_.commands_.authorityWinners();
            const auto escape=std::ranges::find_if(winners,[this](const Command& command){
                return command.actor==scoutMiningActor_ && command.source=="worker-mineral-walk";
            });
            if(escape!=winners.end() && scoutMiningAccepted_>=2) {
                scoutMiningEscape_=*escape;scoutMiningEscapeFrame_=frame;
                check("planner-real-evacuation-gather-is-exclusive-and-cancels-active-scout",
                    escape->type==CommandType::gather && escape->activity==CommandActivity::unspecified &&
                    module_.scouts_.activeMissionScouts().empty() &&
                    !module_.scouts_.hasRouteWork(scoutMiningActor_) && module_.leasedScouts_.empty());
                ++scoutMiningStage_;
            }
        } else if(scoutMiningStage_==4 && frame>scoutMiningEscapeFrame_+48) {
            // Start a second declared background proof after the first real
            // handoff. This reset is fixture setup, never a simulated handoff.
            module_.scouts_.reset();
            const auto orders=scoutMiningAssign();
            check("planner-second-unleased-proof-exposed",orders.empty() &&
                module_.scouts_.hasRouteWork(scoutMiningActor_) && module_.scouts_.activeMissionScouts().empty());
            scoutMiningEscape_.earliestFrame=scoutMiningEscape_.deadlineFrame=frame;
            scoutMiningEscape_.alreadyActive=false;
            scoutMiningReject_=true;
            module_.commands_.submit(scoutMiningEscape_);
            const auto before=module_.scouts_.navigationDiagnostics().frameWork;
            module_.dispatchFrameCommands();scoutMiningReject_=false;
            check("planner-rejected-exclusive-gather-keeps-proof",
                scoutMiningRejected_==1 && module_.scouts_.hasRouteWork(scoutMiningActor_) &&
                module_.scouts_.navigationDiagnostics().frameWork==before);
            ++scoutMiningStage_;
        } else if(scoutMiningStage_==5) {
            module_.bridge_.beginCommandBudget(frame,0);
            scoutMiningEscape_.earliestFrame=scoutMiningEscape_.deadlineFrame=frame;
            module_.commands_.submit(scoutMiningEscape_);module_.dispatchFrameCommands();
            check("planner-budget-deferred-exclusive-gather-keeps-proof",
                module_.commands_.stats().budgetDeferred==1 &&
                module_.scouts_.hasRouteWork(scoutMiningActor_));
            ++scoutMiningStage_;
        } else if(scoutMiningStage_==6) {
            scoutMiningEscape_.earliestFrame=scoutMiningEscape_.deadlineFrame=frame;
            module_.commands_.submit(scoutMiningEscape_);
            const auto before=module_.scouts_.navigationDiagnostics().frameWork;
            const auto accepted=scoutMiningAccepted_;
            module_.dispatchFrameCommands();
            check("planner-confirmed-exclusive-gather-cancels-background-proof",
                scoutMiningAccepted_==accepted+1 && !module_.scouts_.hasRouteWork(scoutMiningActor_) &&
                module_.scouts_.navigationDiagnostics().queued==0 &&
                module_.scouts_.navigationDiagnostics().frameWork==before);
            check("planner-single-native-order-per-actor-frame",!badDuplicate_);
            result_<<"SCOUT_PLANNING_SUMMARY,work="<<scoutMiningWork_<<",mining_frames="<<scoutMiningFrames_
                <<",mineral_gain="<<BWAPI::Broodwar->self()->minerals()-scoutMiningInitialBank_<<'\n';
            finish(frame);
        }
        if(!finished_ && frame>=2400){check("planner-bounded-native-completion",false);finish(frame);}
    }

    bool depletionCase_{}, depletionBlocked_{}, depletionSurvival_{}, depletionBlockedObserved_{};
    bool depletionOrganic_{}, depletionPressure_{}, depletionPostHarvest_{}, depletionPressureAttacked_{};
    bool depletionLiveUnsafeCase_{}, depletionLiveUnsafeWitness_{};
    int depletionPostUnits_{};
    Frame depletionUnsafe_{-1}, depletionPressureReleased_{-1};
    Frame depletionInfluenceFrame_{-1}, depletionClearedDangerFrame_{-1};
    int depletionLiveDangerSamples_{};
    bool depletionLiveDangerRetained_{true}, depletionClearedDangerRefreshed_{};
    Position depletionPriorTarget_{-1,-1};
    std::map<UnitId,bool> depletionPressureCannons_;
    UnitId depletionVision_{-1};
    Position depletionVisionStart_{-1,-1};
    bool depletionVisionMissing_{}, depletionVisionMoved_{}, depletionVisionCommanded_{};
    int depletionPressureVisionGaps_{}, depletionVisionFrames_{};
    int depletionReadyFundingFrames_{}, depletionMatureFundingFrames_{};
    bool depletionFundingLeaseHeld_{true};
    Frame depletionExpansionCancelFrame_{-1};
    std::vector<UnitId> depletionInitialActors_;
    int depletionInitialMain_{}, depletionNaturalInitial_{}, depletionNaturalMinimum_{};
    int depletionNewUnits_{}, depletionNaturalMiners_{}, depletionNativeFrames_{};
    Frame depletionPrepared_{-1}, depletionEmpty_{-1}, depletionSurvivalFrame_{-1};
    Frame depletionNexusBuild_{-1}, depletionNexusComplete_{-1};
    UnitId depletionNexus_{-1};
    std::map<UnitId,int> depletionMainMinerals_, depletionReplacementMinerals_;
    bool depletionReplacementHarvest_{}, depletionBlockerLost_{};
    std::map<UnitId,Position> depletionBlockerDepots_;
    int trackedMainMinerals() {
        int remaining=0;
        for(auto& [id,amount]:depletionMainMinerals_) {
            const auto mineral=BWAPI::Broodwar->getUnit(id);
            // Fog disappearance is not depletion. Lifecycle destruction is
            // recorded separately and visible native counts update the cache.
            if(mineral && mineral->exists() && mineral->isVisible())
                amount=std::max(0,mineral->getResources());
            remaining+=amount;
        }
        return remaining;
    }
    int nativeMineralsAt(Position center) const {
        int remaining=0;
        for(const auto mineral:BWAPI::Broodwar->getMinerals())
            if(mineral && mineral->exists() &&
               distanceSquared(Position{mineral->getPosition().x,mineral->getPosition().y},center)<=384*384)
                remaining+=std::max(0,mineral->getResources());
        return remaining;
    }
    void depletionStart() {
        module_.onStart();
        for(const auto mineral:BWAPI::Broodwar->getStaticMinerals())
            if(mineral && distanceSquared(Position{mineral->getInitialPosition().x,
                mineral->getInitialPosition().y},{3808,464})<=384*384) {
                depletionMainMinerals_.emplace(mineral->getID(),std::max(0,mineral->getResources()));
                result_<<"ECON_MAIN_MINERAL,"<<mineral->getID()<<",resources="<<mineral->getResources()<<"\n";
            }
        depletionInitialMain_=trackedMainMinerals();
        depletionNaturalInitial_=depletionNaturalMinimum_=nativeMineralsAt({3680,1328});
        for(const auto u:BWAPI::Broodwar->self()->getUnits()) {
            depletionInitialActors_.push_back(u->getID());
            if(depletionPressure_ && u->getType()==BWAPI::UnitTypes::Zerg_Overlord && u->isInvincible()) {
                depletionVision_=u->getID();
                depletionVisionStart_={u->getPosition().x,u->getPosition().y};
                result_<<"ECON_PASSIVE_VISION,"<<u->getID()<<",type="<<u->getType().getID()
                    <<",owner="<<u->getPlayer()->getID()<<",x="<<depletionVisionStart_.x
                    <<",y="<<depletionVisionStart_.y<<",invincible="<<u->isInvincible()<<'\n';
            }
            result_<<"ECON_INITIAL_ACTOR,"<<u->getID()<<','<<u->getType().getID()<<'\n';
        }
        const auto previous=module_.bridge_.actionDiagnostic;
        module_.bridge_.actionDiagnostic=[this,previous](const ActionDiagnostic& d) {
            if(previous)previous(d);
            const auto frame=BWAPI::Broodwar->getFrameCount();
            if(d.accepted && ++acceptedPerActor_[{frame,d.actor}]>1)badDuplicate_=true;
            if(depletionPressure_ && d.actor==depletionVision_ && d.attempted)depletionVisionCommanded_=true;
            if(depletionPressure_ && d.accepted && d.source.starts_with("expansion-cancel"))
                depletionExpansionCancelFrame_=frame;
            if(d.accepted && d.type=="Build" && d.extra==BWAPI::UnitTypes::Protoss_Nexus.getID() &&
               depletionNexusBuild_<0)depletionNexusBuild_=frame;
            result_<<"ECON_ACTION,"<<frame<<','<<d.actor<<','<<d.source<<','<<d.type
                <<','<<d.attempted<<','<<d.accepted<<','<<d.position.x<<','<<d.position.y
                <<','<<d.extra<<','<<d.outcome<<'\n';
        };
        result_.flush();
    }
    void depletionFrame(Frame frame) {
        // Exercise the production module's complete callback and lifecycle.
        // The fixture observes; it never supplies plans, route feedback or orders.
        const auto influenceCalls = [this] {
            const auto found = module_.phases_.find("influence");
            return found == module_.phases_.end() ? std::uint64_t{} : found->second.calls;
        };
        const auto fundingBefore = depletionPressure_
            ? module_.bridge_.expansionFeedback(module_.plan_.expansionTarget)
            : ExpansionFeedback{};
        const auto fundingBankBefore = BWAPI::Broodwar->self()->minerals();
        struct NativeFundingWitness {
            Position position{-1,-1}, lastTarget{-1,-1};
            int owner{-1}, type{-1};
            bool alive{}, underAttack{}, loaded{}, canBuildHere{};
            std::string lastCommand;
        } fundingNativeBefore;
        if (depletionPressure_ && fundingBefore.readyAwaitingFunds && fundingBefore.builder >= 0) {
            const auto probe = BWAPI::Broodwar->getUnit(fundingBefore.builder);
            if (probe && probe->exists()) {
                fundingNativeBefore.alive = true;
                fundingNativeBefore.position = {probe->getPosition().x,probe->getPosition().y};
                fundingNativeBefore.owner = probe->getPlayer()->getID();
                fundingNativeBefore.type = probe->getType().getID();
                fundingNativeBefore.underAttack = probe->isUnderAttack();
                fundingNativeBefore.loaded = probe->isLoaded();
                const BWAPI::TilePosition tile{(fundingBefore.site.x-64)/32,
                                              (fundingBefore.site.y-48)/32};
                fundingNativeBefore.canBuildHere = BWAPI::Broodwar->canBuildHere(
                    tile,BWAPI::UnitTypes::Protoss_Nexus,probe,true);
                const auto last = probe->getLastCommand();
                fundingNativeBefore.lastCommand = last.getType().toString();
                fundingNativeBefore.lastTarget = {last.getTargetPosition().x,last.getTargetPosition().y};
            }
        }
        const auto influenceCallsBefore = influenceCalls();
        const auto cadenceBefore = module_.frameBudget_.expensiveCadenceMultiplier(frame);
        module_.onFrame();++depletionNativeFrames_;
        if (depletionPressure_ && fundingBefore.readyAwaitingFunds && fundingBefore.pending) {
            ++depletionReadyFundingFrames_;
            const auto mature = fundingBefore.stalledFrames >= 8 * 24 &&
                                fundingBefore.stalledFrames < 30 * 24;
            if (mature) ++depletionMatureFundingFrames_;
            const auto after = module_.bridge_.expansionFeedback(module_.plan_.expansionTarget);
            const auto held = depletionExpansionCancelFrame_ != frame &&
                after.pending && after.taskId == fundingBefore.taskId &&
                after.site == fundingBefore.site;
            if (mature && !held) depletionFundingLeaseHeld_ = false;
            if (!fundingNativeBefore.alive) depletionFundingLeaseHeld_ = false;
            const auto actor = std::ranges::find_if(module_.state_.self.units,
                [&fundingBefore](const UnitSnapshot& unit){return unit.id == fundingBefore.builder;});
            result_ << "ECON_FUNDING_WAIT," << frame << ",task=" << fundingBefore.taskId
                << ",siteX=" << fundingBefore.site.x << ",siteY=" << fundingBefore.site.y
                << ",stalled=" << fundingBefore.stalledFrames << ",mineralsBefore=" << fundingBankBefore
                << ",builder=" << fundingBefore.builder << ",firstSeen="
                << (actor == module_.state_.self.units.end() ? -1 : actor->firstSeen)
                << ",alive=" << fundingNativeBefore.alive
                << ",x=" << fundingNativeBefore.position.x << ",y=" << fundingNativeBefore.position.y
                << ",owner=" << fundingNativeBefore.owner << ",type=" << fundingNativeBefore.type
                << ",underAttack=" << fundingNativeBefore.underAttack << ",loaded=" << fundingNativeBefore.loaded
                << ",canBuildHere=" << fundingNativeBefore.canBuildHere
                << ",lastCommand=" << fundingNativeBefore.lastCommand
                << ",lastX=" << fundingNativeBefore.lastTarget.x << ",lastY=" << fundingNativeBefore.lastTarget.y
                << ",afterTask=" << after.taskId << ",afterSiteX=" << after.site.x
                << ",afterSiteY=" << after.site.y << ",afterPending=" << after.pending
                << ",cancelAccepted=" << (depletionExpansionCancelFrame_ == frame)
                << ",held=" << held << '\n';
        }
        const auto influenceCallsAfter = influenceCalls();
        if (influenceCallsAfter > influenceCallsBefore) depletionInfluenceFrame_ = frame;
        if (depletionPressure_ && frame >= (depletionLiveUnsafeCase_ ? 0 : 1200) &&
            frame <= (depletionLiveUnsafeCase_ ? 3500 : 2000)) {
            int rememberedCannons = 0, visibleCannons = 0, nativeCannons = 0;
            for (const auto& enemy : module_.state_.enemy.units) {
                if (enemy.kind != UnitKind::photonCannon) continue;
                ++rememberedCannons;
                if (enemy.visible) ++visibleCannons;
            }
            for (const auto enemy : BWAPI::Broodwar->enemy()->getUnits())
                if (enemy && enemy->exists() && enemy->isVisible() &&
                    enemy->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon) ++nativeCannons;
            const auto anchorDanger = module_.influence_.at({3680,1328}).groundThreat;
            if (nativeCannons == 5 && rememberedCannons == 5) {
                ++depletionLiveDangerSamples_;
                if (anchorDanger <= 0.25F) depletionLiveDangerRetained_ = false;
            }
            if (nativeCannons == 0 && rememberedCannons == 0 &&
                depletionPressureCannons_.size() == 5 &&
                std::ranges::none_of(depletionPressureCannons_,
                    [](const auto& entry) { return entry.second; }) &&
                depletionClearedDangerFrame_ < 0) {
                depletionClearedDangerFrame_ = frame;
                depletionClearedDangerRefreshed_ =
                    depletionInfluenceFrame_ == frame && anchorDanger <= 0.25F;
            }
            result_ << "ECON_DANGER_SNAPSHOT," << frame
                << ",stateFrame=" << module_.state_.frame
                << ",influenceCallsBefore=" << influenceCallsBefore
                << ",influenceCallsAfter=" << influenceCallsAfter
                << ",lastInfluenceFrame=" << depletionInfluenceFrame_
                << ",cadenceBefore=" << cadenceBefore
                << ",rememberedCannons=" << rememberedCannons
                << ",visibleCannons=" << visibleCannons
                << ",nativeCannons=" << nativeCannons
                << ",anchorThreat=" << module_.influence_.at({3680,1328}).groundThreat << '\n';
        }
        if(depletionBlocked_)
            for(const auto depot:BWAPI::Broodwar->enemy()->getUnits())
                if(depot && depot->exists() && depot->isVisible() && depot->isInvincible() &&
                   depot->isCompleted() && depot->getType().isResourceDepot() &&
                   !depletionBlockerDepots_.contains(depot->getID())) {
                    const Position at{depot->getPosition().x,depot->getPosition().y};
                    depletionBlockerDepots_.emplace(depot->getID(),at);
                    result_<<"ECON_BLOCKING_DEPOT,"<<frame<<",actor="<<depot->getID()
                        <<",type="<<depot->getType().getID()<<",owner="<<depot->getPlayer()->getID()
                        <<",x="<<at.x<<",y="<<at.y<<",invincible="<<depot->isInvincible()<<'\n';
                }

        if(depletionPressure_) {
            const auto vision=BWAPI::Broodwar->getUnit(depletionVision_);
            const auto alive=vision && vision->exists() && vision->isInvincible() &&
                vision->getPlayer()==BWAPI::Broodwar->self();
            if(!alive)depletionVisionMissing_=true;
            else {
                ++depletionVisionFrames_;
                if(vision->getPosition().x!=depletionVisionStart_.x ||
                   vision->getPosition().y!=depletionVisionStart_.y)depletionVisionMoved_=true;
            }
            int visibleWeapons=0;
            for(const auto enemy:BWAPI::Broodwar->enemy()->getUnits())
                if(enemy && enemy->exists() && enemy->isVisible() &&
                   enemy->getType()==BWAPI::UnitTypes::Protoss_Photon_Cannon && enemy->isInvincible()) {
                    ++visibleWeapons;
                    const auto [entry,first]=depletionPressureCannons_.try_emplace(enemy->getID(),true);
                    if(first)result_<<"ECON_PRESSURE_WEAPON,"<<frame<<",actor="<<enemy->getID()
                        <<",owner="<<enemy->getPlayer()->getID()<<",x="<<enemy->getPosition().x
                        <<",y="<<enemy->getPosition().y<<",invincible="<<enemy->isInvincible()<<"\n";
                    if(enemy->isAttacking())depletionPressureAttacked_=true;
                    if(frame%24==0)result_<<"ECON_PRESSURE_SAMPLE,"<<frame<<",actor="<<enemy->getID()
                        <<",attacking="<<enemy->isAttacking()<<",target="<<(enemy->getOrderTarget()?enemy->getOrderTarget()->getID():-1)
                        <<",hp="<<enemy->getHitPoints()<<",shields="<<enemy->getShields()<<"\n";
                }
            if(std::ranges::any_of(depletionPressureCannons_,[](const auto& entry){return entry.second;}) &&
               visibleWeapons!=5)++depletionPressureVisionGaps_;
            if(frame%24==0) {
                result_<<"ECON_VISION_SAMPLE,"<<frame<<",actor="<<depletionVision_
                    <<",alive="<<alive<<",x="<<(alive?vision->getPosition().x:-1)
                    <<",y="<<(alive?vision->getPosition().y:-1)
                    <<",order="<<(alive?vision->getOrder().toString():"absent")
                    <<",visibleWeapons="<<visibleWeapons<<",visionGaps="<<depletionPressureVisionGaps_<<'\n';
                const auto builders=module_.bridge_.reservedBuilders();
                int workersAttacked=0,homeWorkersAttacked=0;
                for(const auto probe:BWAPI::Broodwar->self()->getUnits()) {
                    if(!probe || !probe->exists() || probe->getType()!=BWAPI::UnitTypes::Protoss_Probe)continue;
                    const auto builder=std::ranges::find(builders,probe->getID())!=builders.end();
                    if(probe->isUnderAttack()) {
                        ++workersAttacked;
                        if(probe->getDistance(BWAPI::Position(3808,464))<=384)++homeWorkersAttacked;
                    }
                    if(builder || probe->isUnderAttack())
                        result_<<"ECON_WORKER_CONTACT,"<<frame<<",actor="<<probe->getID()
                            <<",builder="<<builder<<",underAttack="<<probe->isUnderAttack()
                            <<",x="<<probe->getPosition().x<<",y="<<probe->getPosition().y
                            <<",order="<<probe->getOrder().toString()<<",hp="<<probe->getHitPoints()
                            <<",shields="<<probe->getShields()<<'\n';
                }
                int criticalNexus=0;
                for(const auto& goal:module_.plan_.goals)
                    if(goal.target==UnitKind::nexus && goal.priority>=114 && goal.blocking)++criticalNexus;
                result_<<"ECON_RECOVERY_FUNDING,"<<frame<<",sustain="<<module_.plan_.sustainEconomy
                    <<",criticalNexus="<<criticalNexus<<",workersAttacked="<<workersAttacked
                    <<",homeWorkersAttacked="<<homeWorkersAttacked<<",builders="<<builders.size()
                    <<",minerals="<<BWAPI::Broodwar->self()->minerals()<<'\n';
            }
            if(depletionPressureCannons_.size()==5 && std::ranges::none_of(
                depletionPressureCannons_,[](const auto& entry){return entry.second;}) && depletionPressureReleased_<0)
                depletionPressureReleased_=frame;
        }
        if(depletionOrganic_) {
            const auto feedback=module_.bridge_.expansionFeedback(module_.plan_.expansionTarget);
            if(feedback.unsafeRoute && depletionUnsafe_<0)depletionUnsafe_=frame;
            if (depletionLiveUnsafeCase_ && feedback.unsafeRoute) {
                int nativeWeapons = 0, rememberedWeapons = 0;
                for (const auto weapon : BWAPI::Broodwar->enemy()->getUnits())
                    if (weapon && weapon->exists() && weapon->isVisible() && weapon->isCompleted() &&
                        weapon->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon)
                        ++nativeWeapons;
                for (const auto& enemy : module_.state_.enemy.units)
                    if (enemy.kind == UnitKind::photonCannon && enemy.visible && enemy.completed)
                        ++rememberedWeapons;
                const auto witness = nativeWeapons == 5 && rememberedWeapons == 5 &&
                    depletionPressureReleased_ < 0;
                depletionLiveUnsafeWitness_ = depletionLiveUnsafeWitness_ || witness;
                result_ << "ECON_LIVE_UNSAFE," << frame << ",task=" << feedback.taskId
                    << ",siteX=" << feedback.site.x << ",siteY=" << feedback.site.y
                    << ",observedAt=" << feedback.observedAt << ",unsafe=" << feedback.unsafeRoute
                    << ",unknown=" << feedback.routePending << ",nativeWeapons=" << nativeWeapons
                    << ",rememberedWeapons=" << rememberedWeapons << ",released=" << depletionPressureReleased_
                    << ",witness=" << witness << '\n';
            }
            if(frame%24==0)result_<<"ECON_ROUTE_FEEDBACK,"<<frame<<",task="<<feedback.taskId
                <<",x="<<feedback.site.x<<",y="<<feedback.site.y<<",pending="<<feedback.pending
                <<",unsafe="<<feedback.unsafeRoute<<",unknown="<<feedback.routePending
                <<",stalled="<<feedback.stalledFrames<<",observedAt="<<feedback.observedAt
                <<",cancelling="<<feedback.cancelling<<",readyAwaitingFunds="<<feedback.readyAwaitingFunds
                <<",builder="<<feedback.builder<<",reason="<<module_.expansion_.reason()<<"\n";
            if(module_.plan_.expansionTarget.valid() && module_.plan_.expansionTarget!=depletionPriorTarget_) {
                depletionPriorTarget_=module_.plan_.expansionTarget;
                result_<<"ECON_TARGET,"<<frame<<",x="<<depletionPriorTarget_.x
                    <<",y="<<depletionPriorTarget_.y<<",reason="<<module_.expansion_.reason()<<"\n";
            }
        }
        const auto mainRemaining=trackedMainMinerals();
        const auto naturalRemaining=nativeMineralsAt({3680,1328});
        depletionNaturalMinimum_=std::min(depletionNaturalMinimum_,naturalRemaining);
        if(mainRemaining==0 && depletionEmpty_<0)depletionEmpty_=frame;
        if(module_.plan_.desiredBases>1 && module_.plan_.expansionTarget.valid() &&
           !module_.plan_.deferExpansion && depletionPrepared_<0)depletionPrepared_=frame;
        if(module_.plan_.name.find("[survival:")!=std::string::npos &&
           module_.plan_.sustainEconomy && module_.plan_.desiredGasWorkers==0 &&
           depletionSurvivalFrame_<0) {
            depletionSurvival_=true;depletionSurvivalFrame_=frame;
            result_<<"ECON_SURVIVAL,"<<frame<<",mainRemaining="<<mainRemaining
                <<",deferred="<<module_.plan_.deferExpansion
                <<",desiredBases="<<module_.plan_.desiredBases
                <<",gasWorkers="<<module_.plan_.desiredGasWorkers
                <<",reason="<<module_.expansion_.reason()<<'\n';
        }
        int ready=0,neutral=0,nexuses=0,newUnits=0,naturalMiners=0;
        for(const auto& base:module_.state_.bases)
            if(base.ownerId!=module_.state_.self.id && base.mineralPatches>=4 && base.mineralsRemaining>=4000) {
                ++neutral;
                if(base.ownerId==-1 && base.depotFootprintAvailable && !base.island && base.groundDistanceFromMain>=0)++ready;
            }
        if(frame>=240 && neutral>=8 && ready==0 && depletionBlockerDepots_.size()>=11 &&
           !depletionBlockerLost_ && !depletionBlockedObserved_) {
            depletionBlockedObserved_=true;
            result_<<"ECON_BLOCKED_SITES,"<<frame<<",occupiedDepots="<<depletionBlockerDepots_.size()
                <<",replacementSites="<<neutral<<",readySites="<<ready<<'\n';
        }
        for(const auto u:BWAPI::Broodwar->self()->getUnits()) {
            if(!u || !u->exists())continue;
            if(u->getType()==BWAPI::UnitTypes::Protoss_Nexus && u->isCompleted()) {
                ++nexuses;
                if(std::ranges::find(depletionInitialActors_,u->getID())==depletionInitialActors_.end() &&
                   depletionNexusComplete_<0) {
                    depletionNexusComplete_=frame;depletionNexus_=u->getID();
                    result_<<"ECON_NEXUS_COMPLETE,"<<frame<<",actor="<<u->getID()
                        <<",x="<<u->getPosition().x<<",y="<<u->getPosition().y
                        <<",mainRemaining="<<mainRemaining<<'\n';
                }
            }
            if(u->isCompleted() && !u->getType().isBuilding() &&
               std::ranges::find(depletionInitialActors_,u->getID())==depletionInitialActors_.end())
                ++newUnits;
            const auto target=u->getOrderTarget();
            const auto replacement=target && std::ranges::any_of(
                BWAPI::Broodwar->self()->getUnits(),[this,target](BWAPI::Unit depot) {
                    return depot && depot->exists() && depot->isCompleted() &&
                        depot->getType()==BWAPI::UnitTypes::Protoss_Nexus &&
                        std::ranges::find(depletionInitialActors_,depot->getID())==depletionInitialActors_.end() &&
                        depot->getDistance(target)<=384;
                });
            if(u->getType()==BWAPI::UnitTypes::Protoss_Probe && target && target->exists() &&
               target->isVisible() && target->getType().isMineralField() && replacement &&
               u->getDistance(target)<=128) {
                ++naturalMiners;
                const auto amount=std::max(0,target->getResources());
                const auto [prior,inserted]=depletionReplacementMinerals_.try_emplace(target->getID(),amount);
                if(!inserted && amount<prior->second) {
                    depletionReplacementHarvest_=true;
                    if(depletionEmpty_>=0 && frame>=depletionEmpty_)depletionPostHarvest_=true;
                    result_<<"ECON_REPLACEMENT_HARVEST,"<<frame<<",probe="<<u->getID()
                        <<",mineral="<<target->getID()<<",previous="<<prior->second
                        <<",remaining="<<amount<<",x="<<target->getPosition().x
                        <<",y="<<target->getPosition().y
                        <<",nativeOrder="<<u->getOrder().toString()<<'\n';
                }
                prior->second=amount;
            }

        }
        depletionNewUnits_=std::max(depletionNewUnits_,newUnits);
        depletionNaturalMiners_=std::max(depletionNaturalMiners_,naturalMiners);
        if(frame%24==0)
            result_<<"ECON_NATIVE,"<<frame<<",mainRemaining="<<mainRemaining
                <<",visibleNaturalRemaining="<<naturalRemaining<<",completedNexuses="<<nexuses
                <<",naturalMiners="<<naturalMiners<<",newCompletedUnits="<<newUnits
                <<",neutralSites="<<neutral<<",readySites="<<ready
                <<",runway="<<module_.plan_.estimatedMiningRunwayFrames
                <<",desiredBases="<<module_.plan_.desiredBases
                <<",targetX="<<module_.plan_.expansionTarget.x
                <<",targetY="<<module_.plan_.expansionTarget.y
                <<",deferred="<<module_.plan_.deferExpansion
                <<",minerals="<<BWAPI::Broodwar->self()->minerals()<<'\n';
        const Frame frameLimit=depletionOrganic_?12000:7200;
        if(frame<frameLimit)return;
        check("depletion-starts-with-limited-live-owned-minerals",
            depletionInitialMain_>1000 && depletionInitialMain_<=4000);
        check("depletion-native-owned-line-actually-empties",depletionEmpty_>=0);
        check("depletion-full-module-callbacks-run",depletionNativeFrames_>=frameLimit);
        check("depletion-new-units-complete",depletionNewUnits_>0);
        check("depletion-no-duplicate-accepted-actor-frame",!badDuplicate_);
        if(depletionBlocked_) {
            check("depletion-eleven-live-occupied-blockers-observed",
                depletionBlockerDepots_.size()>=11 && !depletionBlockerLost_);
            check("depletion-all-replacement-footprints-observed-blocked",depletionBlockedObserved_);
            check("depletion-explicit-survival-before-income-collapse",
                depletionSurvival_ && depletionSurvivalFrame_<depletionEmpty_);
            check("depletion-no-illegal-nexus-commit",depletionNexusBuild_<0 && depletionNexusComplete_<0);
        } else {
            check("depletion-preparation-before-income-collapse",
                depletionPrepared_>=0 && depletionPrepared_<depletionEmpty_);
            check("depletion-native-nexus-build-before-income-collapse",
                depletionNexusBuild_>=0 && depletionNexusBuild_<depletionEmpty_);
            check("depletion-replacement-nexus-completes",depletionNexusComplete_>depletionNexusBuild_);
            check("depletion-native-workers-reach-replacement-minerals",depletionNaturalMiners_>0);
            check("depletion-native-replacement-minerals-are-mined",
                depletionReplacementHarvest_);
        }
        if(depletionOrganic_) {
            check("depletion-post-exhaustion-replacement-harvest",depletionPostHarvest_);
            check("depletion-post-exhaustion-units-complete",depletionPostUnits_>0);
        }
        if(depletionPressure_) {
            check("depletion-five-live-enemy-pressure-weapons",depletionPressureCannons_.size()==5);
            check("depletion-native-enemy-weapons-attack",depletionPressureAttacked_);
            check("depletion-native-route-unsafe-before-release",
                depletionUnsafe_>=0 && depletionUnsafe_<depletionPressureReleased_);
            check("depletion-pressure-weapons-actually-removed",depletionPressureReleased_>=0);
            check("depletion-passive-native-vision-survives-all-callbacks",
                depletionVision_>=0 && !depletionVisionMissing_ && depletionVisionFrames_==depletionNativeFrames_);
            check("depletion-passive-native-vision-stationary",!depletionVisionMoved_);
            check("depletion-passive-native-vision-no-production-orders",!depletionVisionCommanded_);
            check("depletion-all-five-weapons-continuously-visible",depletionPressureVisionGaps_==0);
            check("depletion-live-weapon-danger-retained",
                depletionLiveDangerSamples_ > 0 && depletionLiveDangerRetained_);
            check("depletion-observed-removal-refreshes-before-route",
                depletionClearedDangerFrame_ >= 0 && depletionClearedDangerRefreshed_);
            if (depletionLiveUnsafeCase_) {
                check("depletion-native-live-unsafe-feedback",depletionLiveUnsafeWitness_);
            } else {
                check("depletion-native-arrived-funding-phase-observed",depletionReadyFundingFrames_ > 0);
                check("depletion-arrived-funding-wait-survives-stall-boundary",
                    depletionMatureFundingFrames_ > 0 && depletionFundingLeaseHeld_);
            }
        }
        result_<<"ECON_RESULT,"<<frame<<",initialMain="<<depletionInitialMain_
            <<",emptyFrame="<<depletionEmpty_<<",preparedFrame="<<depletionPrepared_
            <<",survivalFrame="<<depletionSurvivalFrame_<<",nexusBuildFrame="<<depletionNexusBuild_
            <<",nexusCompleteFrame="<<depletionNexusComplete_<<",nexusActor="<<depletionNexus_
            <<",visibleNaturalInitial="<<depletionNaturalInitial_<<",visibleNaturalMinimum="<<depletionNaturalMinimum_
            <<",naturalMiners="<<depletionNaturalMiners_<<",newUnits="<<depletionNewUnits_
            <<",postUnits="<<depletionPostUnits_<<",postHarvest="<<depletionPostHarvest_
            <<",unsafeFrame="<<depletionUnsafe_<<",pressureReleasedFrame="<<depletionPressureReleased_
            <<",visionActor="<<depletionVision_<<",visionFrames="<<depletionVisionFrames_
            <<",visionGaps="<<depletionPressureVisionGaps_<<",visionCommanded="<<depletionVisionCommanded_
            <<",visionMoved="<<depletionVisionMoved_<<",visionMissing="<<depletionVisionMissing_
            <<",callbacks="<<depletionNativeFrames_<<'\n';
        finish(frame);
    }
    bool rawCase_{}, rawPermissionReject_{};
    RawActor rawAmmo_;Frame rawAmmoTrainFrame_{-1};int rawAmmoInitial_{};
    void rawCallRecord(const char* label, bool accepted, UnitId actor, Frame frame) {
        result_ << "RAW_CALL," << frame << ',' << label << ',' << actor << ',' << accepted
                << ",budget=" << module_.bridge_.commandBudgetRemaining() << '\n';result_.flush();
    }
    void rawFrame(Frame frame) {
        if(stage_==0) {
            std::vector<UnitId> probes;UnitId fighter=-1,shuttle=-1;
            for(const auto& u:module_.state_.self.units) {
                if(u.kind==UnitKind::probe)probes.push_back(u.id);
                if(u.kind==UnitKind::dragoon)fighter=u.id;
                if(u.kind==UnitKind::shuttle)shuttle=u.id;
            }
            std::ranges::sort(probes);
            check("raw-five-live-actors",probes.size()==3 && fighter>=0 && shuttle>=0);
            if(failures_){finish(frame);return;}
            for(std::size_t i=0;i<3;++i)rawActors_[i].actor=probes[i];
            rawActors_[3].actor=shuttle;rawActors_[4].actor=fighter;
            for(std::size_t i=0;i<rawActors_.size();++i) {
                auto& r=rawActors_[i];auto u=BWAPI::Broodwar->getUnit(r.actor);
                r.from={u->getPosition().x,u->getPosition().y};
                const Position first{r.from.x,r.from.y+256},second{r.from.x+256,r.from.y};
                const auto a=BWAPI::UnitCommand::move(u,{first.x,first.y});
                const auto b=BWAPI::UnitCommand::move(u,{second.x,second.y});
                const auto firstAccepted=i==1?module_.bridge_.execute(move(r.actor,first,frame,"raw-typed-first")):
                    i<=2?module_.bridge_.executeWholeGame(a,24):module_.bridge_.executeProduction(a);
                rawCallRecord(("first-"+std::to_string(i)).c_str(),firstAccepted,r.actor,frame);
                const auto budget=module_.bridge_.commandBudgetRemaining();
                const auto secondAccepted=i==2 || i==4?module_.bridge_.execute(move(r.actor,second,frame,"raw-typed-second")):
                    i<=1?module_.bridge_.executeWholeGame(b,24):module_.bridge_.executeProduction(b);
                rawCallRecord(("second-"+std::to_string(i)).c_str(),secondAccepted,r.actor,frame);
                check(("raw-first-accepted-"+std::to_string(i)).c_str(),firstAccepted);
                check(("raw-second-rejected-"+std::to_string(i)).c_str(),!secondAccepted);
                check(("raw-rejection-keeps-budget-"+std::to_string(i)).c_str(),
                    module_.bridge_.commandBudgetRemaining()==budget);
                r.goal=secondAccepted?second:first;r.accepted=frame;
            }
            stage_=1;return;
        }
        if(stage_==1) {
            auto& r=rawActors_[0];auto u=BWAPI::Broodwar->getUnit(r.actor);
            const Position target{r.from.x,r.from.y+384};
            const auto cmd=BWAPI::UnitCommand::move(u,{target.x,target.y});
            module_.bridge_.beginCommandBudget(frame,0);
            check("raw-budget-deferred-is-not-a-claim",!module_.bridge_.executeWholeGame(cmd,24));
            module_.bridge_.beginCommandBudget(frame,96);
            check("raw-budget-retry-accepted",module_.bridge_.executeWholeGame(cmd,24));
            check("raw-budget-retry-second-rejected",!module_.bridge_.executeWholeGame(cmd,24));
            r.goal=target;r.accepted=frame;++stage_;return;
        }
        if(stage_==2) {
            auto& r=rawActors_[3];auto u=BWAPI::Broodwar->getUnit(r.actor);
            const Position target{r.from.x-256,r.from.y};
            const auto cmd=BWAPI::UnitCommand::move(u,{target.x,target.y});
            rawPermissionReject_=true;
            check("raw-permission-rejection-is-not-a-claim",!module_.bridge_.executeProduction(cmd));
            rawPermissionReject_=false;
            check("raw-permission-retry-accepted",module_.bridge_.executeProduction(cmd));
            check("raw-permission-retry-second-rejected",!module_.bridge_.executeProduction(cmd));
            r.goal=target;r.accepted=frame;++stage_;return;
        }
        if(stage_==3) {
            auto& r=rawActors_[1];auto u=BWAPI::Broodwar->getUnit(r.actor);
            const auto invalid=BWAPI::UnitCommand::train(u,BWAPI::UnitTypes::Protoss_Scarab);
            check("raw-api-rejection-is-not-a-claim",!module_.bridge_.executeWholeGame(invalid,24));
            const Position target{r.from.x,r.from.y+384};
            const auto cmd=BWAPI::UnitCommand::move(u,{target.x,target.y});
            check("raw-api-retry-accepted",module_.bridge_.executeWholeGame(cmd,24));
            check("raw-api-retry-second-rejected",!module_.bridge_.executeWholeGame(cmd,24));
            r.goal=target;r.accepted=frame;++stage_;return;
        }
        if(stage_==4) {
            for(const auto& u:module_.state_.self.units)if(u.kind==UnitKind::reaver)rawAmmo_.actor=u.id;
            const auto u=BWAPI::Broodwar->getUnit(rawAmmo_.actor);
            check("raw-ammunition-live-actor",u && u->exists());
            if(!u){finish(frame);return;}
            rawAmmo_.from={u->getPosition().x,u->getPosition().y};
            rawAmmoInitial_=u->getScarabCount();
            module_.spendingLedger_.beginFrame(module_.state_.self.minerals,module_.state_.self.gas);
            module_.bridge_.setSpendingLedger(&module_.spendingLedger_);
            const auto before=module_.spendingLedger_.minerals;
            const auto cmd=BWAPI::UnitCommand::train(u,BWAPI::UnitTypes::Protoss_Scarab);
            const auto first=module_.bridge_.executeProduction(cmd);
            rawCallRecord("ammunition-first",first,rawAmmo_.actor,frame);
            check("raw-ammunition-first-accepted",first);
            check("raw-ammunition-ledger-charged-once",
                module_.spendingLedger_.minerals==before-BWAPI::UnitTypes::Protoss_Scarab.mineralPrice());
            const auto paid=module_.spendingLedger_.minerals;
            const auto budget=module_.bridge_.commandBudgetRemaining();
            check("raw-ammunition-second-rejected",!module_.bridge_.executeProduction(cmd));
            check("raw-ammunition-same-frame-primary-rejected",
                !module_.bridge_.execute(move(rawAmmo_.actor,{1312,1024},frame,"raw-ammunition-primary")));
            check("raw-ammunition-rejections-do-not-charge",
                module_.spendingLedger_.minerals==paid && module_.bridge_.commandBudgetRemaining()==budget);
            result_ << "RAW_RESOURCE," << frame << ",actor=" << rawAmmo_.actor << ",before=" << before
                << ",after=" << paid << ",cost=" << BWAPI::UnitTypes::Protoss_Scarab.mineralPrice() << '\n';
            module_.bridge_.setSpendingLedger(nullptr);
            rawAmmoTrainFrame_=frame;++stage_;return;
        }
        if(stage_==5) {
            const auto u=BWAPI::Broodwar->getUnit(rawAmmo_.actor);
            const auto queue=u->getTrainingQueue();
            const bool active=std::ranges::find(queue,BWAPI::UnitTypes::Protoss_Scarab)!=queue.end() ||
                u->getScarabCount()>rawAmmoInitial_;
            if(active && u->getLastCommand().getType()==BWAPI::UnitCommandTypes::Train &&
                u->getLastCommandFrame()==rawAmmoTrainFrame_ && frame>rawAmmoTrainFrame_) {
                result_ << "RAW_NATIVE_TRAIN," << frame << ",actor=" << rawAmmo_.actor
                    << ",commandFrame=" << rawAmmoTrainFrame_ << ",queue=" << queue.size()
                    << ",scarabs=" << u->getScarabCount() << '\n';
                check("raw-ammunition-native-training-observed",true);
                rawAmmo_.goal={1312,1024};rawAmmo_.accepted=frame;
                check("raw-ammunition-later-frame-primary-accepted",
                    module_.bridge_.executeProduction(BWAPI::UnitCommand::move(u,{1312,1024})));
                ++stage_;
            }
            if(frame>=360){check("raw-ammunition-native-training-bounded",false);finish(frame);}
            return;
        }
        for(std::size_t i=0;i<rawActors_.size();++i) {
            auto& r=rawActors_[i];if(r.observed)continue;
            const auto u=BWAPI::Broodwar->getUnit(r.actor);const auto order=u->getOrderTargetPosition();
            const Position at{u->getPosition().x,u->getPosition().y};
            const bool active=u->getLastCommand().getType()==BWAPI::UnitCommandTypes::Move &&
                u->getLastCommandFrame()==r.accepted && order.isValid() &&
                distanceSquared(r.goal,{order.x,order.y})<=64 &&
                distanceSquared(at,r.from)>=256 &&
                distance(at,r.goal)+8<=distance(r.from,r.goal);
            if(!active)continue;
            result_ << "RAW_NATIVE_MOVE," << frame << ",role=" << i << ",actor=" << r.actor
                << ",commandFrame=" << r.accepted << ",x=" << r.goal.x << ",y=" << r.goal.y
                << ",orderX=" << order.x << ",orderY=" << order.y << ",fromX=" << r.from.x << ",fromY=" << r.from.y
                << ",observedX=" << at.x << ",observedY=" << at.y << '\n';
            check(("raw-measured-native-effect-"+std::to_string(i)).c_str(),true);r.observed=true;
        }
        if(!rawAmmo_.observed) {
            const auto u=BWAPI::Broodwar->getUnit(rawAmmo_.actor);const auto order=u->getOrderTargetPosition();
            const Position at{u->getPosition().x,u->getPosition().y};
            if(u->getLastCommand().getType()==BWAPI::UnitCommandTypes::Move &&
                u->getLastCommandFrame()==rawAmmo_.accepted && order.isValid() &&
                distanceSquared(rawAmmo_.goal,{order.x,order.y})<=64 &&
                distanceSquared(at,rawAmmo_.from)>=256 &&
                distance(at,rawAmmo_.goal)+8<=distance(rawAmmo_.from,rawAmmo_.goal)) {
                result_ << "RAW_NATIVE_MOVE," << frame << ",role=5,actor=" << rawAmmo_.actor
                    << ",commandFrame=" << rawAmmo_.accepted << ",x=" << rawAmmo_.goal.x << ",y=" << rawAmmo_.goal.y
                    << ",orderX=" << order.x << ",orderY=" << order.y
                    << ",fromX=" << rawAmmo_.from.x << ",fromY=" << rawAmmo_.from.y
                    << ",observedX=" << at.x << ",observedY=" << at.y << '\n';
                check("raw-ammunition-later-frame-primary-native-progress",true);rawAmmo_.observed=true;
            }
        }
        if(rawAmmo_.observed && std::ranges::all_of(rawActors_,[](const auto& r){return r.observed;})) {
            check("raw-native-no-duplicate-accepted-actor-frame",!badDuplicate_);
            check("raw-native-no-diagnostic-errors",module_.bridge_.diagnosticErrors()==0);
            finish(frame);
        }
        if(!finished_ && frame>=360){check("raw-bounded-native-completion",false);finish(frame);}
    }

    bool learnedCase_{}, learnedActiveCase_{}, learnedReject_{}, learnedFreshRawCase_{}, learnedFreshAccepted_{}, learnedUnconfirmedRawCase_{}, learnedGenerationChecked_{};
    Position learnedFreshGoal_{-1,-1};
    UnitId learnedActor_{-1};Frame learnedSeedFrame_{-1},learnedWinnerFrame_{-1};
    Position learnedFrom_{-1,-1},learnedGoal_{-1,-1};
    Command learnedSeed_,learnedWinner_;
    int learnedWinnerAccepted_{};
    bool learnedHeld() const {
        const auto ids=module_.bridge_.reservedBuilders();
        return std::ranges::find(ids,learnedActor_)!=ids.end();
    }
    void learnedFrame(Frame frame) {
        if(stage_==0) {
            for(const auto& u:module_.state_.self.units)if(u.kind==UnitKind::probe){learnedActor_=u.id;learnedFrom_=u.position;break;}
            check("learned-live-probe",learnedActor_>=0);
            if(learnedActor_<0){finish(frame);return;}
            learnedGoal_={learnedFrom_.x,learnedFrom_.y+384};
            learnedSeed_=move(learnedActor_,learnedGoal_,frame,"whole-game",70);
            learnedSeed_.owner=CommandOwner::learned;learnedSeed_.leaseGeneration=1;
            check("learned-seed-api-accepted",module_.bridge_.executeWholeGame(
                BWAPI::UnitCommand::move(BWAPI::Broodwar->getUnit(learnedActor_),{learnedGoal_.x,learnedGoal_.y}),240));
            check("learned-seed-lease-held",learnedHeld());learnedSeedFrame_=frame;++stage_;return;
        }
        if(stage_==1) {
            if(!module_.bridge_.commandActive(learnedSeed_))return;
            const auto u=BWAPI::Broodwar->getUnit(learnedActor_);
            const auto at=u->getPosition();
            if(distanceSquared({at.x,at.y},learnedFrom_)<256)return;
            result_ << "LEARNED_NATIVE_SEED," << frame << ",actor=" << learnedActor_
                << ",commandFrame=" << u->getLastCommandFrame() << ",x=" << learnedGoal_.x << ",y=" << learnedGoal_.y
                << ",orderX=" << u->getOrderTargetPosition().x << ",orderY=" << u->getOrderTargetPosition().y
                << ",fromX=" << learnedFrom_.x << ",fromY=" << learnedFrom_.y << ",observedX=" << at.x << ",observedY=" << at.y << '\n';
            check("learned-seed-native-active-before-preemption",frame>learnedSeedFrame_ && learnedHeld());
            const auto routine=move(learnedActor_,{learnedFrom_.x+256,learnedFrom_.y},
                frame,"learned-native-routine",50);
            const auto budget=module_.bridge_.commandBudgetRemaining();
            module_.commands_.submit(routine);module_.dispatchFrameCommands();
            check("learned-lower-urgency-retains-owner",learnedHeld() && !badDuplicate_);
            check("learned-lower-urgency-does-not-consume-budget",module_.bridge_.commandBudgetRemaining()==budget);
            learnedFrom_={at.x,at.y};++stage_;return;
        }
        if(learnedUnconfirmedRawCase_ && stage_==2) {
            learnedFreshGoal_={learnedFrom_.x+256,learnedFrom_.y};
            learnedWinner_=move(learnedActor_,learnedFreshGoal_,frame,"learned-native-unconfirmed",1000);
            learnedWinner_.alreadyActive=true; // Deliberately false adapter hint; native state must veto it.
            check("learned-unconfirmed-native-hint-is-false",!module_.bridge_.commandActive(learnedWinner_));
            module_.commands_.submit(learnedWinner_);module_.dispatchFrameCommands();
            check("learned-unconfirmed-hint-keeps-old-owner",learnedHeld());
            const auto u=BWAPI::Broodwar->getUnit(learnedActor_);
            const auto fresh=BWAPI::UnitCommand::move(u,{learnedFreshGoal_.x,learnedFreshGoal_.y});
            const auto budget=module_.bridge_.commandBudgetRemaining();
            learnedReject_=true;
            check("learned-unconfirmed-raw-permission-rejected",!module_.bridge_.executeWholeGame(fresh,240));
            learnedReject_=false;
            module_.bridge_.beginCommandBudget(frame,0);
            check("learned-unconfirmed-raw-budget-deferred",!module_.bridge_.executeWholeGame(fresh,240));
            module_.bridge_.beginCommandBudget(frame,budget);
            learnedFreshAccepted_=module_.bridge_.executeWholeGame(fresh,240);
            result_ << "LEARNED_FRESH_RAW," << frame << ",actor=" << learnedActor_
                << ",accepted=" << learnedFreshAccepted_ << ",x=" << learnedFreshGoal_.x
                << ",y=" << learnedFreshGoal_.y << ",budgetBefore=" << budget
                << ",budgetAfter=" << module_.bridge_.commandBudgetRemaining() << '\n';
            check("learned-unconfirmed-raw-retry-accepted",learnedFreshAccepted_);
            check("learned-unconfirmed-raw-budget-charged-once",module_.bridge_.commandBudgetRemaining()+1==budget);
            check("learned-unconfirmed-raw-retains-new-owner",learnedHeld());
            learnedWinnerFrame_=frame;stage_=5;return;
        }
        if(stage_<=4) {
            learnedWinner_=move(learnedActor_,learnedActiveCase_?learnedGoal_:Position{learnedFrom_.x+256,learnedFrom_.y},
                frame,"learned-native-emergency",1000);
            learnedWinner_.alreadyActive=learnedActiveCase_;
            if(stage_==2) {
                // A pending, unconfirmed native intent must not retire the learned lease.
                learnedWinner_.alreadyActive=false;module_.bridge_.beginCommandBudget(frame,0);
            }
            if(stage_==3){learnedWinner_.alreadyActive=false;learnedReject_=true;}
            module_.commands_.submit(learnedWinner_);module_.dispatchFrameCommands();
            if(stage_==2)check("learned-budget-deferred-retains-owner",learnedHeld() && learnedWinnerAccepted_==0);
            else if(stage_==3)check("learned-api-rejected-retains-owner",learnedHeld() && learnedWinnerAccepted_==0);
            else {
                learnedWinnerFrame_=frame;
                check("learned-confirmed-native-releases-owner",!learnedHeld());
                check("learned-native-acceptance-or-active-suppression",learnedActiveCase_?
                    learnedWinnerAccepted_==0 && module_.bridge_.commandActive(learnedWinner_):learnedWinnerAccepted_==1);
                if(!learnedActiveCase_ && learnedWinnerAccepted_==0){finish(frame);return;}
            }
            if(stage_==4 && learnedFreshRawCase_) {
                const auto u=BWAPI::Broodwar->getUnit(learnedActor_);
                learnedFreshGoal_={learnedFrom_.x+256,learnedFrom_.y};
                const auto fresh=BWAPI::UnitCommand::move(u,{learnedFreshGoal_.x,learnedFreshGoal_.y});
                const auto budget=module_.bridge_.commandBudgetRemaining();
                learnedReject_=true;
                check("learned-active-raw-permission-rejected",!module_.bridge_.executeWholeGame(fresh,240));
                learnedReject_=false;
                module_.bridge_.beginCommandBudget(frame,0);
                check("learned-active-raw-zero-budget-rejected",!module_.bridge_.executeWholeGame(fresh,240));
                module_.bridge_.beginCommandBudget(frame,budget);
                learnedFreshAccepted_=module_.bridge_.executeWholeGame(fresh,240);
                result_ << "LEARNED_FRESH_RAW," << frame << ",actor=" << learnedActor_
                    << ",accepted=" << learnedFreshAccepted_ << ",x=" << learnedFreshGoal_.x
                    << ",y=" << learnedFreshGoal_.y << ",budgetBefore=" << budget
                    << ",budgetAfter=" << module_.bridge_.commandBudgetRemaining() << '\n';
                check("learned-active-fresh-raw-rejected",!learnedFreshAccepted_);
                check("learned-active-raw-keeps-budget",module_.bridge_.commandBudgetRemaining()==budget);
                check("learned-active-raw-does-not-recreate-owner",!learnedHeld());
            }
            auto old=learnedSeed_;old.earliestFrame=frame;old.deadlineFrame=frame;old.alreadyActive=false;
            const auto staleBefore=module_.commands_.stats().staleLease;
            module_.commands_.submit(old);
            check(stage_==4?"learned-confirmed-retires-old-generation":"learned-unconfirmed-retains-old-generation",
                module_.commands_.stats().staleLease==staleBefore+(stage_==4?1:0));
            learnedReject_=false;++stage_;return;
        }
        if(learnedUnconfirmedRawCase_ && !learnedGenerationChecked_ && frame>learnedWinnerFrame_) {
            auto old=learnedSeed_;old.earliestFrame=frame;old.deadlineFrame=frame;old.alreadyActive=false;
            const auto budget=module_.bridge_.commandBudgetRemaining();
            check("learned-raw-refresh-retires-old-generation",!module_.bridge_.execute(old));
            check("learned-stale-generation-does-not-consume-budget",module_.bridge_.commandBudgetRemaining()==budget);
            learnedGenerationChecked_=true;
        }
        const auto u=BWAPI::Broodwar->getUnit(learnedActor_);const auto order=u->getOrderTargetPosition();
        const Position at{u->getPosition().x,u->getPosition().y};
        const auto expectedFrame=learnedFreshAccepted_?learnedWinnerFrame_:learnedActiveCase_?learnedSeedFrame_:learnedWinnerFrame_;
        const auto expectedGoal=learnedFreshAccepted_?learnedFreshGoal_:learnedWinner_.targetPosition;
        if(u->getLastCommand().getType()==BWAPI::UnitCommandTypes::Move &&
           u->getLastCommandFrame()==expectedFrame && order.isValid() &&
           distanceSquared(expectedGoal,{order.x,order.y})<=64 &&
           distanceSquared(at,learnedFrom_)>=256 &&
           distance(at,expectedGoal)+8<=distance(learnedFrom_,expectedGoal)) {
            result_ << "LEARNED_NATIVE_WINNER," << frame << ",actor=" << learnedActor_
                << ",commandFrame=" << expectedFrame << ",winnerFrame=" << learnedWinnerFrame_
                << ",x=" << expectedGoal.x << ",y=" << expectedGoal.y
                << ",orderX=" << order.x << ",orderY=" << order.y << ",fromX=" << learnedFrom_.x << ",fromY=" << learnedFrom_.y
                << ",observedX=" << at.x << ",observedY=" << at.y << '\n';
            if(learnedFreshRawCase_)check("learned-active-native-winner-preserved",!learnedFreshAccepted_);
            check("learned-native-winner-measured",true);
            check("learned-no-ghost-owner-after-native-effect",learnedUnconfirmedRawCase_?learnedHeld():!learnedHeld());
            check("learned-no-duplicate-accepted-actor-frame",!badDuplicate_);finish(frame);
        }
        if(!finished_ && frame>=180){check("learned-bounded-native-completion",false);finish(frame);}
    }


    bool builderAdmissionCase_{};

    bool fragileRetreatCase_{};
    UnitId fragileActor_{-1}, healthyActor_{-1};
    Position fragileOrigin_{-1,-1}, healthyOrigin_{-1,-1};
    std::array<Position,3> fragileGoals_{};
    void fragileRetreatFrame(Frame frame) {
        if(frame==24) {
            for(const auto& u:module_.state_.self.units)if(u.kind==UnitKind::zealot) {
                if(u.healthFraction()<0.28){fragileActor_=u.id;fragileOrigin_=u.position;}
                else {healthyActor_=u.id;healthyOrigin_=u.position;}
            }
            check("fragile-live-wounded-actor",fragileActor_>=0 && fragileOrigin_==Position{1024,1024});
            check("fragile-live-healthy-control",healthyActor_>=0 && healthyOrigin_==Position{1536,1024});
            if(fragileActor_<0 || healthyActor_<0){finish(frame);return;}
            const auto healthy=*module_.state_.findUnit(healthyActor_);
            CombatEstimate engage;engage.decision=FightDecision::engage;engage.ratio=2;
            const DefenseArea healthyDefense{{1536,1280},96,{-1,-1},{-1,-1}};
            auto healthyCommands=module_.tactics_.control(std::span{&healthy,1},{},engage,
                healthyDefense.center,{1536,768},module_.influence_,healthy.position,0,false,
                healthyDefense,TacticalIntent::battle,{},nullptr,{},nullptr,true,{}, {},frame);
            check("fragile-healthy-return-remains-legal",healthyCommands.size()==1 &&
                healthyCommands[0].source=="defense-return" &&
                healthyCommands[0].targetPosition==healthyDefense.center);
            if(healthyCommands.size()==1) {
                healthyCommands[0].owner=CommandOwner::combat;
                check("fragile-healthy-return-api-accepted",module_.bridge_.execute(healthyCommands[0]));
            }
        }
        if(frame==24 || frame==48 || frame==72) {
            const auto round=(frame-24)/24;
            const auto actor=module_.state_.findUnit(fragileActor_);
            if(!actor){check("fragile-actor-survives",false);finish(frame);return;}
            CombatEstimate estimate;estimate.ratio=round==1?0.5:2;
            estimate.decision=round==1?FightDecision::retreat:FightDecision::engage;
            const DefenseArea defense{{1024,1280},96,{-1,-1},{-1,-1}};
            auto commands=module_.tactics_.control(std::span{&*actor,1},{},estimate,
                defense.center,{1024,768},module_.influence_,actor->position,0,false,
                defense,TacticalIntent::battle,{},nullptr,{},nullptr,true,{}, {},frame);
            check(("fragile-candidate-still-escapes-"+std::to_string(round)).c_str(),
                commands.size()==1 && commands[0].source=="combat-retreat" &&
                commands[0].targetPosition.y<fragileOrigin_.y);
            if(commands.size()!=1){finish(frame);return;}
            auto command=commands[0];command.owner=CommandOwner::combat;
            fragileGoals_[round]=command.targetPosition;
            result_ << "FRAGILE_PROPOSAL," << frame << ",round=" << round
                << ",actor=" << actor->id << ",fromX=" << actor->position.x << ",fromY=" << actor->position.y
                << ",x=" << command.targetPosition.x << ",y=" << command.targetPosition.y
                << ",decision=" << static_cast<int>(estimate.decision)
                << ",health=" << actor->healthFraction() << '\n';
            check(("fragile-api-accepted-"+std::to_string(round)).c_str(),module_.bridge_.execute(command));
        }
        if(frame==47 || frame==71 || frame==95) {
            const auto round=(frame-47)/24;const auto commandFrame=24+round*24;
            const auto u=BWAPI::Broodwar->getUnit(fragileActor_);
            const auto goal=fragileGoals_[round];
            const auto native=u->getLastCommand();const auto pos=u->getPosition();
            const auto nativeGoal=native.getTargetPosition();
            result_ << "FRAGILE_NATIVE," << frame << ",round=" << round << ",actor=" << fragileActor_
                << ",commandFrame=" << u->getLastCommandFrame()
                << ",x=" << nativeGoal.x << ",y=" << nativeGoal.y
                << ",orderX=" << u->getOrderTargetPosition().x << ",orderY=" << u->getOrderTargetPosition().y
                << ",fromX=" << fragileOrigin_.x << ",fromY=" << fragileOrigin_.y
                << ",observedX=" << pos.x << ",observedY=" << pos.y << '\n';
            check(("fragile-native-order-bound-"+std::to_string(round)).c_str(),
                u->getLastCommandFrame()==commandFrame && native.getType()==BWAPI::UnitCommandTypes::Move &&
                nativeGoal.x==goal.x && nativeGoal.y==goal.y);
            check(("fragile-native-safe-travel-"+std::to_string(round)).c_str(),pos.y<=fragileOrigin_.y-16);
            if(round==0) {
                const auto healthy=BWAPI::Broodwar->getUnit(healthyActor_);
                result_ << "FRAGILE_HEALTHY_NATIVE," << frame << ",actor=" << healthyActor_
                    << ",commandFrame=" << healthy->getLastCommandFrame()
                    << ",x=" << healthy->getLastCommand().getTargetPosition().x
                    << ",y=" << healthy->getLastCommand().getTargetPosition().y
                    << ",fromY=" << healthyOrigin_.y << ",observedY=" << healthy->getPosition().y << '\n';
                check("fragile-healthy-native-return",healthy->getLastCommandFrame()==24 &&
                    healthy->getLastCommand().getType()==BWAPI::UnitCommandTypes::Move &&
                    healthy->getLastCommand().getTargetPosition()==BWAPI::Position{1536,1280} &&
                    healthy->getPosition().y>=healthyOrigin_.y+16);
            }
            if(round==2) {
                check("fragile-no-duplicate-actor-frame",!badDuplicate_);
                check("fragile-no-build-or-load-spending",seedBuilds_==0 && seedLoads_==0);
                finish(frame);
            }
        }
    }

    void builderAdmissionFrame(Frame frame) {
        module_.navigation_=module_.bridge_.navigationGrid();
        module_.bridge_.initializeNavigationObstacles(module_.navigation_);
        module_.bridge_.advanceConstructionRoutes(frame,module_.navigation_);
        const auto site=std::ranges::min_element(module_.state_.bases,[](const auto& a,const auto& b){
            return distanceSquared(a.center,Position{3680,1328})<distanceSquared(b.center,Position{3680,1328});});
        check("admission-native-four-probes",std::ranges::count(module_.state_.self.units,UnitKind::probe,&UnitSnapshot::kind)==4);
        check("admission-native-natural-site",site!=module_.state_.bases.end() && site->mineralPatches>=4 &&
              distanceSquared(site->center,Position{3680,1328})<96*96);
        if(failures_){finish(frame);return;}
        MacroAction action;action.action=MacroActionKind::expand;action.target=UnitKind::nexus;
        action.priority=130;action.minerals=400;action.reserved=true;action.blocksLowerPriority=true;
        action.constructionSite={0x4040,site->id,site->center};
        module_.plan_.desiredBases=2;module_.plan_.expansionTarget=site->center;module_.plan_.rallyPoint={3808,464};
        int pendingScreens=0;
        module_.bridge_.buildRouteDiagnostic=[this,&pendingScreens](const BuildRouteDiagnostic& d){
            if(d.stage=="builder-screen" && d.searchBudgetExhausted)++pendingScreens;
            result_ << "ADMISSION_ROUTE," << d.frame << ",actor=" << d.builder
                << ",fromX=" << d.from.x << ",fromY=" << d.from.y
                << ",x=" << d.anchor.x << ",y=" << d.anchor.y
                << ",nativeHasPath=" << d.nativeHasPath << ",pending=" << d.searchBudgetExhausted << '\n';
        };
        static_cast<void>(module_.bridge_.executeMacro(std::span{&action,1},module_.plan_,
            module_.influence_,{},1,&module_.navigation_,1'000'000));
        module_.bridge_.buildRouteDiagnostic={};
        const auto work=module_.bridge_.constructionRoutingDiagnostics();
        result_ << "ADMISSION_QUEUE," << frame << ",queued=" << work.queued
            << ",work=" << work.frameWork << ",activeOwner=" << work.activeOwner
            << ",activeFrom=" << work.activeFrom.x << 'x' << work.activeFrom.y
            << ",activeTo=" << work.activeTo.x << 'x' << work.activeTo.y
            << ",admissionDeferred=" << work.admissionDeferred << ",screens=" << pendingScreens << '\n';
        check("admission-native-screen-is-pending",pendingScreens>0 && work.activeOwner>0);
        check("admission-one-uncertain-builder-route",work.queued<=1);
        check("admission-shared-work-cap",work.frameWork<=2048);
        check("admission-no-premature-build-spending",seedBuilds_==0 && acceptedPerActor_.empty());
        finish(frame);
    }

    void competingProposals(Frame frame) {
        for(std::size_t i=0;i<roles_.size();++i) {
            auto old=roles_[i].oldOrder;old.earliestFrame=frame;old.deadlineFrame=frame;old.alreadyActive=false;
            module_.commands_.submit(old);
            if(stage_==3) {
                const auto actor=module_.state_.findUnit(roles_[i].actor);
                if(actor)roles_[i].acceptedPosition=actor->position;
            }
            roles_[i].emergency=move(roles_[i].actor,
                i==0?Position{1776,2528}:i==1?Position{640,1600}:i==2?Position{1024,1500}:
                i==3?Position{768,1024}:Position{1280,1800},frame,"mixed-emergency-"+std::to_string(i));
            module_.commands_.submit(roles_[i].emergency);
        }
    }
    void staleOrders(Frame frame) {
        for(std::size_t i=0;i<4;++i) {
            auto old=roles_[i].oldOrder;old.deadlineFrame=frame;old.earliestFrame=frame;old.alreadyActive=false;
            const auto before=module_.commands_.stats().staleLease;
            module_.commands_.submit(old);
            check(("mixed-retired-generation-"+std::to_string(i)).c_str(),
                  module_.commands_.stats().staleLease==before+1);
        }
    }
    void initialize(Frame frame) {
        const auto& state=module_.state_;
        initialMinerals_=state.self.minerals;
        for(const auto& u:state.self.units) {
            if(u.kind==UnitKind::probe) {
                if(distanceSquared(u.position,Position{1776,2192})<64*64)roles_[0].actor=u.id;
                if(distanceSquared(u.position,Position{640,1200})<64*64)roles_[1].actor=u.id;
                if(distanceSquared(u.position,Position{800,1500})<64*64)roles_[2].actor=u.id;
            }
            if(u.kind==UnitKind::shuttle)shuttle_=u.id;
            if(u.kind==UnitKind::reaver)passenger_=u.id;
            if(u.kind==UnitKind::dragoon)roles_[4].actor=u.id;
        }
        roles_[3].actor=loadedCase_?shuttle_:passenger_;
        check("mixed-five-distinct-live-actors",std::ranges::all_of(roles_,[](const auto& r){return r.actor>=0;}) &&
            [&]{std::vector<UnitId> ids;for(const auto& r:roles_)ids.push_back(r.actor);std::ranges::sort(ids);
                return std::adjacent_find(ids.begin(),ids.end())==ids.end();}());
        const auto site=std::ranges::min_element(state.bases,[](const auto& a,const auto& b){
            return distanceSquared(a.center,Position{2050,2000})<distanceSquared(b.center,Position{2050,2000});});
        const bool valid=site!=state.bases.end() && site->mineralPatches>=4 && site->ownerId<0 &&
            distanceSquared(site->center,Position{2050,2000})<600*600;
        check("mixed-remote-nexus-site",valid);
        if(failures_ || !valid){finish(frame);return;}
        build_.action=MacroActionKind::expand;build_.target=UnitKind::nexus;build_.priority=130;
        build_.minerals=400;build_.reserved=true;build_.reason="mixed-seed-build";
        build_.constructionSite={0x1212,site->id,site->center};
        module_.plan_.desiredBases=2;module_.plan_.expansionTarget=site->center;
        module_.plan_.rallyPoint={512,528};
        check("mixed-build-proposal",module_.bridge_.executeMacro(std::span{&build_,1},module_.plan_,
            module_.influence_,{},1,nullptr,1'000'000)==1);
        const std::array<UnitId,1> available{roles_[1].actor};
        module_.pendingScoutOrders_=module_.scouts_.assign(state,available,module_.influence_,{},nullptr);
        check("mixed-real-scout-mission",module_.pendingScoutOrders_.size()==1 &&
            module_.pendingScoutOrders_.front().leaseGeneration>0);
        if(module_.pendingScoutOrders_.size()!=1){finish(frame);return;}
        module_.leasedScouts_.push_back(roles_[1].actor);
        static_cast<void>(module_.bridge_.submitScouts(module_.pendingScoutOrders_,module_.commands_));
        const WorkerAssignment worker{roles_[2].actor,WorkerJob::rebuild,1,-1,{1024,2048},60};
        check("mixed-real-worker-proposal",module_.bridge_.submitWorkerCommands(std::span{&worker,1},module_.commands_)==1);
        static_cast<void>(module_.transports_.control(state,{2200,1024},{512,528},module_.influence_));
        Command load{shuttle_,CommandType::load,passenger_,{-1,-1},UnitKind::unknown,200,frame,"mixed-seed-load"};
        load.owner=CommandOwner::transport;load.deadlineFrame=frame;
        load.leaseGeneration=module_.transports_.leaseGeneration(shuttle_);module_.commands_.submit(load);
        auto fighter=move(roles_[4].actor,{1800,1500},frame,"mixed-seed-fighter",70);module_.commands_.submit(fighter);
        module_.dispatchFrameCommands();
        const auto& winners=module_.commands_.authorityWinners();
        for(auto& role:roles_) {
            const auto found=std::ranges::find(winners,role.actor,&Command::actor);
            if(found!=winners.end())role.oldOrder=*found;
        }
        // The unissued passenger Load intent still holds the transport generation.
        if(!loadedCase_) {
            roles_[3].oldOrder={passenger_,CommandType::move,-1,{1024,1024},UnitKind::unknown,100,frame,"mixed-old-passenger"};
            roles_[3].oldOrder.owner=CommandOwner::transport;
            roles_[3].oldOrder.leaseGeneration=module_.transports_.leaseGeneration(passenger_);
        }
        check("mixed-seed-one-accepted-command-per-role",winners.size()==5 && seedBuilds_==1 && seedLoads_==1 &&
            std::ranges::all_of(acceptedPerActor_,[](const auto& x){return x.second==1;}) && acceptedPerActor_.size()==5);
        check("mixed-native-owners-held-after-acceptance",builderHeld() && scoutHeld() && module_.transports_.ownsReaver(passenger_));
        seedFrame_=frame;stage_=1;
    }
    void compete(Frame frame) {
        competingProposals(frame);
        if(stage_==1)module_.bridge_.beginCommandBudget(frame,0);
        module_.dispatchFrameCommands();
        const auto& winners=module_.commands_.authorityWinners();
        check("mixed-five-emergency-winners",winners.size()==5 && std::ranges::all_of(winners,[](const auto& c){
            return c.owner==CommandOwner::combat && c.source.starts_with("mixed-emergency-");}));
        if(stage_==1) {
            check("mixed-budget-deferred-retains-all-owner-state",module_.commands_.budgetDeferredCommands().size()==5 &&
                emergencyAccepted_==0 && builderHeld() && scoutHeld() && module_.transports_.ownsReaver(passenger_));
            ++stage_;return;
        }
        if(stage_==2) {
            check("mixed-rejected-commands-retain-all-owner-state",emergencyRejected_==5 && emergencyAccepted_==0 &&
                builderHeld() && scoutHeld() && module_.transports_.ownsReaver(passenger_));
            ++stage_;return;
        }
        acceptedFrame_=frame;
        check("mixed-five-api-accepted-in-same-frame",emergencyAccepted_==5 &&
            std::ranges::all_of(roles_,[this,frame](const auto& r){return acceptedPerActor_[{frame,r.actor}]==1;}));
        check("mixed-confirmed-scout-state-release",module_.leasedScouts_.empty() && module_.pendingScoutOrders_.empty() &&
            module_.scouts_.missionMetrics().cancelled==1);
        check("mixed-construction-held-until-later-native-ack",builderHeld());
        check("mixed-confirmed-transport-mission-release",module_.transports_.ownsReaver(passenger_)==loadedCase_);
        staleOrders(frame);
        module_.scouts_.recordCommandFeedback({roles_[1].actor,ScoutCommandStatus::rejected,roles_[1].oldOrder.leaseGeneration},frame);
        check("mixed-stale-scout-feedback-cannot-resurrect",module_.scouts_.missionMetrics().cancelled==1);
        for(const auto& role:roles_) {
            auto bypass=move(role.actor,{2200,2200},frame,"mixed-bypass-probe");
            check("mixed-native-claim-rejects-second-same-frame-order",!module_.bridge_.execute(bypass));
        }
        module_.bridge_.beginFrameCommands(module_.commands_,frame);
        check("mixed-no-replacement-build-before-native-ack",module_.bridge_.executeMacro(std::span{&build_,1},
            module_.plan_,module_.influence_,{},1,nullptr,1'000'000)==0);
        stage_=4;
    }
    void observeEffects(Frame frame) {
        const auto& state=module_.state_;
        for(std::size_t i=0;i<roles_.size();++i) {
            auto& r=roles_[i];const auto native=BWAPI::Broodwar->getUnit(r.actor);
            const auto liveTarget=native?native->getOrderTargetPosition():BWAPI::Positions::Unknown;
            const bool liveDestinationMatches=liveTarget.isValid() &&
                distanceSquared(r.emergency.targetPosition,Position{liveTarget.x,liveTarget.y})<=8*8;
            if(!r.moved && native && native->exists() && native->getLastCommandFrame()==acceptedFrame_) {
                result_ << "MIXED_MOVE_PENDING," << frame << ",role=" << i << ",actor=" << r.actor
                    << ",commandFrame=" << acceptedFrame_ << ",lastX=" << native->getLastCommand().getTargetPosition().x
                    << ",lastY=" << native->getLastCommand().getTargetPosition().y << ",orderX=" << liveTarget.x
                    << ",orderY=" << liveTarget.y << ",observedX=" << native->getPosition().x << ",observedY=" << native->getPosition().y
                    << ",fromX=" << r.acceptedPosition.x << ",fromY=" << r.acceptedPosition.y << ",nativeOrder=" << native->getOrder().toString()
                    << ",moving=" << native->isMoving() << ",active=" << module_.bridge_.commandActive(r.emergency) << '\n';
            }
            if(!r.moved && native && native->exists() && native->getLastCommandFrame()==acceptedFrame_ &&
                liveDestinationMatches && module_.bridge_.commandActive(r.emergency) &&
                r.acceptedPosition.valid() &&
                distanceSquared(Position{native->getPosition().x,native->getPosition().y},r.acceptedPosition)>=16*16 &&
                distance(Position{native->getPosition().x,native->getPosition().y},r.emergency.targetPosition)+8.0 <=
                    distance(r.acceptedPosition,r.emergency.targetPosition)) {
                r.moved=true;
                result_ << "MIXED_NATIVE_MOVE," << frame << ",role=" << i << ",actor=" << r.actor
                    << ",commandFrame=" << native->getLastCommandFrame() << ",x=" << r.emergency.targetPosition.x
                    << ",y=" << r.emergency.targetPosition.y << ",fromX=" << r.acceptedPosition.x << ",fromY=" << r.acceptedPosition.y
                    << ",observedX=" << native->getPosition().x << ",observedY=" << native->getPosition().y << ",orderX=" << liveTarget.x << ",orderY=" << liveTarget.y << ",moving=" << native->isMoving() << '\n';
                check("mixed-physical-emergency-order-bound",frame>acceptedFrame_ && native->isMoving());
            }
        }
        if(!builderHeld() && !buildReleased_) {
            buildReleased_=true;
            const auto builderNative=BWAPI::Broodwar->getUnit(roles_[0].actor);
            const auto orderTarget=builderNative?builderNative->getOrderTargetPosition():BWAPI::Positions::Unknown;
            result_ << "MIXED_BUILD_RELEASE," << frame << ",acceptedFrame=" << acceptedFrame_
                << ",actor=" << roles_[0].actor << ",orderX=" << orderTarget.x << ",orderY=" << orderTarget.y << '\n';
            // Lease acknowledgment and measured travel are independent gates.
            check("mixed-builder-release-after-native-replacement",frame>acceptedFrame_ && builderNative &&
                builderNative->getLastCommandFrame()==acceptedFrame_ && builderNative->getOrder()==BWAPI::Orders::Move &&
                orderTarget.isValid() && distanceSquared(Position{orderTarget.x,orderTarget.y},roles_[0].emergency.targetPosition)<=8*8);
        }
        const auto ns=BWAPI::Broodwar->getUnit(shuttle_);
        if(!nativeStop_ && stopFrame_>=0 && ns && ns->exists() && ns->getLastCommandFrame()==stopFrame_ &&
            ns->getLastCommand().getType()==BWAPI::UnitCommandTypes::Stop && ns->isIdle()) {
            nativeStop_=true;result_ << "MIXED_NATIVE_STOP," << frame << ",actor=" << shuttle_ << ",commandFrame=" << stopFrame_ << '\n';
        }
        const auto recovery=module_.transports_.control(state,{2200,1024},{512,528},module_.influence_);
        for(const auto& command:recovery)module_.commands_.submit(command);
        module_.dispatchFrameCommands();
        const auto cargo=state.findUnit(passenger_);
        const bool aboard=cargo && cargo->loaded && cargo->transportId==shuttle_;
        const bool nativeCargoReleased=loadedCase_?unloadFrame_>=0 && !aboard && frame>unloadFrame_:
            nativeStop_ && !aboard && frame>=acceptedFrame_+48;
        if(nativeCargoReleased && std::ranges::all_of(roles_,[](const auto& r){return r.moved;}) && buildReleased_) {
            check("mixed-no-ghost-owner-after-native-effects",!builderHeld() && module_.leasedScouts_.empty() &&
                module_.pendingScoutOrders_.empty() && !module_.transports_.ownsReaver(passenger_));
            check("mixed-no-duplicate-build-load-or-spending",seedBuilds_==1 && seedLoads_==1 &&
                state.self.minerals==initialMinerals_ && std::ranges::count(state.self.units,UnitKind::nexus,&UnitSnapshot::kind)==1 &&
                !badDuplicate_ && (loadedCase_?stops_==0 && unloads_==1:stops_==1 && unloads_==0));
            if(loadedCase_) {
                result_ << "MIXED_NATIVE_UNLOADED," << frame << ",shuttle=" << shuttle_ << ",passenger=" << passenger_
                    << ",commandFrame=" << ns->getLastCommandFrame() << ",loaded=" << aboard << '\n';
                check("mixed-actual-safe-cargo-unload",cargo && !cargo->loaded && ns->getLastCommandFrame()==unloadFrame_ &&
                    ns->getLastCommand().getType()==BWAPI::UnitCommandTypes::Unload_All_Position);
            } else check("mixed-native-pickup-stop-and-no-reload",nativeStop_ && recovery.empty());
            finish(frame);
        }
    }
public:
    void onStart() override {
        std::ifstream scenarioFile("bwapi-data/read/scenario.txt");scenarioFile>>scenario_;
        scoutMiningCase_=scenario_=="scout-mining-planner";
        depletionLiveUnsafeCase_=scenario_=="depletion-live-unsafe";
        depletionOrganic_=scenario_=="depletion-organic" || scenario_=="depletion-pressure" || depletionLiveUnsafeCase_;
        depletionPressure_=scenario_=="depletion-pressure" || depletionLiveUnsafeCase_;
        depletionCase_=scenario_=="depletion-ready" || scenario_=="depletion-blocked" || depletionOrganic_;
        depletionBlocked_=scenario_=="depletion-blocked";
        loadedCase_=scenario_=="owner-mixed-loaded";
        builderAdmissionCase_=scenario_=="builder-admission";
        fragileRetreatCase_=scenario_=="fragile-defense-retreat";
        rawCase_=scenario_=="raw-actor-claims";
        learnedUnconfirmedRawCase_=scenario_=="learned-owner-unconfirmed-raw";
        learnedFreshRawCase_=scenario_=="learned-owner-active-raw";
        learnedCase_=scenario_=="learned-owner-emergency" || scenario_=="learned-owner-active" || learnedFreshRawCase_ || learnedUnconfirmedRawCase_;
        learnedActiveCase_=scenario_=="learned-owner-active" || learnedFreshRawCase_;
        result_.open("bwapi-data/write/scenario.csv",std::ios::trunc);
        result_ << "START," << scenario_ << ',' << BWAPI::Broodwar->mapFileName() << '\n';
        BWAPI::Broodwar->setLocalSpeed(0);BWAPI::Broodwar->setFrameSkip(1);
        if(depletionCase_){depletionStart();return;}
        if(scoutMiningCase_){scoutMiningStart();return;}
        module_.bridge_.onStart();
        module_.bridge_.productionPermission=[this](const BWAPI::UnitCommand&,std::string_view source){
            return !learnedReject_ && !rawPermissionReject_ && !(stage_==2 && source.starts_with("mixed-emergency-"));};
        module_.bridge_.actionDiagnostic=[this](const ActionDiagnostic& d){
            const auto frame=BWAPI::Broodwar->getFrameCount();
            if(d.accepted) {
                if(++acceptedPerActor_[{frame,d.actor}]>1)badDuplicate_=true;
                if(d.source=="learned-native-emergency")++learnedWinnerAccepted_;
                if(d.type=="Build")++seedBuilds_;
                if(d.source=="mixed-seed-load")++seedLoads_;
                if(d.source.starts_with("mixed-emergency-"))++emergencyAccepted_;
                if(d.type=="Stop"){++stops_;stopFrame_=frame;}
                if(d.source=="transport-handoff-unload"){++unloads_;unloadFrame_=frame;}
            } else if(stage_==2 && d.source.starts_with("mixed-emergency-") && d.outcome=="production-permission-rejected")++emergencyRejected_;
            result_ << (fragileRetreatCase_?"FRAGILE_ACTION,":learnedCase_?"LEARNED_ACTION,":rawCase_?"RAW_ACTION,":"MIXED_ACTION,") << frame << ',' << d.actor << ',' << d.target << ',' << d.source << ',' << d.type
                << ',' << d.attempted << ',' << d.accepted << ',' << d.position.x << ',' << d.position.y << ',' << d.outcome << '\n';
            result_.flush();
        };
    }
    void onFrame() override {
        if(finished_)return;const auto frame=BWAPI::Broodwar->getFrameCount();
        if(depletionCase_){depletionFrame(frame);return;}
        if(frame<24)return;
        module_.commands_.beginFrame(frame,BWAPI::Broodwar->getLatencyFrames());
        module_.bridge_.beginCommandBudget(frame,96);module_.bridge_.beginFrameCommands(module_.commands_,frame);
        struct Scope{BwapiBridge& bridge;~Scope(){bridge.endFrameCommands();}} scope{module_.bridge_};
        module_.state_=module_.bridge_.observe();module_.influence_.update(module_.state_);
        if(scoutMiningCase_) {scoutMiningFrame(frame);return;}
        if(fragileRetreatCase_) {fragileRetreatFrame(frame);return;}
        if(builderAdmissionCase_) {builderAdmissionFrame(frame);return;}
        if(learnedCase_) {learnedFrame(frame);return;}
        if(rawCase_) {rawFrame(frame);return;}
        if(stage_==0)initialize(frame);
        else if(stage_==1) {
            const auto ns=BWAPI::Broodwar->getUnit(shuttle_);const auto cargo=module_.state_.findUnit(passenger_);
            const bool loadReady=loadedCase_?cargo && cargo->loaded && cargo->transportId==shuttle_:
                ns && ns->exists() && ns->getLastCommand().getType()==BWAPI::UnitCommandTypes::Load &&
                ns->getOrderTarget() && ns->getOrderTarget()->getID()==passenger_;
            const bool ready=loadReady && module_.bridge_.commandActive(roles_[1].oldOrder) &&
                module_.bridge_.commandActive(roles_[2].oldOrder) && module_.bridge_.commandActive(roles_[4].oldOrder);
            if(ready) {check("mixed-seed-orders-observed-before-conflict",frame>seedFrame_ && builderHeld());compete(frame);}
        } else if(stage_<4)compete(frame);
        else observeEffects(frame);
        if(!finished_ && frame>=720){check("mixed-bounded-native-completion",false);finish(frame);}
    }
    void onUnitDiscover(BWAPI::Unit u) override {if(depletionCase_)module_.onUnitDiscover(u);}
    void onUnitShow(BWAPI::Unit u) override {if(depletionCase_)module_.onUnitShow(u);}
    void onUnitDestroy(BWAPI::Unit u) override {
        if(depletionCase_){
            if(u && depletionPressureCannons_.contains(u->getID())) {
                depletionPressureCannons_[u->getID()]=false;
                result_<<"ECON_PRESSURE_DESTROY,"<<BWAPI::Broodwar->getFrameCount()<<",actor="<<u->getID()<<"\n";
            }
            if(u && depletionBlockerDepots_.contains(u->getID()))depletionBlockerLost_=true;
            if(u && depletionMainMinerals_.contains(u->getID())) {
                depletionMainMinerals_[u->getID()]=0;
                result_<<"ECON_MAIN_DESTROY,"<<BWAPI::Broodwar->getFrameCount()<<",mineral="<<u->getID()<<"\n";
            }
            module_.onUnitDestroy(u);
        }
    }
    void onUnitMorph(BWAPI::Unit u) override {if(depletionCase_)module_.onUnitMorph(u);}
    void onUnitRenegade(BWAPI::Unit u) override {if(depletionCase_)module_.onUnitRenegade(u);}
    void onUnitCreate(BWAPI::Unit u) override {if(depletionCase_)module_.onUnitCreate(u);}
    void onUnitComplete(BWAPI::Unit u) override {
        if(depletionCase_) {
            module_.onUnitComplete(u);
            if(depletionOrganic_ && depletionEmpty_>=0 && u && u->getPlayer()==BWAPI::Broodwar->self() &&
               !u->getType().isBuilding() && std::ranges::find(depletionInitialActors_,u->getID())==depletionInitialActors_.end()) {
                ++depletionPostUnits_;
                result_<<"ECON_POST_DEPLETION_UNIT,"<<BWAPI::Broodwar->getFrameCount()<<",actor="<<u->getID()
                    <<",type="<<u->getType().getID()<<"\n";
            }
        }
    }
    void onEnd(bool won) override {if(depletionCase_)module_.onEnd(won);result_ << "END," << BWAPI::Broodwar->getFrameCount() << ',' << won << '\n';result_.flush();}
};
}
extern "C" __declspec(dllexport) void gameInit(BWAPI::Game* game){BWAPI::BroodwarPtr=game;}
extern "C" __declspec(dllexport) BWAPI::AIModule* newAIModule(){return new protodd::bwapi::ActorAuthorityFixture;}
BOOL APIENTRY DllMain(HMODULE,DWORD,LPVOID){return TRUE;}
