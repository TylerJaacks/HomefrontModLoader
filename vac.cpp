// VAC off for the dedicated server, without a patched exe on disk.
//
// HFDedicated.exe (2011-08-10) computes the eServerMode argument of SteamGameServer_Init at 0x00B1568B:
//
//     and eax, 0x400000      ; a game-settings flag bit
//     or  eax, 0x800000
//     shr eax, 0x16          ; -> 3 (eServerModeAuthenticationAndSecure = VAC) or 2
//     push eax
//
// DisableVac replaces those 13 bytes in memory with `mov eax, 2` (eServerModeAuthentication: Steam
// authentication, no VAC) and NOPs, while the loader is being loaded - before the exe's own code runs. It refuses
// (and the loader stops the process) when the bytes are neither the stock ones nor already patched: a different
// build could register as secure.
//
// CheckVac then asks Steam itself: once the game server has logged on, SteamGameServer_BSecure() must be false. If
// Steam ever says the server is secure, the server is stopped at once.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "loader.h"

namespace loader {
namespace {

constexpr uintptr_t kServerModeCode = 0x00B1568B;
const uint8_t kStock[13] = {0x25, 0x00, 0x00, 0x40, 0x00, 0x0D, 0x00, 0x00, 0x80, 0x00, 0xC1, 0xE8, 0x16};
const uint8_t kPatched[13] = {0xB8, 0x02, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

using BSecureFn = bool(__cdecl*)();
using GetSteamIdFn = uint64_t(__cdecl*)();

unsigned long g_next_check;
bool g_logged_on, g_waiting_logged;

// Steam calls before the game server exists return false/0; guarded anyway, as they run inside the game's tick.
bool TrySecure(BSecureFn fn, bool& secure)
{
    __try {
        secure = fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TrySteamId(GetSteamIdFn fn, uint64_t& id)
{
    __try {
        id = fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool DisableVac()
{
    auto code = reinterpret_cast<uint8_t*>(kServerModeCode);
    if (memcmp(code, kPatched, sizeof kPatched) == 0) {
        Log("VAC: the exe is already patched on disk (eServerMode 2)");
        return true;
    }
    if (memcmp(code, kStock, sizeof kStock) != 0) {
        Log("VAC: unexpected code at 0x%08X (%02X %02X %02X ...); not the known exe, so VAC can't be turned off",
            static_cast<unsigned>(kServerModeCode), code[0], code[1], code[2]);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(code, sizeof kPatched, PAGE_EXECUTE_READWRITE, &old)) {
        Log("VAC: can't unprotect the exe's code (error %lu)", GetLastError());
        return false;
    }
    memcpy(code, kPatched, sizeof kPatched);
    VirtualProtect(code, sizeof kPatched, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof kPatched);
    Log("VAC: off (SteamGameServer_Init gets eServerMode 2, authentication without VAC)");
    return true;
}

void CheckVac(unsigned long now_ms)
{
    if (now_ms < g_next_check)
        return;
    g_next_check = now_ms + 2000;
    HMODULE steam = GetModuleHandleA("steam_api.dll");
    if (!steam)
        return;
    auto secure = reinterpret_cast<BSecureFn>(GetProcAddress(steam, "SteamGameServer_BSecure"));
    auto get_id = reinterpret_cast<GetSteamIdFn>(GetProcAddress(steam, "SteamGameServer_GetSteamID"));
    bool is_secure = false;
    if (secure && TrySecure(secure, is_secure) && is_secure) {
        Log("VAC: Steam reports this server as VAC secure; stopping it");
        TerminateProcess(GetCurrentProcess(), 0xDEAD0AC);
    }
    // A CSteamID is universe (8 bits), account type (4), instance (20), account number (32). Before Steam answers,
    // the server reports an anonymous game-server ID with account number 0; the number comes with the logon.
    if (!g_logged_on && get_id) {
        uint64_t id = 0;
        if (TrySteamId(get_id, id) && (id & 0xFFFFFFFFull) != 0) {
            g_logged_on = true;
            Log("VAC: game server logged on to Steam as %llu (account type %u), not secure",
                static_cast<unsigned long long>(id), static_cast<unsigned>((id >> 52) & 0xF));
        } else if (!g_waiting_logged && id != 0) {
            g_waiting_logged = true;
            Log("VAC: game server waiting for Steam to log it on (id %llu so far)", static_cast<unsigned long long>(id));
        }
    }
}

}  // namespace loader
