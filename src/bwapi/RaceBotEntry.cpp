#include "RaceBotModule.hpp"

#include <BWAPI.h>
#include <Windows.h>

#ifndef PROTODD_BOT_RACE
#error Define PROTODD_BOT_RACE=1 for TerranTodd or =2 for ZergTodd
#endif
static_assert(PROTODD_BOT_RACE == 1 || PROTODD_BOT_RACE == 2,
              "PROTODD_BOT_RACE must be 1 (Terran) or 2 (Zerg)");

extern "C" __declspec(dllexport) void gameInit(BWAPI::Game* game) {
    BWAPI::BroodwarPtr = game;
}

extern "C" __declspec(dllexport) BWAPI::AIModule* newAIModule() {
    return new protodd::bwapi::RaceBotModule(PROTODD_BOT_RACE);
}

BOOL APIENTRY DllMain(HANDLE, DWORD, LPVOID) {
    return TRUE;
}
