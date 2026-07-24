// libSceFiber HLE — cooperative fiber (coroutine) stubs via Windows Fibers.
//
// PS5 games call sceFiberInitialize to set up co-routines for their task
// systems (job schedulers, UI state machines, etc.).  We implement the
// PS5 fiber API on top of Win32 CreateFiber / SwitchToFiber / DeleteFiber.
//
// SceLibcFiber layout (guest, 256 bytes):
//   0x00  u64  stack_ptr    (host fiber handle stored here)
//   0x08  u64  stack_size
//   0x10  u64  entry_addr   (guest RIP to jump to on first run)
//   0x18  u64  arg_addr     (argument passed to entry)
//   0x20  u64  caller_fiber_ptr
//   0x28  u8   state        (0=fresh, 1=running, 2=finished)
//   … (rest zeroed)
//
// Because "entry_addr" is a GUEST function pointer we cannot call it directly
// from a Win32 fiber proc; instead we stash it and transfer control to the
// guest's HLE dispatch path the same way sceKernelCreateThread does.
//
// For the initial implementation we use a simplified "single fiber at a time"
// model: fibers don't actually switch; sceFiberRun/Switch return immediately,
// letting the guest keep running.  This lets titles boot that use fibers only
// for their job system init while we build out the full context-switch path.
#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace HLE {

namespace {

constexpr u64 ORBIS_OK                        = 0;
constexpr u64 ORBIS_FIBER_ERROR_NULL          = 0x80590001;
constexpr u64 ORBIS_FIBER_ERROR_INVALID       = 0x80590002;
constexpr u64 ORBIS_FIBER_ERROR_NOMEM         = 0x80590003;
constexpr u64 ORBIS_FIBER_ERROR_RANGE         = 0x80590004;
constexpr u64 ORBIS_FIBER_ERROR_ALREADY_INIT  = 0x80590005;
constexpr u64 ORBIS_FIBER_ERROR_NOT_INIT      = 0x80590006;

// Guest fiber context offsets
constexpr u64 FIBER_OFFSET_HANDLE    = 0x00; // we store HANDLE here
constexpr u64 FIBER_OFFSET_STACKSZ   = 0x08;
constexpr u64 FIBER_OFFSET_ENTRY     = 0x10;
constexpr u64 FIBER_OFFSET_ARG       = 0x18;
constexpr u64 FIBER_OFFSET_CALLER    = 0x20;
constexpr u64 FIBER_OFFSET_STATE     = 0x28; // u8: 0=fresh,1=running,2=done

std::mutex g_fiber_mutex;
// Map guest fiber ptr -> host fiber handle (LPVOID)
std::unordered_map<guest_addr_t, LPVOID> g_fiber_map;

// Thread-local: "current" guest fiber pointer so sceFiberGetSelf works.
static thread_local guest_addr_t tls_current_fiber = 0;

} // namespace

void RegisterLibFiber() {
    LOG_INFO(HLE, "Registering libSceFiber HLE symbols...");

    // sceFiberInitialize — set up a fiber context struct.
    // Prototype: int sceFiberInitialize(SceFiber* fiber, const char* name,
    //   SceFiberEntry entry, u64 argOnInitialize,
    //   void* addrContext, size_t sizeContext, const SceFiberOptParam* optParam)
    auto InitializeImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t fiber_ptr   = args.arg1;
        // args.arg2 = name ptr (ignored)
        const guest_addr_t entry_addr  = args.arg3;
        const u64 arg_on_init          = args.arg4;
        // args.arg5 = stack buffer ptr (ignored — we use Win32 fiber stack)
        const u64 stack_size           = args.arg6;

        if (!fiber_ptr) return ORBIS_FIBER_ERROR_NULL;

        // Zero out the context first
        for (u64 i = 0; i < 0x100; i += 8) {
            Memory::Write<u64>(fiber_ptr + i, 0);
        }

        // Store entry and arg so sceFiberRun can call them later
        Memory::Write<u64>(fiber_ptr + FIBER_OFFSET_ENTRY, entry_addr);
        Memory::Write<u64>(fiber_ptr + FIBER_OFFSET_ARG,   arg_on_init);
        Memory::Write<u64>(fiber_ptr + FIBER_OFFSET_STACKSZ, stack_size ? stack_size : 128 * 1024);
        Memory::Write<u8> (fiber_ptr + FIBER_OFFSET_STATE, 0); // fresh

        // Create a Win32 fiber placeholder (we don't actually execute guest code
        // in it yet — the fiber body below does nothing for now)
        LPVOID hFiber = CreateFiberEx(
            static_cast<SIZE_T>(stack_size ? stack_size : 128 * 1024),
            0, FIBER_FLAG_FLOAT_SWITCH,
            [](LPVOID /*param*/) { /* placeholder — stays idle */ },
            nullptr);

        if (!hFiber) {
            LOG_WARN(HLE, "sceFiberInitialize: CreateFiberEx failed (GLE=%u)", GetLastError());
            return ORBIS_FIBER_ERROR_NOMEM;
        }

        {
            std::lock_guard<std::mutex> lk(g_fiber_mutex);
            g_fiber_map[fiber_ptr] = hFiber;
        }

        // Store the Win32 handle in the context (host-side only, games don't parse this field)
        Memory::Write<u64>(fiber_ptr + FIBER_OFFSET_HANDLE,
                           reinterpret_cast<u64>(hFiber));

        LOG_DEBUG(HLE, "sceFiberInitialize(fiber: 0x%llx, entry: 0x%llx, arg: 0x%llx) -> 0",
                  fiber_ptr, entry_addr, arg_on_init);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceFiber", "sceFiberInitialize",     InitializeImpl);
    RegisterSymbol("libSceFiber", "sceFiberInitialize#T#T", InitializeImpl);

    // sceFiberRun — "run" the fiber (simplified: mark running, return immediately).
    // Full context switch requires running the guest entry on the fiber stack, which
    // needs the JIT recompiler or the existing guest thread mechanism. For now we
    // return immediately so the boot sequence proceeds.
    auto RunImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t fiber_ptr = args.arg1;
        const u64 arg_on_run         = args.arg2;
        // args.arg3 = argOnReturn ptr (we write 0)
        const guest_addr_t arg_return_ptr = args.arg3;

        if (!fiber_ptr) return ORBIS_FIBER_ERROR_NULL;

        const u8 state = Memory::Read<u8>(fiber_ptr + FIBER_OFFSET_STATE);
        if (state != 0) {
            LOG_WARN(HLE, "sceFiberRun(0x%llx): fiber not in fresh state (state=%u)", fiber_ptr, state);
        }

        Memory::Write<u8>(fiber_ptr + FIBER_OFFSET_STATE, 2); // mark done immediately
        if (arg_return_ptr) Memory::Write<u64>(arg_return_ptr, 0);

        LOG_DEBUG(HLE, "sceFiberRun(fiber: 0x%llx, argOnRun: 0x%llx) -> stub (immediate return)",
                  fiber_ptr, arg_on_run);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceFiber", "sceFiberRun",     RunImpl);
    RegisterSymbol("libSceFiber", "sceFiberRun#T#T", RunImpl);

    // sceFiberSwitch — switch to another fiber (stub: noop).
    auto SwitchImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t fiber_ptr      = args.arg1;
        const u64 arg_on_switch           = args.arg2;
        const guest_addr_t arg_return_ptr = args.arg3;
        if (arg_return_ptr) Memory::Write<u64>(arg_return_ptr, 0);
        LOG_DEBUG(HLE, "sceFiberSwitch(to: 0x%llx, arg: 0x%llx) -> stub", fiber_ptr, arg_on_switch);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceFiber", "sceFiberSwitch",     SwitchImpl);
    RegisterSymbol("libSceFiber", "sceFiberSwitch#T#T", SwitchImpl);

    // sceFiberReturnToThread — return from fiber back to calling thread.
    auto ReturnToThreadImpl = [](const GuestArgs& args) -> u64 {
        const u64 arg_on_return           = args.arg1;
        const guest_addr_t arg_return_ptr = args.arg2;
        if (arg_return_ptr) Memory::Write<u64>(arg_return_ptr, 0);
        LOG_DEBUG(HLE, "sceFiberReturnToThread(arg: 0x%llx) -> stub", arg_on_return);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceFiber", "sceFiberReturnToThread",     ReturnToThreadImpl);
    RegisterSymbol("libSceFiber", "sceFiberReturnToThread#T#T", ReturnToThreadImpl);

    // sceFiberFinalize — clean up the fiber.
    auto FinalizeImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t fiber_ptr = args.arg1;
        if (!fiber_ptr) return ORBIS_FIBER_ERROR_NULL;

        LPVOID hFiber = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_fiber_mutex);
            auto it = g_fiber_map.find(fiber_ptr);
            if (it != g_fiber_map.end()) {
                hFiber = it->second;
                g_fiber_map.erase(it);
            }
        }
        if (hFiber) DeleteFiber(hFiber);

        LOG_DEBUG(HLE, "sceFiberFinalize(fiber: 0x%llx) -> 0", fiber_ptr);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceFiber", "sceFiberFinalize",     FinalizeImpl);
    RegisterSymbol("libSceFiber", "sceFiberFinalize#T#T", FinalizeImpl);

    // sceFiberGetSelf — return the current fiber pointer (thread-local).
    auto GetSelfImpl = [](const GuestArgs& /*args*/) -> u64 {
        LOG_DEBUG(HLE, "sceFiberGetSelf() -> 0x%llx", tls_current_fiber);
        return tls_current_fiber;
    };
    RegisterSymbol("libSceFiber", "sceFiberGetSelf",     GetSelfImpl);
    RegisterSymbol("libSceFiber", "sceFiberGetSelf#T#T", GetSelfImpl);

    // sceFiberGetInfo — fill an info struct with name and stack info.
    auto GetInfoImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t fiber_ptr = args.arg1;
        const guest_addr_t info_ptr  = args.arg2;
        if (!fiber_ptr || !info_ptr) return ORBIS_FIBER_ERROR_NULL;
        // Zero the struct (64 bytes)
        for (int i = 0; i < 64; i += 8) Memory::Write<u64>(info_ptr + i, 0);
        // Fill stack size
        Memory::Write<u64>(info_ptr + 0x10, Memory::Read<u64>(fiber_ptr + FIBER_OFFSET_STACKSZ));
        return ORBIS_OK;
    };
    RegisterSymbol("libSceFiber", "sceFiberGetInfo",     GetInfoImpl);
    RegisterSymbol("libSceFiber", "sceFiberGetInfo#T#T", GetInfoImpl);

    // sceFiberOptParamInitialize — zero the options struct.
    auto OptParamInitImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t param_ptr = args.arg1;
        if (param_ptr) {
            for (int i = 0; i < 64; i += 8) Memory::Write<u64>(param_ptr + i, 0);
        }
        return ORBIS_OK;
    };
    RegisterSymbol("libSceFiber", "sceFiberOptParamInitialize",     OptParamInitImpl);
    RegisterSymbol("libSceFiber", "sceFiberOptParamInitialize#T#T", OptParamInitImpl);
}

} // namespace HLE
