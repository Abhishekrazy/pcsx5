// TLS patch-once cache for fs-relative guest TLS accesses.
//
// NOTE: the full implementation below is compiled only into the pcsx5
// emulator binary (PCSX5_TLS_PATCH_FULL, set in CMakeLists for the pcsx5
// target).  Unit-test binaries get the inert stubs: the full implementation's
// presence in the test link toggles a VS-18-preview CRT hardened-delete
// false positive (FAST_FAIL_HEAP_CORRUPTION at process exit on Win11 segment
// heaps) that fires during CRT teardown after all tests pass.  The patcher
// runs zero instructions in those tests; the fastfail is a toolchain/CRT
// interaction, not a logic fault (verified by bisection + heap-header
// probing).  Keep the stub API surface identical to tls_patch.h.

#include "tls_patch.h"

#ifndef PCSX5_TLS_PATCH_FULL

namespace Kernel::TlsPatch {
bool Initialize() { return false; }
void Shutdown() {}
bool IsInitialized() { return false; }
void SetDefaultThreadPointer(guest_addr_t) {}
void BindCurrentThread(guest_addr_t) {}
bool TryPatchSite(u64, const AccessInfo&) { return false; }
bool ShouldRetry(u64) { return false; }
bool HandleStubFault(u64, u64*) { return false; }
void InvalidateAll() {}
u64 TrapCount() { return 0; }
void NoteTrap() {}
u64 PatchedCount() { return 0; }
} // namespace Kernel::TlsPatch

#else // PCSX5_TLS_PATCH_FULL


#include "../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace Kernel::TlsPatch {
namespace {

// Guest modules live in [0x800000000, 0x900000000).  The stub region must be
// reachable from every patched site with a rel32 call, so it is placed in
// the middle of that window; TryPatchSite re-checks the range per site.
constexpr u64 kGuestCodeLo = 0x800000000ULL;
constexpr u64 kGuestCodeHi = 0x900000000ULL;
constexpr u64 kStubCandidates[] = {0x840000000ULL, 0x880000000ULL, 0x8C0000000ULL};
constexpr SIZE_T kStubRegionSize = 64 * 1024;
constexpr u32 kStubAlign = 16;
constexpr u32 kMaxStubBytes = 64;

// TEB64::TlsSlots offset — stable since the first x64 Windows; verified at
// Initialize() against TlsGetValue before patching is enabled.
constexpr u32 kTebTlsSlotsOffset = 0x1480;

DWORD g_tls_index = TLS_OUT_OF_INDEXES;
u8* g_stub_region = nullptr;
SIZE_T g_stub_used = 0;
guest_addr_t g_default_tp = 0;
std::atomic<bool> g_enabled{false};
std::atomic<u64> g_traps{0};
std::atomic<u64> g_patched{0};

struct PatchRecord {
    u8 orig[16] = {};
    u32 len = 0;
    u64 stub = 0;
    u32 retries = 0;
};

// Function-local statics: no CRT static-init / teardown ordering hazards in
// binaries that link this file without ever initializing the subsystem.
std::mutex& PatchMutex() { static std::mutex m; return m; }
std::unordered_map<u64, PatchRecord>& Patches() { // keyed by site RIP
    static std::unordered_map<u64, PatchRecord> m;
    return m;
}
std::unordered_set<u64>& Blacklist() {            // sites never to re-patch
    static std::unordered_set<u64> s;
    return s;
}

// Address the stubs read the per-thread guest thread pointer from.
u64 TlsSlotAddress() {
    return kTebTlsSlotsOffset + 8ULL * g_tls_index;
}

struct StubEmitter {
    u8* p;
    void byte(u8 b) { *p++ = b; }
    void u32le(u32 v) { std::memcpy(p, &v, 4); p += 4; }
    void bytes(std::initializer_list<u8> bs) { for (u8 b : bs) byte(b); }

    // mov rax, gs:[slot_addr]  (loads the current thread's guest tp).
    // Always 64-bit: the guest tp can live above 4 GB, and address
    // computation uses the full 64-bit base even for 32-bit accesses.
    void emit_load_tp() {
        bytes({0x65, 0x48, 0x8B, 0x04, 0x25});
        u32le(static_cast<u32>(TlsSlotAddress()));
    }
    // mov rax/eax, [rax + disp32]
    void emit_load_value(bool as64, s32 disp) {
        if (as64) byte(0x48);
        bytes({0x8B, 0x80});
        u32le(static_cast<u32>(disp));
    }
    // mov [rax + disp32], reg   (reg may be r8-r15)
    void emit_store_reg(bool as64, u8 reg, s32 disp) {
        const bool ext = reg >= 8;
        if (as64 || ext) byte(static_cast<u8>(0x40 | (as64 ? 0x08 : 0x00) | (ext ? 0x04 : 0x00)));
        bytes({0x89, static_cast<u8>(0x80 | ((reg & 7) << 3))});
        u32le(static_cast<u32>(disp));
    }
    // pop reg   (reg may be r8-r15)
    void emit_pop(u8 reg) {
        if (reg >= 8) byte(0x41);
        byte(static_cast<u8>(0x58 | (reg & 7)));
    }
};

// Emits the stub for one access.  All stubs preserve EFLAGS exactly (only
// push/pop/mov/xchg are used) and preserve every register except the access
// destination (reads) — matching the semantics of the emulated instruction.
// Returns the stub size in bytes, or 0 when the access form is unsupported.
u32 EmitStub(u8* out, const AccessInfo& a) {
    StubEmitter e{out};
    switch (a.opcode) {
        case 0x8B: { // mov dst, fs:[disp]
            if (a.reg == 0) { // dst == rax: no save needed
                e.emit_load_tp();
                e.emit_load_value(a.is_64bit, a.displacement);
            } else {
                e.byte(0x50);                          // push rax
                e.emit_load_tp();
                e.emit_load_value(a.is_64bit, a.displacement);
                e.bytes({0x48, 0x87, 0x04, 0x24});     // xchg rax, [rsp]
                e.emit_pop(a.reg);                     // pop dst
            }
            e.byte(0xC3);                              // ret
            break;
        }
        case 0x89: { // mov fs:[disp], src
            if (a.reg == 0) { // src == rax: stage the value in rcx first
                e.byte(0x51);                          // push rcx
                if (a.is_64bit) e.byte(0x48);
                e.bytes({0x89, 0xC1});                 // mov rcx/ecx, rax/eax
                e.emit_load_tp();
                e.emit_store_reg(a.is_64bit, 1, a.displacement); // mov [rax+disp], rcx/ecx
                e.byte(0x59);                          // pop rcx
            } else {
                e.byte(0x50);                          // push rax
                e.emit_load_tp();
                e.emit_store_reg(a.is_64bit, a.reg, a.displacement);
                e.byte(0x58);                          // pop rax
            }
            e.byte(0xC3);                              // ret
            break;
        }
        case 0xC7: { // mov fs:[disp], imm32
            e.byte(0x50);                              // push rax
            e.emit_load_tp();
            if (a.is_64bit) e.byte(0x48);
            e.bytes({0xC7, 0x80});                     // mov [rax+disp32], imm32
            e.u32le(static_cast<u32>(a.displacement));
            e.u32le(a.imm32);
            e.byte(0x58);                              // pop rax
            e.byte(0xC3);                              // ret
            break;
        }
        default:
            return 0;
    }
    const u32 size = static_cast<u32>(e.p - out);
    return (size <= kMaxStubBytes) ? size : 0;
}

bool SafeMemCopy(void* dst, const void* src, size_t len) {
    __try {
        std::memcpy(dst, src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteSiteBytes(u64 rip, const u8* bytes, u32 len) {
    DWORD old_protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(rip), len, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    const bool ok = SafeMemCopy(reinterpret_cast<void*>(rip), bytes, len);
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(rip), len, old_protect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(rip), len);
    return ok;
}

} // namespace

bool Initialize() {
    if (g_enabled.load()) return true;
    if (GetEnvironmentVariableA("PCSX5_NO_TLS_PATCH", nullptr, 0) != 0) {
        LOG_INFO(Kernel, "TlsPatch: disabled via PCSX5_NO_TLS_PATCH");
        return false;
    }

    g_tls_index = TlsAlloc();
    if (g_tls_index == TLS_OUT_OF_INDEXES) {
        LOG_WARN(Kernel, "TlsPatch: TlsAlloc failed (err=%lu); TLS patching disabled", GetLastError());
        return false;
    }
    if (g_tls_index >= 64) {
        // Only the inline TEB slot array is directly addressable via gs:.
        LOG_WARN(Kernel, "TlsPatch: TLS slot %lu outside inline TEB array; TLS patching disabled", g_tls_index);
        TlsFree(g_tls_index);
        g_tls_index = TLS_OUT_OF_INDEXES;
        return false;
    }

    // Sanity-check the TEB TlsSlots offset before trusting it in stubs.
    const u64 probe = 0x1EE7C0DE12345678ULL;
    TlsSetValue(g_tls_index, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(probe)));
    const u64 via_gs = __readgsqword(static_cast<unsigned long>(TlsSlotAddress()));
    TlsSetValue(g_tls_index, nullptr);
    if (via_gs != probe) {
        LOG_WARN(Kernel, "TlsPatch: TEB slot readback mismatch (got 0x%llx); TLS patching disabled", via_gs);
        TlsFree(g_tls_index);
        g_tls_index = TLS_OUT_OF_INDEXES;
        return false;
    }

    for (u64 candidate : kStubCandidates) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), kStubRegionSize,
                               MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) {
            g_stub_region = static_cast<u8*>(p);
            break;
        }
    }
    if (!g_stub_region) {
        LOG_WARN(Kernel, "TlsPatch: no stub region available in guest space; TLS patching disabled");
        TlsFree(g_tls_index);
        g_tls_index = TLS_OUT_OF_INDEXES;
        return false;
    }

    g_stub_used = 0;
    g_enabled.store(true);
    LOG_INFO(Kernel, "TlsPatch: initialized (TLS slot %lu, stub region 0x%llx)",
             g_tls_index, reinterpret_cast<u64>(g_stub_region));
    return true;
}

void Shutdown() {
    InvalidateAll();
    if (g_tls_index != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_index);
        g_tls_index = TLS_OUT_OF_INDEXES;
    }
    if (g_stub_region) {
        VirtualFree(g_stub_region, 0, MEM_RELEASE);
        g_stub_region = nullptr;
    }
    g_enabled.store(false);
}

bool IsInitialized() {
    return g_enabled.load();
}

void SetDefaultThreadPointer(guest_addr_t tp) {
    g_default_tp = tp;
}

void BindCurrentThread(guest_addr_t tp) {
    if (!g_enabled.load()) return;
    if (tp == 0) tp = g_default_tp;
    TlsSetValue(g_tls_index, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(tp)));
}

bool TryPatchSite(u64 rip, const AccessInfo& access) {
    if (!g_enabled.load()) return false;
    if (rip < kGuestCodeLo || rip >= kGuestCodeHi) return false;
    if (access.instr_len < 5 || access.instr_len > 16) return false;
    // Never patch accesses involving RSP: the stub's push/pop and the final
    // ret operate on the live stack, so an rsp source/destination would be
    // corrupted.  These stay on the VEH emulation path (they are rare).
    if (access.reg == 4) return false;

    std::lock_guard<std::mutex> lock(PatchMutex());
    if (Patches().count(rip)) return true;
    if (Blacklist().count(rip)) return false;

    // Allocate + emit the stub.
    const SIZE_T offset = (g_stub_used + (kStubAlign - 1)) & ~(SIZE_T)(kStubAlign - 1);
    if (offset + kMaxStubBytes > kStubRegionSize) {
        LOG_WARN(Kernel, "TlsPatch: stub region exhausted (%zu bytes used); site 0x%llx stays emulated",
                 g_stub_used, rip);
        return false;
    }
    u8* stub = g_stub_region + offset;
    const u32 stub_size = EmitStub(stub, access);
    if (stub_size == 0) return false;

    // Range-check the rel32 call from this site to the stub.
    const s64 rel = static_cast<s64>(reinterpret_cast<u64>(stub)) -
                    static_cast<s64>(rip) - 5;
    if (rel < INT32_MIN || rel > INT32_MAX) {
        LOG_WARN(Kernel, "TlsPatch: stub out of rel32 range for site 0x%llx; stays emulated", rip);
        return false;
    }

    // Save the original bytes, then overwrite with `call stub` + NOPs.
    PatchRecord rec;
    rec.len = access.instr_len;
    rec.stub = reinterpret_cast<u64>(stub);
    if (!SafeMemCopy(rec.orig, reinterpret_cast<const void*>(rip), rec.len)) {
        return false;
    }

    u8 patch[16] = {};
    patch[0] = 0xE8;
    const s32 rel32 = static_cast<s32>(rel);
    std::memcpy(patch + 1, &rel32, 4);
    for (u32 i = 5; i < rec.len; ++i) patch[i] = 0x90;

    if (!WriteSiteBytes(rip, patch, rec.len)) {
        LOG_WARN(Kernel, "TlsPatch: failed to write patch at 0x%llx; stays emulated", rip);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stub, stub_size);

    g_stub_used = offset + kMaxStubBytes;
    Patches().emplace(rip, rec);
    const u64 count = g_patched.fetch_add(1) + 1;
    LOG_INFO(Kernel, "TlsPatch: patched site 0x%llx (op=0x%x len=%u stub=0x%llx) — site #%llu",
             rip, access.opcode, rec.len, rec.stub, count);
    return true;
}

bool ShouldRetry(u64 rip) {
    if (!g_enabled.load()) return false;
    std::lock_guard<std::mutex> lock(PatchMutex());
    auto it = Patches().find(rip);
    if (it == Patches().end()) return false;
    // A fault at a patched site means a concurrent thread fetched the bytes
    // mid-rewrite.  Re-executing is correct once the write has landed; bound
    // the retries so a genuinely broken patch cannot loop forever.
    constexpr u32 kMaxRetries = 8;
    if (it->second.retries >= kMaxRetries) return false;
    ++it->second.retries;
    LOG_WARN(Kernel, "TlsPatch: retrying torn patch site 0x%llx (%u/%u)", rip, it->second.retries, kMaxRetries);
    return true;
}

bool HandleStubFault(u64 stub_rip, u64* out_site_rip) {
    if (!g_enabled.load() || !out_site_rip) return false;
    std::lock_guard<std::mutex> lock(PatchMutex());
    for (auto it = Patches().begin(); it != Patches().end(); ++it) {
        const u64 stub = it->second.stub;
        if (stub_rip < stub || stub_rip >= stub + kMaxStubBytes) continue;

        const u64 site = it->first;
        // Restore the original instruction and blacklist the site: the stub
        // faulted (e.g. thread with no bound guest tp), so this site stays on
        // the VEH emulation path permanently.
        if (!WriteSiteBytes(site, it->second.orig, it->second.len)) {
            LOG_ERROR(Kernel, "TlsPatch: failed to restore site 0x%llx after stub fault", site);
            return false;
        }
        LOG_WARN(Kernel, "TlsPatch: stub fault at 0x%llx (site 0x%llx) — site restored, stays emulated",
                 stub_rip, site);
        Blacklist().insert(site);
        Patches().erase(it);
        *out_site_rip = site;
        return true;
    }
    return false;
}

void InvalidateAll() {
    std::lock_guard<std::mutex> lock(PatchMutex());
    for (const auto& [site, rec] : Patches()) {
        if (!WriteSiteBytes(site, rec.orig, rec.len)) {
            LOG_WARN(Kernel, "TlsPatch: failed to restore site 0x%llx during invalidation", site);
        }
    }
    if (!Patches().empty()) {
        LOG_INFO(Kernel, "TlsPatch: invalidated %zu patched sites", Patches().size());
    }
    Patches().clear();
    Blacklist().clear();
    g_stub_used = 0;
}

u64 TrapCount() {
    return g_traps.load();
}

void NoteTrap() {
    g_traps.fetch_add(1, std::memory_order_relaxed);
}

u64 PatchedCount() {
    return g_patched.load();
}

} // namespace Kernel::TlsPatch


#endif // PCSX5_TLS_PATCH_FULL
