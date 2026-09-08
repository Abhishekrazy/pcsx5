// TEST ONLY: abort-only link isolation for unused monolithic kernel/syscall
// dependencies. No exercised trap, crash-state, syscall-dispatch or exit path
// is replaced. The host-stack fixture excludes guest entry with a tripwire;
// the combined lifecycle target links real guest entry and assembly instead.
// The fixtures supply diagnostic history explicitly.
#ifndef PCSX5_KERNEL_TRAP_CHARACTERIZATION_ONLY
#error "Kernel dependency tripwires require the kernel trap characterization target"
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "kernel/fd_table.h"
#include "kernel/guest_clock.h"
#include "kernel/guest_execution.h"
#include "kernel/memory.h"
#include "kernel/thread.h"
#include "cpu/cpu.h"
#include "hle/hle.h"
#include "gpu/gpu.h"
#include "config/config.h"
#include "loader/module_graph.h"
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void Unexpected(const char* name) {
    std::fprintf(stderr, "FAIL kernel-trap characterization reached excluded dependency: %s\n", name);
    std::abort();
}
}

namespace HLE {
std::vector<TraceEntry> GetImportTrace(size_t) { Unexpected("HLE::GetImportTrace default diagnostic provider"); }
bool CommitPhysPool(guest_addr_t) { Unexpected("HLE::CommitPhysPool"); }
std::string GetEffectiveSaveDataDir() { Unexpected("HLE::GetEffectiveSaveDataDir"); }
bool HasRealImplementation(const std::string&) { Unexpected("HLE::HasRealImplementation"); }
guest_addr_t ResolveAny(const std::string&) { Unexpected("HLE::ResolveAny"); }
bool IsStrictImportMode() { Unexpected("HLE::IsStrictImportMode"); }
void SetGuestMainAddress(guest_addr_t) { Unexpected("HLE::SetGuestMainAddress"); }
void SetGuestEhFrameHdr(guest_addr_t, u64) { Unexpected("HLE::SetGuestEhFrameHdr"); }
void SetDtInitAddress(guest_addr_t) { Unexpected("HLE::SetDtInitAddress"); }
void QueuePrxInitAddress(const std::string&, guest_addr_t, guest_addr_t, guest_addr_t, u64, guest_addr_t, u64) {
    Unexpected("HLE::QueuePrxInitAddress");
}
std::vector<PrxInitRecord>& GetPrxInitQueue() { Unexpected("HLE::GetPrxInitQueue"); }
}
namespace GPU {
std::vector<std::string> GetBootTimeline() { Unexpected("GPU::GetBootTimeline"); }
void SetBootStatus(const char*, int, int) { Unexpected("GPU::SetBootStatus"); }
}
namespace ConfigService {
const std::string& Directory() { Unexpected("ConfigService::Directory"); }
}
namespace CpuCore {
u64 GetCurrentThreadId() { Unexpected("CpuCore::GetCurrentThreadId"); }
GuestThread* GetThreadById(u64) { Unexpected("CpuCore::GetThreadById"); }
std::vector<GuestThread*> GetAllThreads() { Unexpected("CpuCore::GetAllThreads"); }
}
namespace Loader {
bool Load(const std::string&, LoadedModule&) { Unexpected("Loader::Load"); }
void ModuleResolver::SetSearchDirectories(std::vector<std::filesystem::path>) {
    Unexpected("ModuleResolver::SetSearchDirectories");
}
std::vector<NeededLibraryResolution> ModuleResolver::ResolveNeededLibraries(const LoadedModule&) const {
    Unexpected("ModuleResolver::ResolveNeededLibraries");
}
void ModuleGraph::AddModule(const std::string&, const std::vector<std::string>&) {
    Unexpected("ModuleGraph::AddModule");
}
std::vector<std::string> ModuleGraph::ResolveLoadOrder(CycleReport*) const {
    Unexpected("ModuleGraph::ResolveLoadOrder");
}
}
namespace Kernel {
#ifndef PCSX5_WITH_REAL_KERNEL_VEH
bool StartGuestCaptured(guest_addr_t, guest_addr_t, u32*) { Unexpected("Kernel::StartGuestCaptured"); }
#endif
void InitializeFdTable() { Unexpected("Kernel::InitializeFdTable"); }
void ShutdownFdTable() { Unexpected("Kernel::ShutdownFdTable"); }
int AllocateFd(int, void*, int, int, const std::string&) { Unexpected("Kernel::AllocateFd"); }
bool CloseFd(int) { Unexpected("Kernel::CloseFd"); }
FdEntry* GetFd(int) { Unexpected("Kernel::GetFd"); }
bool IsValidFd(int) { Unexpected("Kernel::IsValidFd"); }
int GetFdFlags(int) { Unexpected("Kernel::GetFdFlags"); }
bool SetFdFlags(int, int) { Unexpected("Kernel::SetFdFlags"); }
u64 GuestClockCounter() { Unexpected("Kernel::GuestClockCounter"); }
u64 GuestClockCounterFrequency() { Unexpected("Kernel::GuestClockCounterFrequency"); }
void GuestClockRealtime(s64*, s64*) { Unexpected("Kernel::GuestClockRealtime"); }
void InitializeGuestMemory() { Unexpected("Kernel::InitializeGuestMemory"); }
void ShutdownGuestMemory() { Unexpected("Kernel::ShutdownGuestMemory"); }
bool ProtectGuestMemory(guest_addr_t, u64, int) { Unexpected("Kernel::ProtectGuestMemory"); }
guest_addr_t MapGuestMemory(guest_addr_t, u64, int, int, int, s64) { Unexpected("Kernel::MapGuestMemory"); }
bool UnmapGuestMemory(guest_addr_t, u64) { Unexpected("Kernel::UnmapGuestMemory"); }
guest_addr_t GetBreak() { Unexpected("Kernel::GetBreak"); }
guest_addr_t SetBreak(guest_addr_t) { Unexpected("Kernel::SetBreak"); }
u64 GetCurrentThreadId() { Unexpected("Kernel::GetCurrentThreadId"); }
void SetCurrentThreadId(u64) { Unexpected("Kernel::SetCurrentThreadId"); }
HANDLE CreateThreadEx(guest_addr_t, guest_addr_t, u64, guest_addr_t, u64, u64*) {
    Unexpected("Kernel::CreateThreadEx");
}
void ExitThread(u64) { Unexpected("Kernel::ExitThread"); }
bool SuspendCurrentThread(const timespec*) { Unexpected("Kernel::SuspendCurrentThread"); }
bool WakeThread(u64) { Unexpected("Kernel::WakeThread"); }
bool CheckThreadActive(u64) { Unexpected("Kernel::CheckThreadActive"); }
bool TerminateThreadByTid(u64) { Unexpected("Kernel::TerminateThreadByTid"); }
}
#ifndef PCSX5_WITH_REAL_KERNEL_VEH
extern "C" u64 InvokeGuestOnStack(u64, u64, u64) { Unexpected("InvokeGuestOnStack"); }
#endif
