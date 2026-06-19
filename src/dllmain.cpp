// dwmapi.dll proxy for UE4SS: forwards the DWM exports and loads UE4SS,
// with an optional in-DLL updater (UPDATER_ENABLED).

#include <cstdint>
#include <fstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <filesystem>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

#if defined(UPDATER_ENABLED)
    #include "updater.hpp"
#endif

namespace fs = std::filesystem;

HMODULE SOriginalDll = nullptr;

// mProcs[] + setup_functions(), generated from tools/dwmapi.exports.
#include "proxy_generated.inc"

void load_original_dll()
{
    wchar_t path[MAX_PATH];
    GetSystemDirectory(path, MAX_PATH);

    std::wstring dll_path = std::wstring(path) + L"\\dwmapi.dll";

    SOriginalDll = LoadLibrary(dll_path.c_str());
    if (!SOriginalDll)
    {
        MessageBox(nullptr, L"Failed to load proxy DLL", L"UE4SS Error", MB_OK | MB_ICONERROR);
        ExitProcess(0);
    }
}

bool is_absolute_path(const std::string& path)
{
    return fs::path(path).is_absolute();
}

bool should_disable_ue4ss()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
    {
        return false;
    }

    bool disable = false;
    for (int i = 0; i < argc; i++)
    {
        if (wcscmp(argv[i], L"--disable-ue4ss") == 0)
        {
            disable = true;
            break;
        }
    }

    LocalFree(argv);
    return disable;
}

std::wstring get_ue4ss_path_from_args()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
    {
        return L"";
    }

    std::wstring ue4ss_path;
    for (int i = 0; i < argc - 1; i++)
    {
        if (wcscmp(argv[i], L"--ue4ss-path") == 0)
        {
            ue4ss_path = argv[i + 1];
            break;
        }
    }

    LocalFree(argv);
    return ue4ss_path;
}

HMODULE load_ue4ss_dll(HMODULE moduleHandle)
{
    HMODULE hModule = nullptr;
    wchar_t moduleFilenameBuffer[1024]{'\0'};
    GetModuleFileNameW(moduleHandle, moduleFilenameBuffer, sizeof(moduleFilenameBuffer) / sizeof(wchar_t));
    const auto currentPath = std::filesystem::path(moduleFilenameBuffer).parent_path();
    const fs::path ue4ssPath = currentPath / "ue4ss" / "UE4SS.dll";

    // Check for --ue4ss-path command line argument
    std::wstring cmdLineUe4ssPath = get_ue4ss_path_from_args();
    if (!cmdLineUe4ssPath.empty())
    {
        fs::path ue4ssArgPath = cmdLineUe4ssPath;
        if (!ue4ssArgPath.is_absolute())
        {
            ue4ssArgPath = currentPath / ue4ssArgPath;
        }

        // Attempt to load UE4SS.dll from the command line path
        hModule = LoadLibrary(ue4ssArgPath.c_str());
        if (hModule)
        {
            return hModule;
        }
    }

    // Check for override.txt
    const fs::path overrideFilePath = currentPath / "override.txt";
    if (fs::exists(overrideFilePath))
    {
        std::ifstream overrideFile(overrideFilePath);
        std::string overridePath;
        if (std::getline(overrideFile, overridePath))
        {
            fs::path ue4ssOverridePath = overridePath;
            if (!is_absolute_path(overridePath))
            {
                ue4ssOverridePath = currentPath / overridePath;
            }

            ue4ssOverridePath = ue4ssOverridePath / "UE4SS.dll";

            // Attempt to load UE4SS.dll from the override path
            hModule = LoadLibrary(ue4ssOverridePath.c_str());
            if (hModule)
            {
                return hModule;
            }
        }
    }

    // Attempt to load UE4SS.dll from ue4ss directory
    hModule = LoadLibrary(ue4ssPath.c_str());
    if (!hModule)
    {
        // If loading from ue4ss directory fails, load from the current directory
        hModule = LoadLibrary(L"UE4SS.dll");
    }

    return hModule;
}

HMODULE g_selfModule = nullptr;

// Loads UE4SS; shows an error and exits on failure.
static void load_ue4ss_or_die()
{
    HMODULE hUE4SSDll = load_ue4ss_dll(g_selfModule);
    if (!hUE4SSDll)
    {
        MessageBox(nullptr,
                   L"Failed to load UE4SS.dll. Please see the docs on correct installation: "
                   L"https://docs.ue4ss.com/installation-guide",
                   L"UE4SS Error", MB_OK | MB_ICONERROR);
        ExitProcess(0);
    }
}

#if defined(UPDATER_ENABLED)
// Runs the updater, then loads UE4SS. Off the loader lock (WinHTTP needs that).
static DWORD WINAPI updater_thread(LPVOID)
{
    wchar_t selfPath[1024]{'\0'};
    GetModuleFileNameW(g_selfModule, selfPath, sizeof(selfPath) / sizeof(wchar_t));
    const fs::path baseDir = fs::path(selfPath).parent_path();

    updater::run_blocking(baseDir);
    load_ue4ss_or_die();
    return 0;
}
#endif

BOOL WINAPI DllMain(HMODULE hInstDll, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        g_selfModule = hInstDll;

        load_original_dll();
        setup_functions();

        // --disable-ue4ss: forward only.
        if (should_disable_ue4ss())
        {
            return TRUE;
        }

#if defined(UPDATER_ENABLED)
        // Update + load UE4SS on a worker thread (runs once the loader lock clears).
        HANDLE th = CreateThread(nullptr, 0, updater_thread, nullptr, 0, nullptr);
        if (th)
        {
            CloseHandle(th);
        }
        else
        {
            load_ue4ss_or_die();  // fall back to loading without an update
        }
#else
        load_ue4ss_or_die();
#endif
    }
    else if (fdwReason == DLL_PROCESS_DETACH)
    {
        FreeLibrary(SOriginalDll);
    }
    return TRUE;
}
