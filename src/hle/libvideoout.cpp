// libSceVideoOut HLE — real flip/queue model (SharpEmu design study).
//
// Per-handle port state (flip rate, vblank/flip counters, 16 display-buffer
// slots), a 60 Hz vblank pump thread, and flip/vblank notifications delivered
// through the libkernel equeue implementation (EVFILT_VIDEO_OUT = -13):
//
//   - sceVideoOutAddVblankEvent registers an (equeue, udata) pair and signals
//     one edge immediately — engines wait on the queue before their first flip.
//   - sceVideoOutSubmitFlip validates the buffer index, bumps the flip
//     counters, posts FLIP events, and presents the framebuffer via the GPU
//     backend (the presentation integration that the legacy sceVideoOutFlip
//     already had).
//   - sceVideoOutGetFlipStatus fills the classic 40-byte SceVideoOutFlipStatus:
//     count@0, processTime@8, tsc@0x10, flipArg@0x18, currentBuffer@0x20.
//
// Error codes follow Orbis: 0x80290001..0x8029001A.
#include "hle.h"
#include "libkernel_sync.h"
#include "../gpu/gpu.h"
#include "../common/log.h"
#include "../kernel/guest_clock.h"
#include "../memory/memory.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace HLE {

namespace {

// Orbis error codes.
constexpr u64 kErrorInvalidValue      = 0x80290001;
constexpr u64 kErrorInvalidAddress    = 0x80290002;
constexpr u64 kErrorResourceBusy      = 0x80290009;
constexpr u64 kErrorInvalidIndex      = 0x8029000A;
constexpr u64 kErrorInvalidHandle     = 0x8029000B;
constexpr u64 kErrorInvalidEventQueue = 0x8029000C;
constexpr u64 kErrorInvalidEvent      = 0x8029000D;
constexpr u64 kErrorInvalidOption     = 0x8029001A;

constexpr u32 kMaxOpenPorts      = 4;
constexpr u32 kMaxDisplayBuffers = 16;

// kevent identity for video-out notifications.
constexpr s16 EVFILT_VIDEO_OUT              = -13;
constexpr u64 kVideoOutInternalEventVblank  = 0x5;
constexpr u64 kVideoOutInternalEventFlip    = 0x6;

constexpr u64 kBufferAttributeSize  = 0x28;
constexpr u64 kOutputStatusSize     = 0x30;
constexpr u64 kFlipStatusSize       = 0x28; // 40 bytes
constexpr double kVblankHz          = 60.0;

struct VideoOutEventReg {
    u32 equeue = 0;
    u64 udata  = 0;
};

struct VideoOutPort {
    u32 handle = 0;
    u32 flip_rate = 0;
    u64 vblank_count = 0;
    u64 flip_count = 0;
    s64 last_flip_arg = 0;
    s32 current_buffer = -1;
    std::array<guest_addr_t, kMaxDisplayBuffers> buffers{};
    std::array<bool, kMaxDisplayBuffers>         buffer_used{};
    u32 width  = 1920;
    u32 height = 1080;
    u32 pitch  = 1920;
    float gamma = 1.0f;
    std::vector<VideoOutEventReg> vblank_events;
    std::vector<VideoOutEventReg> flip_events;
};

std::mutex                                              g_vo_mutex;
std::unordered_map<u32, std::shared_ptr<VideoOutPort>>  g_vo_ports;
u32                                                     g_next_vo_handle = 0x4000;

// Vblank pump: one host thread, 60 Hz edge sequence.
std::mutex              g_vblank_mtx;
std::condition_variable g_vblank_cv;
std::atomic<u64>        g_vblank_edge_seq{0};
std::atomic<bool>       g_vblank_started{false};
std::atomic<bool>       g_vblank_stop{false};

std::shared_ptr<VideoOutPort> FindPort(u32 handle) {
    std::lock_guard<std::mutex> lk(g_vo_mutex);
    auto it = g_vo_ports.find(handle);
    return (it != g_vo_ports.end()) ? it->second : nullptr;
}

// Posts one vblank edge to every registered queue of `port` and bumps the
// port's vblank counter.  Port fields are read under g_vo_mutex; the actual
// equeue posting happens after the snapshot so the two lock domains never
// nest.
void SignalVblank(const std::shared_ptr<VideoOutPort>& port) {
    std::vector<VideoOutEventReg> regs;
    u64 count = 0;
    {
        std::lock_guard<std::mutex> lk(g_vo_mutex);
        port->vblank_count++;
        count = port->vblank_count;
        regs  = port->vblank_events;
    }
    const s64 hint = static_cast<s64>(kVideoOutInternalEventVblank |
                                      ((count & 0x0000FFFFFFFFFFFFULL) << 16));
    for (const auto& reg : regs) {
        SceKernelPostEvent(reg.equeue, kVideoOutInternalEventVblank, EVFILT_VIDEO_OUT,
                           reg.udata, hint);
    }
}

void VblankPumpLoop() {
    const auto interval = std::chrono::duration<double>(1.0 / kVblankHz);
    auto next_edge = std::chrono::steady_clock::now() + interval;
    while (!g_vblank_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_until(next_edge);
        next_edge += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);

        g_vblank_edge_seq.fetch_add(1, std::memory_order_release);
        g_vblank_cv.notify_all();

        // Signal only ports that actually have vblank queues registered.
        std::vector<std::shared_ptr<VideoOutPort>> ports;
        {
            std::lock_guard<std::mutex> lk(g_vo_mutex);
            for (const auto& kv : g_vo_ports) {
                if (!kv.second->vblank_events.empty()) {
                    ports.push_back(kv.second);
                }
            }
        }
        for (const auto& port : ports) {
            SignalVblank(port);
        }
    }
}

void EnsureVblankPumpStarted() {
    bool expected = false;
    if (!g_vblank_started.compare_exchange_strong(expected, true)) {
        return;
    }
    g_vblank_stop.store(false, std::memory_order_relaxed);
    // Registered at runtime, so it runs before namespace-scope static
    // destructors: cleanly ends the pump loop in processes that exit through
    // main() (unit tests) instead of ExitProcess (the emulator itself).
    std::atexit(+[] { g_vblank_stop.store(true, std::memory_order_relaxed); });
    std::thread(VblankPumpLoop).detach();
}

// Shared tail of SubmitFlip/sceVideoOutFlip: bump counters, fire events,
// present the framebuffer.
u64 SubmitFlipImpl(const GuestArgs& args) {
    const u32 handle      = static_cast<u32>(args.arg1);
    const s32 bufferIndex = static_cast<s32>(args.arg2);
    const u32 flipMode    = static_cast<u32>(args.arg3);
    const s64 flipArg     = static_cast<s64>(args.arg4);
    (void)flipMode;

    auto port = FindPort(handle);
    if (!port) return kErrorInvalidHandle;
    if (bufferIndex < -1 || bufferIndex >= static_cast<s32>(kMaxDisplayBuffers)) {
        return kErrorInvalidIndex;
    }

    std::vector<VideoOutEventReg> regs;
    guest_addr_t fb_addr = 0;
    {
        std::lock_guard<std::mutex> lk(g_vo_mutex);
        if (bufferIndex != -1 && !port->buffer_used[static_cast<size_t>(bufferIndex)]) {
            LOG_WARN(HLE, "sceVideoOutSubmitFlip(0x%X): buffer %d not registered", handle, bufferIndex);
            return kErrorInvalidIndex;
        }
        port->current_buffer = bufferIndex;
        port->flip_count++;
        port->last_flip_arg  = flipArg;
        regs = port->flip_events;
        if (bufferIndex >= 0) {
            fb_addr = port->buffers[static_cast<size_t>(bufferIndex)];
        }
    }

    const s64 hint = static_cast<s64>(kVideoOutInternalEventFlip |
                                      ((static_cast<u64>(flipArg) & 0x0000FFFFFFFFFFFFULL) << 16));
    for (const auto& reg : regs) {
        SceKernelPostEvent(reg.equeue, kVideoOutInternalEventFlip, EVFILT_VIDEO_OUT,
                           reg.udata, hint);
    }

    LOG_DEBUG(HLE, "sceVideoOutSubmitFlip(0x%X, buffer: %d, arg: %lld) -> flip #%llu",
              handle, bufferIndex, flipArg, port->flip_count);

    // Present (or re-present the boot screen when no buffer is bound) and keep
    // the host window responsive.
    GPU::RenderFrame(fb_addr);
    GPU::PollEvents();
    if (GPU::HasWindow() && GPU::ShouldCloseWindow()) {
        LOG_INFO(HLE, "GLFW Window close requested. Terminating emulator.");
        ExitProcess(0);
    }
    return 0;
}

// Upserts an (equeue, udata) registration into `list` (caller holds g_vo_mutex).
void UpsertEventReg(std::vector<VideoOutEventReg>& list, u32 equeue, u64 udata) {
    for (auto& reg : list) {
        if (reg.equeue == equeue) {
            reg.udata = udata;
            return;
        }
    }
    list.push_back({equeue, udata});
}

u64 AddEventImpl(const GuestArgs& args, bool is_vblank) {
    const u32 equeue = static_cast<u32>(args.arg1);
    const u32 handle = static_cast<u32>(args.arg2);
    const u64 udata  = args.arg3;

    auto port = FindPort(handle);
    if (!port) return kErrorInvalidHandle;
    if (!SceKernelEqueueExists(equeue)) {
        LOG_WARN(HLE, "sceVideoOutAdd%sEvent: bad equeue 0x%X", is_vblank ? "Vblank" : "Flip", equeue);
        return kErrorInvalidEventQueue;
    }

    {
        std::lock_guard<std::mutex> lk(g_vo_mutex);
        UpsertEventReg(is_vblank ? port->vblank_events : port->flip_events, equeue, udata);
    }
    LOG_INFO(HLE, "sceVideoOutAdd%sEvent(eq: 0x%X, handle: 0x%X, udata: 0x%llx) -> 0",
             is_vblank ? "Vblank" : "Flip", equeue, handle, udata);

    if (is_vblank) {
        // Some engines wait on this queue before issuing their first flip;
        // provide a first edge immediately.
        SignalVblank(port);
    }
    return 0;
}

// Reads a SceVideoOutBufferAttribute (0x28 bytes) into the port state and
// forwards the geometry to the GPU backend.
void ApplyBufferAttribute(const std::shared_ptr<VideoOutPort>& port, guest_addr_t attr) {
    const u64 pixel_format = Memory::Read<u64>(attr + 0x00);
    const u32 width        = Memory::Read<u32>(attr + 0x10);
    const u32 height       = Memory::Read<u32>(attr + 0x14);
    const u32 pitch        = Memory::Read<u32>(attr + 0x18);
    {
        std::lock_guard<std::mutex> lk(g_vo_mutex);
        if (width)  port->width  = width;
        if (height) port->height = height;
        if (pitch)  port->pitch  = pitch;
    }
    if (width && height) {
        GPU::SetFramebufferConfig(width, height, static_cast<u32>(pixel_format & 0xFFFFFFFFu));
    }
}

} // namespace

// AGC DCB RFlip packets forward here (libagc.cpp submit walker); reuses the
// SubmitFlip path so flip counters, equeue events and presentation all behave
// exactly as if the game had called sceVideoOutSubmitFlip directly.
u64 VideoOutSubmitFlipFromAgc(u32 handle, s32 buffer_index, u32 flip_mode, s64 flip_arg) {
    GuestArgs args{};
    args.arg1 = handle;
    args.arg2 = static_cast<u64>(static_cast<s64>(buffer_index));
    args.arg3 = flip_mode;
    args.arg4 = static_cast<u64>(flip_arg);
    return SubmitFlipImpl(args);
}

void RegisterLibVideoOut() {
    LOG_INFO(HLE, "Registering libSceVideoOut HLE symbols (port/flip model)...");

    // sceVideoOutOpen (Up36PTk687E)
    auto OpenImpl = [](const GuestArgs& args) -> u64 {
        const u32 userId = static_cast<u32>(args.arg1);
        const u32 type   = static_cast<u32>(args.arg2);
        const u32 index  = static_cast<u32>(args.arg3);
        (void)args.arg4; // reserved

        std::lock_guard<std::mutex> lk(g_vo_mutex);
        if (g_vo_ports.size() >= kMaxOpenPorts) {
            LOG_ERROR(HLE, "sceVideoOutOpen: too many open ports");
            return kErrorResourceBusy;
        }
        const u32 handle = g_next_vo_handle++;
        auto port = std::make_shared<VideoOutPort>();
        port->handle = handle;
        g_vo_ports[handle] = port;
        LOG_INFO(HLE, "sceVideoOutOpen(userId: %u, type: %u, index: %u) -> handle 0x%X",
                 userId, type, index, handle);

        EnsureVblankPumpStarted();
        return handle;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutOpen", OpenImpl);
    RegisterSymbol("libSceVideoOut", "Up36PTk687E#T#T", OpenImpl);
    RegisterSymbol("libkernel", "Up36PTk687E#C#D", OpenImpl); // legacy libkernel stub slot

    // sceVideoOutClose (uquVH4-Du78)
    auto CloseImpl = [](const GuestArgs& args) -> u64 {
        const u32 handle = static_cast<u32>(args.arg1);
        std::lock_guard<std::mutex> lk(g_vo_mutex);
        g_vo_ports.erase(handle);
        LOG_INFO(HLE, "sceVideoOutClose(handle: 0x%X) -> 0", handle);
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutClose", CloseImpl);
    RegisterSymbol("libSceVideoOut", "uquVH4-Du78#T#T", CloseImpl);

    // sceVideoOutSetFlipRate (CBiu4mCE1DA)
    auto SetFlipRateImpl = [](const GuestArgs& args) -> u64 {
        const u32 handle = static_cast<u32>(args.arg1);
        const s32 rate   = static_cast<s32>(args.arg2);
        if (rate < 0 || rate > 2) return kErrorInvalidValue;
        auto port = FindPort(handle);
        if (!port) return kErrorInvalidHandle;
        {
            std::lock_guard<std::mutex> lk(g_vo_mutex);
            port->flip_rate = static_cast<u32>(rate);
        }
        LOG_INFO(HLE, "sceVideoOutSetFlipRate(0x%X, rate: %d) -> 0", handle, rate);
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutSetFlipRate", SetFlipRateImpl);
    RegisterSymbol("libSceVideoOut", "CBiu4mCE1DA#T#T", SetFlipRateImpl);

    // sceVideoOutInitializeOutputOptions (+I4K03i3EL0) — accept, no-op.
    auto TrivialOkImpl = [](const GuestArgs&) -> u64 { return 0; };
    RegisterSymbol("libSceVideoOut", "sceVideoOutInitializeOutputOptions", TrivialOkImpl);
    RegisterSymbol("libSceVideoOut", "+I4K03i3EL0#T#T", TrivialOkImpl);

    // sceVideoOutConfigureOutput (w0hLuNarQxY) — accept any configuration.
    auto ConfigureOutputImpl = [](const GuestArgs& args) -> u64 {
        const u32 handle = static_cast<u32>(args.arg1);
        if (!FindPort(handle)) return kErrorInvalidHandle;
        LOG_INFO(HLE, "sceVideoOutConfigureOutput(0x%X, ...) -> 0", handle);
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutConfigureOutput", ConfigureOutputImpl);
    RegisterSymbol("libSceVideoOut", "w0hLuNarQxY#T#T", ConfigureOutputImpl);

    // sceVideoOutGetOutputStatus (utPrVdxio-8) — 0x30-byte status block.
    auto GetOutputStatusImpl = [](const GuestArgs& args) -> u64 {
        const u32 handle = static_cast<u32>(args.arg1);
        const guest_addr_t status_ptr = args.arg2;
        if (!status_ptr) return kErrorInvalidAddress;
        auto port = FindPort(handle);
        if (!port) return kErrorInvalidHandle;
        for (u64 i = 0; i < kOutputStatusSize; ++i) {
            Memory::Write<u8>(status_ptr + i, 0);
        }
        const u32 res_class = (port->width >= 3840 || port->height >= 2160) ? 2 : 1;
        Memory::Write<s32>(status_ptr + 0x00, static_cast<s32>(res_class));
        Memory::Write<s32>(status_ptr + 0x04, 1);
        Memory::Write<u64>(status_ptr + 0x08, 60); // refresh rate
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutGetOutputStatus", GetOutputStatusImpl);
    RegisterSymbol("libSceVideoOut", "utPrVdxio-8#T#T", GetOutputStatusImpl);

    // sceVideoOutColorSettingsSetGamma_ (DYhhWbJSeRg) — gamma arrives in XMM0,
    // which the dispatcher does not capture; accept and report success.
    RegisterSymbol("libSceVideoOut", "sceVideoOutColorSettingsSetGamma_",
                   [](const GuestArgs& args) -> u64 {
                       if (!args.arg1) return kErrorInvalidAddress;
                       return 0;
                   });
    RegisterSymbol("libSceVideoOut", "DYhhWbJSeRg#T#T",
                   [](const GuestArgs& args) -> u64 {
                       if (!args.arg1) return kErrorInvalidAddress;
                       return 0;
                   });

    // sceVideoOutAdjustColor_ (pv9CI5VC+R0) — read gamma float, store it.
    auto AdjustColorImpl = [](const GuestArgs& args) -> u64 {
        const u32 handle = static_cast<u32>(args.arg1);
        const guest_addr_t settings = args.arg2;
        if (!settings) return kErrorInvalidAddress;
        auto port = FindPort(handle);
        if (!port) return kErrorInvalidHandle;
        const float gamma = Memory::Read<float>(settings);
        {
            std::lock_guard<std::mutex> lk(g_vo_mutex);
            port->gamma = gamma;
        }
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutAdjustColor_", AdjustColorImpl);
    RegisterSymbol("libSceVideoOut", "pv9CI5VC+R0#T#T", AdjustColorImpl);

    // sceVideoOutWaitVblank (j6RaAUlaLv0) — block until the next 60 Hz edge
    // (bounded), then signal the port's vblank queues like the pump would.
    auto WaitVblankImpl = [](const GuestArgs& args) -> u64 {
        const u32 handle = static_cast<u32>(args.arg1);
        auto port = FindPort(handle);
        if (!port) return kErrorInvalidHandle;
        EnsureVblankPumpStarted();
        const u64 entry = g_vblank_edge_seq.load(std::memory_order_acquire);
        std::unique_lock<std::mutex> lk(g_vblank_mtx);
        g_vblank_cv.wait_for(lk, std::chrono::milliseconds(100), [&] {
            return g_vblank_edge_seq.load(std::memory_order_acquire) != entry;
        });
        lk.unlock();
        SignalVblank(port);
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutWaitVblank", WaitVblankImpl);
    RegisterSymbol("libSceVideoOut", "j6RaAUlaLv0#T#T", WaitVblankImpl);

    // sceVideoOutAddVblankEvent (Xru92wHJRmg) — registers an equeue for vblank
    // notifications and signals one edge immediately.
    auto AddVblankEventImpl = [](const GuestArgs& args) -> u64 { return AddEventImpl(args, true); };
    RegisterSymbol("libSceVideoOut", "sceVideoOutAddVblankEvent", AddVblankEventImpl);
    RegisterSymbol("libSceVideoOut", "Xru92wHJRmg#T#T", AddVblankEventImpl);

    // sceVideoOutAddFlipEvent (HXzjK9yI30k)
    auto AddFlipEventImpl = [](const GuestArgs& args) -> u64 { return AddEventImpl(args, false); };
    RegisterSymbol("libSceVideoOut", "sceVideoOutAddFlipEvent", AddFlipEventImpl);
    RegisterSymbol("libSceVideoOut", "HXzjK9yI30k#T#T", AddFlipEventImpl);
    RegisterSymbol("libkernel", "HXzjK9yI30k#C#D", AddFlipEventImpl); // legacy libkernel stub slot

    // sceVideoOutSubmitFlip (U46NwOiJpys) — the PS5 flip entry point.
    RegisterSymbol("libSceVideoOut", "sceVideoOutSubmitFlip", SubmitFlipImpl);
    RegisterSymbol("libSceVideoOut", "U46NwOiJpys#T#T", SubmitFlipImpl);
    // sceVideoOutFlip — classic name kept as an alias.
    RegisterSymbol("libSceVideoOut", "sceVideoOutFlip", SubmitFlipImpl);

    // sceVideoOutGetFlipStatus (SbU3dwp80lQ) — 40-byte status struct:
    // count@0, processTime@8, tsc@0x10, flipArg@0x18, currentBuffer@0x20.
    auto GetFlipStatusImpl = [](const GuestArgs& args) -> u64 {
        const u32 handle = static_cast<u32>(args.arg1);
        const guest_addr_t status_ptr = args.arg2;
        if (!status_ptr) return kErrorInvalidAddress;
        auto port = FindPort(handle);
        if (!port) return kErrorInvalidHandle;

        u64 count, current;
        s64 flip_arg;
        {
            std::lock_guard<std::mutex> lk(g_vo_mutex);
            count    = port->flip_count;
            flip_arg = port->last_flip_arg;
            current  = static_cast<u64>(static_cast<u32>(port->current_buffer));
        }
        Memory::Write<u64>(status_ptr + 0x00, count);
        Memory::Write<u64>(status_ptr + 0x08, 0); // processTime
        Memory::Write<u64>(status_ptr + 0x10, Kernel::GuestClockCounter()); // tsc
        Memory::Write<u64>(status_ptr + 0x18, static_cast<u64>(flip_arg));
        Memory::Write<u64>(status_ptr + 0x20, current);
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutGetFlipStatus", GetFlipStatusImpl);
    RegisterSymbol("libSceVideoOut", "SbU3dwp80lQ#T#T", GetFlipStatusImpl);

    // sceVideoOutIsFlipPending (zgXifHT9ErY) — flips complete synchronously.
    auto IsFlipPendingImpl = [](const GuestArgs& args) -> u64 {
        const u32 handle = static_cast<u32>(args.arg1);
        if (!FindPort(handle)) return kErrorInvalidHandle;
        return 0; // rax = 0: not pending, SCE_OK
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutIsFlipPending", IsFlipPendingImpl);
    RegisterSymbol("libSceVideoOut", "zgXifHT9ErY#T#T", IsFlipPendingImpl);

    // sceVideoOutGetEventId (U2JJtSqNKZI) — validate a video-out kevent.
    auto GetEventIdImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t ev = args.arg1;
        if (!ev) return kErrorInvalidAddress;
        const u64 ident  = Memory::Read<u64>(ev + 0x00);
        const s16 filter = Memory::Read<s16>(ev + 0x08);
        if (filter != EVFILT_VIDEO_OUT ||
            (ident != kVideoOutInternalEventVblank && ident != kVideoOutInternalEventFlip)) {
            return kErrorInvalidEvent;
        }
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutGetEventId", GetEventIdImpl);
    RegisterSymbol("libSceVideoOut", "U2JJtSqNKZI#T#T", GetEventIdImpl);

    // sceVideoOutGetEventData (rWUTcKdkUzQ) — kevent data field -> *dataPtr.
    auto GetEventDataImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t ev       = args.arg1;
        const guest_addr_t data_ptr = args.arg2;
        if (!ev || !data_ptr) return kErrorInvalidAddress;
        const u64 data = Memory::Read<u64>(ev + 0x10);
        Memory::Write<u64>(data_ptr, data);
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutGetEventData", GetEventDataImpl);
    RegisterSymbol("libSceVideoOut", "rWUTcKdkUzQ#T#T", GetEventDataImpl);

    // sceVideoOutSetWindowModeMargins (MTxxrOCeSig) — accept, no-op.
    RegisterSymbol("libSceVideoOut", "sceVideoOutSetWindowModeMargins", TrivialOkImpl);
    RegisterSymbol("libSceVideoOut", "MTxxrOCeSig#T#T", TrivialOkImpl);

    // sceVideoOutUnregisterBuffers (N5KDtkIjjJ4) — drop all slots of the port.
    auto UnregisterBuffersImpl = [](const GuestArgs& args) -> u64 {
        const u32 handle = static_cast<u32>(args.arg1);
        auto port = FindPort(handle);
        if (!port) return kErrorInvalidHandle;
        {
            std::lock_guard<std::mutex> lk(g_vo_mutex);
            port->buffers.fill(0);
            port->buffer_used.fill(false);
        }
        LOG_INFO(HLE, "sceVideoOutUnregisterBuffers(0x%X) -> 0", handle);
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutUnregisterBuffers", UnregisterBuffersImpl);
    RegisterSymbol("libSceVideoOut", "N5KDtkIjjJ4#T#T", UnregisterBuffersImpl);

    // sceVideoOutSetBufferAttribute (i6-sR91Wt-4) — fill a 0x28-byte attribute
    // struct from register arguments (pitch comes in on the guest stack, which
    // the dispatcher does not capture; default pitch = width).
    auto SetBufferAttributeImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t attr = args.arg1;
        if (!attr) return kErrorInvalidAddress;
        const u64 pixel_format = args.arg2;
        const u32 tiling_mode  = static_cast<u32>(args.arg3);
        const u32 aspect_ratio = static_cast<u32>(args.arg4);
        const u32 width        = static_cast<u32>(args.arg5);
        const u32 height       = static_cast<u32>(args.arg6);
        for (u64 i = 0; i < kBufferAttributeSize; ++i) {
            Memory::Write<u8>(attr + i, 0);
        }
        Memory::Write<u64>(attr + 0x00, pixel_format);
        Memory::Write<u32>(attr + 0x08, tiling_mode);
        Memory::Write<u32>(attr + 0x0C, aspect_ratio);
        Memory::Write<u32>(attr + 0x10, width);
        Memory::Write<u32>(attr + 0x14, height);
        Memory::Write<u32>(attr + 0x18, width); // pitchInPixel ~= width
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutSetBufferAttribute", SetBufferAttributeImpl);
    RegisterSymbol("libSceVideoOut", "i6-sR91Wt-4#T#T", SetBufferAttributeImpl);

    // sceVideoOutSetBufferAttribute2 (PjS5uASwcV8) — extended 0x50-byte
    // attribute; write the fields we have, zero the rest.
    auto SetBufferAttribute2Impl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t attr = args.arg1;
        if (!attr) return kErrorInvalidAddress;
        for (u64 i = 0; i < 0x50; ++i) {
            Memory::Write<u8>(attr + i, 0);
        }
        Memory::Write<u64>(attr + 0x20, args.arg2);                    // pixelFormat
        Memory::Write<u32>(attr + 0x04, static_cast<u32>(args.arg3));  // tilingMode
        Memory::Write<u32>(attr + 0x0C, static_cast<u32>(args.arg4));  // width
        Memory::Write<u32>(attr + 0x10, static_cast<u32>(args.arg5));  // height
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutSetBufferAttribute2", SetBufferAttribute2Impl);
    RegisterSymbol("libSceVideoOut", "PjS5uASwcV8#T#T", SetBufferAttribute2Impl);
    RegisterSymbol("libkernel", "PjS5uASwcV8#C#D", SetBufferAttribute2Impl); // legacy stub slot

    // sceVideoOutRegisterBuffers (w3BY+tAEiQY)
    auto RegisterBuffersImpl = [](const GuestArgs& args) -> u64 {
        const u32 handle          = static_cast<u32>(args.arg1);
        const s32 startIndex      = static_cast<s32>(args.arg2);
        const guest_addr_t addrs  = args.arg3;
        const s32 count           = static_cast<s32>(args.arg4);
        const guest_addr_t attr   = args.arg5;

        auto port = FindPort(handle);
        if (!port) return kErrorInvalidHandle;
        if (!addrs) return kErrorInvalidAddress;
        if (!attr)  return kErrorInvalidOption;
        if (startIndex < 0 || count < 1 ||
            startIndex + count > static_cast<s32>(kMaxDisplayBuffers)) {
            return kErrorInvalidValue;
        }

        {
            std::lock_guard<std::mutex> lk(g_vo_mutex);
            for (s32 i = 0; i < count; ++i) {
                const guest_addr_t addr =
                    Memory::Read<guest_addr_t>(addrs + static_cast<u64>(i) * sizeof(guest_addr_t));
                port->buffers[static_cast<size_t>(startIndex + i)]     = addr;
                port->buffer_used[static_cast<size_t>(startIndex + i)] = true;
                LOG_DEBUG(HLE, "  Buffer #%d Address: 0x%llx", startIndex + i, addr);
            }
        }
        ApplyBufferAttribute(port, attr);
        LOG_INFO(HLE, "sceVideoOutRegisterBuffers(0x%X, start: %d, count: %d) -> 0",
                 handle, startIndex, count);
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutRegisterBuffers", RegisterBuffersImpl);
    RegisterSymbol("libSceVideoOut", "w3BY+tAEiQY#T#T", RegisterBuffersImpl);

    // sceVideoOutRegisterBuffers2 (rKBUtgRrtbk) — 0x20-byte entries, address@0.
    // The trailing category/option arguments arrive on the guest stack and are
    // not captured by the dispatcher; they are accepted unchecked.
    auto RegisterBuffers2Impl = [](const GuestArgs& args) -> u64 {
        const u32 handle            = static_cast<u32>(args.arg1);
        const s32 setIndex          = static_cast<s32>(args.arg2);
        const s32 bufferIndexStart  = static_cast<s32>(args.arg3);
        const guest_addr_t bufs     = args.arg4;
        const s32 count             = static_cast<s32>(args.arg5);
        const guest_addr_t attr     = args.arg6;

        auto port = FindPort(handle);
        if (!port) return kErrorInvalidHandle;
        if (!bufs) return kErrorInvalidAddress;
        if (!attr) return kErrorInvalidOption;
        if (bufferIndexStart < 0 || count < 1 ||
            bufferIndexStart + count > static_cast<s32>(kMaxDisplayBuffers)) {
            return kErrorInvalidValue;
        }

        {
            std::lock_guard<std::mutex> lk(g_vo_mutex);
            for (s32 i = 0; i < count; ++i) {
                const guest_addr_t entry = bufs + static_cast<u64>(i) * 0x20;
                const guest_addr_t addr  = Memory::Read<guest_addr_t>(entry + 0x00);
                port->buffers[static_cast<size_t>(bufferIndexStart + i)]     = addr;
                port->buffer_used[static_cast<size_t>(bufferIndexStart + i)] = true;
                LOG_DEBUG(HLE, "  Buffer #%d Address: 0x%llx", bufferIndexStart + i, addr);
            }
        }
        ApplyBufferAttribute(port, attr);
        LOG_INFO(HLE, "sceVideoOutRegisterBuffers2(0x%X, set: %d, start: %d, count: %d) -> %d",
                 handle, setIndex, bufferIndexStart, count, setIndex);
        return static_cast<u64>(static_cast<u32>(setIndex));
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutRegisterBuffers2", RegisterBuffers2Impl);
    RegisterSymbol("libSceVideoOut", "rKBUtgRrtbk#T#T", RegisterBuffers2Impl);
    RegisterSymbol("libkernel", "rKBUtgRrtbk#C#D", RegisterBuffers2Impl); // legacy stub slot

    // sceVideoOutGetResolution — report the port geometry (default 1920x1080).
    auto GetResolutionImpl = [](const GuestArgs& args) -> u64 {
        const u32 handle = static_cast<u32>(args.arg1);
        const guest_addr_t width_ptr  = args.arg2;
        const guest_addr_t height_ptr = args.arg3;
        auto port = FindPort(handle);
        const u32 w = port ? port->width  : 1920;
        const u32 h = port ? port->height : 1080;
        if (width_ptr)  Memory::Write<u32>(width_ptr, w);
        if (height_ptr) Memory::Write<u32>(height_ptr, h);
        return 0;
    };
    RegisterSymbol("libSceVideoOut", "sceVideoOutGetResolution", GetResolutionImpl);
}

} // namespace HLE
