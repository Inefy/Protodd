// Isolated stock-engine fixtures for the standalone RaceBot command issuers.
#include "RaceBotModule.hpp"

#include <BWAPI.h>
#include <windows.h>

#include <fstream>
#include <string>
#include <vector>

using namespace protodd::bwapi;

class RaceBotAuditScenario final : public BWAPI::AIModule {
    RaceBotModule bot{1};
    std::ofstream log_;
    std::string scenario_;
    bool fixtureChecked_{}, finished_{};
    bool acceptedRecordObserved_{};
    bool nativeTargetObserved_{};
    int observedCommandFrame_{-1};
    int failures_{};
    int actorId_{-1}, targetId_{-1};

    void check(const char* name, const bool value) {
        log_ << "CHECK," << name << ',' << (value ? 1 : 0) << '\n';
        if (!value) ++failures_;
        log_.flush();
    }

    [[nodiscard]] bool acceptedRecordExists() const {
        std::ifstream input("bwapi-data/write/RaceBot.log");
        if (observedCommandFrame_ < 0) return false;
        const auto expectedActor = "COMMAND," + std::to_string(observedCommandFrame_) +
            "," + std::to_string(actorId_) + ",";
        const auto expectedTarget = "target=" + std::to_string(targetId_) + ",";
        const auto expectedIssuer = scenario_ == "racebot-worker-defense"
            ? ",worker-defense,Attack_Unit," : ",combat-defense,Attack_Unit,";
        std::string line;
        while (std::getline(input, line))
            if (line.starts_with(expectedActor) &&
                line.find(expectedIssuer) != std::string::npos &&
                line.find(expectedTarget) != std::string::npos &&
                line.find("accepted=1,error=accepted") != std::string::npos)
                return true;
        return false;
    }

    int recoveryBuildFrame_{-1}, recoveryFixtureFrame_{-1}, recoveryCommandFrame_{-1};
    int recoveryStartedFrame_{-1}, recoveryCompletedFrame_{-1}, recoveryPausedRemaining_{-1};
    BWAPI::UnitType recoveryType_{BWAPI::UnitTypes::None};
    BWAPI::Position recoveryPosition_{BWAPI::Positions::None};
    bool recoveryFixtureAccepted_{}, recoveryInterrupted_{}, recoveryProgress_{};
    bool recoveryNoEarlyStructure_{true}, recoveryHeld_{};

    [[nodiscard]] bool constructionCase() const {
        return scenario_ == "racebot-construction-resume" || scenario_ == "racebot-construction-timeout";
    }

    [[nodiscard]] bool acceptedRecoveryRecord(const int frame, const int actor,
        const std::string& issuer, const std::string& command, const int target,
        const BWAPI::Position position = BWAPI::Positions::None) const {
        if (frame < 0) return false;
        std::ifstream input("bwapi-data/write/RaceBot.log");
        const auto prefix = "COMMAND," + std::to_string(frame) + ',' + std::to_string(actor) +
            ',' + issuer + ',' + command + ",target=" + std::to_string(target) + ',';
        std::string line;
        while (std::getline(input, line)) {
            if (!line.starts_with(prefix) ||
                line.find("accepted=1,error=accepted") == std::string::npos) continue;
            if (position.isValid() && line.find("x=" + std::to_string(position.x) +
                ",y=" + std::to_string(position.y) + ',') == std::string::npos) continue;
            return true;
        }
        return false;
    }

    bool issueFixture(const BWAPI::UnitCommand& command, const char* reason) {
        const bool accepted = command.getUnit()->issueCommand(command);
        const auto error = BWAPI::Broodwar->getLastError().toString();
        log_ << "FIXTURE," << BWAPI::Broodwar->getFrameCount() << ','
             << command.getUnit()->getID() << ',' << reason << ','
             << command.getType().toString() << ",accepted=" << accepted
             << ",error=" << (accepted ? "accepted" : error) << '\n';
        log_.flush();
        return accepted;
    }

    void finishRecovery(const int frame) {
        const bool resume = scenario_ == "racebot-construction-resume";
        check("recovery-initial-build-accepted-record", acceptedRecoveryRecord(
            recoveryBuildFrame_, actorId_, "macro-build", "Build", -1, recoveryPosition_));
        check("recovery-fixture-command-accepted", recoveryFixtureAccepted_);
        check("recovery-native-interruption-observed", resume ? recoveryInterrupted_ : recoveryHeld_);
        check("recovery-production-command-accepted-record", acceptedRecoveryRecord(
            recoveryCommandFrame_, actorId_, resume ? "construction-resume" : "construction-timeout-release",
            resume ? "Right_Click_Unit" : "Stop", resume ? targetId_ : -1));
        check("recovery-later-native-effect", nativeTargetObserved_);
        if (resume) {
            check("recovery-native-structure-started", recoveryStartedFrame_ > recoveryBuildFrame_);
            check("recovery-progress-after-resume", recoveryProgress_);
            check("recovery-exact-building-completed", recoveryCompletedFrame_ > recoveryCommandFrame_);
        } else {
            check("recovery-no-structure-before-timeout", recoveryNoEarlyStructure_);
            check("recovery-release-after-480-frame-lease", recoveryCommandFrame_ - recoveryBuildFrame_ > 480);
        }
        check("engine-postcondition", failures_ == 0);
        log_ << "DONE," << frame << ',' << failures_ << '\n';
        log_.flush(); finished_ = true; BWAPI::Broodwar->leaveGame();
    }

    void constructionFrame(const int frame) {
        using namespace BWAPI;
        const bool resume = scenario_ == "racebot-construction-resume";
        if (!fixtureChecked_) {
            fixtureChecked_ = true;
            std::vector<Unit> workers;
            bool nearbyEnemy = false;
            for (const auto unit : Broodwar->getAllUnits()) {
                if (unit == nullptr || !unit->exists()) continue;
                if (unit->getPlayer() == Broodwar->self() && unit->getType() == UnitTypes::Terran_SCV)
                    workers.push_back(unit);
                if (unit->getPlayer() != Broodwar->self() && unit->isVisible() &&
                    unit->getType().canAttack() && unit->getDistance(Position(512,512)) < 640)
                    nearbyEnemy = true;
            }
            check("recovery-fixture-terran-player", Broodwar->self()->getRace() == Races::Terran);
            check("recovery-fixture-one-scv", workers.size() == 1);
            check("recovery-fixture-no-nearby-threat", !nearbyEnemy);
            check("recovery-fixture-immediate-supply-demand",
                Broodwar->self()->supplyTotal() - Broodwar->self()->supplyUsed() <= 12);
            if (!workers.empty()) actorId_ = workers.front()->getID();
        }
        bot.onFrame();
        const auto actor = Broodwar->getUnit(actorId_);
        if (actor != nullptr && actor->exists()) {
            const auto last = actor->getLastCommand();
            if (recoveryBuildFrame_ < 0 && last.getType() == UnitCommandTypes::Build &&
                actor->getLastCommandFrame() == frame) {
                recoveryBuildFrame_ = frame;
                recoveryType_ = last.getUnitType(); recoveryPosition_ = last.getTargetPosition();
                log_ << "BUILD_CAPTURE," << frame << ',' << actorId_ << ",type=" << recoveryType_.getID()
                     << ",x=" << recoveryPosition_.x << ",y=" << recoveryPosition_.y << '\n';
                if (!resume) {
                    recoveryFixtureFrame_ = frame;
                    recoveryFixtureAccepted_ = issueFixture(UnitCommand::holdPosition(actor), "hold-walking-builder");
                }
            }
            Unit building = nullptr;
            if (recoveryBuildFrame_ >= 0) {
                for (const auto unit : Broodwar->self()->getUnits()) {
                    if (unit != nullptr && unit->exists() && unit->getType() == recoveryType_ &&
                        unit->getTilePosition() == TilePosition(recoveryPosition_)) { building = unit; break; }
                }
            }
            if (resume && building != nullptr && targetId_ < 0) {
                targetId_ = building->getID(); recoveryStartedFrame_ = frame;
                log_ << "STRUCTURE_START," << frame << ',' << targetId_ << ",type=" << recoveryType_.getID() << '\n';
            }
            if (resume && building != nullptr && recoveryFixtureFrame_ < 0 &&
                !building->isCompleted() && actor->isConstructing() && actor->getBuildUnit() == building) {
                recoveryFixtureFrame_ = frame;
                recoveryFixtureAccepted_ = issueFixture(UnitCommand::stop(actor), "interrupt-started-builder");
            }
            const auto current = actor->getLastCommand();
            const auto commandFrame = actor->getLastCommandFrame();
            if (recoveryFixtureAccepted_ && frame > recoveryFixtureFrame_) {
                if (resume && current.getType() == UnitCommandTypes::Stop &&
                    !actor->isConstructing() && building != nullptr && !building->isCompleted()) {
                    recoveryInterrupted_ = true;
                    recoveryPausedRemaining_ = building->getRemainingBuildTime();
                }
                if (!resume && current.getType() == UnitCommandTypes::Hold_Position &&
                    actor->getOrder() == Orders::HoldPosition) recoveryHeld_ = true;
                const bool resumed = resume && building != nullptr && !building->isCompleted() &&
                    current.getType() == UnitCommandTypes::Right_Click_Unit && current.getTarget() == building &&
                    frame > commandFrame && actor->isConstructing() && actor->getBuildUnit() == building;
                const bool stopped = !resume && frame > commandFrame &&
                    commandFrame - recoveryBuildFrame_ > 480 && current.getType() == UnitCommandTypes::Stop &&
                    actor->isIdle() && !actor->isConstructing();
                if ((resumed || stopped) && !nativeTargetObserved_) {
                    nativeTargetObserved_ = true; recoveryCommandFrame_ = commandFrame;
                }
                if (resume && nativeTargetObserved_ && building != nullptr &&
                    recoveryPausedRemaining_ >= 0 && building->getRemainingBuildTime() < recoveryPausedRemaining_)
                    recoveryProgress_ = true;
                if (!resume && recoveryCommandFrame_ < 0 && building != nullptr) recoveryNoEarlyStructure_ = false;
                if (frame % 8 == 0 || resumed || stopped) {
                    log_ << "RECOVERY_NATIVE," << frame << ',' << actorId_ << ',' << targetId_
                         << ",order=" << actor->getOrder().getID() << ",last_command_frame=" << commandFrame
                         << ",last_command=" << current.getType().getID()
                         << ",last_target=" << (current.getTarget() ? current.getTarget()->getID() : -1)
                         << ",constructing=" << actor->isConstructing()
                         << ",build_unit=" << (actor->getBuildUnit() ? actor->getBuildUnit()->getID() : -1)
                         << ",idle=" << actor->isIdle() << ",remaining="
                         << (building ? building->getRemainingBuildTime() : -1)
                         << ",interrupted=" << recoveryInterrupted_ << ",held=" << recoveryHeld_
                         << ",resumed=" << resumed << ",stopped=" << stopped << '\n';
                    log_.flush();
                }
            }
        }
        const bool bound = acceptedRecoveryRecord(recoveryCommandFrame_, actorId_,
            resume ? "construction-resume" : "construction-timeout-release",
            resume ? "Right_Click_Unit" : "Stop", resume ? targetId_ : -1);
        if (bound && nativeTargetObserved_ && (!resume || recoveryCompletedFrame_ >= 0)) {
            finishRecovery(frame); return;
        }
        if (frame >= 1440) finishRecovery(frame);
    }

    void finish(const int frame) {
        check("racebot-accepted-command-record", acceptedRecordObserved_);
        check("racebot-engine-order-target-observed", nativeTargetObserved_);
        check("engine-postcondition", failures_ == 0);
        log_ << "DONE," << frame << ',' << failures_ << '\n';
        log_.flush();
        finished_ = true;
        BWAPI::Broodwar->leaveGame();
    }

public:
    void onStart() override {
        std::ifstream("bwapi-data/read/scenario.txt") >> scenario_;
        log_.open("bwapi-data/write/scenario.csv");
        log_ << "START," << scenario_ << ',' << BWAPI::Broodwar->mapFileName() << '\n';
        log_.flush();
        BWAPI::Broodwar->setLocalSpeed(0);
        BWAPI::Broodwar->setFrameSkip(1);
        const int posture = scenario_ == "racebot-combat" ? 2 : 1;
        bot.setActionChoiceHook([posture](const RaceBotModule::Observation&) {
            return RaceBotModule::ActionChoice{0, posture};
        });
        bot.onStart();
        log_ << "READY," << BWAPI::Broodwar->isPaused() << ','
             << BWAPI::Broodwar->isInGame() << '\n';
        log_.flush();
    }

    void onEnd(const bool won) override {
        bot.onEnd(won);
        log_ << "END," << BWAPI::Broodwar->getFrameCount() << ',' << won << '\n';
        log_.flush();
    }

    void onUnitComplete(BWAPI::Unit unit) override {
        if (unit != nullptr && unit->exists() && unit->getID() == targetId_ && constructionCase()) {
            recoveryCompletedFrame_ = BWAPI::Broodwar->getFrameCount();
            log_ << "LIFECYCLE," << recoveryCompletedFrame_ << ",complete," << targetId_
                 << ",type=" << unit->getType().getID() << '\n';
            log_.flush();
        }
    }

    void onFrame() override {
        if (finished_) return;
        const int frame = BWAPI::Broodwar->getFrameCount();
        if (frame < 24) return;
        if (constructionCase()) { constructionFrame(frame); return; }
        bot.onFrame();

        if (!fixtureChecked_) {
            fixtureChecked_ = true;
            std::vector<BWAPI::Unit> workers;
            std::vector<BWAPI::Unit> marines;
            for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
                if (unit == nullptr || !unit->exists()) continue;
                if (unit->getType() == BWAPI::UnitTypes::Terran_SCV) workers.push_back(unit);
                if (unit->getType() == BWAPI::UnitTypes::Terran_Marine) marines.push_back(unit);
            }
            BWAPI::Unit threat = nullptr;
            for (const auto unit : BWAPI::Broodwar->getAllUnits())
                if (unit != nullptr && unit->exists() && unit->isVisible() &&
                    unit->getPlayer() != BWAPI::Broodwar->self() &&
                    unit->getType() == BWAPI::UnitTypes::Zerg_Zergling)
                    threat = unit;

            const bool workerCase = scenario_ == "racebot-worker-defense";
            check("racebot-fixture-terran-player",
                BWAPI::Broodwar->self()->getRace() == BWAPI::Races::Terran);
            check("racebot-fixture-visible-zergling", threat != nullptr);
            check(workerCase ? "racebot-fixture-one-scv" : "racebot-fixture-four-marines",
                workerCase ? workers.size() == 1 : marines.size() == 4);
            const auto actor = workerCase
                ? (workers.empty() ? nullptr : workers.front())
                : (marines.empty() ? nullptr : marines.front());
            if (actor != nullptr && threat != nullptr) {
                actorId_ = actor->getID();
                targetId_ = threat->getID();
            }
        }

        if (actorId_ >= 0 && targetId_ >= 0) {
            acceptedRecordObserved_ = acceptedRecordObserved_ || acceptedRecordExists();
            const auto actor = BWAPI::Broodwar->getUnit(actorId_);
            const auto target = BWAPI::Broodwar->getUnit(targetId_);
            // COMMAND records are buffered until a later snapshot. Preserve the
            // engine effect while actor/target still exist, then bind it to the
            // matching accepted actor/target/issuer record once that log flushes.
            if (actor != nullptr && actor->exists() && target != nullptr && target->exists()) {
                const auto last = actor->getLastCommand();
                const auto orderTarget = actor->getOrderTarget();
                const auto explicitTarget = actor->getTarget();
                const bool nativeEffect = frame > actor->getLastCommandFrame() &&
                    last.getType() == BWAPI::UnitCommandTypes::Attack_Unit &&
                    last.getTarget() == target && actor->getOrder() == BWAPI::Orders::AttackUnit &&
                    (orderTarget == target || explicitTarget == target);
                if (frame <= 96 || (nativeEffect && !nativeTargetObserved_)) {
                    log_ << "NATIVE," << frame << ',' << actorId_ << ',' << targetId_
                         << ",order=" << actor->getOrder().getID()
                         << ",order_target=" << (orderTarget ? orderTarget->getID() : -1)
                         << ",target=" << (explicitTarget ? explicitTarget->getID() : -1)
                         << ",last_command_frame=" << actor->getLastCommandFrame()
                         << ",last_command=" << last.getType().getID()
                         << ",last_target=" << (last.getTarget() ? last.getTarget()->getID() : -1)
                         << ",native_effect=" << nativeEffect << '\n';
                    log_.flush();
                }
                if (nativeEffect && !nativeTargetObserved_)
                    observedCommandFrame_ = actor->getLastCommandFrame();
                nativeTargetObserved_ = nativeTargetObserved_ || nativeEffect;
            }
            if (acceptedRecordObserved_ && nativeTargetObserved_) {
                finish(frame);
                return;
            }
        }

        if (frame >= 240) finish(frame);
    }
};

extern "C" __declspec(dllexport) void gameInit(BWAPI::Game* game) {
    BWAPI::BroodwarPtr = game;
}

extern "C" __declspec(dllexport) BWAPI::AIModule* newAIModule() {
    return new RaceBotAuditScenario;
}

BOOL APIENTRY DllMain(HANDLE, DWORD, LPVOID) { return TRUE; }
