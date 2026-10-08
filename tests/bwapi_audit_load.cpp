// Exercise the exact playing DLL in a dense 200-supply stock-engine battle.
// This wrapper adds no commands; it records callback latency and memory use.
#include <BWAPI.h>
#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>

class AuditLoad final : public BWAPI::AIModule {
    BWAPI::AIModule* bot{};
    std::ofstream timing;
    std::ofstream events;
    std::ofstream callbackTiming;
    std::uint64_t callbackRows{};
    int stopFrame{1200};

    int currentFrame() const {
        return BWAPI::Broodwar->getFrameCount();
    }

    template <typename Callback>
    std::int64_t measureBotCallback(const char* name, const int unitId,
                                    Callback callback) {
        const auto begin = std::chrono::steady_clock::now();
        bool threw = false;
        try {
            callback();
        } catch (...) {
            threw = true;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin).count();
        callbackTiming << name << ',' << currentFrame() << ',' << unitId << ','
                       << elapsed << ',' << (threw ? 1 : 0) << '\n';
        if (++callbackRows % 256 == 0) callbackTiming.flush();
        if (threw) {
            events << "CALLBACK_THROWN," << name << ',' << currentFrame() << ','
                   << unitId << '\n';
            events.flush();
        }
        return elapsed;
    }

    template <typename Callback>
    void forwardUnitCallback(const char* name, BWAPI::Unit unit,
                             Callback callback) {
        if (!bot) return;
        const int unitId = unit ? unit->getID() : -1;
        measureBotCallback(name, unitId, [callback, unit]() { callback(unit); });
    }

    void recordMemory(const int frame) {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (GetProcessMemoryInfo(
                GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                sizeof(memory))) {
            events << "MEMORY," << frame << ',' << memory.WorkingSetSize << ','
                   << memory.PeakWorkingSetSize << ',' << memory.PrivateUsage << ','
                   << memory.PagefileUsage << '\n';
        } else {
            events << "MEMORY_FAILED," << frame << ',' << GetLastError() << '\n';
        }
    }

public:
    void onStart() override {
        events.open("bwapi-data/write/load-scenario.csv");
        callbackTiming.open("bwapi-data/write/callback-timing.csv");
        callbackTiming << "callback,frame,unit_id,duration_us,threw\n";

        std::ifstream frameLimitFile("bwapi-data/read/AuditLoad-max-frame.txt");
        long long requestedStopFrame = 0;
        if (frameLimitFile >> requestedStopFrame && requestedStopFrame >= 0 &&
            requestedStopFrame <= 2147483646LL) {
            stopFrame = static_cast<int>(requestedStopFrame);
        }

        const auto library = LoadLibraryA("bwapi-data/AI/Protodd.dll");
        if (!library) {
            events << "LOAD_FAILED," << GetLastError() << '\n';
            events.flush();
            ExitProcess(2);
        }
        const auto init = reinterpret_cast<void (*)(BWAPI::Game*)>(
            GetProcAddress(library, "gameInit"));
        const auto create = reinterpret_cast<BWAPI::AIModule* (*)()>(
            GetProcAddress(library, "newAIModule"));
        if (!init || !create) {
            events << "LOAD_FAILED," << GetLastError() << '\n';
            events.flush();
            ExitProcess(2);
        }

        init(BWAPI::BroodwarPtr);
        bot = create();
        if (!bot) {
            events << "CREATE_FAILED\n";
            events.flush();
            ExitProcess(2);
        }
        measureBotCallback("onStart", -1, [this]() { bot->onStart(); });
        BWAPI::Broodwar->setLocalSpeed(0);
        BWAPI::Broodwar->setFrameSkip(256);
        timing.open("bwapi-data/write/full-callback-us.bin", std::ios::binary);
        events << "START," << BWAPI::Broodwar->self()->getUnits().size() << ','
               << BWAPI::Broodwar->enemy()->getUnits().size() << ','
               << BWAPI::Broodwar->self()->supplyUsed() << '\n';
        events << "FRAME_LIMIT," << stopFrame << '\n';
        recordMemory(-1);
        events.flush();
    }

    void onFrame() override {
        const auto elapsed = measureBotCallback(
            "onFrame", -1, [this]() { bot->onFrame(); });
        timing.write(reinterpret_cast<const char*>(&elapsed), sizeof(elapsed));
        const int frame = BWAPI::Broodwar->getFrameCount();
        if (frame % 120 == 0) {
            events << "FRAME," << frame << ','
                   << BWAPI::Broodwar->self()->getUnits().size() << ','
                   << BWAPI::Broodwar->enemy()->getUnits().size() << '\n';
            recordMemory(frame);
            events.flush();
        }
        if (stopFrame > 0 && frame >= stopFrame) BWAPI::Broodwar->leaveGame();
    }

    void onEnd(const bool won) override {
        if (bot) {
            measureBotCallback("onEnd", -1,
                               [this, won]() { bot->onEnd(won); });
        }
        timing.flush();
        callbackTiming.flush();
        events << "DONE," << currentFrame() << '\n';
        events.flush();
        // Return to BWAPI so StarCraft finishes replay/movie teardown. Exiting
        // inside this callback can re-enter SmackClose with an already freed
        // movie handle. The owning runner closes the process after onEnd.
    }

    void onSendText(std::string text) override {
        if (!bot) return;
        measureBotCallback("onSendText", -1,
                           [this, text]() { bot->onSendText(text); });
    }
    void onReceiveText(BWAPI::Player player, std::string text) override {
        if (!bot) return;
        measureBotCallback("onReceiveText", -1,
                           [this, player, text]() { bot->onReceiveText(player, text); });
    }
    void onPlayerLeft(BWAPI::Player player) override {
        if (!bot) return;
        measureBotCallback("onPlayerLeft", -1,
                           [this, player]() { bot->onPlayerLeft(player); });
    }
    void onNukeDetect(BWAPI::Position target) override {
        if (!bot) return;
        measureBotCallback("onNukeDetect", -1,
                           [this, target]() { bot->onNukeDetect(target); });
    }
    void onSaveGame(std::string gameName) override {
        if (!bot) return;
        measureBotCallback("onSaveGame", -1,
                           [this, gameName]() { bot->onSaveGame(gameName); });
    }
    void onUnitDiscover(BWAPI::Unit unit) override {
        forwardUnitCallback("onUnitDiscover", unit,
                            [this](BWAPI::Unit value) { bot->onUnitDiscover(value); });
    }
    void onUnitEvade(BWAPI::Unit unit) override {
        forwardUnitCallback("onUnitEvade", unit,
                            [this](BWAPI::Unit value) { bot->onUnitEvade(value); });
    }
    void onUnitShow(BWAPI::Unit unit) override {
        forwardUnitCallback("onUnitShow", unit,
                            [this](BWAPI::Unit value) { bot->onUnitShow(value); });
    }
    void onUnitHide(BWAPI::Unit unit) override {
        forwardUnitCallback("onUnitHide", unit,
                            [this](BWAPI::Unit value) { bot->onUnitHide(value); });
    }
    void onUnitCreate(BWAPI::Unit unit) override {
        forwardUnitCallback("onUnitCreate", unit,
                            [this](BWAPI::Unit value) { bot->onUnitCreate(value); });
    }
    void onUnitDestroy(BWAPI::Unit unit) override {
        forwardUnitCallback("onUnitDestroy", unit,
                            [this](BWAPI::Unit value) { bot->onUnitDestroy(value); });
    }
    void onUnitMorph(BWAPI::Unit unit) override {
        forwardUnitCallback("onUnitMorph", unit,
                            [this](BWAPI::Unit value) { bot->onUnitMorph(value); });
    }
    void onUnitComplete(BWAPI::Unit unit) override {
        forwardUnitCallback("onUnitComplete", unit,
                            [this](BWAPI::Unit value) { bot->onUnitComplete(value); });
    }
    void onUnitRenegade(BWAPI::Unit unit) override {
        forwardUnitCallback("onUnitRenegade", unit,
                            [this](BWAPI::Unit value) { bot->onUnitRenegade(value); });
    }
};

extern "C" __declspec(dllexport) void gameInit(BWAPI::Game* game) {
    BWAPI::BroodwarPtr = game;
}
extern "C" __declspec(dllexport) BWAPI::AIModule* newAIModule() {
    return new AuditLoad;
}
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
