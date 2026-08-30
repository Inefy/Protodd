#include "AstraModule.hpp"

#include <BWAPI.h>
#include <Windows.h>

extern "C" __declspec(dllexport) void gameInit(BWAPI::Game* game) {
    BWAPI::BroodwarPtr = game;
}

extern "C" __declspec(dllexport) BWAPI::AIModule* newAIModule() {
    return new astra::bwapi::AstraModule();
}

BOOL APIENTRY DllMain(HANDLE, DWORD, LPVOID) {
    return TRUE;
}
