#include "hle.h"
#include "../memory/memory.h"
#include "../common/log.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" void HleCommonDispatcher();

namespace HLE {
    void RegisterLibKernel();
    void RegisterLibPad();
    void RegisterLibVideoOut();
    void RegisterLibAgc();

    static std::unordered_map<std::string, HleSymbol> g_symbol_registry;
    static std::unordered_map<u64, HleSymbol>         g_id_index;       // fast O(1) dispatch
    static std::unordered_map<u64, ImportStats>       g_stats;          // per-symbol runtime stats
    static std::unordered_set<u64>                    g_stubbed_ids;    // symbols that were auto-stubbed
    static std::mutex                                  g_stats_mutex;
    static guest_addr_t g_thunk_page_base   = 0;
    static u64          g_thunk_page_offset = 0;
    static constexpr u64 THUNK_SIZE         = 32;
    static constexpr u64 THUNK_PAGE_SIZE    = 1 * 1024 * 1024; // 1MB = 32768 slots
    static std::mutex g_hle_mutex;
    static bool        g_strict_import_mode = false;  // Phase-0 test mode toggle

    // ---------------------------------------------------------------------
    // Import-call trace ring buffer.  Bounded (256) for crash-report use.
    // ---------------------------------------------------------------------
    constexpr size_t kTraceCapacity = 256;
    struct TraceRing {
        mutable std::mutex mutex;
        TraceEntry        entries[kTraceCapacity];
        size_t            write_index = 0;
        size_t            total_writes = 0;

        void Push(const TraceEntry& e) {
            std::lock_guard<std::mutex> lock(mutex);
            entries[write_index] = e;
            write_index = (write_index + 1) & (kTraceCapacity - 1);
            ++total_writes;
        }

        std::vector<TraceEntry> Snapshot(size_t max_count) const {
            std::lock_guard<std::mutex> lock(mutex);
            const size_t valid = std::min(total_writes, kTraceCapacity);
            const size_t n = std::min(max_count, valid);
            std::vector<TraceEntry> out;
            out.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                const size_t slot = (write_index + kTraceCapacity - 1 - i) & (kTraceCapacity - 1);
                out.push_back(entries[slot]);
            }
            std::reverse(out.begin(), out.end());
            return out;
        }

        void Clear() {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& e : entries) e = TraceEntry{};
            write_index = 0;
            total_writes = 0;
        }
    } g_trace;

    // Guest addresses set by the loader after module is mapped
    static guest_addr_t g_guest_main_addr  = 0;
    static guest_addr_t g_dt_init_addr     = 0;

    // -------------------------------------------------------------------------
    // NID normalization: strip the trailing "#X#Y" variant suffix so that
    // e.g.  bzQExy189ZI#j#j  resolves to  bzQExy189ZI#T#T  if we registered
    // the latter.  The base is everything before the second '#'.
    // -------------------------------------------------------------------------
    static std::string NidBase(const std::string& nid) {
        auto pos = nid.find('#');
        if (pos == std::string::npos) return nid;
        return nid.substr(0, pos);
    }

    bool Initialize() {
        LOG_INFO(HLE, "Initializing HLE subsystem...");

        // Allocate a large executable page for our thunks (1 MB = 32768 32-byte slots)
        g_thunk_page_base = Memory::Map(0, THUNK_PAGE_SIZE, Memory::PROT_READ | Memory::PROT_WRITE | Memory::PROT_EXEC);
        if (!g_thunk_page_base) {
            LOG_ERROR(HLE, "Failed to allocate HLE thunk page.");
            return false;
        }

        g_thunk_page_offset = 0;
        LOG_INFO(HLE, "Allocated HLE thunk page at: 0x%llx", g_thunk_page_base);

        RegisterLibKernel();
        RegisterLibPad();
        RegisterLibVideoOut();
        RegisterLibAgc();

        return true;
    }

    void Shutdown() {
        LOG_INFO(HLE, "Shutting down HLE subsystem...");
        if (g_thunk_page_base) {
            Memory::Unmap(g_thunk_page_base, THUNK_PAGE_SIZE);
            g_thunk_page_base = 0;
        }
        std::lock_guard<std::mutex> lock(g_hle_mutex);
        g_symbol_registry.clear();
        g_id_index.clear();
        {
            std::lock_guard<std::mutex> sl(g_stats_mutex);
            g_stats.clear();
            g_stubbed_ids.clear();
        }
    }

    // -------------------------------------------------------------------------
    // Test-mode and reporting helpers
    // -------------------------------------------------------------------------
    void SetStrictImportMode(bool enabled) {
        g_strict_import_mode = enabled;
        LOG_INFO(HLE, "Strict-import mode %s.", enabled ? "ENABLED" : "disabled");
    }

    bool IsStrictImportMode() {
        return g_strict_import_mode;
    }

    static void RecordStats(const HleSymbol& sym, const GuestArgs& args, guest_addr_t guest_rip) {
        std::lock_guard<std::mutex> sl(g_stats_mutex);
        auto& s = g_stats[sym.id];
        s.module_name     = sym.module_name;
        s.name            = sym.name;
        s.thunk_address   = sym.thunk_address;
        s.call_count     += 1;
        s.last_caller_rip = guest_rip;
        // total_caller_samples is currently a count of recorded calls; tracking
        // distinct RIPs would require additional storage and is unnecessary for
        // the Phase-0 deliverable.
        s.total_caller_samples = s.call_count;
        (void)args;
    }

    static void MarkAutoStubbed(u64 symbol_id) {
        std::lock_guard<std::mutex> sl(g_stats_mutex);
        g_stubbed_ids.insert(symbol_id);
    }

    std::vector<ImportStats> GetImportReport() {
        std::lock_guard<std::mutex> sl(g_stats_mutex);
        std::vector<ImportStats> out;
        out.reserve(g_stats.size());
        for (const auto& kv : g_stats) {
            if (kv.second.call_count == 0) continue;
            out.push_back(kv.second);
        }
        return out;
    }

    u64 GetUnresolvedImportCount() {
        std::lock_guard<std::mutex> sl(g_stats_mutex);
        return g_stubbed_ids.size();
    }

    void ResetRunStatistics() {
        std::lock_guard<std::mutex> sl(g_stats_mutex);
        g_stats.clear();
        g_stubbed_ids.clear();
        g_trace.Clear();
    }

    std::vector<TraceEntry> GetImportTrace(size_t max_count) {
        return g_trace.Snapshot(max_count);
    }

    void ClearImportTrace() {
        g_trace.Clear();
    }

    static guest_addr_t CreateThunk(u64 symbol_id) {
        if (g_thunk_page_offset + THUNK_SIZE > THUNK_PAGE_SIZE) {
            LOG_ERROR(HLE, "Out of executable memory slots in HLE thunk page!");
            return 0;
        }

        guest_addr_t thunk_addr = g_thunk_page_base + g_thunk_page_offset;
        g_thunk_page_offset += THUNK_SIZE;

        // Assembly thunk:
        // mov r10, symbol_id        (49 BA [8-byte val])
        // mov rax, HleCommonDispatcher (48 B8 [8-byte val])
        // jmp rax                   (FF E0)
        u8 machine_code[22] = {
            0x49, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,
            0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
            0xFF, 0xE0
        };

        u64 sym_id_val = symbol_id;
        std::memcpy(&machine_code[2], &sym_id_val, sizeof(u64));

        u64 dispatcher_ptr = reinterpret_cast<u64>(HleCommonDispatcher);
        std::memcpy(&machine_code[12], &dispatcher_ptr, sizeof(u64));

        Memory::WriteBuffer(thunk_addr, machine_code, sizeof(machine_code));
        return thunk_addr;
    }

    void RegisterSymbol(const std::string& module_name, const std::string& name, HleHandler handler) {
        std::lock_guard<std::mutex> lock(g_hle_mutex);

        std::string key = module_name + "::" + name;
        u64 symbol_id = g_symbol_registry.size() + 1;

        guest_addr_t thunk_addr = CreateThunk(symbol_id);
        if (!thunk_addr) {
            LOG_ERROR(HLE, "Failed to register symbol %s: thunk creation failed.", key.c_str());
            return;
        }

        HleSymbol sym;
        sym.module_name = module_name;
        sym.name        = name;
        sym.id          = symbol_id;
        sym.thunk_address = thunk_addr;
        sym.handler     = handler;

        auto& entry = g_symbol_registry[key];
        entry = sym;
        g_id_index[symbol_id] = entry;
        LOG_DEBUG(HLE, "Registered HLE Symbol: %s (ID: %llu) -> Thunk: 0x%llx", key.c_str(), symbol_id, thunk_addr);
    }

    guest_addr_t Resolve(const std::string& module_name, const std::string& name) {
        std::unique_lock<std::mutex> lock(g_hle_mutex);

        // 1. Exact key match
        std::string key = module_name + "::" + name;
        auto it = g_symbol_registry.find(key);
        if (it != g_symbol_registry.end()) {
            return it->second.thunk_address;
        }

        // 2. Base NID match — try every registered key whose base matches this NID's base.
        //    Allows  bzQExy189ZI#j#j  to resolve via  bzQExy189ZI#T#T
        std::string base = NidBase(name);
        if (!base.empty() && base != name) {
            for (auto& kv : g_symbol_registry) {
                // key format: "module::nid#X#Y"
                auto pos = kv.first.find("::");
                if (pos == std::string::npos) continue;
                std::string kmod = kv.first.substr(0, pos);
                std::string kname = kv.first.substr(pos + 2);
                if (kmod == module_name && NidBase(kname) == base) {
                    LOG_DEBUG(HLE, "NID variant match: %s -> %s", key.c_str(), kv.first.c_str());
                    return kv.second.thunk_address;
                }
            }
        }

        // 3. Auto-create a stub if not registered (skipped in strict-import mode)
        if (g_strict_import_mode) {
            LOG_ERROR(HLE, "Unresolved symbol in strict mode: %s", key.c_str());
            return 0;
        }
        LOG_WARN(HLE, "Unresolved symbol requested: %s. Creating stub...", key.c_str());
        lock.unlock();
        RegisterSymbol(module_name, name, [key](const GuestArgs& args) -> u64 {
            LOG_WARN(HLE, "Called unimplemented stub function: %s (Args: 0x%llx, 0x%llx, 0x%llx)",
                     key.c_str(), args.arg1, args.arg2, args.arg3);
            return 0;
        });

        lock.lock();
        auto found = g_symbol_registry.find(key);
        if (found == g_symbol_registry.end()) {
            return 0;
        }
        MarkAutoStubbed(found->second.id);
        return found->second.thunk_address;
    }

    // Search ALL registered modules for a matching NID (exact or base match).
    // Used by LinkModule when the true owning module is unknown.
    guest_addr_t ResolveAny(const std::string& name) {
        std::unique_lock<std::mutex> lock(g_hle_mutex);

        std::string base = NidBase(name);

        // 1. Scan every registered symbol for an exact or base NID match
        for (auto& kv : g_symbol_registry) {
            auto pos = kv.first.find("::");
            if (pos == std::string::npos) continue;
            std::string kname = kv.first.substr(pos + 2);

            // Exact match
            if (kname == name) {
                return kv.second.thunk_address;
            }
            // Base NID match (ignore #X#Y suffix variant)
            if (!base.empty() && NidBase(kname) == base) {
                LOG_DEBUG(HLE, "ResolveAny NID variant match: %s -> %s", name.c_str(), kv.first.c_str());
                return kv.second.thunk_address;
            }
        }

        // 2. Not found anywhere - auto-stub under "unknown" module (skipped in strict mode)
        if (g_strict_import_mode) {
            LOG_ERROR(HLE, "ResolveAny: Unresolved NID in strict mode: '%s'", name.c_str());
            return 0;
        }
        std::string stub_key = "unknown::" + name;
        LOG_WARN(HLE, "ResolveAny: Unresolved NID '%s'. Creating cross-module stub...", name.c_str());
        lock.unlock();
        RegisterSymbol("unknown", name, [stub_key](const GuestArgs& args) -> u64 {
            LOG_WARN(HLE, "Called unimplemented stub: %s (Args: 0x%llx, 0x%llx, 0x%llx)",
                     stub_key.c_str(), args.arg1, args.arg2, args.arg3);
            return 0;
        });
        lock.lock();
        auto found = g_symbol_registry.find(stub_key);
        if (found == g_symbol_registry.end()) {
            return 0;
        }
        MarkAutoStubbed(found->second.id);
        return found->second.thunk_address;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Guest address store for main() and DT_INIT
    // ─────────────────────────────────────────────────────────────────────────
    void SetGuestMainAddress(guest_addr_t addr) {
        g_guest_main_addr = addr;
        LOG_INFO(HLE, "Guest main() address set: 0x%llx", addr);
    }

    guest_addr_t GetGuestMainAddress() {
        return g_guest_main_addr;
    }

    void SetDtInitAddress(guest_addr_t addr) {
        g_dt_init_addr = addr;
        LOG_INFO(HLE, "DT_INIT address set: 0x%llx", addr);
    }

    guest_addr_t GetDtInitAddress() {
        return g_dt_init_addr;
    }

    static bool SafeRead(void* dest, const void* src, size_t size) {
        __try {
            std::memcpy(dest, src, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool GetStringDataSafely(const void* p_str_raw, const char** out_ptr, size_t* out_size) {
        __try {
            const std::string* p_str = reinterpret_cast<const std::string*>(p_str_raw);
            *out_ptr = p_str->data();
            *out_size = p_str->size();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static std::string SafeString(const std::string& str) {
        const char* data_ptr = nullptr;
        size_t size = 0;
        if (!GetStringDataSafely(&str, &data_ptr, &size)) {
            return "[Corrupt String Object]";
        }
        if (!data_ptr || size == 0) {
            return "";
        }
        if (size > 1024) {
            return "[String Too Long]";
        }
        std::string result;
        result.resize(size);
        if (SafeRead(result.data(), data_ptr, size)) {
            return result;
        } else {
            return "[Corrupt String Content]";
        }
    }

    static u64 SafeInvokeHandler(const HleHandler* handler, const GuestArgs& args, bool* out_crashed, DWORD* out_exc_code) {
        __try {
            *out_crashed = false;
            return (*handler)(args);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            *out_crashed = true;
            *out_exc_code = GetExceptionCode();
            return 0;
        }
    }

    extern "C" u64 HleDispatch(u64 symbol_id, u64 rdi, u64 rsi, u64 rdx, u64 rcx, u64 r8, u64 r9, u64 guest_rip) {
        HleSymbol target_sym;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_hle_mutex);
            auto it = g_id_index.find(symbol_id);
            if (it != g_id_index.end()) {
                target_sym = it->second;
                found = true;
            }
        }

        if (!found) {
            LOG_ERROR(HLE, "HleDispatch: Received invalid symbol ID: %llu", symbol_id);
            return 0;
        }

        std::string safe_mod = SafeString(target_sym.module_name);
        std::string safe_name = SafeString(target_sym.name);

        LOG_DEBUG(HLE, "HLE Call: %s::%s from RIP 0x%llx",
                  safe_mod.c_str(), safe_name.c_str(), guest_rip);

        GuestArgs args;
        args.arg1 = rdi;
        args.arg2 = rsi;
        args.arg3 = rdx;
        args.arg4 = rcx;
        args.arg5 = r8;
        args.arg6 = r9;

        // Push the call into the import-call trace ring.  Done before invoking
        // the handler so the trace reflects what the guest requested, not
        // whether the handler actually returned.
        TraceEntry te;
        te.timestamp_us  = ProcessUptimeMicros();
        te.module_name   = target_sym.module_name;
        te.name          = target_sym.name;
        te.symbol_id     = target_sym.id;
        te.caller_rip    = guest_rip;
        te.thunk_address = target_sym.thunk_address;
        te.arg1 = rdi; te.arg2 = rsi; te.arg3 = rdx;
        te.arg4 = rcx; te.arg5 = r8;  te.arg6 = r9;
        g_trace.Push(te);

        // Record per-symbol statistics (call count, last caller). Failures here
        // must not affect dispatch, so we wrap defensively.
        RecordStats(target_sym, args, guest_rip);

        if (!target_sym.handler) {
            LOG_ERROR(HLE, "HLE handler for %s::%s is null!", safe_mod.c_str(), safe_name.c_str());
            return 0;
        }

        bool crashed = false;
        DWORD exc_code = 0;
        u64 result = SafeInvokeHandler(&target_sym.handler, args, &crashed, &exc_code);
        if (crashed) {
            LOG_ERROR(HLE, "Crash executing HLE handler for %s::%s! Exception Code: 0x%X",
                      safe_mod.c_str(), safe_name.c_str(), exc_code);
        }
        return result;
    }
}
// namespace HLE
