#include "ProtoddModule.hpp"

#include <BWAPI.h>
#include <Windows.h>

namespace protodd::bwapi {
HINSTANCE moduleInstance = nullptr;
}

extern "C" __declspec(dllexport) void gameInit(BWAPI::Game* game) {
    BWAPI::BroodwarPtr = game;
}

extern "C" __declspec(dllexport) BWAPI::AIModule* newAIModule() {
    return new protodd::bwapi::ProtoddModule();
}

BOOL APIENTRY DllMain(HANDLE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        protodd::bwapi::moduleInstance = static_cast<HINSTANCE>(instance);
    return TRUE;
}
