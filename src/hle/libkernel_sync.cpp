#define _CRT_SECURE_NO_WARNINGS
#include "libkernel_sync.h"
#include "../kernel/thread.h"
#include "../kernel/guest_clock.h"
#include "../memory/memory.h"
#include "../common/log.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// Real libkernel synchronization primitives, event queues, and process clock.
//
// Object model
// ------------
// On Orbis/Prospero, ScePthreadMutex / ScePthreadCond / ScePthreadRwlock are
// opaque pointer-sized handles stored in a guest variable; *Init writes the
// handle into *ptr.  We mirror that: each *Init writes a tagged token into
// the guest variable and keeps the host object in a token-keyed map.  Lock
// operations look the token up; an unrecognised value (zero-initialized or a
// static-initializer magic) is lazily promoted to a real object, which covers
// PTHREAD_MUTEX_INITIALIZER-style usage without us knowing the exact magic.
//
// sceKernel* objects (mutex, sema, event flag, equeue) are integer handles
// from per-family counters backed by handle tables.
//
// Error codes follow the Orbis convention used by the rest of libkernel.cpp:
// 0 on success, SCE_KERNEL_ERROR_E* (0x80020000 + errno) on failure.
// ---------------------------------------------------------------------------

namespace HLE {

namespace {

// Orbis-style error codes (positive, 0x80020000 | errno).
constexpr u32 SCE_KERNEL_ERROR_EPERM     = 0x80020001;
constexpr u32 SCE_KERNEL_ERROR_ESRCH     = 0x80020003;
constexpr u32 SCE_KERNEL_ERROR_ENOMEM    = 0x8002000C;
constexpr u32 SCE_KERNEL_ERROR_EBUSY     = 0x80020010;
constexpr u32 SCE_KERNEL_ERROR_EINVAL    = 0x80020016;
constexpr u32 SCE_KERNEL_ERROR_ETIMEDOUT = 0x8002003C;
constexpr u32 SCE_KERNEL_ERROR_EDEADLK   = 0x80020023;

// Token tags written into guest mutex/cond/rwlock variables (top 16 bits).
constexpr u64 kMutexTokenTag  = 0x4D54000000000000ULL; // 'MT'
constexpr u64 kCondTokenTag   = 0x434E000000000000ULL; // 'CN'
constexpr u64 kRwlockTokenTag = 0x5257000000000000ULL; // 'RW'
constexpr u64 kTokenTagMask   = 0xFFFF000000000000ULL;

bool SafeReadU64(guest_addr_t addr, u64& out) {
    __try {
        out = Memory::Read<u64>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}

bool SafeWriteU64(guest_addr_t addr, u64 value) {
    __try {
        Memory::Write<u64>(addr, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadU32(guest_addr_t addr, u32& out) {
    __try {
        out = Memory::Read<u32>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}

bool TryReadU8(guest_addr_t addr, u8& out) {
    __try {
        out = Memory::Read<u8>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}

bool TryWriteS32(guest_addr_t addr, s32 value) {
    __try {
        Memory::Write<s32>(addr, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// NB: helpers containing __try must stay free of C++ locals that require
// unwinding (C2712) — keep SEH confined to the tiny Try*/Safe* functions.
std::string ReadGuestName(guest_addr_t name_ptr) {
    if (!name_ptr) return "<unnamed>";
    std::string name;
    for (u64 i = 0; i < 64; ++i) {
        u8 c = 0;
        if (!TryReadU8(name_ptr + i, c)) return "<invalid>";
        if (!c) break;
        name += static_cast<char>(c);
    }
    return name;
}

// Reads an Orbis SceKernelUseconds* timeout argument.
// Returns: 0 = no timeout (NULL pointer), 1 = poll (usec==0), 2 = timed wait.
int ReadTimeout(guest_addr_t timo_ptr, u32& out_usec) {
    if (!timo_ptr) return 0;
    if (!SafeReadU32(timo_ptr, out_usec)) return 0;
    return out_usec == 0 ? 1 : 2;
}

// ---------------------------------------------------------------------------
// pthread mutex
//
// Ported from SharpEmu's KernelPthreadCompatExports (commits 73e8821 and
// 90c72eb): a state-based mutex with an explicit owner, recursion count,
// type (taken from the attr) and a FIFO waiter queue.  Unlock hands
// ownership directly to the head waiter instead of only waking it and
// relying on the woken thread to re-acquire: a lost/racing wake would leave
// the mutex "free with a queued waiter", a state the fast-acquire path
// refuses, and every later locker would then queue behind a head that never
// advances.
// ---------------------------------------------------------------------------

// Orbis pthread mutex types (scePthreadMutexattrSettype values).
enum class MutexType : int {
    ErrorCheck = 1,
    Recursive  = 2,
    Normal     = 3,
    AdaptiveNp = 4,
};

MutexType NormalizeMutexType(int type) {
    switch (type) {
    case 2:  return MutexType::Recursive;
    case 3:  return MutexType::Normal;
    case 4:  return MutexType::AdaptiveNp;
    default: return MutexType::ErrorCheck; // 0/1 and unrecognised values
    }
}

// Identity for mutex ownership.  Registered guest threads use their CpuCore
// tid; unregistered host threads (unit tests, host-only callers) fall back to
// the OS thread id so ownership is still per-thread.
u64 CurrentTlsIdentity() {
    u64 tid = Kernel::GetCurrentThreadId();
    if (tid == 0) {
        tid = 0x8000000000000000ULL | ::GetCurrentThreadId();
    }
    return tid;
}

struct GuestMutex {
    std::mutex m;
    std::condition_variable cv;
    u64 owner = 0;                        // CurrentTlsIdentity() of the owner
    int recursion = 0;
    MutexType type = MutexType::ErrorCheck;
    std::deque<u64> waiters;              // FIFO of blocked thread identities
};

// Takes an unowned mutex.  Caller holds m.m.
void MutexAcquireLocked(GuestMutex& mtx, u64 tid) {
    mtx.owner     = tid;
    mtx.recursion = 1;
}

// Releases a fully-unwound mutex, granting it directly to the head waiter
// when one is queued so the mutex is never observable as free-with-waiter.
// Caller holds m.m; mtx.recursion must already be 0.
void MutexHandoffLocked(GuestMutex& mtx) {
    if (!mtx.waiters.empty()) {
        mtx.owner = mtx.waiters.front();
        mtx.waiters.pop_front();
        mtx.recursion = 1;
    } else {
        mtx.owner = 0;
    }
    mtx.cv.notify_all();
}

u64 MutexLockImpl(GuestMutex& mtx, u64 tid, bool try_only) {
    std::unique_lock<std::mutex> lk(mtx.m);
    if (mtx.owner == tid) {
        // Re-lock from the current owner, per type.  Several Gen5 runtimes
        // layer their own owner/count bookkeeping over the kernel mutex, so
        // RECURSIVE and NORMAL keep compatibility recursion and ADAPTIVE is
        // idempotent (one matching unlock fully releases); only ERRORCHECK
        // takes the strict EDEADLK path.
        switch (mtx.type) {
        case MutexType::Recursive:
            ++mtx.recursion;
            return 0;
        case MutexType::AdaptiveNp:
            return try_only ? SCE_KERNEL_ERROR_EBUSY : 0;
        case MutexType::Normal:
            if (try_only) return SCE_KERNEL_ERROR_EBUSY;
            ++mtx.recursion;
            return 0;
        case MutexType::ErrorCheck:
        default:
            return try_only ? SCE_KERNEL_ERROR_EBUSY : SCE_KERNEL_ERROR_EDEADLK;
        }
    }
    // trylock may barge past queued waiters (it can never wedge: a spinning
    // trylock loop still observes the free mutex); the blocking lock honours
    // FIFO so real blocked waiters are not starved by a barging locker.
    if (mtx.owner == 0 && (try_only || mtx.waiters.empty())) {
        MutexAcquireLocked(mtx, tid);
        return 0;
    }
    if (try_only) return SCE_KERNEL_ERROR_EBUSY;
    mtx.waiters.push_back(tid);
    // The head waiter is granted ownership directly by MutexHandoffLocked.
    mtx.cv.wait(lk, [&] { return mtx.owner == tid; });
    return 0;
}

u64 MutexUnlockImpl(GuestMutex& mtx, u64 tid, bool require_owner) {
    std::lock_guard<std::mutex> lk(mtx.m);
    if (mtx.recursion <= 0) return SCE_KERNEL_ERROR_EINVAL;
    if (require_owner && mtx.owner != tid) return SCE_KERNEL_ERROR_EPERM;
    if (--mtx.recursion == 0) {
        MutexHandoffLocked(mtx);
    }
    return 0;
}

// pthread mutex attributes — type keyed by the guest attr variable address.
std::mutex g_mutexattr_lock;
std::unordered_map<guest_addr_t, MutexType> g_mutexattr_types;

MutexType LookupMutexAttrType(guest_addr_t attr) {
    if (!attr) return MutexType::ErrorCheck;
    std::lock_guard<std::mutex> lk(g_mutexattr_lock);
    auto it = g_mutexattr_types.find(attr);
    return it != g_mutexattr_types.end() ? it->second : MutexType::ErrorCheck;
}

std::mutex g_mutex_map_lock;
std::unordered_map<u64, std::unique_ptr<GuestMutex>> g_mutexes; // token -> obj
u64 g_next_mutex_token = 1;

GuestMutex* LookupOrCreateMutex(guest_addr_t var, bool create_if_missing) {
    u64 token = 0;
    if (SafeReadU64(var, token) && (token & kTokenTagMask) == kMutexTokenTag) {
        auto it = g_mutexes.find(token);
        if (it != g_mutexes.end()) return it->second.get();
    }
    if (!create_if_missing) return nullptr;
    const u64 new_token = kMutexTokenTag | g_next_mutex_token++;
    auto obj = std::make_unique<GuestMutex>();
    // A guest word of 1 is the static adaptive-mutex initializer; any other
    // unrecognised value (zero included) promotes to a default mutex.
    if (token == 1) obj->type = MutexType::AdaptiveNp;
    GuestMutex* raw = obj.get();
    g_mutexes[new_token] = std::move(obj);
    SafeWriteU64(var, new_token);
    return raw;
}

// ---------------------------------------------------------------------------
// pthread condition variable
// ---------------------------------------------------------------------------
struct GuestCond {
    std::condition_variable cv;
};

std::mutex g_cond_map_lock;
std::unordered_map<u64, std::unique_ptr<GuestCond>> g_conds; // token -> obj
u64 g_next_cond_token = 1;

GuestCond* LookupOrCreateCond(guest_addr_t var, bool create_if_missing) {
    u64 token = 0;
    if (SafeReadU64(var, token) && (token & kTokenTagMask) == kCondTokenTag) {
        auto it = g_conds.find(token);
        if (it != g_conds.end()) return it->second.get();
    }
    if (!create_if_missing) return nullptr;
    const u64 new_token = kCondTokenTag | g_next_cond_token++;
    auto obj = std::make_unique<GuestCond>();
    GuestCond* raw = obj.get();
    g_conds[new_token] = std::move(obj);
    SafeWriteU64(var, new_token);
    return raw;
}

// ---------------------------------------------------------------------------
// pthread read-write lock
// ---------------------------------------------------------------------------
struct GuestRwlock {
    SRWLOCK srw;
    std::atomic<u32> readers{0};
    std::atomic<bool> writer{false};
    GuestRwlock() { InitializeSRWLock(&srw); }
};

std::mutex g_rwlock_map_lock;
std::unordered_map<u64, std::unique_ptr<GuestRwlock>> g_rwlocks; // token -> obj
u64 g_next_rwlock_token = 1;

GuestRwlock* LookupOrCreateRwlock(guest_addr_t var, bool create_if_missing) {
    u64 token = 0;
    if (SafeReadU64(var, token) && (token & kTokenTagMask) == kRwlockTokenTag) {
        auto it = g_rwlocks.find(token);
        if (it != g_rwlocks.end()) return it->second.get();
    }
    if (!create_if_missing) return nullptr;
    const u64 new_token = kRwlockTokenTag | g_next_rwlock_token++;
    auto obj = std::make_unique<GuestRwlock>();
    GuestRwlock* raw = obj.get();
    g_rwlocks[new_token] = std::move(obj);
    SafeWriteU64(var, new_token);
    return raw;
}

// ---------------------------------------------------------------------------
// pthread once — completed control variables
// ---------------------------------------------------------------------------
std::mutex g_once_lock;
std::unordered_set<guest_addr_t> g_once_done;

// ---------------------------------------------------------------------------
// pthread TLS keys — per-thread value maps
// ---------------------------------------------------------------------------
struct TlsKey {
    u64 destructor = 0;
    std::unordered_map<u64, u64> values; // guest tid -> value
};
std::mutex g_tls_lock;
std::unordered_map<u64, TlsKey> g_tls_keys;
u64 g_next_tls_key = 1;

// ---------------------------------------------------------------------------
// sceKernel* mutex objects (integer handles)
// ---------------------------------------------------------------------------
std::mutex g_kmutex_lock;
std::unordered_map<u32, std::shared_ptr<GuestMutex>> g_kmutexes;
std::atomic<u32> g_next_kmutex{0x2000};

// ---------------------------------------------------------------------------
// sceKernel* counting semaphores (integer handles)
// ---------------------------------------------------------------------------
struct GuestSema {
    std::mutex m;
    std::condition_variable cv;
    s32 count = 0;
    s32 max_count = 0;
};
std::mutex g_sema_lock;
std::unordered_map<u32, std::shared_ptr<GuestSema>> g_semas;
std::atomic<u32> g_next_sema{0x4000};

// ---------------------------------------------------------------------------
// sceKernel* event flags (integer handles)
// ---------------------------------------------------------------------------
struct GuestEventFlag {
    std::mutex m;
    std::condition_variable cv;
    u64 bits = 0;
};
std::mutex g_eflag_lock;
std::unordered_map<u32, std::shared_ptr<GuestEventFlag>> g_eflags;
std::atomic<u32> g_next_eflag{0x5000};

constexpr u32 kEvfWaitAnd      = 0x01;
constexpr u32 kEvfWaitOr       = 0x02;
constexpr u32 kEvfClearPattern = 0x10;
constexpr u32 kEvfClearAll     = 0x20;

// ---------------------------------------------------------------------------
// sceKernel* event queues (integer handles)
// ---------------------------------------------------------------------------
constexpr s16 EVFILT_READ = -1;
constexpr s16 EVFILT_USER = -5;
constexpr u16 EV_ADD      = 0x0001;
constexpr u16 EV_ENABLE   = 0x0004;
constexpr u16 EV_ONESHOT  = 0x0010;
constexpr u32 NOTE_TRIGGER = 0x01000000;
constexpr u64 kKeventSize = 32;

struct EqueueEvent {
    u64 ident  = 0;
    s16 filter = 0;
    u16 flags  = 0;
    u32 fflags = 0;
    s64 data   = 0;
    u64 udata  = 0;
    bool pending = false;
};

struct GuestEqueue {
    std::mutex m;
    std::condition_variable cv;
    std::vector<EqueueEvent> events;
};
std::mutex g_equeue_lock;
std::unordered_map<u32, std::shared_ptr<GuestEqueue>> g_equeues;
std::atomic<u32> g_next_equeue{0x6000};

// Writes one pending event to the guest kevent array; returns false on fault.
bool WriteKevent(guest_addr_t addr, const EqueueEvent& ev) {
    __try {
        Memory::Write<u64>(addr + 0x00, ev.ident);
        Memory::Write<s16>(addr + 0x08, ev.filter);
        Memory::Write<u16>(addr + 0x0A, ev.flags);
        Memory::Write<u32>(addr + 0x0C, ev.fflags);
        Memory::Write<u64>(addr + 0x10, static_cast<u64>(ev.data));
        Memory::Write<u64>(addr + 0x18, ev.udata);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Drains up to `capacity` pending events into the guest buffer.  Caller holds
// eq->m.  Returns the number of events written.
int DrainEqueueEvents(GuestEqueue& eq, guest_addr_t events_ptr, int capacity) {
    int delivered = 0;
    for (auto it = eq.events.begin(); it != eq.events.end() && delivered < capacity;) {
        if (!it->pending) {
            ++it;
            continue;
        }
        if (!WriteKevent(events_ptr + static_cast<u64>(delivered) * kKeventSize, *it)) {
            break; // guest buffer faulted — stop here
        }
        ++delivered;
        if (it->flags & EV_ONESHOT) {
            it = eq.events.erase(it);
        } else {
            it->pending = false;
            it->fflags &= ~NOTE_TRIGGER;
            ++it;
        }
    }
    return delivered;
}

} // namespace

// ===========================================================================
// pthread mutex handlers
// ===========================================================================
u64 ScePthreadMutexInit(const GuestArgs& args) {
    const guest_addr_t var  = args.arg1;
    const guest_addr_t attr = args.arg2;
    if (!var) return SCE_KERNEL_ERROR_EINVAL;
    const MutexType type = LookupMutexAttrType(attr);
    std::lock_guard<std::mutex> lk(g_mutex_map_lock);
    const u64 token = kMutexTokenTag | g_next_mutex_token++;
    auto obj = std::make_unique<GuestMutex>();
    obj->type = type;
    g_mutexes[token] = std::move(obj);
    SafeWriteU64(var, token);
    LOG_DEBUG(HLE, "scePthreadMutexInit(0x%llx) -> token 0x%llx", var, token);
    return 0;
}

u64 ScePthreadMutexattrInit(const GuestArgs& args) {
    const guest_addr_t attr = args.arg1;
    if (!attr) return SCE_KERNEL_ERROR_EINVAL;
    {
        std::lock_guard<std::mutex> lk(g_mutexattr_lock);
        g_mutexattr_types[attr] = MutexType::ErrorCheck;
    }
    SafeWriteU64(attr, 0);
    return 0;
}

u64 ScePthreadMutexattrSettype(const GuestArgs& args) {
    const guest_addr_t attr = args.arg1;
    if (!attr) return SCE_KERNEL_ERROR_EINVAL;
    std::lock_guard<std::mutex> lk(g_mutexattr_lock);
    g_mutexattr_types[attr] = NormalizeMutexType(static_cast<int>(args.arg2));
    return 0;
}

u64 ScePthreadMutexattrDestroy(const GuestArgs& args) {
    std::lock_guard<std::mutex> lk(g_mutexattr_lock);
    g_mutexattr_types.erase(args.arg1);
    return 0;
}

u64 ScePthreadMutexLock(const GuestArgs& args) {
    const guest_addr_t var = args.arg1;
    if (!var) return SCE_KERNEL_ERROR_EINVAL;
    GuestMutex* m = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mutex_map_lock);
        m = LookupOrCreateMutex(var, true);
    }
    if (!m) return SCE_KERNEL_ERROR_EINVAL;
    return MutexLockImpl(*m, CurrentTlsIdentity(), /*try_only=*/false);
}

u64 ScePthreadMutexTrylock(const GuestArgs& args) {
    const guest_addr_t var = args.arg1;
    if (!var) return SCE_KERNEL_ERROR_EINVAL;
    GuestMutex* m = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mutex_map_lock);
        m = LookupOrCreateMutex(var, true);
    }
    if (!m) return SCE_KERNEL_ERROR_EINVAL;
    return MutexLockImpl(*m, CurrentTlsIdentity(), /*try_only=*/true);
}

u64 ScePthreadMutexUnlock(const GuestArgs& args) {
    const guest_addr_t var = args.arg1;
    if (!var) return SCE_KERNEL_ERROR_EINVAL;
    GuestMutex* m = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mutex_map_lock);
        m = LookupOrCreateMutex(var, false);
    }
    if (!m) {
        LOG_WARN(HLE, "scePthreadMutexUnlock(0x%llx): unknown mutex", var);
        return SCE_KERNEL_ERROR_EINVAL;
    }
    return MutexUnlockImpl(*m, CurrentTlsIdentity(), /*require_owner=*/true);
}

u64 ScePthreadMutexDestroy(const GuestArgs& args) {
    const guest_addr_t var = args.arg1;
    if (!var) return SCE_KERNEL_ERROR_EINVAL;
    u64 token = 0;
    std::lock_guard<std::mutex> lk(g_mutex_map_lock);
    if (SafeReadU64(var, token) && (token & kTokenTagMask) == kMutexTokenTag) {
        g_mutexes.erase(token);
    } else {
        LOG_WARN(HLE, "scePthreadMutexDestroy(0x%llx): unknown mutex", var);
    }
    SafeWriteU64(var, 0);
    return 0;
}

// ===========================================================================
// pthread condition variable handlers
// ===========================================================================
u64 ScePthreadCondInit(const GuestArgs& args) {
    const guest_addr_t var = args.arg1;
    (void)args.arg2; // attr ignored
    if (!var) return SCE_KERNEL_ERROR_EINVAL;
    std::lock_guard<std::mutex> lk(g_cond_map_lock);
    const u64 token = kCondTokenTag | g_next_cond_token++;
    g_conds[token] = std::make_unique<GuestCond>();
    SafeWriteU64(var, token);
    LOG_DEBUG(HLE, "scePthreadCondInit(0x%llx) -> token 0x%llx", var, token);
    return 0;
}

static u64 CondWaitImpl(guest_addr_t cond_var, guest_addr_t mutex_var, DWORD timeout_ms) {
    if (!cond_var || !mutex_var) return SCE_KERNEL_ERROR_EINVAL;
    GuestCond* c = nullptr;
    GuestMutex* m = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_cond_map_lock);
        c = LookupOrCreateCond(cond_var, true);
    }
    {
        std::lock_guard<std::mutex> lk(g_mutex_map_lock);
        m = LookupOrCreateMutex(mutex_var, true);
    }
    if (!c || !m) return SCE_KERNEL_ERROR_EINVAL;
    const u64 tid = CurrentTlsIdentity();
    std::unique_lock<std::mutex> lk(m->m);
    // Games occasionally condwait on a mutex they never locked; adopt
    // ownership so the unlock/wait/re-lock cycle stays balanced and the
    // mutex is actually released while waiting.
    if (m->owner == 0 && m->recursion == 0) {
        MutexAcquireLocked(*m, tid);
    }
    if (m->owner != tid || m->recursion != 1) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    // Caller holds the mutex: release it (handing off to any head waiter)
    // while waiting and re-acquire before returning.
    m->recursion = 0;
    MutexHandoffLocked(*m);
    bool ok;
    if (timeout_ms == INFINITE) {
        c->cv.wait(lk);
        ok = true;
    } else {
        ok = c->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms)) ==
             std::cv_status::no_timeout;
    }
    // Re-acquire the mutex, honouring the FIFO waiter queue.
    if (m->owner == 0 && m->waiters.empty()) {
        MutexAcquireLocked(*m, tid);
    } else if (m->owner != tid) {
        m->waiters.push_back(tid);
        m->cv.wait(lk, [&] { return m->owner == tid; });
    }
    return ok ? 0 : SCE_KERNEL_ERROR_ETIMEDOUT;
}

u64 ScePthreadCondWait(const GuestArgs& args) {
    return CondWaitImpl(args.arg1, args.arg2, INFINITE);
}

u64 ScePthreadCondTimedwait(const GuestArgs& args) {
    const u64 usec = args.arg3;
    u64 ms = (usec + 999ULL) / 1000ULL;
    if (ms > 0xFFFFFFFEULL) ms = 0xFFFFFFFEULL;
    return CondWaitImpl(args.arg1, args.arg2, static_cast<DWORD>(ms));
}

u64 ScePthreadCondSignal(const GuestArgs& args) {
    GuestCond* c = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_cond_map_lock);
        c = LookupOrCreateCond(args.arg1, true);
    }
    if (!c) return SCE_KERNEL_ERROR_EINVAL;
    c->cv.notify_one();
    return 0;
}

u64 ScePthreadCondBroadcast(const GuestArgs& args) {
    GuestCond* c = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_cond_map_lock);
        c = LookupOrCreateCond(args.arg1, true);
    }
    if (!c) return SCE_KERNEL_ERROR_EINVAL;
    c->cv.notify_all();
    return 0;
}

u64 ScePthreadCondDestroy(const GuestArgs& args) {
    const guest_addr_t var = args.arg1;
    if (!var) return SCE_KERNEL_ERROR_EINVAL;
    u64 token = 0;
    std::lock_guard<std::mutex> lk(g_cond_map_lock);
    if (SafeReadU64(var, token) && (token & kTokenTagMask) == kCondTokenTag) {
        g_conds.erase(token);
    }
    SafeWriteU64(var, 0);
    return 0;
}

// ===========================================================================
// pthread read-write lock handlers
// ===========================================================================
u64 ScePthreadRwlockInit(const GuestArgs& args) {
    const guest_addr_t var = args.arg1;
    (void)args.arg2; // attr ignored
    if (!var) return SCE_KERNEL_ERROR_EINVAL;
    std::lock_guard<std::mutex> lk(g_rwlock_map_lock);
    const u64 token = kRwlockTokenTag | g_next_rwlock_token++;
    g_rwlocks[token] = std::make_unique<GuestRwlock>();
    SafeWriteU64(var, token);
    LOG_DEBUG(HLE, "scePthreadRwlockInit(0x%llx) -> token 0x%llx", var, token);
    return 0;
}

u64 ScePthreadRwlockRdlock(const GuestArgs& args) {
    const guest_addr_t var = args.arg1;
    if (!var) return SCE_KERNEL_ERROR_EINVAL;
    GuestRwlock* rw = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_rwlock_map_lock);
        rw = LookupOrCreateRwlock(var, true);
    }
    if (!rw) return SCE_KERNEL_ERROR_EINVAL;
    AcquireSRWLockShared(&rw->srw);
    rw->readers.fetch_add(1);
    return 0;
}

u64 ScePthreadRwlockWrlock(const GuestArgs& args) {
    const guest_addr_t var = args.arg1;
    if (!var) return SCE_KERNEL_ERROR_EINVAL;
    GuestRwlock* rw = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_rwlock_map_lock);
        rw = LookupOrCreateRwlock(var, true);
    }
    if (!rw) return SCE_KERNEL_ERROR_EINVAL;
    AcquireSRWLockExclusive(&rw->srw);
    rw->writer.store(true);
    return 0;
}

u64 ScePthreadRwlockUnlock(const GuestArgs& args) {
    const guest_addr_t var = args.arg1;
    if (!var) return SCE_KERNEL_ERROR_EINVAL;
    GuestRwlock* rw = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_rwlock_map_lock);
        rw = LookupOrCreateRwlock(var, false);
    }
    if (!rw) {
        LOG_WARN(HLE, "scePthreadRwlockUnlock(0x%llx): unknown rwlock", var);
        return SCE_KERNEL_ERROR_EINVAL;
    }
    if (rw->writer.exchange(false)) {
        ReleaseSRWLockExclusive(&rw->srw);
    } else if (rw->readers.load() > 0) {
        rw->readers.fetch_sub(1);
        ReleaseSRWLockShared(&rw->srw);
    } else {
        LOG_WARN(HLE, "scePthreadRwlockUnlock(0x%llx): unlock of unlocked lock", var);
    }
    return 0;
}

u64 ScePthreadRwlockDestroy(const GuestArgs& args) {
    const guest_addr_t var = args.arg1;
    if (!var) return SCE_KERNEL_ERROR_EINVAL;
    u64 token = 0;
    std::lock_guard<std::mutex> lk(g_rwlock_map_lock);
    if (SafeReadU64(var, token) && (token & kTokenTagMask) == kRwlockTokenTag) {
        g_rwlocks.erase(token);
    }
    SafeWriteU64(var, 0);
    return 0;
}

// ===========================================================================
// pthread once
// ===========================================================================
u64 ScePthreadOnce(const GuestArgs& args) {
    const guest_addr_t once_ptr = args.arg1;
    const guest_addr_t init_fn  = args.arg2;
    if (!once_ptr || !init_fn) return SCE_KERNEL_ERROR_EINVAL;
    {
        std::lock_guard<std::mutex> lk(g_once_lock);
        if (g_once_done.count(once_ptr)) return 0;
        g_once_done.insert(once_ptr);
    }
    LOG_DEBUG(HLE, "scePthreadOnce(0x%llx): invoking init 0x%llx", once_ptr, init_fn);
    InvokeGuestFunction(init_fn, 0, 0, 0);
    SafeWriteU64(once_ptr, 1);
    return 0;
}

// ===========================================================================
// pthread TLS keys
// ===========================================================================
u64 ScePthreadKeyCreate(const GuestArgs& args) {
    const guest_addr_t key_ptr = args.arg1;
    const u64 destructor       = args.arg2;
    if (!key_ptr) return SCE_KERNEL_ERROR_EINVAL;
    std::lock_guard<std::mutex> lk(g_tls_lock);
    const u64 key = g_next_tls_key++;
    TlsKey tk;
    tk.destructor = destructor;
    g_tls_keys[key] = std::move(tk);
    SafeWriteU64(key_ptr, key);
    LOG_DEBUG(HLE, "scePthreadKeyCreate(dtor=0x%llx) -> key %llu", destructor, key);
    return 0;
}

u64 ScePthreadKeyDelete(const GuestArgs& args) {
    std::lock_guard<std::mutex> lk(g_tls_lock);
    g_tls_keys.erase(args.arg1);
    return 0;
}

u64 ScePthreadGetspecific(const GuestArgs& args) {
    const u64 tid = CurrentTlsIdentity();
    std::lock_guard<std::mutex> lk(g_tls_lock);
    auto it = g_tls_keys.find(args.arg1);
    if (it == g_tls_keys.end()) return 0;
    auto vit = it->second.values.find(tid);
    return (vit != it->second.values.end()) ? vit->second : 0;
}

u64 ScePthreadSetspecific(const GuestArgs& args) {
    const u64 tid = CurrentTlsIdentity();
    std::lock_guard<std::mutex> lk(g_tls_lock);
    auto it = g_tls_keys.find(args.arg1);
    if (it == g_tls_keys.end()) {
        LOG_WARN(HLE, "scePthreadSetspecific: unknown key %llu", args.arg1);
        return SCE_KERNEL_ERROR_EINVAL;
    }
    it->second.values[tid] = args.arg2;
    return 0;
}

// ===========================================================================
// sceKernel* mutex objects
// ===========================================================================
u64 SceKernelCreateMutex(const GuestArgs& args) {
    const std::string name = ReadGuestName(args.arg1);
    (void)args.arg2; // attr
    (void)args.arg3; // opt
    const u32 handle = g_next_kmutex.fetch_add(1);
    auto obj = std::make_shared<GuestMutex>();
    obj->type = MutexType::Recursive; // count-based lock/unlock is recursive
    std::lock_guard<std::mutex> lk(g_kmutex_lock);
    g_kmutexes[handle] = std::move(obj);
    LOG_DEBUG(HLE, "sceKernelCreateMutex('%s') -> 0x%X", name.c_str(), handle);
    return handle;
}

u64 SceKernelLockMutex(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    s32 count = static_cast<s32>(args.arg2);
    const guest_addr_t timo_ptr = args.arg3;
    if (count <= 0) count = 1;
    std::shared_ptr<GuestMutex> m;
    {
        std::lock_guard<std::mutex> lk(g_kmutex_lock);
        auto it = g_kmutexes.find(handle);
        if (it != g_kmutexes.end()) m = it->second;
    }
    if (!m) return SCE_KERNEL_ERROR_EINVAL;
    const u64 tid = CurrentTlsIdentity();

    u32 usec = 0;
    const int timo = ReadTimeout(timo_ptr, usec);
    if (timo == 0) {
        for (s32 i = 0; i < count; ++i) MutexLockImpl(*m, tid, /*try_only=*/false);
        return 0;
    }
    const u64 start = Kernel::GuestClockMicros();
    for (s32 i = 0; i < count; ++i) {
        while (MutexLockImpl(*m, tid, /*try_only=*/true) == SCE_KERNEL_ERROR_EBUSY) {
            if (timo == 1 || Kernel::GuestClockMicros() - start >= usec) {
                // Back out any partial acquisitions.
                for (s32 j = 0; j < i; ++j) MutexUnlockImpl(*m, tid, true);
                return SCE_KERNEL_ERROR_ETIMEDOUT;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return 0;
}

u64 SceKernelUnlockMutex(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    s32 count = static_cast<s32>(args.arg2);
    if (count <= 0) count = 1;
    std::shared_ptr<GuestMutex> m;
    {
        std::lock_guard<std::mutex> lk(g_kmutex_lock);
        auto it = g_kmutexes.find(handle);
        if (it != g_kmutexes.end()) m = it->second;
    }
    if (!m) return SCE_KERNEL_ERROR_EINVAL;
    const u64 tid = CurrentTlsIdentity();
    for (s32 i = 0; i < count; ++i) MutexUnlockImpl(*m, tid, true);
    return 0;
}

u64 SceKernelDeleteMutex(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    std::lock_guard<std::mutex> lk(g_kmutex_lock);
    g_kmutexes.erase(handle);
    return 0;
}

// ===========================================================================
// sceKernel* counting semaphores
// ===========================================================================
u64 SceKernelCreateSema(const GuestArgs& args) {
    const guest_addr_t out_ptr = args.arg1;
    const std::string name     = ReadGuestName(args.arg2);
    (void)args.arg3; // attr
    const s32 init_count = static_cast<s32>(args.arg4);
    const s32 max_count  = static_cast<s32>(args.arg5);
    (void)args.arg6; // opt
    if (!out_ptr || init_count < 0 || max_count <= 0 || init_count > max_count) {
        LOG_WARN(HLE, "sceKernelCreateSema('%s'): bad args (init=%d max=%d out=0x%llx)",
                 name.c_str(), init_count, max_count, out_ptr);
        return SCE_KERNEL_ERROR_EINVAL;
    }
    auto sema = std::make_shared<GuestSema>();
    sema->count     = init_count;
    sema->max_count = max_count;
    const u32 handle = g_next_sema.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(g_sema_lock);
        g_semas[handle] = sema;
    }
    SafeWriteU64(out_ptr, handle);
    LOG_DEBUG(HLE, "sceKernelCreateSema('%s', init=%d, max=%d) -> 0x%X",
              name.c_str(), init_count, max_count, handle);
    return 0;
}

static std::shared_ptr<GuestSema> FindSema(u32 handle) {
    std::lock_guard<std::mutex> lk(g_sema_lock);
    auto it = g_semas.find(handle);
    return (it != g_semas.end()) ? it->second : nullptr;
}

static u64 WaitSemaImpl(u32 handle, s32 need, u32 usec, int timo_kind) {
    if (need <= 0) need = 1;
    auto sema = FindSema(handle);
    if (!sema) return SCE_KERNEL_ERROR_EINVAL;
    std::unique_lock<std::mutex> lk(sema->m);
    auto satisfied = [&] { return sema->count >= need; };
    bool ok;
    if (timo_kind == 0) {
        sema->cv.wait(lk, satisfied);
        ok = true;
    } else if (timo_kind == 1) {
        ok = satisfied();
    } else {
        ok = sema->cv.wait_for(lk, std::chrono::microseconds(usec), satisfied);
    }
    if (!ok) return SCE_KERNEL_ERROR_ETIMEDOUT;
    sema->count -= need;
    return 0;
}

u64 SceKernelWaitSema(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    const s32 need   = static_cast<s32>(args.arg2);
    u32 usec = 0;
    const int timo = ReadTimeout(args.arg3, usec);
    return WaitSemaImpl(handle, need, usec, timo);
}

u64 SceKernelPollSema(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    const s32 need   = static_cast<s32>(args.arg2);
    return WaitSemaImpl(handle, need, 0, 1);
}

u64 SceKernelSignalSema(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    s32 signal = static_cast<s32>(args.arg2);
    if (signal <= 0) signal = 1;
    auto sema = FindSema(handle);
    if (!sema) return SCE_KERNEL_ERROR_EINVAL;
    {
        std::lock_guard<std::mutex> lk(sema->m);
        const s64 next = static_cast<s64>(sema->count) + signal;
        sema->count = (next > sema->max_count) ? sema->max_count
                                               : static_cast<s32>(next);
    }
    sema->cv.notify_all();
    return 0;
}

u64 SceKernelDeleteSema(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    std::lock_guard<std::mutex> lk(g_sema_lock);
    g_semas.erase(handle);
    return 0;
}

// ===========================================================================
// sceKernel* event flags
// ===========================================================================
u64 SceKernelCreateEventFlag(const GuestArgs& args) {
    const guest_addr_t out_ptr = args.arg1;
    const std::string name     = ReadGuestName(args.arg2);
    (void)args.arg3; // attr
    const u64 init_pattern = args.arg4;
    (void)args.arg5; // opt
    if (!out_ptr) return SCE_KERNEL_ERROR_EINVAL;
    auto ef = std::make_shared<GuestEventFlag>();
    ef->bits = init_pattern;
    const u32 handle = g_next_eflag.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(g_eflag_lock);
        g_eflags[handle] = ef;
    }
    SafeWriteU64(out_ptr, handle);
    LOG_DEBUG(HLE, "sceKernelCreateEventFlag('%s', init=0x%llx) -> 0x%X",
              name.c_str(), init_pattern, handle);
    return 0;
}

static std::shared_ptr<GuestEventFlag> FindEventFlag(u32 handle) {
    std::lock_guard<std::mutex> lk(g_eflag_lock);
    auto it = g_eflags.find(handle);
    return (it != g_eflags.end()) ? it->second : nullptr;
}

u64 SceKernelSetEventFlag(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    auto ef = FindEventFlag(handle);
    if (!ef) return SCE_KERNEL_ERROR_EINVAL;
    {
        std::lock_guard<std::mutex> lk(ef->m);
        ef->bits |= args.arg2;
    }
    ef->cv.notify_all();
    return 0;
}

u64 SceKernelClearEventFlag(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    auto ef = FindEventFlag(handle);
    if (!ef) return SCE_KERNEL_ERROR_EINVAL;
    std::lock_guard<std::mutex> lk(ef->m);
    ef->bits &= ~args.arg2;
    return 0;
}

static u64 WaitEventFlagImpl(u32 handle, u64 pattern, u32 mode,
                             guest_addr_t result_ptr, u32 usec, int timo_kind) {
    auto ef = FindEventFlag(handle);
    if (!ef) return SCE_KERNEL_ERROR_EINVAL;
    if (pattern == 0) return SCE_KERNEL_ERROR_EINVAL;
    std::unique_lock<std::mutex> lk(ef->m);
    auto satisfied = [&] {
        return (mode & kEvfWaitOr) ? (ef->bits & pattern) != 0
                                   : (ef->bits & pattern) == pattern;
    };
    bool ok;
    if (timo_kind == 0) {
        ef->cv.wait(lk, satisfied);
        ok = true;
    } else if (timo_kind == 1) {
        ok = satisfied();
    } else {
        ok = ef->cv.wait_for(lk, std::chrono::microseconds(usec), satisfied);
    }
    if (!ok) return SCE_KERNEL_ERROR_ETIMEDOUT;
    if (result_ptr) SafeWriteU64(result_ptr, ef->bits);
    if (mode & kEvfClearPattern) {
        ef->bits &= ~pattern;
    } else if (mode & kEvfClearAll) {
        ef->bits = 0;
    }
    return 0;
}

u64 SceKernelWaitEventFlag(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    u32 usec = 0;
    const int timo = ReadTimeout(args.arg5, usec);
    return WaitEventFlagImpl(handle, args.arg2, static_cast<u32>(args.arg3),
                             args.arg4, usec, timo);
}

u64 SceKernelPollEventFlag(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    return WaitEventFlagImpl(handle, args.arg2, static_cast<u32>(args.arg3),
                             args.arg4, 0, 1);
}

u64 SceKernelDeleteEventFlag(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    std::lock_guard<std::mutex> lk(g_eflag_lock);
    g_eflags.erase(handle);
    return 0;
}

// ===========================================================================
// sceKernel* event queues
// ===========================================================================
u64 SceKernelCreateEqueue(const GuestArgs& args) {
    const guest_addr_t out_ptr = args.arg1;
    const std::string name     = ReadGuestName(args.arg2);
    if (!out_ptr) return SCE_KERNEL_ERROR_EINVAL;
    const u32 handle = g_next_equeue.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(g_equeue_lock);
        g_equeues[handle] = std::make_shared<GuestEqueue>();
    }
    SafeWriteU64(out_ptr, handle);
    LOG_INFO(HLE, "sceKernelCreateEqueue('%s') -> 0x%X", name.c_str(), handle);
    return 0;
}

static std::shared_ptr<GuestEqueue> FindEqueue(u32 handle) {
    std::lock_guard<std::mutex> lk(g_equeue_lock);
    auto it = g_equeues.find(handle);
    return (it != g_equeues.end()) ? it->second : nullptr;
}

u64 SceKernelWaitEqueue(const GuestArgs& args) {
    const u32 handle            = static_cast<u32>(args.arg1);
    const guest_addr_t ev_ptr   = args.arg2;
    const int capacity          = static_cast<int>(args.arg3);
    const guest_addr_t out_ptr  = args.arg4;
    const guest_addr_t timo_ptr = args.arg5;

    auto eq = FindEqueue(handle);
    if (!eq) {
        LOG_WARN(HLE, "sceKernelWaitEqueue: bad handle 0x%X", handle);
        return SCE_KERNEL_ERROR_EINVAL;
    }
    if (!ev_ptr || capacity < 1) return SCE_KERNEL_ERROR_EINVAL;

    u32 usec = 0;
    const int timo = ReadTimeout(timo_ptr, usec);

    std::unique_lock<std::mutex> lk(eq->m);
    auto has_pending = [&] {
        for (const auto& e : eq->events) if (e.pending) return true;
        return false;
    };

    if (!has_pending()) {
        if (timo == 0) {
            eq->cv.wait(lk, has_pending);
        } else if (timo == 1) {
            // poll — fall through with whatever is pending (nothing)
        } else {
            eq->cv.wait_for(lk, std::chrono::microseconds(usec), has_pending);
        }
    }

    const int delivered = DrainEqueueEvents(*eq, ev_ptr, capacity);
    if (out_ptr && !TryWriteS32(out_ptr, delivered)) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    if (delivered == 0 && timo != 0) {
        LOG_DEBUG(HLE, "sceKernelWaitEqueue(0x%X): timeout, 0 events", handle);
        return SCE_KERNEL_ERROR_ETIMEDOUT;
    }
    return 0;
}

u64 SceKernelAddUserEvent(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    const u64 ident  = args.arg2;
    auto eq = FindEqueue(handle);
    if (!eq) return SCE_KERNEL_ERROR_EINVAL;
    std::lock_guard<std::mutex> lk(eq->m);
    for (const auto& e : eq->events) {
        if (e.filter == EVFILT_USER && e.ident == ident) return 0; // already added
    }
    EqueueEvent ev;
    ev.ident  = ident;
    ev.filter = EVFILT_USER;
    ev.flags  = EV_ADD | EV_ENABLE;
    eq->events.push_back(ev);
    LOG_DEBUG(HLE, "sceKernelAddUserEvent(0x%X, ident=%llu)", handle, ident);
    return 0;
}

u64 SceKernelDeleteUserEvent(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    const u64 ident  = args.arg2;
    auto eq = FindEqueue(handle);
    if (!eq) return SCE_KERNEL_ERROR_EINVAL;
    std::lock_guard<std::mutex> lk(eq->m);
    auto& v = eq->events;
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->filter == EVFILT_USER && it->ident == ident) {
            v.erase(it);
            break;
        }
    }
    return 0;
}

u64 SceKernelTriggerUserEvent(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    const u64 ident  = args.arg2;
    const u64 udata  = args.arg3;
    auto eq = FindEqueue(handle);
    if (!eq) return SCE_KERNEL_ERROR_EINVAL;
    {
        std::lock_guard<std::mutex> lk(eq->m);
        EqueueEvent* target = nullptr;
        for (auto& e : eq->events) {
            if (e.filter == EVFILT_USER && e.ident == ident) {
                target = &e;
                break;
            }
        }
        if (!target) {
            // Forgiving path: Orbis requires AddUserEvent first, but auto-add
            // so a guest that skips the add cannot hang its waiter.
            LOG_WARN(HLE, "sceKernelTriggerUserEvent(0x%X, ident=%llu): not added; auto-adding",
                     handle, ident);
            EqueueEvent ev;
            ev.ident  = ident;
            ev.filter = EVFILT_USER;
            ev.flags  = EV_ADD | EV_ENABLE;
            eq->events.push_back(ev);
            target = &eq->events.back();
        }
        target->pending = true;
        target->fflags |= NOTE_TRIGGER;
        target->udata   = udata;
        target->data    = 1;
    }
    eq->cv.notify_all();
    return 0;
}

u64 SceKernelAddReadEvent(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    const s64 fd     = static_cast<s64>(args.arg2);
    const u64 ident  = args.arg3;
    const u64 udata  = args.arg4;
    auto eq = FindEqueue(handle);
    if (!eq) return SCE_KERNEL_ERROR_EINVAL;
    std::lock_guard<std::mutex> lk(eq->m);
    for (const auto& e : eq->events) {
        if (e.filter == EVFILT_READ && e.ident == ident) return 0;
    }
    EqueueEvent ev;
    ev.ident  = ident;
    ev.filter = EVFILT_READ;
    ev.flags  = EV_ADD | EV_ENABLE;
    ev.data   = fd;
    ev.udata  = udata;
    // Best-effort readiness: report the descriptor readable once (edge-style).
    // Without a real fd poll loop, level-reporting would spin the guest;
    // firing once lets engines make progress, and they re-add if needed.
    ev.pending = true;
    eq->events.push_back(ev);
    LOG_DEBUG(HLE, "sceKernelAddReadEvent(0x%X, fd=%lld, ident=%llu) -> armed once",
              handle, fd, ident);
    return 0;
}

u64 SceKernelGetKqueueFromEqueue(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    if (!FindEqueue(handle)) return SCE_KERNEL_ERROR_EINVAL;
    return handle;
}

u64 SceKernelGetEventData(const GuestArgs& args) {
    u64 data = 0;
    SafeReadU64(args.arg1 + 0x10, data);
    return data;
}

u64 SceKernelGetEventFflags(const GuestArgs& args) {
    u32 fflags = 0;
    SafeReadU32(args.arg1 + 0x0C, fflags);
    return fflags;
}

u64 SceKernelDeleteEqueue(const GuestArgs& args) {
    const u32 handle = static_cast<u32>(args.arg1);
    std::lock_guard<std::mutex> lk(g_equeue_lock);
    g_equeues.erase(handle);
    return 0;
}

// ===========================================================================
// Cross-module event posting (libSceVideoOut flip/vblank notifications)
// ===========================================================================
bool SceKernelEqueueExists(u32 handle) {
    return FindEqueue(handle) != nullptr;
}

bool SceKernelPostEvent(u32 handle, u64 ident, s16 filter, u64 udata, s64 data) {
    auto eq = FindEqueue(handle);
    if (!eq) return false;
    {
        std::lock_guard<std::mutex> lk(eq->m);
        EqueueEvent* target = nullptr;
        for (auto& e : eq->events) {
            if (e.filter == filter && e.ident == ident) {
                target = &e;
                break;
            }
        }
        if (!target) {
            EqueueEvent ev;
            ev.ident  = ident;
            ev.filter = filter;
            ev.flags  = EV_ADD | EV_ENABLE;
            eq->events.push_back(ev);
            target = &eq->events.back();
        }
        target->pending = true;
        target->udata   = udata;
        target->data    = data;
    }
    eq->cv.notify_all();
    return true;
}

// ===========================================================================
// Process clock (shared QPC origin — src/kernel/guest_clock.cpp)
// ===========================================================================
u64 SceKernelGetProcessTime(const GuestArgs& /*args*/) {
    return Kernel::GuestClockMicros();
}

u64 SceKernelGetProcessTimeCounter(const GuestArgs& /*args*/) {
    return Kernel::GuestClockCounter();
}

u64 SceKernelGetProcessTimeCounterFrequency(const GuestArgs& /*args*/) {
    return Kernel::GuestClockCounterFrequency();
}

// ===========================================================================
// Registration
// ===========================================================================
void RegisterLibKernelSync() {
    LOG_INFO(HLE, "Registering libkernel sync/equeue/clock HLE symbols...");

    // pthread mutex
    RegisterSymbol("libkernel", "scePthreadMutexInit", ScePthreadMutexInit);
    RegisterSymbol("libkernel", "scePthreadMutexLock", ScePthreadMutexLock);
    RegisterSymbol("libkernel", "scePthreadMutexTrylock", ScePthreadMutexTrylock);
    RegisterSymbol("libkernel", "scePthreadMutexUnlock", ScePthreadMutexUnlock);
    RegisterSymbol("libkernel", "scePthreadMutexDestroy", ScePthreadMutexDestroy);
    // mutex attr (type recorded per guest attr address, read by MutexInit)
    RegisterSymbol("libkernel", "scePthreadMutexattrInit", ScePthreadMutexattrInit);
    RegisterSymbol("libkernel", "scePthreadMutexattrDestroy", ScePthreadMutexattrDestroy);
    RegisterSymbol("libkernel", "scePthreadMutexattrSettype", ScePthreadMutexattrSettype);
    RegisterSymbol("libkernel", "scePthreadMutexattrSetprotocol", [](const GuestArgs&) -> u64 { return 0; });

    // pthread condition variable
    RegisterSymbol("libkernel", "scePthreadCondInit", ScePthreadCondInit);
    RegisterSymbol("libkernel", "scePthreadCondWait", ScePthreadCondWait);
    RegisterSymbol("libkernel", "scePthreadCondTimedwait", ScePthreadCondTimedwait);
    RegisterSymbol("libkernel", "scePthreadCondSignal", ScePthreadCondSignal);
    RegisterSymbol("libkernel", "scePthreadCondBroadcast", ScePthreadCondBroadcast);
    RegisterSymbol("libkernel", "scePthreadCondDestroy", ScePthreadCondDestroy);
    RegisterSymbol("libkernel", "scePthreadCondattrInit", [](const GuestArgs& a) -> u64 {
        if (a.arg1) SafeWriteU64(a.arg1, 0);
        return 0;
    });
    RegisterSymbol("libkernel", "scePthreadCondattrDestroy", [](const GuestArgs&) -> u64 { return 0; });

    // pthread read-write lock
    RegisterSymbol("libkernel", "scePthreadRwlockInit", ScePthreadRwlockInit);
    RegisterSymbol("libkernel", "scePthreadRwlockRdlock", ScePthreadRwlockRdlock);
    RegisterSymbol("libkernel", "scePthreadRwlockWrlock", ScePthreadRwlockWrlock);
    RegisterSymbol("libkernel", "scePthreadRwlockUnlock", ScePthreadRwlockUnlock);
    RegisterSymbol("libkernel", "scePthreadRwlockDestroy", ScePthreadRwlockDestroy);
    RegisterSymbol("libkernel", "scePthreadRwlockattrInit", [](const GuestArgs& a) -> u64 {
        if (a.arg1) SafeWriteU64(a.arg1, 0);
        return 0;
    });
    RegisterSymbol("libkernel", "scePthreadRwlockattrDestroy", [](const GuestArgs&) -> u64 { return 0; });

    // pthread once + TLS keys
    RegisterSymbol("libkernel", "scePthreadOnce", ScePthreadOnce);
    RegisterSymbol("libkernel", "scePthreadKeyCreate", ScePthreadKeyCreate);
    RegisterSymbol("libkernel", "scePthreadKeyDelete", ScePthreadKeyDelete);
    RegisterSymbol("libkernel", "scePthreadGetspecific", ScePthreadGetspecific);
    RegisterSymbol("libkernel", "scePthreadSetspecific", ScePthreadSetspecific);

    // sceKernel* mutex objects
    RegisterSymbol("libkernel", "sceKernelCreateMutex", SceKernelCreateMutex);
    RegisterSymbol("libkernel", "sceKernelLockMutex", SceKernelLockMutex);
    RegisterSymbol("libkernel", "sceKernelUnlockMutex", SceKernelUnlockMutex);
    RegisterSymbol("libkernel", "sceKernelDeleteMutex", SceKernelDeleteMutex);

    // sceKernel* counting semaphores
    RegisterSymbol("libkernel", "sceKernelCreateSema", SceKernelCreateSema);
    RegisterSymbol("libkernel", "sceKernelWaitSema", SceKernelWaitSema);
    RegisterSymbol("libkernel", "sceKernelPollSema", SceKernelPollSema);
    RegisterSymbol("libkernel", "sceKernelSignalSema", SceKernelSignalSema);
    RegisterSymbol("libkernel", "sceKernelDeleteSema", SceKernelDeleteSema);

    // sceKernel* event flags
    RegisterSymbol("libkernel", "sceKernelCreateEventFlag", SceKernelCreateEventFlag);
    RegisterSymbol("libkernel", "sceKernelSetEventFlag", SceKernelSetEventFlag);
    RegisterSymbol("libkernel", "sceKernelClearEventFlag", SceKernelClearEventFlag);
    RegisterSymbol("libkernel", "sceKernelWaitEventFlag", SceKernelWaitEventFlag);
    RegisterSymbol("libkernel", "sceKernelPollEventFlag", SceKernelPollEventFlag);
    RegisterSymbol("libkernel", "sceKernelDeleteEventFlag", SceKernelDeleteEventFlag);

    // sceKernel* event queues
    RegisterSymbol("libkernel", "sceKernelCreateEqueue", SceKernelCreateEqueue);
    RegisterSymbol("libkernel", "sceKernelWaitEqueue", SceKernelWaitEqueue);
    RegisterSymbol("libkernel", "sceKernelAddUserEvent", SceKernelAddUserEvent);
    RegisterSymbol("libkernel", "sceKernelAddUserEventEdge", SceKernelAddUserEvent);
    RegisterSymbol("libkernel", "sceKernelDeleteUserEvent", SceKernelDeleteUserEvent);
    RegisterSymbol("libkernel", "sceKernelTriggerUserEvent", SceKernelTriggerUserEvent);
    RegisterSymbol("libkernel", "sceKernelAddReadEvent", SceKernelAddReadEvent);
    RegisterSymbol("libkernel", "sceKernelGetKqueueFromEqueue", SceKernelGetKqueueFromEqueue);
    RegisterSymbol("libkernel", "sceKernelGetEventData", SceKernelGetEventData);
    RegisterSymbol("libkernel", "sceKernelGetEventFflags", SceKernelGetEventFflags);
    RegisterSymbol("libkernel", "sceKernelDeleteEqueue", SceKernelDeleteEqueue);

    // Process clock
    RegisterSymbol("libkernel", "sceKernelGetProcessTime", SceKernelGetProcessTime);
    RegisterSymbol("libkernel", "sceKernelGetProcessTimeCounter", SceKernelGetProcessTimeCounter);
    RegisterSymbol("libkernel", "sceKernelGetProcessTimeCounterFrequency", SceKernelGetProcessTimeCounterFrequency);
}

} // namespace HLE
