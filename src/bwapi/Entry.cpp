#include "ProtoddModule.hpp"

#include <BWAPI.h>
#include <Windows.h>

#include <exception>

namespace protodd::bwapi {
HINSTANCE moduleInstance = nullptr;
}

extern "C" __declspec(dllexport) void gameInit(BWAPI::Game* game) {
    BWAPI::BroodwarPtr = game;
}

extern "C" __declspec(dllexport) BWAPI::AIModule* newAIModule() {
    try {
        return new protodd::bwapi::ProtoddModule();
    } catch (const std::exception&) {
        OutputDebugStringA("Protodd: module construction failed\n");
    } catch (...) {
        OutputDebugStringA("Protodd: unknown module construction failure\n");
    }
    return nullptr;
}

BOOL APIENTRY DllMain(HANDLE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        protodd::bwapi::moduleInstance = static_cast<HINSTANCE>(instance);
    return TRUE;
}
