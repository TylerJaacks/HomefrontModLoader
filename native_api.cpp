// The native API the loader hands to the SDK (hfsdk_native.h): crash-guarded calls into game code, guarded memory
// reads, and the hook slots.
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

#include "loader.h"

namespace loader {
namespace {

constexpr uint32_t kProcessEventSlot = 0xD0 / 4;  // UObject::ProcessEvent(UFunction*, void* Parms, void* Result)
constexpr uint32_t kFunctionFunc = 0xA8;          // UFunction::Func

// Pushes `n` words and calls `fn` with `self` in ecx. edi holds the caller's esp across the call so any convention
// (callee or caller cleanup, wrong argument count) returns with a correct stack.
__declspec(noinline) uint32_t RawCall(void* fn, void* self, const uint32_t* args, int32_t n)
{
    uint32_t result;
    __asm {
        mov  edi, esp
        mov  ecx, n
        mov  esi, args
    push_next:
        test ecx, ecx
        jz   call_it
        dec  ecx
        push dword ptr [esi + ecx * 4]
        jmp  push_next
    call_it:
        mov  ecx, self
        call fn
        mov  esp, edi
        mov  result, eax
    }
    return result;
}

int32_t __cdecl ProcessEvent(void* object, void* function, void* parms, uint32_t* code)
{
    using Fn = void(__fastcall*)(void* self, void* edx, void* function, void* parms, void* result);
    __try {
        auto vtable = *static_cast<void***>(object);
        reinterpret_cast<Fn>(vtable[kProcessEventSlot])(object, nullptr, function, parms, nullptr);
        *code = 0;
        return 1;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int32_t __cdecl CallNative(void* fn, void* self, const uint32_t* args, int32_t num_args, uint32_t* ret,
                           uint32_t* code)
{
    __try {
        *ret = RawCall(fn, self, args, num_args);
        *code = 0;
        return 1;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int32_t __cdecl ReadMemory(const void* address, void* out, int32_t size)
{
    __try {
        memcpy(out, address, size);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---- hooks
// A native function called from script reads its arguments from the caller's FFrame, so a hook can't tell which
// function it is from the frame: each hooked UFunction gets its own thunk.
using NativeFn = void(__fastcall*)(void* self, void* edx, void* frame, void* result);  // thiscall (FFrame&, RESULT)

constexpr int kSlots = 1024;

struct Slot {
    void* function;
    NativeFn original;
};
Slot g_slots[kSlots];
int g_used;

void Dispatch(int n, void* self, void* frame, void* result)
{
    const HFManagedApi& m = Managed();
    int32_t flags = m.hook_pre ? m.hook_pre(n, self, frame) : 1;
    if (flags & 1)
        g_slots[n].original(self, nullptr, frame, result);
    if ((flags & 2) && m.hook_post)
        m.hook_post(n, self, frame, result);
}

template <int N>
void __fastcall Thunk(void* self, void*, void* frame, void* result)
{
    Dispatch(N, self, frame, result);
}

template <int... N>
constexpr auto MakeThunks(std::integer_sequence<int, N...>)
{
    return std::array<NativeFn, sizeof...(N)>{&Thunk<N>...};
}
const auto g_thunks = MakeThunks(std::make_integer_sequence<int, kSlots>{});

int32_t __cdecl HookFunction(void* function)
{
    for (int i = 0; i < g_used; ++i)
        if (g_slots[i].function == function)
            return i;
    if (g_used == kSlots)
        return -1;
    auto& func = *reinterpret_cast<NativeFn*>(static_cast<uint8_t*>(function) + kFunctionFunc);
    Slot& s = g_slots[g_used];
    s.function = function;
    s.original = func;  // another hook's thunk if something else hooked it first: the hooks chain
    func = g_thunks[g_used];
    return g_used++;
}

}  // namespace

void FillNativeApi(HFNativeApi& api)
{
    api.process_event = ProcessEvent;
    api.call_native = CallNative;
    api.read_memory = ReadMemory;
    api.hook_function = HookFunction;
    api.hook_slots = kSlots;
}

}  // namespace loader
