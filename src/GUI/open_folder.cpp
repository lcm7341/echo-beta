#include "GUI/gui.hpp"

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>

#include <string>
#include <thread>

#define WINEBROWSER_WAIT_MILLISECONDS 5000

static bool running_on_wine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

static bool open_with_linux(const fs::path& folder) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"winebrowser.exe \"" + folder.wstring() + L"\"";
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    WaitForSingleObject(process.hProcess, WINEBROWSER_WAIT_MILLISECONDS);
    DWORD code = 0;
    bool opened = GetExitCodeProcess(process.hProcess, &code) && (code == 0 || code == STILL_ACTIVE);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return opened;
}
#endif

void GUI::open_folder(const fs::path& folder) {
#ifdef GEODE_IS_WINDOWS
    if (running_on_wine()) {
        std::thread([folder] {
            if (open_with_linux(folder)) return;
            geode::queueInMainThread([folder] { geode::utils::file::openFolder(folder); });
        }).detach();
        return;
    }
#endif
    geode::utils::file::openFolder(folder);
}
