// HomefrontModLoader: everything that has to happen inside the dedicated server before mods can run.
//
// Built as msimg32.dll and placed next to the game exe. The exe imports AlphaBlend from msimg32.dll, and msimg32
// is not one of Windows' KnownDLLs, so this copy in the exe's folder is the one that loads, before the exe's own
// code runs. Its five exports jump straight to the real system msimg32.dll. Nothing here depends on Steam or a Steam
// emulator.
//   1. While loading: checks the exe is the known build and turns VAC off in memory (vac.cpp). If it can't, it stops
//      the process rather than let the server register as secure.
//   2. A watcher thread waits for the engine object (GEngine) and points GCGameEngine's vtable slot for
//      UEngine::Tick(FLOAT) at EngineTick.
//   3. The first EngineTick starts the .NET runtime through hostfxr (found from the 32-bit .NET install; no nethost
//      library needed), loads HomefrontSDK\HomefrontSDK.dll and hands off to HomefrontSDK.Internal.Loader.Initialize
//      with the native API (hfsdk_native.h). Every later tick checks VAC (vac.cpp) and calls the SDK's tick, on the
//      game thread.
// For an exe it doesn't know it only forwards msimg32 and logs why.
//
// Log: HomefrontSDK\HomefrontSDK.log next to the exe (shared with the SDK).
#include <windows.h>
#include <share.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "loader.h"

namespace loader {
namespace {

// ---- msimg32 forwarding
const char* const kForwarded[] = {"vSetDdrawflag", "AlphaBlend", "DllInitialize", "GradientFill", "TransparentBlt"};

// ---- supported builds
struct Build {
    const char* exe;
    uintptr_t gengine;    // UEngine* GEngine
    uint32_t tick_slot;   // vtable byte offset of UEngine::Tick(FLOAT DeltaSeconds)
    uintptr_t tick_impl;  // what the engine's vtable holds there in this build: the build check
    uintptr_t gobjects, gnames, gworld;
};
// Found by reversing (re/README.md): the main loop (0x00B2CA7A) calls GEngine->vtable[0xFC](DeltaSeconds) right
// after the frame-time helper; for GCGameEngine that is 0x00B8D370 (ret 4).
const Build kBuilds[] = {
    {"HFDedicated.exe", 0x018D10A4, 0xFC, 0x00B8D370, 0x018BBD6C, 0x018BBD90, 0x018D5078},  // server, 2011-08-10
};

const Build* g_build;
std::wstring g_dir;      // the exe's folder, trailing backslash
std::wstring g_sdk_dir;  // g_dir + HomefrontSDK\ (trailing backslash)
FILE* g_log;
HFNativeApi g_native;
HFManagedApi g_managed;
bool g_started;

// ---- .NET hosting (the hostfxr API, declared here so no SDK headers or libraries are needed)
using hostfxr_handle = void*;
struct hostfxr_initialize_parameters {
    size_t size;
    const wchar_t* host_path;
    const wchar_t* dotnet_root;
};
using InitForConfigFn = int32_t(__cdecl*)(const wchar_t* config, const hostfxr_initialize_parameters*, hostfxr_handle*);
using GetDelegateFn = int32_t(__cdecl*)(hostfxr_handle, int32_t type, void** delegate);
using ErrorWriterFn = void(__cdecl*)(const wchar_t* message);
using SetErrorWriterFn = ErrorWriterFn(__cdecl*)(ErrorWriterFn);
using LoadAssemblyFn = int32_t(__stdcall*)(const wchar_t* path, void* load_context, void* reserved);
using GetFunctionPointerFn = int32_t(__stdcall*)(const wchar_t* type, const wchar_t* method, const wchar_t* delegate_type,
                                                 void* load_context, void* reserved, void** fn);
constexpr int32_t kDelegateGetFunctionPointer = 6, kDelegateLoadAssembly = 7;  // hostfxr_delegate_type
const wchar_t* const kUnmanagedCallersOnly = reinterpret_cast<const wchar_t*>(-1);

std::string Narrow(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

void __cdecl HostfxrError(const wchar_t* message) { Log("hostfxr: %s", Narrow(message).c_str()); }

// The 32-bit .NET install: DOTNET_ROOT(x86), the installer's registry entry, then the default folder.
std::wstring DotnetRoot()
{
    wchar_t buf[MAX_PATH];
    if (GetEnvironmentVariableW(L"DOTNET_ROOT(x86)", buf, MAX_PATH))
        return buf;
    DWORD size = sizeof(buf);
    // A 32-bit process reads the 32-bit registry view, where the x86 installer records itself.
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\dotnet\\Setup\\InstalledVersions\\x86", L"InstallLocation",
                     RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", buf, MAX_PATH))
        return std::wstring(buf) + L"\\dotnet";
    return L"C:\\Program Files (x86)\\dotnet";
}

// host\fxr\<highest version>\hostfxr.dll under the install.
std::wstring FindHostfxr(const std::wstring& root)
{
    std::wstring base = root;
    if (!base.empty() && base.back() != L'\\')
        base += L'\\';
    base += L"host\\fxr\\";
    std::wstring best;
    std::vector<int> best_ver;
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW((base + L"*").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE)
        return L"";
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.')
            continue;
        std::vector<int> ver;
        for (const wchar_t* p = found.cFileName; *p;) {
            ver.push_back(_wtoi(p));
            while (*p && *p != L'.')
                ++p;
            if (*p)
                ++p;
        }
        if (ver > best_ver) {
            best_ver = ver;
            best = found.cFileName;
        }
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return best.empty() ? L"" : base + best + L"\\hostfxr.dll";
}

// Starts .NET and hands off to the SDK.
bool StartSdk()
{
    std::wstring dll = g_sdk_dir + L"HomefrontSDK.dll";
    std::wstring config = g_sdk_dir + L"HomefrontSDK.runtimeconfig.json";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("no %s; nothing to run", Narrow(dll).c_str());
        return false;
    }
    std::wstring root = DotnetRoot();
    std::wstring fxr_path = FindHostfxr(root);
    HMODULE fxr = fxr_path.empty() ? nullptr : LoadLibraryW(fxr_path.c_str());
    if (!fxr) {
        Log("no 32-bit .NET found under %s (install the x86 .NET 10 runtime)", Narrow(root).c_str());
        return false;
    }
    auto init = reinterpret_cast<InitForConfigFn>(GetProcAddress(fxr, "hostfxr_initialize_for_runtime_config"));
    auto get_delegate = reinterpret_cast<GetDelegateFn>(GetProcAddress(fxr, "hostfxr_get_runtime_delegate"));
    auto set_writer = reinterpret_cast<SetErrorWriterFn>(GetProcAddress(fxr, "hostfxr_set_error_writer"));
    if (!init || !get_delegate) {
        Log("%s lacks the hosting API", Narrow(fxr_path).c_str());
        return false;
    }
    if (set_writer)
        set_writer(HostfxrError);
    Log("hostfxr: %s", Narrow(fxr_path).c_str());

    hostfxr_initialize_parameters params{sizeof(params), nullptr, root.c_str()};
    hostfxr_handle ctx = nullptr;
    int32_t rc = init(config.c_str(), &params, &ctx);
    if (rc < 0 || !ctx) {  // 1 and 2 are success codes too (runtime already loaded / different properties)
        Log("hostfxr_initialize_for_runtime_config failed: 0x%08X", rc);
        return false;
    }
    LoadAssemblyFn load_assembly = nullptr;
    GetFunctionPointerFn get_function = nullptr;
    if (get_delegate(ctx, kDelegateLoadAssembly, reinterpret_cast<void**>(&load_assembly)) < 0 ||
        get_delegate(ctx, kDelegateGetFunctionPointer, reinterpret_cast<void**>(&get_function)) < 0) {
        Log("the runtime doesn't offer load_assembly/get_function_pointer (needs .NET 8 or later)");
        return false;
    }
    rc = load_assembly(dll.c_str(), nullptr, nullptr);
    if (rc < 0) {
        Log("loading %s failed: 0x%08X", Narrow(dll).c_str(), rc);
        return false;
    }
    HFManagedInitFn initialize = nullptr;
    rc = get_function(L"HomefrontSDK.Internal.Loader, HomefrontSDK", L"Initialize", kUnmanagedCallersOnly, nullptr,
                      nullptr, reinterpret_cast<void**>(&initialize));
    if (rc < 0 || !initialize) {
        Log("HomefrontSDK.Internal.Loader.Initialize not found: 0x%08X", rc);
        return false;
    }

    g_native.version = HFSDK_NATIVE_API_VERSION;
    g_native.size = sizeof(g_native);
    g_native.gobjects = reinterpret_cast<void*>(g_build->gobjects);
    g_native.gnames = reinterpret_cast<void*>(g_build->gnames);
    g_native.gworld = reinterpret_cast<void*>(g_build->gworld);
    g_native.gengine = reinterpret_cast<void*>(g_build->gengine);
    g_native.binaries_dir = g_dir.c_str();
    g_native.sdk_dir = g_sdk_dir.c_str();
    g_native.log = [](const char* line) { Log("%s", line ? line : ""); };
    FillNativeApi(g_native);

    HFManagedApi managed{};
    managed.size = sizeof(managed);
    if (!initialize(&g_native, &managed)) {
        Log("the SDK declined to start (see above)");
        return false;
    }
    g_managed = managed;
    return true;
}

// ---- the engine tick hook
using TickFn = void(__fastcall*)(void* engine, void* edx, float delta);  // thiscall, one float
TickFn g_real_tick;

void __fastcall EngineTick(void* engine, void* edx, float delta)
{
    g_real_tick(engine, edx, delta);
    CheckVac(GetTickCount());
    if (!g_started) {
        g_started = true;
        Log(StartSdk() ? "SDK started" : "SDK not started; the game runs without mods");
    }
    if (g_managed.tick)
        g_managed.tick(delta);
}

// Waits for GEngine, checks the build, and swaps the vtable slot. Runs on its own thread from DllMain; the slot is
// one aligned pointer, so the game thread sees either the old or the new function.
DWORD WINAPI Watch(void*)
{
    const Build& b = *g_build;
    for (int waited = 0; waited < 10 * 60 * 1000; waited += 50) {
        void* engine = *reinterpret_cast<void* volatile*>(b.gengine);
        if (!engine) {
            Sleep(50);
            continue;
        }
        void** slot = *static_cast<void***>(engine) + b.tick_slot / 4;
        if (reinterpret_cast<uintptr_t>(*slot) != b.tick_impl) {
            Log("GEngine's tick is %p, not %p: a different build; no mods", *slot, reinterpret_cast<void*>(b.tick_impl));
            return 0;
        }
        g_real_tick = reinterpret_cast<TickFn>(*slot);
        DWORD old;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            Log("can't unprotect the engine vtable (error %lu); no mods", GetLastError());
            return 0;
        }
        *slot = reinterpret_cast<void*>(&EngineTick);
        VirtualProtect(slot, sizeof(void*), old, &old);
        Log("hooked UEngine::Tick (engine %p, vtable slot %p); the SDK starts on the next tick", engine,
            static_cast<void*>(slot));
        return 0;
    }
    Log("GEngine never appeared; no mods");
    return 0;
}

}  // namespace

FARPROC g_real[5];

void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

const HFManagedApi& Managed() { return g_managed; }

void Start(HMODULE self)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    while (n > 0 && path[n - 1] != L'\\')
        --n;
    g_dir.assign(path, n);
    g_sdk_dir = g_dir + L"HomefrontSDK\\";
    CreateDirectoryW(g_sdk_dir.c_str(), nullptr);
    g_log = _wfsopen((g_sdk_dir + L"HomefrontSDK.log").c_str(), L"a", _SH_DENYNO);

    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);  // redirected to SysWOW64 for this 32-bit process
    HMODULE real = LoadLibraryW((std::wstring(sys) + L"\\msimg32.dll").c_str());
    for (int i = 0; i < 5; ++i)
        g_real[i] = real ? GetProcAddress(real, kForwarded[i]) : nullptr;
    Log("---- HomefrontModLoader in pid %lu; system msimg32 %s", GetCurrentProcessId(), real ? "loaded" : "MISSING");

    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    const char* exe = strrchr(exe_path, '\\');
    exe = exe ? exe + 1 : exe_path;
    for (const Build& b : kBuilds)
        if (_stricmp(exe, b.exe) == 0)
            g_build = &b;
    if (!g_build) {
        Log("%s is not a supported game exe; forwarding msimg32 only", exe);
        return;
    }
    // Before anything of the server's runs: a server that can't be kept off VAC doesn't run at all.
    if (!DisableVac()) {
        Log("stopping the server: VAC could not be turned off");
        TerminateProcess(GetCurrentProcess(), 0xDEAD0AC);
    }
    if (HANDLE t = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr))
        CloseHandle(t);
}

}  // namespace loader

// msimg32's exports, in its ordinal order, jumping to the system DLL (naked: the arguments stay untouched).
extern "C" __declspec(naked) void Forward_vSetDdrawflag() { __asm { jmp dword ptr [loader::g_real + 0] } }
extern "C" __declspec(naked) void Forward_AlphaBlend() { __asm { jmp dword ptr [loader::g_real + 4] } }
extern "C" __declspec(naked) void Forward_DllInitialize() { __asm { jmp dword ptr [loader::g_real + 8] } }
extern "C" __declspec(naked) void Forward_GradientFill() { __asm { jmp dword ptr [loader::g_real + 12] } }
extern "C" __declspec(naked) void Forward_TransparentBlt() { __asm { jmp dword ptr [loader::g_real + 16] } }
#pragma comment(linker, "/export:vSetDdrawflag=_Forward_vSetDdrawflag,@1")
#pragma comment(linker, "/export:AlphaBlend=_Forward_AlphaBlend,@2")
#pragma comment(linker, "/export:DllInitialize=_Forward_DllInitialize,PRIVATE")
#pragma comment(linker, "/export:GradientFill=_Forward_GradientFill,@4")
#pragma comment(linker, "/export:TransparentBlt=_Forward_TransparentBlt,@5")

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        loader::Start(inst);
    }
    return TRUE;
}
