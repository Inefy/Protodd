// Exercise the exact playing DLL in a preplaced 200-supply battle. This wrapper
// adds no commands; it records the complete callback and ends the local fixture.
#include <BWAPI.h>
#include <windows.h>
#include <psapi.h>
#include <fstream>
#include <chrono>
class AuditLoad final : public BWAPI::AIModule {
    BWAPI::AIModule* bot{};
    std::ofstream timing,events;
    void recordMemory(const int frame) {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb=sizeof(memory);
        if(GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))) {
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
        auto library=LoadLibraryA("bwapi-data/AI/Protodd.dll");
        auto init=reinterpret_cast<void(*)(BWAPI::Game*)>(GetProcAddress(library,"gameInit"));
        auto create=reinterpret_cast<BWAPI::AIModule*(*)()>(GetProcAddress(library,"newAIModule"));
        if(!init||!create) { events << "LOAD_FAILED\n";events.flush();ExitProcess(2); }
        init(BWAPI::BroodwarPtr);bot=create();bot->onStart();
        BWAPI::Broodwar->setLocalSpeed(0);BWAPI::Broodwar->setFrameSkip(256);
        timing.open("bwapi-data/write/full-callback-us.bin",std::ios::binary);
        events << "START," << BWAPI::Broodwar->self()->getUnits().size() << ',' << BWAPI::Broodwar->enemy()->getUnits().size() << ',' << BWAPI::Broodwar->self()->supplyUsed() << '\n';
        recordMemory(-1);events.flush();
    }
    void onFrame() override {
        auto begin=std::chrono::steady_clock::now();bot->onFrame();
        auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
        timing.write(reinterpret_cast<const char*>(&us),sizeof(us));
        if(BWAPI::Broodwar->getFrameCount()%120==0){events << "FRAME," << BWAPI::Broodwar->getFrameCount() << ',' << BWAPI::Broodwar->self()->getUnits().size() << ',' << BWAPI::Broodwar->enemy()->getUnits().size() << '\n';recordMemory(BWAPI::Broodwar->getFrameCount());events.flush();}
        if(BWAPI::Broodwar->getFrameCount()>=1200) BWAPI::Broodwar->leaveGame();
    }
    void onEnd(bool won) override {bot->onEnd(won);timing.flush();events << "DONE," << BWAPI::Broodwar->getFrameCount() << '\n';events.flush();ExitProcess(0);}
    void onUnitDiscover(BWAPI::Unit u)override{if(bot)bot->onUnitDiscover(u);}
    void onUnitEvade(BWAPI::Unit u)override{if(bot)bot->onUnitEvade(u);}
    void onUnitShow(BWAPI::Unit u)override{if(bot)bot->onUnitShow(u);}
    void onUnitHide(BWAPI::Unit u)override{if(bot)bot->onUnitHide(u);}
    void onUnitCreate(BWAPI::Unit u)override{if(bot)bot->onUnitCreate(u);}
    void onUnitDestroy(BWAPI::Unit u)override{if(bot)bot->onUnitDestroy(u);}
    void onUnitMorph(BWAPI::Unit u)override{if(bot)bot->onUnitMorph(u);}
    void onUnitComplete(BWAPI::Unit u)override{if(bot)bot->onUnitComplete(u);}
    void onUnitRenegade(BWAPI::Unit u)override{if(bot)bot->onUnitRenegade(u);}
};
extern "C" __declspec(dllexport)void gameInit(BWAPI::Game* g){BWAPI::BroodwarPtr=g;}
extern "C" __declspec(dllexport)BWAPI::AIModule* newAIModule(){return new AuditLoad;}
BOOL APIENTRY DllMain(HMODULE,DWORD,LPVOID){return TRUE;}
