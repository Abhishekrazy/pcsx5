#pragma once
#include "hle.h"

namespace HLE {

// Registers the real (non-stub) libkernel synchronization, event-queue, and
// process-clock HLE implementations.  Called from HLE::Initialize() after
// RegisterLibKernel(); any legacy stub registration for the same symbol is
// replaced (RegisterSymbol is last-registration-win per key).
void RegisterLibKernelSync();

// ---------------------------------------------------------------------------
// Individual handlers, exported so unit tests can drive them directly with a
// GuestArgs struct (no thunk/dispatcher round-trip needed).
// ---------------------------------------------------------------------------

// pthread mutex (keyed by the guest mutex variable; token written to *mutex)
u64 ScePthreadMutexInit(const GuestArgs& args);
u64 ScePthreadMutexattrInit(const GuestArgs& args);
u64 ScePthreadMutexattrSettype(const GuestArgs& args);
u64 ScePthreadMutexattrDestroy(const GuestArgs& args);
u64 ScePthreadMutexLock(const GuestArgs& args);
u64 ScePthreadMutexTrylock(const GuestArgs& args);
u64 ScePthreadMutexUnlock(const GuestArgs& args);
u64 ScePthreadMutexDestroy(const GuestArgs& args);

// pthread condition variable (paired with a guest mutex)
u64 ScePthreadCondInit(const GuestArgs& args);
u64 ScePthreadCondWait(const GuestArgs& args);
u64 ScePthreadCondTimedwait(const GuestArgs& args);
u64 ScePthreadCondSignal(const GuestArgs& args);
u64 ScePthreadCondBroadcast(const GuestArgs& args);
u64 ScePthreadCondDestroy(const GuestArgs& args);

// pthread read-write lock (host SRWLOCK)
u64 ScePthreadRwlockInit(const GuestArgs& args);
u64 ScePthreadRwlockRdlock(const GuestArgs& args);
u64 ScePthreadRwlockWrlock(const GuestArgs& args);
u64 ScePthreadRwlockUnlock(const GuestArgs& args);
u64 ScePthreadRwlockDestroy(const GuestArgs& args);

// pthread once
u64 ScePthreadOnce(const GuestArgs& args);

// sceKernelSyncOnAddressWait / sceKernelSyncOnAddressWake (libKernel address-wait primitives)
u64 SceKernelSyncOnAddressWait(const GuestArgs& args);
u64 SceKernelSyncOnAddressWake(const GuestArgs& args);

// pthread TLS keys (per-thread values keyed by guest key id)
u64 ScePthreadKeyCreate(const GuestArgs& args);
u64 ScePthreadKeyDelete(const GuestArgs& args);
u64 ScePthreadGetspecific(const GuestArgs& args);
u64 ScePthreadSetspecific(const GuestArgs& args);

// sceKernel* mutex objects (handle-based)
u64 SceKernelCreateMutex(const GuestArgs& args);
u64 SceKernelLockMutex(const GuestArgs& args);
u64 SceKernelUnlockMutex(const GuestArgs& args);
u64 SceKernelDeleteMutex(const GuestArgs& args);

// sceKernel* counting semaphores (handle-based)
u64 SceKernelCreateSema(const GuestArgs& args);
u64 SceKernelWaitSema(const GuestArgs& args);
u64 SceKernelPollSema(const GuestArgs& args);
u64 SceKernelSignalSema(const GuestArgs& args);
u64 SceKernelDeleteSema(const GuestArgs& args);

// pthread POSIX semaphores (sem_t variables)
u64 ScePthreadSemInit(const GuestArgs& args);
u64 ScePthreadSemDestroy(const GuestArgs& args);
u64 ScePthreadSemWait(const GuestArgs& args);
u64 ScePthreadSemTrywait(const GuestArgs& args);
u64 ScePthreadSemPost(const GuestArgs& args);
u64 ScePthreadSemGetValue(const GuestArgs& args);

// sceKernel* event flags (handle-based bitmask, wait-any/all)
u64 SceKernelCreateEventFlag(const GuestArgs& args);
u64 SceKernelSetEventFlag(const GuestArgs& args);
u64 SceKernelClearEventFlag(const GuestArgs& args);
u64 SceKernelWaitEventFlag(const GuestArgs& args);
u64 SceKernelPollEventFlag(const GuestArgs& args);
u64 SceKernelDeleteEventFlag(const GuestArgs& args);

// sceKernel* event queues (handle-based; user events + minimal read events)
u64 SceKernelCreateEqueue(const GuestArgs& args);
u64 SceKernelWaitEqueue(const GuestArgs& args);
u64 SceKernelAddUserEvent(const GuestArgs& args);
u64 SceKernelDeleteUserEvent(const GuestArgs& args);
u64 SceKernelTriggerUserEvent(const GuestArgs& args);
u64 SceKernelAddReadEvent(const GuestArgs& args);
u64 SceKernelGetKqueueFromEqueue(const GuestArgs& args);
u64 SceKernelGetEventData(const GuestArgs& args);
u64 SceKernelGetEventFflags(const GuestArgs& args);
u64 SceKernelDeleteEqueue(const GuestArgs& args);

// Cross-module equeue access for other HLE libraries (libSceVideoOut posts
// its flip/vblank events with filter EVFILT_VIDEO_OUT = -13 through these).
// SceKernelEqueueExists reports whether `handle` is a live equeue.
// SceKernelPostEvent adds-or-updates a pending event with an arbitrary filter
// and wakes any guest thread blocked in sceKernelWaitEqueue.
bool SceKernelEqueueExists(u32 handle);
bool SceKernelPostEvent(u32 handle, u64 ident, s16 filter, u64 udata, s64 data);

// Process clock (shared QPC origin — see src/kernel/guest_clock.h)
u64 SceKernelGetProcessTime(const GuestArgs& args);
u64 SceKernelGetProcessTimeCounter(const GuestArgs& args);
u64 SceKernelGetProcessTimeCounterFrequency(const GuestArgs& args);

} // namespace HLE
