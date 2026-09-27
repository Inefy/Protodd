// Isolated UMS validation module. Never packaged as the playing bot.
#include "BwapiBridge.hpp"
#include "protodd/Technology.hpp"
#include <BWAPI.h>
#include <windows.h>
#include <fstream>
#include <algorithm>

using namespace protodd;
class AuditScenario final : public BWAPI::AIModule {
    protodd::bwapi::BwapiBridge bridge;
    MacroPlanner planner;
    std::ofstream log;
    std::string scenario;
    bool checked{}, requested{}, accepted{}, confirmed{}, finished{};
    int initialEnergy{}, selected{-1}, failures{};
    void check(const char* name, bool value) {
        log << "CHECK," << name << ',' << value << '\n';
        if (!value) ++failures;
        log.flush();
    }
public:
    void onStart() override {
        std::ifstream("bwapi-data/read/scenario.txt") >> scenario;
        log.open("bwapi-data/write/scenario.csv");
        log << "START," << scenario << ',' << BWAPI::Broodwar->mapFileName() << '\n'; log.flush();
        BWAPI::Broodwar->setLocalSpeed(0);
        BWAPI::Broodwar->setFrameSkip(64);
        bridge.onStart();
        log << "READY," << BWAPI::Broodwar->isPaused() << ',' << BWAPI::Broodwar->isInGame() << '\n'; log.flush();
    }
    void onEnd(bool won) override { log << "END," << BWAPI::Broodwar->getFrameCount() << ',' << won << '\n'; log.flush(); ExitProcess(0); }
    void onFrame() override {
        if(finished) return;
        const int frame=BWAPI::Broodwar->getFrameCount();
        if(frame<2) { log << "FRAME," << frame << ',' << BWAPI::Broodwar->isPaused() << '\n'; log.flush(); }
        if (frame<24) return;
        auto state=bridge.observe();
        InfluenceMap influence; influence.update(state);
        if (!checked) {
            checked=true;
            log << "STATE," << frame << ',' << state.self.units.size() << ',' << state.enemy.units.size()
                << ',' << state.self.minerals << ',' << state.self.gas << '\n';
            for(auto u:BWAPI::Broodwar->getAllUnits()) if(u->exists())
                log << "UNIT," << u->getID() << ',' << u->getType().getName() << ',' << u->getPlayer()->getID()
                    << ',' << u->getPosition().x << ',' << u->getPosition().y << ',' << u->isPowered()
                    << ',' << u->getEnergy() << ',' << u->getHitPoints() << ',' << u->isInvincible() << '\n';
            if(scenario.starts_with("storm")) {
                std::vector<UnitSnapshot> squad;
                for(const auto& u:state.self.units) if(u.kind==UnitKind::highTemplar) squad.push_back(u);
                check("fixture-templar",squad.size()==1);
                check("fixture-enemies",state.enemy.units.size()>=4);
                check("fixture-researched",BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Psionic_Storm));
                if(!squad.empty()) { selected=squad[0].id; initialEnergy=squad[0].energy; }
                CombatEstimate estimate; estimate.decision=FightDecision::engage;
                auto commands=TacticalController{}.control(squad,state.enemy.units,estimate,{1100,1000},
                    {300,1000},influence,{800,1000},3,true,{},TacticalIntent::battle,{},nullptr,state.self.units);
                for(const auto& c:commands) if(c.technology==TechnologyKind::psionicStorm) {
                    requested=true; accepted=bridge.execute(c);
                    log << "CAST," << c.targetPosition.x << ',' << c.targetPosition.y << ',' << accepted << '\n';
                }
                check("storm-decision",requested==(scenario=="storm-clear"));
                if(requested) check("cast-accepted",accepted);
            } else if(scenario=="combat") {
                auto actor=std::ranges::find_if(state.self.units,[](const auto& u){return u.kind==UnitKind::zealot;});
                std::vector<UnitSnapshot> targets;
                for(const auto& u:state.enemy.units) if(u.kind==UnitKind::marine) targets.push_back(u);
                check("fixture-melee",actor!=state.self.units.end() && targets.size()==2);
                check("fixture-invincible",std::ranges::any_of(targets,[](const auto& u){return u.invincible;}));
                if(actor!=state.self.units.end()) {
                    auto target=CombatEvaluator{}.selectTarget(*actor,targets);
                    check("melee-legal-target",target!=nullptr && !target->invincible);
                }
                auto dt=std::ranges::find_if(state.enemy.units,[](const auto& u){return u.kind==UnitKind::darkTemplar;});
                check("fixture-cloaked-visible",dt!=state.enemy.units.end());
                if(dt!=state.enemy.units.end()) {
                    // For an undetected Dark Templar BWAPI also withholds the
                    // cloak flag. The relevant contract is unavailable health.
                    check("fixture-health-hidden",!dt->detected && dt->hitPoints==0);
                    log << "HIDDEN_DT," << dt->visible << ',' << dt->detected << ',' << dt->cloaked << ',' << dt->hitPoints << '\n';
                    check("hidden-health-threat",influence.at(dt->position).groundThreat>0);
                }
            } else if(scenario=="producer") {
                check("fixture-funded",state.self.minerals>=100 && state.self.gas>=100);
                std::vector<BWAPI::Unit> forges;
                for(auto u:BWAPI::Broodwar->self()->getUnits()) if(u->getType()==BWAPI::UnitTypes::Protoss_Forge) forges.push_back(u);
                std::sort(forges.begin(),forges.end(),[](auto a,auto b){return a->getID()<b->getID();});
                check("fixture-forges",forges.size()==2);
                if(forges.size()==2) {
                    check("fixture-first-unpowered",!forges[0]->isPowered());
                    check("fixture-second-legal",forges[1]->isPowered() && forges[1]->canUpgrade(BWAPI::UpgradeTypes::Protoss_Ground_Armor));
                    log << "ENGINE_CAN_UPGRADE_UNPOWERED," << forges[0]->canUpgrade(BWAPI::UpgradeTypes::Protoss_Ground_Armor) << '\n';
                    selected=forges[1]->getID();
                }
                MacroAction action{MacroActionKind::upgrade,UnitKind::unknown,100,100,100,true,"fixture",TechnologyKind::protossGroundArmor};
                StrategicPlan plan;
                accepted=bridge.executeMacro(std::span{&action,1},plan,influence)>0;
                check("legal-producer-accepted",accepted);
            } else if(scenario=="prerequisite") {
                check("fixture-level-one",BWAPI::Broodwar->self()->getUpgradeLevel(BWAPI::UpgradeTypes::Protoss_Ground_Weapons)==1);
                bool legal=false;for(auto u:BWAPI::Broodwar->self()->getUnits()) if(u->canUpgrade(BWAPI::UpgradeTypes::Protoss_Ground_Weapons))legal=true;
                check("fixture-upgrade-illegal",!legal);
                StrategicPlan plan;
                plan.goals={{GoalKind::upgrade,UnitKind::unknown,2,100,true,"fixture",TechnologyKind::protossGroundWeapons}};
                ResourceLedger ledger{state.self.minerals,state.self.gas};
                auto actions=planner.reconcile(state,plan,ledger);
                check("archives-first",!actions.empty() && actions[0].target==UnitKind::templarArchives && actions[0].reserved);
                accepted=bridge.executeMacro(actions,plan,influence)>0;
                check("archives-order-accepted",accepted);
                for(const auto& e:bridge.macroExecutions())log << "MACRO," << e.outcome << ',' << e.accepted << '\n';
            }
            log.flush();
        }
        if(scenario=="prerequisite") {
            for(auto u:BWAPI::Broodwar->self()->getUnits()) if(u->getType()==BWAPI::UnitTypes::Protoss_Templar_Archives) confirmed=true;
        } else if(scenario=="producer") {
            auto u=BWAPI::Broodwar->getUnit(selected); if(u && u->isUpgrading()) confirmed=true;
        } else if(scenario=="storm-clear") {
            auto u=BWAPI::Broodwar->getUnit(selected); if(u && u->getEnergy()<initialEnergy-50)confirmed=true;
        } else confirmed=!requested;
        const int limit=scenario=="prerequisite"?720:120;
        if(frame>=limit) {
            finished=true;
            check("engine-postcondition",confirmed);
            log << "DONE," << frame << ',' << failures << '\n'; log.flush();
            BWAPI::Broodwar->leaveGame();
        }
    }
};
extern "C" __declspec(dllexport) void gameInit(BWAPI::Game* game){BWAPI::BroodwarPtr=game;}
extern "C" __declspec(dllexport) BWAPI::AIModule* newAIModule(){return new AuditScenario;}
BOOL APIENTRY DllMain(HMODULE,DWORD,LPVOID){return TRUE;}
