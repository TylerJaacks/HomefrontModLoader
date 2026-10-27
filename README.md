# HomefrontModLoader

The one native file installed into the Homefront dedicated server: `msimg32.dll`, next to `HFDedicated.exe`. It does
everything that has to happen inside the server before mods can run, then hands off to
[HomefrontSDK](../HomefrontSDK/README.md).

A server install with mods is just:

```
Homefront Dedicated Server\Binaries\msimg32.dll                     this loader
Homefront Dedicated Server\Binaries\HomefrontSDK\HomefrontSDK.dll   the SDK (+ .runtimeconfig.json, .deps.json, .pdb)
Homefront Dedicated Server\Binaries\HomefrontSDK\Mods\<Mod>\        the mods
```

The server exe stays stock, and nothing else is patched or replaced. Needs the 32-bit (x86) .NET 10 runtime.

## What it does

1. **Gets in.** The exe imports `AlphaBlend` from `msimg32.dll`, and msimg32 is not one of Windows' KnownDLLs, so
   the copy beside the exe loads, before any of the exe's own code runs. Its five exports (`vSetDdrawflag` @1,
   `AlphaBlend` @2, `DllInitialize`, `GradientFill` @4, `TransparentBlt` @5) jump to the real system msimg32.
2. **Turns VAC off** (`vac.cpp`), in memory, while it is being loaded:
   - **The patch.** The server computes the `eServerMode` argument of `SteamGameServer_Init` at `0x00B1568B`
     (`and eax, 0x400000 / or eax, 0x800000 / shr eax, 0x16`: 3 = authentication and VAC, or 2). The loader
     replaces those 13 bytes with `mov eax, 2`, which keeps Steam authentication without VAC.
   - **Unknown exes stop.** If the bytes there aren't the stock exe's (or the old on-disk patch), the loader stops
     the process. A build it doesn't know could register as secure.
   - **Steam's own answer.** Every two seconds it asks Steam (`SteamGameServer_BSecure`). If Steam ever reports the
     server as secure, the loader stops it at once. It logs the game server's Steam ID once it has logged on.
3. **Hooks the engine.** A watcher thread waits for `GEngine` (`0x018D10A4`) and checks the build: `GCGameEngine`'s
   vtable slot `0xFC`, `UEngine::Tick`, must hold `0x00B8D370`. It then points that slot at its own tick, which runs
   the real one first. The main loop calls it every frame on the game thread.
4. **Hands off to the SDK.** On the first tick it finds the 32-bit .NET install: `DOTNET_ROOT(x86)`, then the
   installer's registry entry, then `Program Files (x86)\dotnet`. It loads `hostfxr` with
   `HomefrontSDK\HomefrontSDK.runtimeconfig.json`, loads `HomefrontSDK.dll`, and calls
   `HomefrontSDK.Internal.Loader.Initialize` with the native API (`hfsdk_native.h`). After that, every tick calls the
   SDK's tick.

The native API it hands over (`native_api.cpp`):
- `ProcessEvent` and calls to any native function (thiscall, stdcall or cdecl), with a crash caught and returned as
  an exception code;
- memory reads that may fail;
- 1024 hook slots: a hooked UFunction's native entry points at a slot thunk, which calls the SDK before and after the
  original.

For an exe it doesn't know, it only forwards msimg32.

Log: `Homefront Dedicated Server\Binaries\HomefrontSDK\HomefrontSDK.log`, shared with the SDK.

## Build

```
HomefrontModLoader\build.cmd          builds build\msimg32.dll (32-bit, Visual Studio x86 C++ tools)
HomefrontSDK\Build.ps1 -Install       builds it with the rest and installs it (stop the server first)
```

| File | What |
|---|---|
| `loader.cpp` | Entry, msimg32 forwarding, build table, tick hook, .NET start and hand-off |
| `vac.cpp` | The in-memory VAC patch and the secure check |
| `native_api.cpp` | The native API for the SDK |
| `hfsdk_native.h` | The loader/SDK contract, mirrored in `HomefrontSDK/SDK/Internal/NativeApi.cs` |

## Supporting another build

Add a row to `kBuilds` in `loader.cpp`: exe name, `GEngine`, the `Tick` vtable offset and what that slot holds, and
GObjects/GNames/GWorld (from `HomefrontSDK/Tools/README.md`). Find the build's `eServerMode` code for `vac.cpp`: search for
`SteamGameServer_Init`'s caller. The SDK's bindings must be regenerated for it too.
