// The contract between HomefrontModLoader (msimg32.dll) and the SDK (HomefrontSDK.dll).
// Mirrored in HomefrontSDK/SDK/Internal/NativeApi.cs: change both together and bump HFSDK_NATIVE_API_VERSION.
#pragma once

#include <stdint.h>

#define HFSDK_NATIVE_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

// What the loader gives the SDK. Functions are __cdecl and run on the game thread. Calls into game code are
// crash-guarded: a crash returns 0 and stores the exception code in *code.
typedef struct HFNativeApi {
    int32_t version;
    int32_t size;
    void* gobjects;  // TArray<UObject*>
    void* gnames;    // TArray<FNameEntry*>
    void* gworld;    // UWorld**
    void* gengine;   // UEngine**
    const wchar_t* binaries_dir;  // ...\Binaries\ (trailing backslash)
    const wchar_t* sdk_dir;       // ...\Binaries\HomefrontSDK\ (trailing backslash)
    void(__cdecl* log)(const char* utf8_line);  // HomefrontSDK.log
    // UObject::ProcessEvent (vtable 0xD0) on `object`.
    int32_t(__cdecl* process_event)(void* object, void* function, void* parms, uint32_t* code);
    // Calls `fn` with `self` in ecx and `args` pushed as 32-bit words: thiscall, stdcall or cdecl alike (the stack
    // pointer is restored afterwards). *ret receives eax.
    int32_t(__cdecl* call_native)(void* fn, void* self, const uint32_t* args, int32_t num_args, uint32_t* ret,
                                  uint32_t* code);
    // Copies memory that may be unreadable: 1 if all of it was read.
    int32_t(__cdecl* read_memory)(const void* address, void* out, int32_t size);
    // Routes a UFunction's native entry (Func) through hook slot N, which calls the managed hook_pre/hook_post.
    // Idempotent per function: returns its slot, or -1 when all slots are used. Slots are never released.
    int32_t(__cdecl* hook_function)(void* function);
    int32_t hook_slots;
} HFNativeApi;

// What the SDK gives back.
typedef struct HFManagedApi {
    int32_t size;
    void(__cdecl* tick)(float delta_seconds);  // after every engine tick
    // Before a hooked function runs. Returns flags: 1 = run the original, 2 = call hook_post afterwards.
    int32_t(__cdecl* hook_pre)(int32_t slot, void* self, void* frame);
    void(__cdecl* hook_post)(int32_t slot, void* self, void* frame, void* result);
} HFManagedApi;

// HomefrontSDK.Internal.Loader.Initialize, [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])].
// Returns 1 when the runtime started.
typedef int32_t(__cdecl* HFManagedInitFn)(const HFNativeApi* native, HFManagedApi* managed);

#ifdef __cplusplus
}
#endif
