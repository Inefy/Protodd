#include "ProtoddModule.hpp"

#include <BWAPI.h>
#include <Windows.h>

extern "C" __declspec(dllexport) void gameInit(BWAPI::Game* game) {
    BWAPI::BroodwarPtr = game;
}

extern "C" __declspec(dllexport) BWAPI::AIModule* newAIModule() {
    return new protodd::bwapi::ProtoddModule();
}

BOOL APIENTRY DllMain(HANDLE, DWORD, LPVOID) {
    return TRUE;
}
