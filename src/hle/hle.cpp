#include "hle.h"
#include "../memory/memory.h"
#include "../common/log.h"
#include "../common/nid.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" void HleCommonDispatcher();

// Per-thread host stack pointer: each guest thread has its own copy so that
// concurrent HLE dispatches on different threads never clobber each other's RSP.
// Accessed from assembly via GetHostStackPointer/SetHostStackPointer helpers.
static __declspec(thread) uintptr_t tls_host_stack_pointer = 0;

extern "C" uintptr_t GetHostStackPointer() {
    return tls_host_stack_pointer;
}

extern "C" void SetHostStackPointer(uintptr_t rsp) {
    tls_host_stack_pointer = rsp;
}

namespace HLE {
    void RegisterLibKernel();
    void RegisterLibKernelSync();
    void RegisterLibPad();
    void RegisterLibScePad();
    void RegisterLibVideoOut();
    void RegisterLibAgc();
    void RegisterLibSaveData();
    void RegisterLibUserService();
    void RegisterLibSysmodule();
    void RegisterLibLibc();
    void RegisterLibSystemService();
    void RegisterLibNp();

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

    // -------------------------------------------------------------------------
    // Symmetric symbol-name matching for Resolve/ResolveAny.
    //
    // The registry holds a mix of plain names ("strcpy#T#T") and NIDs
    // ("kiZSXIWd9vg#T#T"), while the guest may request either form.  A bare
    // "strcpy" must match a registered "strcpy#T#T", and a NID request must
    // match a plain-name registration when the NID database knows the name.
    // Both directions are compared on their reduced base forms plus, when
    // available, the friendly name from the NID table.
    // -------------------------------------------------------------------------
    static std::string FriendlyNameFor(const std::string& raw) {
        auto parsed = Common::ParseNidString(raw);
        if (parsed) {
            if (auto friendly = Common::LookupNidName(parsed->nid)) {
                return std::string(*friendly);
            }
        }
        return {};
    }

    static bool SymbolNamesMatch(const std::string& requested, const std::string& registered) {
        if (requested == registered) return true;
        const std::string req_base = NidBase(requested);
        const std::string reg_base = NidBase(registered);
        if (!req_base.empty() && req_base == reg_base) return true;
        // Bridge via the NID->name table in both directions.
        const std::string req_friendly = FriendlyNameFor(requested);
        if (!req_friendly.empty() && req_friendly == reg_base) return true;
        const std::string reg_friendly = FriendlyNameFor(registered);
        if (!reg_friendly.empty() && reg_friendly == req_base) return true;
        return false;
    }

    // -------------------------------------------------------------------------
    // NID-database gap filler.
    //
    // Walks the merged NID name table (built-in + assets/nid_db.txt, loaded by
    // main() before HLE::Initialize) and registers a log-and-return-0 stub
    // under its real name for every entry no HLE module implemented.  Called
    // LAST in Initialize(): RegisterSymbol is last-registration-win, so this
    // function must check-and-skip existing keys instead of blindly
    // overwriting — a real implementation registered earlier always wins.
    // Truly unknown NIDs still fall through to the Resolve-time auto-stub.
    // -------------------------------------------------------------------------
    void RegisterNidDbStubs() {
        const auto entries = Common::EnumerateNidEntries();
        size_t added = 0;
        for (const auto& e : entries) {
            if (e.name.empty()) continue;
            std::string module = e.module.empty() ? "unknown" : e.module;
            // The DB spells the kernel library "libKernel" while every HLE
            // module registers under "libkernel".  Normalize so the existence
            // check below shares one key space — otherwise a DB stub could
            // shadow a real implementation in ResolveAny's friendly-name
            // bridge (both base names would match).
            if (_stricmp(module.c_str(), "libKernel") == 0) module = "libkernel";
            {
                std::lock_guard<std::mutex> lock(g_hle_mutex);
                if (g_symbol_registry.count(module + "::" + e.name)) continue;
                // A registration under the NID itself also counts as implemented.
                if (g_symbol_registry.count(module + "::" + e.nid)) continue;
                if (g_symbol_registry.count(module + "::" + e.name + "#T#T")) continue;
            }
            const std::string mod_copy = module;
            const std::string name_copy = e.name;
            RegisterSymbol(mod_copy, name_copy,
                           [mod_copy, name_copy](const GuestArgs& /*args*/) -> u64 {
                               LogStubCallOnce(mod_copy, name_copy);
                               return 0;
                           });
            ++added;
        }
        LOG_INFO(HLE, "Registered %zu NID-database stub(s) (%zu entries scanned).", added, entries.size());
    }

    bool Initialize() {
        LOG_INFO(HLE, "Initializing HLE subsystem...");
        // Allocate a large executable page for our thunks (1 MB = 32768 32-byte slots)
        if (Memory::Map(0, THUNK_PAGE_SIZE,
                        Memory::PROT_READ | Memory::PROT_WRITE | Memory::PROT_EXEC,
                        &g_thunk_page_base) != Memory::Status::Ok) {
            LOG_ERROR(HLE, "Failed to allocate HLE thunk page.");
            return false;
        }

        g_thunk_page_offset = 0;
        LOG_INFO(HLE, "Allocated HLE thunk page at: 0x%llx", g_thunk_page_base);

        RegisterLibKernel();
        // Real sync/equeue/clock implementations — registered after
        // RegisterLibKernel() so they overwrite any legacy stub registrations
        // for the same symbols (RegisterSymbol is last-registration-win).
        RegisterLibKernelSync();
        RegisterLibPad();
        RegisterLibScePad();
        RegisterLibVideoOut();
        RegisterLibAgc();
        RegisterLibSaveData();
        RegisterLibUserService();
        RegisterLibSysmodule();
        // Real guest-heap libc family — replaces the leaky page-per-call
        // malloc/calloc/realloc registrations in libkernel (last-win).
        RegisterLibLibc();
        RegisterLibSystemService();
        RegisterLibNp();

        // Gap filler: log-and-return-0 stubs under their real names for every
        // NID-database entry no module implemented.  Must run LAST so it
        // never overrides a real registration (it skips existing keys).
        RegisterNidDbStubs();

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

    static std::string ResolveFriendlyName(const std::string& raw);

    static void RecordStats(const HleSymbol& sym, const GuestArgs& args, guest_addr_t guest_rip) {
        std::lock_guard<std::mutex> sl(g_stats_mutex);
        auto& s = g_stats[sym.id];
        s.module_name     = sym.module_name;
        s.name            = sym.name;
        if (s.resolved_name.empty()) {
            s.resolved_name = ResolveFriendlyName(sym.name);
        }
        s.thunk_address   = sym.thunk_address;
        s.call_count     += 1;
        s.last_caller_rip = guest_rip;
        s.auto_stubbed    = g_stubbed_ids.count(sym.id) != 0;
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

    // Resolve a raw requested symbol string to a friendly name when it parses
    // as a PS5 NID present in the known-name table; otherwise echo the raw
    // string back unchanged.
    static std::string ResolveFriendlyName(const std::string& raw) {
        auto parsed = Common::ParseNidString(raw);
        if (parsed) {
            if (auto friendly = Common::LookupNidName(parsed->nid)) {
                return std::string(*friendly);
            }
        }
        return raw;
    }

    // Keys (module::name) whose stub call has already been logged this run.
    static std::unordered_set<std::string> g_stub_log_keys;
    static std::mutex                        g_stub_log_mutex;

    // Guest RIP of the in-flight HLE call, set by HleDispatch so the stub
    // logger can attribute a call site (invaluable for unknown-NID stubs).
    static thread_local u64 g_current_guest_rip = 0;

    void LogStubCallOnce(const std::string& module_name, const std::string& name) {
        const std::string key = module_name + "::" + name;
        {
            std::lock_guard<std::mutex> lock(g_stub_log_mutex);
            if (!g_stub_log_keys.insert(key).second) {
                return;  // already logged; stats are still recorded by the dispatcher
            }
        }
        const std::string friendly = ResolveFriendlyName(name);
        if (friendly != name) {
            LOG_WARN(HLE, "Unimplemented stub called: %s (nid=%s name=%s) from guest RIP 0x%llx",
                     key.c_str(), name.c_str(), friendly.c_str(), g_current_guest_rip);
        } else {
            LOG_WARN(HLE, "Unimplemented stub called: %s (nid=%s) from guest RIP 0x%llx",
                     key.c_str(), name.c_str(), g_current_guest_rip);
        }
    }

    std::string ExportImportReportJson() {
        std::vector<ImportStats> report = GetImportReport();
        std::sort(report.begin(), report.end(),
                  [](const ImportStats& a, const ImportStats& b) { return a.call_count > b.call_count; });

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : report) {
            char rip[32];
            std::snprintf(rip, sizeof(rip), "0x%llx", (unsigned long long)s.last_caller_rip);
            arr.push_back({
                {"module",          s.module_name},
                {"nid",             s.name},
                {"name",            s.resolved_name},
                {"call_count",      s.call_count},
                {"auto_stubbed",    s.auto_stubbed},
                {"last_caller_rip", rip},
            });
        }
        return arr.dump(2);
    }

    bool WriteImportReportJson(const std::string& path) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << ExportImportReportJson() << '\n';
        return out.good();
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
        {
            std::lock_guard<std::mutex> ll(g_stub_log_mutex);
            g_stub_log_keys.clear();
        }
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

        // Last-registration-win, made explicit: if this exact key already
        // exists, keep its symbol id and thunk (so any previously resolved
        // thunk address stays valid) and only replace the handler.
        auto existing = g_symbol_registry.find(key);
        if (existing != g_symbol_registry.end()) {
            existing->second.handler = handler;
            g_id_index[existing->second.id] = existing->second;
            LOG_DEBUG(HLE, "Re-registered HLE Symbol: %s (ID: %llu) -> Thunk: 0x%llx",
                      key.c_str(), existing->second.id, existing->second.thunk_address);
            return;
        }

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

        // 2. Base NID / friendly-name match — try every registered key in this
        //    module whose reduced name matches (e.g. bzQExy189ZI#j#j via
        //    bzQExy189ZI#T#T, or NID kiZSXIWd9vg#T#T via plain "strcpy#T#T").
        //    Two passes: exact base-NID equality first, the NID-database
        //    friendly-name bridge only afterwards, so a plain-name
        //    registration can never shadow a real NID registration.
        const std::string req_base = NidBase(name);
        for (int pass = 0; pass < 2; ++pass) {
            for (auto& kv : g_symbol_registry) {
                // key format: "module::name"
                auto pos = kv.first.find("::");
                if (pos == std::string::npos) continue;
                std::string kmod = kv.first.substr(0, pos);
                std::string kname = kv.first.substr(pos + 2);
                if (kmod != module_name) continue;
                bool match = (pass == 0)
                    ? (!req_base.empty() && req_base == NidBase(kname))
                    : SymbolNamesMatch(name, kname);
                if (match) {
                    LOG_DEBUG(HLE, "Name variant match: %s -> %s", key.c_str(), kv.first.c_str());
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
        RegisterSymbol(module_name, name, [module_name, name](const GuestArgs& /*args*/) -> u64 {
            LogStubCallOnce(module_name, name);
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

        // 1. Scan every registered symbol for an exact, base-NID, or
        //    friendly-name match.  Exact matches win first, then base-NID
        //    equality, and only then the NID-database friendly-name bridge
        //    (see SymbolNamesMatch) — so plain-name registrations never
        //    shadow a symbol registered under the requested NID itself.
        const std::string req_base = NidBase(name);
        for (auto& kv : g_symbol_registry) {
            auto pos = kv.first.find("::");
            if (pos == std::string::npos) continue;
            if (kv.first.substr(pos + 2) == name) {
                return kv.second.thunk_address;
            }
        }
        for (int pass = 0; pass < 2; ++pass) {
            for (auto& kv : g_symbol_registry) {
                auto pos = kv.first.find("::");
                if (pos == std::string::npos) continue;
                std::string kname = kv.first.substr(pos + 2);
                bool match = (pass == 0)
                    ? (!req_base.empty() && req_base == NidBase(kname))
                    : SymbolNamesMatch(name, kname);
                if (match) {
                    LOG_DEBUG(HLE, "ResolveAny name variant match: %s -> %s", name.c_str(), kv.first.c_str());
                    return kv.second.thunk_address;
                }
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
        RegisterSymbol("unknown", name, [name](const GuestArgs& /*args*/) -> u64 {
            LogStubCallOnce("unknown", name);
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
        if (SafeRead(const_cast<char*>(result.data()), data_ptr, size)) {
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

    extern "C" u64 HleDispatch(u64 symbol_id, u64 rdi, u64 rsi, u64 rdx, u64 rcx, u64 r8, u64 r9, u64 guest_rip, u64 guest_rsp) {
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
        // First SysV stack argument sits right above the guest return address.
        args.stack_args = guest_rsp ? guest_rsp + 8 : 0;
        g_current_guest_rip = guest_rip;

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
