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
    int failures_{};
    int actorId_{-1}, targetId_{-1};

    void check(const char* name, const bool value) {
        log_ << "CHECK," << name << ',' << (value ? 1 : 0) << '\n';
        if (!value) ++failures_;
        log_.flush();
    }

    [[nodiscard]] bool acceptedRecordExists() const {
        std::ifstream input("bwapi-data/write/RaceBot.log");
        const auto expectedActor = "," + std::to_string(actorId_) + ",";
        const auto expectedTarget = "target=" + std::to_string(targetId_) + ",";
        const auto expectedIssuer = scenario_ == "racebot-worker-defense"
            ? ",worker-defense,Attack_Unit," : ",combat-defense,Attack_Unit,";
        std::string line;
        while (std::getline(input, line))
            if (line.starts_with("COMMAND,") && line.find(expectedActor) != std::string::npos &&
                line.find(expectedIssuer) != std::string::npos &&
                line.find(expectedTarget) != std::string::npos &&
                line.find("accepted=1,error=accepted") != std::string::npos)
                return true;
        return false;
    }

    void finish(const int frame) {
        check("racebot-accepted-command-record", acceptedRecordObserved_);
        const auto actor = BWAPI::Broodwar->getUnit(actorId_);
        const auto target = BWAPI::Broodwar->getUnit(targetId_);
        check("racebot-engine-order-target-observed", actor != nullptr && actor->exists() &&
            target != nullptr && target->exists() && actor->getOrderTarget() == target);
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
        ExitProcess(0);
    }

    void onFrame() override {
        if (finished_) return;
        const int frame = BWAPI::Broodwar->getFrameCount();
        if (frame < 24) return;
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
            if (acceptedRecordObserved_ && actor != nullptr && actor->exists() &&
                target != nullptr && target->exists() && actor->getOrderTarget() == target) {
                finish(frame);
                return;
            }
        }

        if (frame >= 240) finish(frame);
    }
};

extern "C" __declspec(dllexport) BWAPI::AIModule* newAIModule() {
    return new RaceBotAuditScenario;
}

BOOL APIENTRY DllMain(HANDLE, DWORD, LPVOID) { return TRUE; }
