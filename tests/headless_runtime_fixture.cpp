#include <windows.h>
#include <cstdint>
#include <fstream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    wchar_t desktopName[256]{};
    DWORD needed{};
    if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                                  desktopName, sizeof(desktopName), &needed)) return 3;
    auto window = CreateWindowExW(0, L"STATIC", L"Headless runtime fixture",
                                 WS_OVERLAPPEDWINDOW, 0, 0, 160, 100,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 4;
    ShowWindow(window, SW_SHOW);
    std::wofstream output(argv[1]);
    output << desktopName << L'\n';
    output.flush();
    output.close();
    if (std::wstring(argv[2]) == L"crash") {
        *reinterpret_cast<volatile int*>(static_cast<std::uintptr_t>(1)) = 7;
    }
    DestroyWindow(window);
    return 0;
}
