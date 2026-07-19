// Phase 2 kernel sync tests — drives the real HLE implementations in
// src/hle/libkernel_sync.cpp directly with GuestArgs structs (no thunk
// round-trip), plus the shared guest clock in src/kernel/guest_clock.cpp.
#include "hle/libkernel_sync.h"
#include "kernel/guest_clock.h"
#include "kernel/kernel.h"
#include "memory/memory.h"
#include "common/log.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

// The Release build defines NDEBUG, which would compile every assert() away
// and silently neuter this suite.  Redefine assert to an always-on check.
#undef assert
#define assert(cond)                                                            \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::fflush(stdout);                                                \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

using namespace HLE;

namespace {

constexpr u64 SCE_KERNEL_ERROR_ETIMEDOUT = 0x8002003C;
constexpr u64 SCE_KERNEL_ERROR_EBUSY     = 0x80020010;

guest_addr_t g_page = 0; // 64 KiB scratch page mapped in main()

HLE::GuestArgs Args(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0, u64 a5 = 0, u64 a6 = 0) {
    HLE::GuestArgs a;
    a.arg1 = a1; a.arg2 = a2; a.arg3 = a3;
    a.arg4 = a4; a.arg5 = a5; a.arg6 = a6;
    return a;
}

void TestMutex() {
    const guest_addr_t mtx = g_page + 0x100;
    assert(ScePthreadMutexInit(Args(mtx, 0)) == 0);
    assert(ScePthreadMutexLock(Args(mtx)) == 0);

    // A second (host) thread must see the mutex held.
    std::atomic<u64> worker_try{0};
    std::atomic<u64> worker_lock{0};
    std::thread t([&] {
        worker_try = ScePthreadMutexTrylock(Args(mtx));
        worker_lock = ScePthreadMutexLock(Args(mtx));
        ScePthreadMutexUnlock(Args(mtx));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(worker_try == SCE_KERNEL_ERROR_EBUSY); // contended trylock fails
    assert(ScePthreadMutexUnlock(Args(mtx)) == 0);
    t.join();
    assert(worker_lock == 0); // blocking lock succeeded after unlock

    // Lazy init: an untouched guest word behaves like a static initializer.
    const guest_addr_t lazy = g_page + 0x108;
    Memory::Write<u64>(lazy, 0);
    assert(ScePthreadMutexLock(Args(lazy)) == 0);
    assert(ScePthreadMutexUnlock(Args(lazy)) == 0);

    assert(ScePthreadMutexDestroy(Args(mtx)) == 0);
    std::printf("  mutex: OK\n");
}

void TestCondvar() {
    const guest_addr_t mtx  = g_page + 0x200;
    const guest_addr_t cond = g_page + 0x208;
    assert(ScePthreadMutexInit(Args(mtx, 0)) == 0);
    assert(ScePthreadCondInit(Args(cond, 0)) == 0);

    // signal/wake across two threads
    std::atomic<bool> woke{false};
    std::thread t([&] {
        assert(ScePthreadMutexLock(Args(mtx)) == 0);
        const u64 r = ScePthreadCondWait(Args(cond, mtx));
        assert(r == 0);
        woke = true;
        ScePthreadMutexUnlock(Args(mtx));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(ScePthreadCondSignal(Args(cond)) == 0);
    t.join();
    assert(woke);

    // timedwait times out when nobody signals (10 ms)
    const guest_addr_t mtx2  = g_page + 0x210;
    const guest_addr_t cond2 = g_page + 0x218;
    assert(ScePthreadMutexInit(Args(mtx2, 0)) == 0);
    assert(ScePthreadCondInit(Args(cond2, 0)) == 0);
    assert(ScePthreadMutexLock(Args(mtx2)) == 0);
    const auto t0 = std::chrono::steady_clock::now();
    const u64 r = ScePthreadCondTimedwait(Args(cond2, mtx2, 10000));
    const auto t1 = std::chrono::steady_clock::now();
    ScePthreadMutexUnlock(Args(mtx2));
    assert(r == SCE_KERNEL_ERROR_ETIMEDOUT);
    assert(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() >= 5);

    assert(ScePthreadCondDestroy(Args(cond)) == 0);
    assert(ScePthreadCondDestroy(Args(cond2)) == 0);
    std::printf("  condvar: OK\n");
}

void TestRwlock() {
    const guest_addr_t rw = g_page + 0x300;
    assert(ScePthreadRwlockInit(Args(rw, 0)) == 0);
    assert(ScePthreadRwlockRdlock(Args(rw)) == 0);
    assert(ScePthreadRwlockUnlock(Args(rw)) == 0);
    assert(ScePthreadRwlockWrlock(Args(rw)) == 0);

    // writer held: a reader thread must block until the writer unlocks
    std::atomic<bool> reader_done{false};
    std::thread t([&] {
        assert(ScePthreadRwlockRdlock(Args(rw)) == 0);
        ScePthreadRwlockUnlock(Args(rw));
        reader_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(!reader_done);
    assert(ScePthreadRwlockUnlock(Args(rw)) == 0);
    t.join();
    assert(reader_done);

    assert(ScePthreadRwlockDestroy(Args(rw)) == 0);
    std::printf("  rwlock: OK\n");
}

void TestTlsKeys() {
    const guest_addr_t key_ptr = g_page + 0x400;
    Memory::Write<u64>(key_ptr, 0);
    assert(ScePthreadKeyCreate(Args(key_ptr, 0)) == 0);
    const u64 key = Memory::Read<u64>(key_ptr);
    assert(key != 0);

    assert(ScePthreadGetspecific(Args(key)) == 0); // unset -> NULL
    assert(ScePthreadSetspecific(Args(key, 0xDEADBEEF)) == 0);
    assert(ScePthreadGetspecific(Args(key)) == 0xDEADBEEF);

    // values are per-thread: another thread sees NULL for the same key
    u64 other_val = 0xFF;
    std::thread t([&] { other_val = ScePthreadGetspecific(Args(key)); });
    t.join();
    assert(other_val == 0);

    assert(ScePthreadKeyDelete(Args(key)) == 0);
    assert(ScePthreadGetspecific(Args(key)) == 0); // deleted -> NULL
    std::printf("  tls keys: OK\n");
}

void TestSemaphore() {
    const guest_addr_t out = g_page + 0x500;
    // create(name=NULL, attr=0, init=1, max=2)
    assert(SceKernelCreateSema(Args(out, 0, 0, 1, 2)) == 0);
    const u64 sem = Memory::Read<u64>(out);
    assert(sem != 0);

    assert(SceKernelWaitSema(Args(sem, 1, 0)) == 0);              // count 1 -> 0
    assert(SceKernelPollSema(Args(sem, 1)) == SCE_KERNEL_ERROR_ETIMEDOUT); // empty
    assert(SceKernelSignalSema(Args(sem, 1)) == 0);               // count 0 -> 1
    assert(SceKernelPollSema(Args(sem, 1)) == 0);                 // count 1 -> 0
    assert(SceKernelSignalSema(Args(sem, 5)) == 0);               // clamps to max=2

    // blocked waiter acquires both tokens (count is already 2 after clamp)
    std::atomic<u64> wait_result{~0ULL};
    std::thread t([&] { wait_result = SceKernelWaitSema(Args(sem, 2, 0)); });
    t.join();
    assert(wait_result == 0);

    // timed wait expires (50 ms timeout via SceKernelUseconds*)
    const guest_addr_t timo = g_page + 0x508;
    Memory::Write<u32>(timo, 50000);
    const u64 r = SceKernelWaitSema(Args(sem, 1, timo));
    assert(r == SCE_KERNEL_ERROR_ETIMEDOUT);

    assert(SceKernelDeleteSema(Args(sem)) == 0);
    std::printf("  semaphore: OK\n");
}

void TestEventFlag() {
    const guest_addr_t out = g_page + 0x600;
    // create(name=NULL, attr=0, init_pattern=0, opt=0)
    assert(SceKernelCreateEventFlag(Args(out, 0, 0, 0, 0)) == 0);
    const u64 ef = Memory::Read<u64>(out);
    assert(ef != 0);

    const guest_addr_t result = g_page + 0x608;
    assert(SceKernelPollEventFlag(Args(ef, 0x3, 0x01 /*AND*/, result)) == SCE_KERNEL_ERROR_ETIMEDOUT);

    assert(SceKernelSetEventFlag(Args(ef, 0x1)) == 0);
    // AND of 0x3 not satisfied yet
    assert(SceKernelPollEventFlag(Args(ef, 0x3, 0x01, result)) == SCE_KERNEL_ERROR_ETIMEDOUT);
    // OR of 0x3 satisfied
    assert(SceKernelPollEventFlag(Args(ef, 0x3, 0x02 /*OR*/, result)) == 0);
    assert((Memory::Read<u64>(result) & 0x1) != 0);

    assert(SceKernelSetEventFlag(Args(ef, 0x2)) == 0);
    // AND satisfied with clear-on-wait
    assert(SceKernelWaitEventFlag(Args(ef, 0x3, 0x01 | 0x10 /*CLEAR_PAT*/, result, 0)) == 0);
    // pattern bits were cleared
    assert(SceKernelPollEventFlag(Args(ef, 0x3, 0x01, result)) == SCE_KERNEL_ERROR_ETIMEDOUT);

    assert(SceKernelClearEventFlag(Args(ef, 0xFFFFFFFFFFFFFFFFULL)) == 0);
    assert(SceKernelDeleteEventFlag(Args(ef)) == 0);
    std::printf("  event flag: OK\n");
}

void TestEqueue() {
    const guest_addr_t out = g_page + 0x700;
    assert(SceKernelCreateEqueue(Args(out, 0)) == 0);
    const u64 eq = Memory::Read<u64>(out);
    assert(eq != 0);

    const guest_addr_t events = g_page + 0x800; // 32-byte kevent slots
    const guest_addr_t nout   = g_page + 0x780;
    const guest_addr_t timo   = g_page + 0x788;

    // poll wait on empty queue -> ETIMEDOUT, out=0
    Memory::Write<u32>(timo, 1000); // 1 ms
    assert(SceKernelWaitEqueue(Args(eq, events, 4, nout, timo)) == SCE_KERNEL_ERROR_ETIMEDOUT);
    assert(Memory::Read<s32>(nout) == 0);

    // user event trigger -> delivered on next wait
    assert(SceKernelAddUserEvent(Args(eq, 0x1234)) == 0);
    assert(SceKernelTriggerUserEvent(Args(eq, 0x1234, 0xCAFE)) == 0);
    assert(SceKernelWaitEqueue(Args(eq, events, 4, nout, timo)) == 0);
    assert(Memory::Read<s32>(nout) == 1);
    assert(Memory::Read<u64>(events + 0x00) == 0x1234);            // ident
    assert(Memory::Read<s16>(events + 0x08) == -5);                // EVFILT_USER
    assert(Memory::Read<u64>(events + 0x18) == 0xCAFE);            // udata

    // blocking wait wakes when triggered from another thread
    std::atomic<u64> wait_result{~0ULL};
    std::thread t([&] {
        wait_result = SceKernelWaitEqueue(Args(eq, events, 4, nout, 0 /*infinite*/));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(SceKernelTriggerUserEvent(Args(eq, 0x1234, 0xBEEF)) == 0);
    t.join();
    assert(wait_result == 0);

    assert(SceKernelDeleteUserEvent(Args(eq, 0x1234)) == 0);
    assert(SceKernelGetKqueueFromEqueue(Args(eq)) == eq);
    assert(SceKernelDeleteEqueue(Args(eq)) == 0);
    std::printf("  equeue: OK\n");
}

void TestClock() {
    const u64 freq = SceKernelGetProcessTimeCounterFrequency(Args());
    assert(freq > 0);
    assert(freq == Kernel::GuestClockCounterFrequency());

    const u64 c0 = SceKernelGetProcessTimeCounter(Args());
    const u64 t0 = SceKernelGetProcessTime(Args());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const u64 c1 = SceKernelGetProcessTimeCounter(Args());
    const u64 t1 = SceKernelGetProcessTime(Args());

    assert(c1 > c0);                 // counter is monotonic
    assert(t1 > t0);                 // process time is monotonic
    assert(t1 - t0 >= 10000);        // >= 10 ms elapsed (20 ms sleep, slack)
    assert(t1 - t0 < 1000000);       // < 1 s — sane units (microseconds)

    // counter delta agrees with frequency within 1 s for a 20 ms sleep
    const u64 delta_us = (c1 - c0) * 1000000ULL / freq;
    assert(delta_us >= 10000 && delta_us < 1000000);
    std::printf("  clock: OK (freq=%llu Hz)\n", (unsigned long long)freq);
}

} // namespace

// Guest path translation (src/kernel/kernel.cpp): /app0, /savedata0 and
// relative-path mapping onto the configured host directories.
namespace {

void TestGuestPathTranslation() {
    Kernel::SetApp0Directory("C:/games/mytitle");
    Kernel::SetSaveDataDirectory("C:/emu/pcsx5_savedata/MYTITLE01");

    assert(Kernel::TranslateGuestPath("/app0") == "C:/games/mytitle");
    assert(Kernel::TranslateGuestPath("/app0/shaders/a.vert") ==
           "C:/games/mytitle/shaders/a.vert");
    assert(Kernel::TranslateGuestPath("/savedata0") ==
           "C:/emu/pcsx5_savedata/MYTITLE01");
    assert(Kernel::TranslateGuestPath("/savedata0/slot0/save.bin") ==
           "C:/emu/pcsx5_savedata/MYTITLE01/slot0/save.bin");
    // Guest CWD is the package root: relative opens resolve under /app0.
    assert(Kernel::TranslateGuestPath("data/file.dat") ==
           "C:/games/mytitle/data/file.dat");
    // Unmapped guest mounts and host-absolute paths pass through unchanged.
    assert(Kernel::TranslateGuestPath("/hostapp/x.bin") == "/hostapp/x.bin");
    assert(Kernel::TranslateGuestPath("D:/host/absolute.bin") ==
           "D:/host/absolute.bin");

    std::printf("  guest path translation: OK\n");
}

} // namespace

int main() {
    LogConfig::SetLevel(LogCategory::HLE, LogLevel::Warn);

    if (!Memory::Initialize()) {
        std::printf("Memory::Initialize failed\n");
        return 1;
    }
    if (Memory::Map(0, 0x10000, Memory::PROT_READ | Memory::PROT_WRITE, &g_page) != Memory::Status::Ok) {
        std::printf("Memory::Map failed\n");
        return 1;
    }

    std::printf("Running kernel sync tests...\n");
    TestMutex();
    TestCondvar();
    TestRwlock();
    TestTlsKeys();
    TestSemaphore();
    TestEventFlag();
    TestEqueue();
    TestClock();
    TestGuestPathTranslation();

    Memory::Shutdown();
    std::printf("All kernel sync tests passed!\n");
    return 0;
}
