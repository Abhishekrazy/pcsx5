// libSceLibcInternal / libc heap family HLE — real guest heap.
//
// The legacy libkernel registrations for malloc/calloc/realloc mapped a fresh
// set of pages per call and leaked them (free was a no-op).  This module
// replaces them (last-registration-win) with a genuine guest heap living in
// the emulator's guest address space:
//
//   - Arena grows in 16 MiB chunks via Memory::Map; a bump pointer hands out
//     new memory, a first-fit free list recycles freed blocks.
//   - Every allocation carries a 16-byte header (magic + user size) placed
//     immediately before the returned pointer, so free/realloc can validate
//     and size the block without a side table.
//   - Alignment up to (and beyond) 4096 is supported by over-allocating and
//     aligning the user pointer within the carved block.
//
// Also provided: __cxa_atexit (recorded, never run), __stack_chk_guard /
// __stack_chk_fail, __cxa_finalize and sceLibcHeapGetTraceInfo.
#include "hle.h"
#include "guest_printf.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace HLE {

namespace {

// Per-thread guest errno cell backing libc __error().  Shared with the POSIX
// file wrappers in libkernel.cpp through HLE::GuestErrnoPtr/SetGuestErrno.
thread_local s32 t_guest_errno = 0;

constexpr u64 kHeapChunkSize  = 16ULL * 1024 * 1024;
constexpr u64 kHeaderSize     = 16;
constexpr u64 kHeaderMagic    = 0x5043583548454150ULL; // "PCX5HEAP"
constexpr u64 kMinAlignment   = 16;

std::mutex g_heap_mutex;

struct FreeBlock {
    u64 addr;
    u64 size;
};
std::vector<FreeBlock> g_heap_free;
guest_addr_t           g_heap_bump = 0;
guest_addr_t           g_heap_end  = 0;

bool HeapGrowLocked(u64 min_size) {
    u64 chunk = kHeapChunkSize;
    while (chunk < min_size) {
        chunk *= 2;
    }
    guest_addr_t base = 0;
    if (Memory::Map(0, chunk, Memory::PROT_READ | Memory::PROT_WRITE, &base) != Memory::Status::Ok) {
        LOG_ERROR(HLE, "libc heap: failed to map %llu-byte chunk", chunk);
        return false;
    }
    g_heap_bump = base;
    g_heap_end  = base + chunk;
    LOG_DEBUG(HLE, "libc heap: mapped %llu MiB arena chunk at 0x%llx", chunk >> 20, base);
    return true;
}

// Writes the allocation header immediately before `user_ptr`.
void WriteHeader(u64 user_ptr, u64 user_size) {
    Memory::Write<u64>(user_ptr - kHeaderSize, kHeaderMagic);
    Memory::Write<u64>(user_ptr - kHeaderSize + 8, user_size);
}

// Core allocator.  Caller holds g_heap_mutex.  Returns a guest pointer whose
// 16-byte header sits at result-16, or 0 on OOM.
u64 HeapAllocLocked(u64 size, u64 align) {
    if (size == 0) size = 1;
    if (align < kMinAlignment) align = kMinAlignment;

    // First-fit over the free list.
    for (auto it = g_heap_free.begin(); it != g_heap_free.end(); ++it) {
        const u64 aligned = (it->addr + kHeaderSize + align - 1) & ~(align - 1);
        const u64 total   = (aligned - it->addr) + size;
        if (total > it->size) continue;
        if (it->size - total >= 64) {
            it->addr += total;
            it->size -= total;
        } else {
            g_heap_free.erase(it);
        }
        WriteHeader(aligned, size);
        // Zero user data: the Construct runtime expects zero-initialized heap
        // (like Orbis OS provides) and its internal allocator reuses blocks
        // without clearing them — stale data from a prior allocation shows up
        // as phantom variants (tag=0xb0) during JSON parsing.
        if (aligned) std::memset(reinterpret_cast<void*>(aligned), 0, size);
        return aligned;
    }

    // Bump-allocate, growing the arena as needed.
    for (;;) {
        const u64 aligned = (g_heap_bump + kHeaderSize + align - 1) & ~(align - 1);
        const u64 total   = (aligned - g_heap_bump) + size;
        if (g_heap_bump != 0 && g_heap_bump + total <= g_heap_end) {
            g_heap_bump += total;
            WriteHeader(aligned, size);
            // Zero user data (safe no-op for fresh zeroed pages, required
            // after arena reuse cycles).
            if (aligned) std::memset(reinterpret_cast<void*>(aligned), 0, size);
            return aligned;
        }
        if (!HeapGrowLocked(total + align + kHeaderSize)) {
            return 0;
        }
    }
}

u64 HeapAlloc(u64 size, u64 align) {
    std::lock_guard<std::mutex> lk(g_heap_mutex);
    return HeapAllocLocked(size, align);
}

// Validates the header at ptr-16.  Returns the user size, or 0 if the pointer
// is not one of ours (caller must NOT touch the free list in that case).
bool HeapValidate(u64 ptr, u64* out_user_size) {
    if (ptr < kHeaderSize) return false;
    u64 magic = 0;
    __try {
        magic = Memory::Read<u64>(ptr - kHeaderSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (magic != kHeaderMagic) return false;
    *out_user_size = Memory::Read<u64>(ptr - kHeaderSize + 8);
    return true;
}

void HeapFree(u64 ptr) {
    if (!ptr) return;
    std::lock_guard<std::mutex> lk(g_heap_mutex);
    u64 user_size = 0;
    if (!HeapValidate(ptr, &user_size)) {
        LOG_WARN(HLE, "libc heap: free(0x%llx): bad header magic — ignoring (not one of ours?)", ptr);
        return;
    }
    g_heap_free.push_back({ptr - kHeaderSize, user_size + kHeaderSize});
}

// ---------------------------------------------------------------------------
// __cxa_atexit bookkeeping: recorded for diagnostics, never invoked (the
// emulator exits the process outright).
// ---------------------------------------------------------------------------
struct AtExitEntry {
    u64 func;
    u64 arg;
    u64 dso;
};
std::vector<AtExitEntry> g_atexit_handlers;

// ---------------------------------------------------------------------------
// Guest C++ exception support (libc++abi / Itanium C++ ABI surface).
//
// Approach: real two-phase DWARF unwinding over the guest's own .eh_frame
// (located via PT_GNU_EH_FRAME / .eh_frame_hdr; every loaded module registers
// its table through SetGuestEhFrameHdr).  __cxa_throw reconstructs the full register context at
// the HLE thunk entry (the dispatcher saves guest rbx/rbp/r12-r15 on the
// guest stack at known offsets), walks frames with a CFI interpreter, and
// calls the guest personality routine (from the CIE 'P' augmentation) through
// InvokeGuestFunction6.  When the personality installs a context, we emit a
// tiny register-restore trampoline into host RWX memory and overwrite the
// guest return-address slot the dispatcher is about to `ret` through, so
// execution resumes at the landing pad with the unwound register state.
// Cleanup landing pads re-enter via _Unwind_Resume, which continues phase 2.
// ---------------------------------------------------------------------------

// Registered .eh_frame_hdr tables in guest memory (direct-pointer: guest VA
// == host VA).  Every loaded module with a PT_GNU_EH_FRAME segment registers
// its own table (main module plus auto-loaded PRXs such as libc.prx); FDE
// lookup tries each table in turn so unwinding can cross module boundaries.
struct EhFrameTable {
    guest_addr_t addr;
    u64          size;
};
std::vector<EhFrameTable> g_eh_tables;

// Base for textrel/datarel DW_EH_PE decoding: the .eh_frame_hdr address of
// the table that owns the record currently being parsed (set by FindFde).
guest_addr_t g_cur_eh_base = 0;

// SEH-safe guest memory readers.  Kept as tiny POD-only functions so __try is
// legal (no C++ objects with destructors in scope).  Return false on fault.
bool SafeReadMem(u64 addr, void* dst, size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(addr), n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

u64 Rd64(u64 addr, bool* ok) {
    u64 v = 0;
    if (!SafeReadMem(addr, &v, sizeof(v))) { *ok = false; return 0; }
    return v;
}

u32 Rd32(u64 addr, bool* ok) {
    u32 v = 0;
    if (!SafeReadMem(addr, &v, sizeof(v))) { *ok = false; return 0; }
    return v;
}

u8 Rd8(u64 addr, bool* ok) {
    u8 v = 0;
    if (!SafeReadMem(addr, &v, sizeof(v))) { *ok = false; return 0; }
    return v;
}

// LEB128 readers; advance *p past the value.
u64 ReadUleb(u64* p, bool* ok) {
    u64 result = 0;
    int shift = 0;
    for (;;) {
        const u8 b = Rd8((*p)++, ok);
        if (!*ok) return 0;
        result |= static_cast<u64>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
        if (shift > 63) { *ok = false; return 0; }
    }
    return result;
}

s64 ReadSleb(u64* p, bool* ok) {
    u64 result = 0;
    int shift = 0;
    u8 b = 0;
    do {
        b = Rd8((*p)++, ok);
        if (!*ok) return 0;
        result |= static_cast<u64>(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) {
        result |= (~0ULL) << shift;
    }
    return static_cast<s64>(result);
}

// DWARF EH pointer encodings (DW_EH_PE_*).
constexpr u8 kEncAbsptr  = 0x00;
constexpr u8 kEncUleb128 = 0x01;
constexpr u8 kEncUdata2  = 0x02;
constexpr u8 kEncUdata4  = 0x03;
constexpr u8 kEncUdata8  = 0x04;
constexpr u8 kEncSleb128 = 0x09;
constexpr u8 kEncSdata2  = 0x0A;
constexpr u8 kEncSdata4  = 0x0B;
constexpr u8 kEncSdata8  = 0x0C;
constexpr u8 kEncPcrel   = 0x10;
constexpr u8 kEncTextrel = 0x20;
constexpr u8 kEncDatarel = 0x30;
constexpr u8 kEncFuncrel = 0x40;
constexpr u8 kEncAligned = 0x50;
constexpr u8 kEncIndirect= 0x80;
constexpr u8 kEncOmit    = 0xFF;

u64 EncodedSize(u8 enc) {
    switch (enc & 0x0F) {
        case kEncAbsptr:  return 8;
        case kEncUdata2:
        case kEncSdata2:  return 2;
        case kEncUdata4:
        case kEncSdata4:  return 4;
        case kEncUdata8:
        case kEncSdata8:  return 8;
        default:          return 0; // LEB128: variable
    }
}

// Reads a DW_EH_PE-encoded pointer at *p and advances *p.  `pcrel_base` for
// pcrel is the address of the field itself; textrel/datarel resolve against
// the .eh_frame_hdr address (no separate GOT base in our flat guest map).
u64 ReadEncodedPointer(u64* p, u8 enc, bool* ok) {
    if (enc == kEncOmit) return 0;
    u64 field_addr = *p;
    if ((enc & 0x0F) == kEncAbsptr && (enc & 0x70) == kEncAligned) {
        field_addr = (field_addr + 7) & ~7ULL;
        *p = field_addr + 8;
    }
    u64 value = 0;
    switch (enc & 0x0F) {
        case kEncAbsptr:  value = Rd64(field_addr, ok); *p = field_addr + 8; break;
        case kEncUdata2:  { u16 v=0; if(!SafeReadMem(field_addr,&v,2)){*ok=false;return 0;} value=v; } *p = field_addr + 2; break;
        case kEncUdata4:  value = Rd32(field_addr, ok); *p = field_addr + 4; break;
        case kEncUdata8:  value = Rd64(field_addr, ok); *p = field_addr + 8; break;
        case kEncUleb128: *p = field_addr; value = ReadUleb(p, ok); break;
        case kEncSleb128: *p = field_addr; value = static_cast<u64>(ReadSleb(p, ok)); break;
        case kEncSdata2:  { u16 v=0; if(!SafeReadMem(field_addr,&v,2)){*ok=false;return 0;} value = static_cast<u64>(static_cast<s64>(static_cast<s16>(v))); } *p = field_addr + 2; break;
        case kEncSdata4:  value = static_cast<u64>(static_cast<s64>(static_cast<s32>(Rd32(field_addr, ok)))); *p = field_addr + 4; break;
        case kEncSdata8:  value = Rd64(field_addr, ok); *p = field_addr + 8; break;
        default:
            LOG_ERROR(HLE, "unwind: unsupported pointer encoding 0x%02x", enc);
            *ok = false;
            return 0;
    }
    if (!*ok) return 0;
    switch (enc & 0x70) {
        case 0x00:        break; // absptr
        case kEncPcrel:   value = field_addr + value; break;
        case kEncTextrel:
        case kEncDatarel: value = g_cur_eh_base + value; break;
        default:
            LOG_ERROR(HLE, "unwind: unsupported pointer base 0x%02x", enc & 0x70);
            *ok = false;
            return 0;
    }
    if (enc & kEncIndirect) {
        value = Rd64(value, ok);
    }
    return value;
}

// --- CIE / FDE parsing ------------------------------------------------------
struct CieInfo {
    u64 code_align      = 1;
    s64 data_align      = -8;
    u64 ret_reg         = 16;
    u8  fde_enc         = kEncAbsptr;   // default: absptr u64
    u8  lsda_enc        = kEncAbsptr;
    u64 personality     = 0;
    u8  personality_enc = kEncAbsptr;
    bool has_personality = false;
    bool signal_frame    = false;
    u64 initial_instr    = 0;           // CIE initial instructions
    u64 initial_instr_end = 0;
};

std::mutex g_cie_mutex;
std::unordered_map<u64, CieInfo> g_cie_cache;

bool ParseCie(u64 cie, CieInfo& out) {
    {
        std::lock_guard<std::mutex> lk(g_cie_mutex);
        auto it = g_cie_cache.find(cie);
        if (it != g_cie_cache.end()) { out = it->second; return true; }
    }
    bool ok = true;
    const u32 len = Rd32(cie, &ok);
    if (!ok || len == 0 || len == 0xFFFFFFFFu) {
        LOG_ERROR(HLE, "unwind: bad CIE at 0x%llx (len=0x%x)", cie, len);
        return false;
    }
    const u32 id = Rd32(cie + 4, &ok);
    if (!ok || id != 0) {
        LOG_ERROR(HLE, "unwind: CIE id != 0 at 0x%llx", cie);
        return false;
    }
    u64 p = cie + 8;
    const u8 version = Rd8(p++, &ok);
    // Augmentation string (NUL-terminated).
    char aug[32] = {};
    size_t ai = 0;
    for (;;) {
        const u8 c = Rd8(p++, &ok);
        if (!ok) return false;
        if (c == 0) break;
        if (ai < sizeof(aug) - 1) aug[ai++] = static_cast<char>(c);
    }
    CieInfo info;
    info.code_align = ReadUleb(&p, &ok);
    info.data_align = ReadSleb(&p, &ok);
    if (!ok) return false;
    // Return-address register: uleb128 in CIE v1 (and in practice for v3 too).
    info.ret_reg = ReadUleb(&p, &ok);
    if (!ok) return false;
    if (version != 1 && version != 3) {
        LOG_WARN(HLE, "unwind: CIE version %u at 0x%llx (best-effort)", version, cie);
    }
    if (aug[0] == 'z') {
        const u64 aug_len = ReadUleb(&p, &ok);
        if (!ok) return false;
        const u64 aug_end = p + aug_len;
        for (size_t i = 1; aug[i] != '\0'; ++i) {
            switch (aug[i]) {
                case 'P': {
                    info.personality_enc = Rd8(p++, &ok);
                    const u64 field = p;
                    info.personality = ReadEncodedPointer(&p, info.personality_enc, &ok);
                    info.has_personality = true;
                    LOG_INFO(HLE, "unwind: CIE 0x%llx 'P' enc=0x%02x field=0x%llx -> personality=0x%llx (tid=%lu)",
                             cie, info.personality_enc, field, info.personality,
                             GetCurrentThreadId());
                    break;
                }
                case 'L':
                    info.lsda_enc = Rd8(p++, &ok);
                    break;
                case 'R':
                    info.fde_enc = Rd8(p++, &ok);
                    break;
                case 'S':
                    info.signal_frame = true;
                    break;
                case 'G': case 'B': case 'H':
                    break; // ignored
                default:
                    LOG_WARN(HLE, "unwind: unknown CIE augmentation '%c'", aug[i]);
                    break;
            }
            if (!ok) return false;
        }
        p = aug_end; // trust the length over per-letter parsing
    }
    info.initial_instr     = p;
    info.initial_instr_end = cie + 4 + len;
    {
        std::lock_guard<std::mutex> lk(g_cie_mutex);
        g_cie_cache[cie] = info;
    }
    LOG_INFO(HLE, "unwind: CIE 0x%llx parsed: aug='%s' fde_enc=0x%02x lsda_enc=0x%02x personality=0x%llx%s",
             cie, aug, info.fde_enc, info.lsda_enc, info.personality,
             info.has_personality ? "" : " (none)");
    out = info;
    return true;
}

struct FdeInfo {
    u64 fde_addr   = 0;
    u64 pc_begin   = 0;
    u64 pc_end     = 0;
    u64 lsda       = 0;
    u64 instr      = 0;
    u64 instr_end  = 0;
    CieInfo cie;
};

// Parses the FDE header at `fde`.  Returns false on malformed data.
bool ParseFde(u64 fde, FdeInfo& out) {
    bool ok = true;
    const u32 len = Rd32(fde, &ok);
    if (!ok || len == 0 || len == 0xFFFFFFFFu) return false;
    const u32 cie_off = Rd32(fde + 4, &ok);
    if (!ok) return false;
    const u64 cie_addr = (fde + 4) - cie_off;
    if (!ParseCie(cie_addr, out.cie)) return false;
    out.fde_addr = fde;
    u64 p = fde + 8;
    out.pc_begin = ReadEncodedPointer(&p, out.cie.fde_enc, &ok);
    LOG_INFO(HLE, "unwind: FDE 0x%llx: cie=0x%llx fde_enc=0x%02x pc_begin=0x%llx ok=%d",
             fde, cie_addr, out.cie.fde_enc, out.pc_begin, (int)ok);
    // Range length: format bits only, no base applied.
    const u8 range_enc = out.cie.fde_enc & 0x0F;
    u64 pc_range = 0;
    switch (range_enc) {
        case kEncAbsptr:
        case kEncUdata8:
        case kEncSdata8: pc_range = Rd64(p, &ok); p += 8; break;
        case kEncUdata4:
        case kEncSdata4: pc_range = Rd32(p, &ok); p += 4; break;
        case kEncUdata2:
        case kEncSdata2: { u16 v=0; if(!SafeReadMem(p,&v,2)){ok=false;} pc_range=v; p += 2; } break;
        case kEncUleb128: pc_range = ReadUleb(&p, &ok); break;
        case kEncSleb128: pc_range = static_cast<u64>(ReadSleb(&p, &ok)); break;
        default: ok = false; break;
    }
    if (!ok) return false;
    out.pc_end = out.pc_begin + pc_range;
    const u64 aug_len = ReadUleb(&p, &ok);
    if (!ok) return false;
    const u64 aug_start = p;
    if (aug_len > 0) {
        out.lsda = ReadEncodedPointer(&p, out.cie.lsda_enc, &ok);
        if (!ok) return false;
    }
    out.instr     = aug_start + aug_len;
    out.instr_end = fde + 4 + len;
    return true;
}

// Finds the FDE covering `pc` via one module's .eh_frame_hdr binary-search
// table.  Sets g_cur_eh_base for any pointers decoded while parsing.
bool FindFdeInTable(const EhFrameTable& tab, u64 pc, FdeInfo& out) {
    bool ok = true;
    u64 p = tab.addr;
    const u8 version         = Rd8(p++, &ok);
    const u8 eh_ptr_enc      = Rd8(p++, &ok);
    const u8 fde_count_enc   = Rd8(p++, &ok);
    const u8 table_enc       = Rd8(p++, &ok);
    if (!ok || version != 1) {
        LOG_INFO(HLE, "unwind: table 0x%llx rejected (ok=%d version=%u)", tab.addr, (int)ok, version);
        return false;
    }
    /* eh_frame_ptr (unused; FDEs come from the table) */
    (void)ReadEncodedPointer(&p, eh_ptr_enc, &ok);
    const u64 fde_count = ReadEncodedPointer(&p, fde_count_enc, &ok);
    if (!ok || fde_count == 0 || fde_count > (1u << 24)) {
        LOG_INFO(HLE, "unwind: table 0x%llx rejected (ok=%d fde_count=%llu)", tab.addr, (int)ok, fde_count);
        return false;
    }
    const u64 entry_sz = EncodedSize(table_enc);
    if (entry_sz == 0 || (table_enc & 0x70) == kEncAligned) {
        LOG_INFO(HLE, "unwind: table 0x%llx rejected (table_enc=0x%02x)", tab.addr, table_enc);
        return false;
    }
    const u64 table = p;
    if (tab.size != 0 &&
        table + fde_count * entry_sz * 2 > tab.addr + tab.size) {
        LOG_INFO(HLE, "unwind: table 0x%llx rejected (bounds: count=%llu entry=%llu size=%llu)",
                 tab.addr, fde_count, entry_sz, tab.size);
        return false;
    }
    // Table entries are sorted by initial_loc; binary-search the last <= pc.
    u64 lo = 0, hi = fde_count; // invariant: entry[lo-1] <= pc
    while (lo < hi) {
        const u64 mid = lo + (hi - lo) / 2;
        u64 ep = table + mid * entry_sz * 2;
        const u64 initial_loc = ReadEncodedPointer(&ep, table_enc, &ok);
        if (!ok) return false;
        if (initial_loc <= pc) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) {
        LOG_INFO(HLE, "unwind: table 0x%llx: pc 0x%llx below first entry", tab.addr, pc);
        return false;
    }
    u64 ep = table + (lo - 1) * entry_sz * 2;
    (void)ReadEncodedPointer(&ep, table_enc, &ok);
    const u64 fde_addr = ReadEncodedPointer(&ep, table_enc, &ok);
    if (!ok || fde_addr == 0) return false;
    if (!ParseFde(fde_addr, out)) {
        LOG_INFO(HLE, "unwind: table 0x%llx: ParseFde failed at fde=0x%llx (pc=0x%llx)",
                 tab.addr, fde_addr, pc);
        return false;
    }
    if (pc < out.pc_begin || pc >= out.pc_end) {
        LOG_INFO(HLE, "unwind: table 0x%llx: pc 0x%llx not in FDE [0x%llx, 0x%llx)",
                 tab.addr, pc, out.pc_begin, out.pc_end);
        return false;
    }
    return true;
}

// Finds the FDE covering `pc` across every registered module table.
bool FindFde(u64 pc, FdeInfo& out) {
    for (const EhFrameTable& tab : g_eh_tables) {
        if (tab.addr == 0) continue;
        g_cur_eh_base = tab.addr;
        if (FindFdeInTable(tab, pc, out)) return true;
    }
    return false;
}

// --- DWARF CFI interpreter ---------------------------------------------------
constexpr u8 kRuleSame     = 0; // unchanged
constexpr u8 kRuleOffset   = 1; // value at cfa + off
constexpr u8 kRuleValOff   = 2; // value == cfa + off
constexpr u8 kRuleRegister = 3; // value == reg
constexpr u8 kRuleUndef    = 4; // unreadable

struct RegRule {
    u8  type = kRuleSame;
    s64 off  = 0;
    u64 reg  = 0;
};

struct CfiState {
    u64 cfa_reg = 7;   // DWARF rsp
    s64 cfa_off = 0;
    bool cfa_expr = false; // DW_CFA_def_cfa_expression — unsupported
    RegRule rules[17];
};

bool ExecCfiRange(u64 instr, u64 instr_end, u64 pc_begin, u64 target_pc,
                  const CieInfo& cie, u64* loc_inout, CfiState& st,
                  std::vector<CfiState>& stack) {
    bool ok = true;
    u64 loc = *loc_inout;
    u64 p = instr;
    while (p < instr_end && loc <= target_pc) {
        const u8 op = Rd8(p++, &ok);
        if (!ok) return false;
        const u8 primary = op & 0xC0;
        const u8 opcode  = op & 0x3F;
        if (primary == 0x40) {          // DW_CFA_advance_loc
            loc += static_cast<u64>(opcode) * cie.code_align;
            continue;
        }
        if (primary == 0x80) {          // DW_CFA_offset
            st.rules[opcode].type = kRuleOffset;
            st.rules[opcode].off  = static_cast<s64>(ReadUleb(&p, &ok)) * cie.data_align;
            if (!ok) return false;
            continue;
        }
        if (primary == 0xC0) {          // DW_CFA_restore
            st.rules[opcode] = RegRule{};
            continue;
        }
        switch (opcode) {
            case 0x00: break;           // nop
            case 0x01: {                // set_loc
                loc = ReadEncodedPointer(&p, cie.fde_enc, &ok);
                if (!ok) return false;
                break;
            }
            case 0x02: {                // advance_loc1
                loc += static_cast<u64>(Rd8(p++, &ok)) * cie.code_align;
                break;
            }
            case 0x03: {                // advance_loc2
                u16 v = 0; if (!SafeReadMem(p, &v, 2)) return false; p += 2;
                loc += static_cast<u64>(v) * cie.code_align;
                break;
            }
            case 0x04: {                // advance_loc4
                loc += static_cast<u64>(Rd32(p, &ok)) * cie.code_align; p += 4;
                break;
            }
            case 0x05: {                // offset_extended
                const u64 r = ReadUleb(&p, &ok);
                const u64 o = ReadUleb(&p, &ok);
                if (!ok || r > 16) return false;
                st.rules[r].type = kRuleOffset;
                st.rules[r].off  = static_cast<s64>(o) * cie.data_align;
                break;
            }
            case 0x06: {                // restore_extended
                const u64 r = ReadUleb(&p, &ok);
                if (!ok || r > 16) return false;
                st.rules[r] = RegRule{};
                break;
            }
            case 0x07: {                // undefined
                const u64 r = ReadUleb(&p, &ok);
                if (!ok || r > 16) return false;
                st.rules[r].type = kRuleUndef;
                break;
            }
            case 0x08: {                // same_value
                const u64 r = ReadUleb(&p, &ok);
                if (!ok || r > 16) return false;
                st.rules[r] = RegRule{};
                break;
            }
            case 0x09: {                // register
                const u64 r = ReadUleb(&p, &ok);
                const u64 r2 = ReadUleb(&p, &ok);
                if (!ok || r > 16 || r2 > 16) return false;
                st.rules[r].type = kRuleRegister;
                st.rules[r].reg  = r2;
                break;
            }
            case 0x0A:                  // remember_state
                stack.push_back(st);
                break;
            case 0x0B:                  // restore_state
                if (stack.empty()) return false;
                st = stack.back();
                stack.pop_back();
                break;
            case 0x0C: {                // def_cfa
                st.cfa_reg = ReadUleb(&p, &ok);
                st.cfa_off = static_cast<s64>(ReadUleb(&p, &ok));
                if (!ok || st.cfa_reg > 16) return false;
                st.cfa_expr = false;
                break;
            }
            case 0x0D: {                // def_cfa_register
                st.cfa_reg = ReadUleb(&p, &ok);
                if (!ok || st.cfa_reg > 16) return false;
                st.cfa_expr = false;
                break;
            }
            case 0x0E: {                // def_cfa_offset
                st.cfa_off = static_cast<s64>(ReadUleb(&p, &ok));
                if (!ok) return false;
                break;
            }
            case 0x0F:                  // def_cfa_expression — unsupported
                LOG_ERROR(HLE, "unwind: DW_CFA_def_cfa_expression not supported");
                return false;
            case 0x10:                  // expression
            case 0x16: {                // val_expression — unsupported
                LOG_ERROR(HLE, "unwind: DW_CFA_(val_)expression not supported");
                return false;
            }
            case 0x11: {                // offset_extended_sf
                const u64 r = ReadUleb(&p, &ok);
                const s64 o = ReadSleb(&p, &ok);
                if (!ok || r > 16) return false;
                st.rules[r].type = kRuleOffset;
                st.rules[r].off  = o * cie.data_align;
                break;
            }
            case 0x12: {                // def_cfa_sf
                st.cfa_reg = ReadUleb(&p, &ok);
                st.cfa_off = ReadSleb(&p, &ok) * cie.data_align;
                if (!ok || st.cfa_reg > 16) return false;
                st.cfa_expr = false;
                break;
            }
            case 0x13: {                // def_cfa_offset_sf
                st.cfa_off = ReadSleb(&p, &ok) * cie.data_align;
                if (!ok) return false;
                break;
            }
            case 0x14: {                // val_offset
                const u64 r = ReadUleb(&p, &ok);
                const u64 o = ReadUleb(&p, &ok);
                if (!ok || r > 16) return false;
                st.rules[r].type = kRuleValOff;
                st.rules[r].off  = static_cast<s64>(o) * cie.data_align;
                break;
            }
            case 0x15: {                // val_offset_sf
                const u64 r = ReadUleb(&p, &ok);
                const s64 o = ReadSleb(&p, &ok);
                if (!ok || r > 16) return false;
                st.rules[r].type = kRuleValOff;
                st.rules[r].off  = o * cie.data_align;
                break;
            }
            case 0x2E:                  // GNU_args_size — informational
                (void)ReadUleb(&p, &ok);
                if (!ok) return false;
                break;
            case 0x2F: {                // GNU_negative_offset_extended
                const u64 r = ReadUleb(&p, &ok);
                const u64 o = ReadUleb(&p, &ok);
                if (!ok || r > 16) return false;
                st.rules[r].type = kRuleOffset;
                st.rules[r].off  = -static_cast<s64>(o) * cie.data_align;
                break;
            }
            default:
                LOG_ERROR(HLE, "unwind: unknown CFI opcode 0x%02x", opcode);
                return false;
        }
        (void)pc_begin;
    }
    *loc_inout = loc;
    return true;
}

// Register order (DWARF x86-64): 0=rax 1=rdx 2=rcx 3=rbx 4=rsi 5=rdi 6=rbp
// 7=rsp 8..15=r8..r15 16=rip.
struct GuestUnwindContext {
    u64 magic = 0;
    u64 gr[17] = {};
    u64 cfa          = 0;
    u64 lsda         = 0;
    u64 region_start = 0;
};
constexpr u64 kUnwindCtxMagic = 0x5043583555584358ULL; // "PCX5UCX"

struct FrameEval {
    u64 cfa          = 0;
    u64 lsda         = 0;
    u64 pc_begin     = 0;
    u64 personality  = 0;
    bool has_personality = false;
    bool signal_frame    = false;
    u64 next_gr[17]  = {};
};

// Evaluates the unwind rules for the frame ctx->gr[16] currently sits in.
// On success, `out` holds the frame's CFA/LSDA/personality and the register
// state of the *caller* frame (what the context becomes after unwinding).
bool EvalFrame(const GuestUnwindContext& ctx, FrameEval& out) {
    FdeInfo fde;
    if (!FindFde(ctx.gr[16], fde)) return false;

    CfiState st;
    std::vector<CfiState> stack;
    u64 loc = fde.pc_begin;
    if (!ExecCfiRange(fde.cie.initial_instr, fde.cie.initial_instr_end,
                      fde.pc_begin, ctx.gr[16], fde.cie, &loc, st, stack)) {
        return false;
    }
    stack.clear();
    if (!ExecCfiRange(fde.instr, fde.instr_end, fde.pc_begin, ctx.gr[16],
                      fde.cie, &loc, st, stack)) {
        return false;
    }
    if (st.cfa_expr) return false;
    if (fde.cie.ret_reg > 16) return false;

    bool ok = true;
    const u64 cfa = ctx.gr[st.cfa_reg] + static_cast<u64>(st.cfa_off);
    u64 next[17];
    for (int i = 0; i < 17; ++i) next[i] = ctx.gr[i];
    for (u64 r = 0; r < 17; ++r) {
        const RegRule& rule = st.rules[r];
        switch (rule.type) {
            case kRuleOffset:
                next[r] = Rd64(cfa + static_cast<u64>(rule.off), &ok);
                if (!ok) return false;
                break;
            case kRuleValOff:
                next[r] = cfa + static_cast<u64>(rule.off);
                break;
            case kRuleRegister:
                next[r] = ctx.gr[rule.reg];
                break;
            case kRuleUndef:
            case kRuleSame:
            default:
                break;
        }
    }
    // Return address: rule for the CIE return-register column.
    const u64 ra = next[fde.cie.ret_reg];
    next[16] = ra;
    next[7]  = cfa; // rsp := CFA

    out.cfa = cfa;
    out.lsda = fde.lsda;
    out.pc_begin = fde.pc_begin;
    out.personality = fde.cie.personality;
    out.has_personality = fde.cie.has_personality;
    out.signal_frame = fde.cie.signal_frame;
    for (int i = 0; i < 17; ++i) out.next_gr[i] = next[i];
    return true;
}

// Reconstructs the unwind context at an HLE thunk entry from the dispatcher's
// saved-register slots on the guest stack.  guest_rsp = args.stack_args - 8:
// [rsp+0]=return rip, [rsp-8]=r8, [rsp-16]=r9, [rsp-24]=r15, [rsp-32]=r14,
// [rsp-40]=r13, [rsp-48]=r12, [rsp-56]=rbp, [rsp-64]=rbx (see dispatcher.asm).
bool ReconstructContext(const GuestArgs& args, GuestUnwindContext& ctx) {
    if (!args.stack_args) return false;
    bool ok = true;
    const u64 rsp = args.stack_args - 8;
    for (int i = 0; i < 17; ++i) ctx.gr[i] = 0;
    ctx.magic = kUnwindCtxMagic;
    ctx.gr[16] = Rd64(rsp, &ok);        // return address into the caller
    ctx.gr[7]  = rsp + 8;               // rsp as seen by the caller
    ctx.gr[3]  = Rd64(rsp - 64, &ok);   // rbx
    ctx.gr[6]  = Rd64(rsp - 56, &ok);   // rbp
    ctx.gr[12] = Rd64(rsp - 48, &ok);   // r12
    ctx.gr[13] = Rd64(rsp - 40, &ok);   // r13
    ctx.gr[14] = Rd64(rsp - 32, &ok);   // r14
    ctx.gr[15] = Rd64(rsp - 24, &ok);   // r15
    ctx.gr[5]  = args.arg1;             // rdi
    ctx.gr[4]  = args.arg2;             // rsi
    ctx.gr[1]  = args.arg3;             // rdx
    ctx.gr[2]  = args.arg4;             // rcx
    ctx.gr[8]  = args.arg5;             // r8
    ctx.gr[9]  = args.arg6;             // r9
    if (!ok) return false;
    return true;
}

// --- Landing trampoline ------------------------------------------------------
// When the personality installs a context we cannot just "return" the guest
// somewhere else: the dispatcher epilogue pops its saved registers and `ret`s
// through the original guest return-address slot.  We overwrite that slot
// with the address of a freshly emitted trampoline that loads the full
// unwound register state from a LandingBlock and jumps to the landing pad.
struct LandingBlock {
    u64 rax, rdx, rcx, rbx, rsi, rdi, rbp, rsp;
    u64 r8, r9, r10, r12, r13, r14, r15, rip;
};

std::mutex  g_tramp_mutex;
u8*         g_tramp_page     = nullptr;
size_t      g_tramp_used     = 0;
constexpr size_t kTrampPageSize = 64 * 1024;
constexpr size_t kTrampChunk    = 224; // LandingBlock (128) + code (~80), aligned

u64 EmitLandingTrampoline(const GuestUnwindContext& ctx) {
    std::lock_guard<std::mutex> lk(g_tramp_mutex);
    if (!g_tramp_page || g_tramp_used + kTrampChunk > kTrampPageSize) {
        void* page = VirtualAlloc(nullptr, kTrampPageSize,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!page) {
            LOG_ERROR(HLE, "unwind: VirtualAlloc for landing trampolines failed (%lu)", GetLastError());
            return 0;
        }
        g_tramp_page = static_cast<u8*>(page);
        g_tramp_used = 0;
    }
    u8* chunk = g_tramp_page + g_tramp_used;
    g_tramp_used += kTrampChunk;

    LandingBlock* blk = reinterpret_cast<LandingBlock*>(chunk);
    blk->rax = ctx.gr[0];  blk->rdx = ctx.gr[1];  blk->rcx = ctx.gr[2];
    blk->rbx = ctx.gr[3];  blk->rsi = ctx.gr[4];  blk->rdi = ctx.gr[5];
    blk->rbp = ctx.gr[6];  blk->rsp = ctx.gr[7];
    blk->r8  = ctx.gr[8];  blk->r9  = ctx.gr[9];  blk->r10 = ctx.gr[10];
    blk->r12 = ctx.gr[12]; blk->r13 = ctx.gr[13];
    blk->r14 = ctx.gr[14]; blk->r15 = ctx.gr[15];
    blk->rip = ctx.gr[16];

    u8* c = chunk + 128;
    size_t n = 0;
    // mov r11, imm64 (LandingBlock*)
    c[n++] = 0x49; c[n++] = 0xBB;
    const u64 blk_addr = reinterpret_cast<u64>(blk);
    std::memcpy(c + n, &blk_addr, 8); n += 8;
    // mov r64, [r11+disp8] encodings (REX, 8B, ModRM mod=01 r/m=011, disp8)
    static const u8 kLoad[15][3] = {
        {0x49, 0x8B, 0x43}, // rax  [r11+0]
        {0x49, 0x8B, 0x53}, // rdx  [r11+8]
        {0x49, 0x8B, 0x4B}, // rcx  [r11+16]
        {0x49, 0x8B, 0x5B}, // rbx  [r11+24]
        {0x49, 0x8B, 0x73}, // rsi  [r11+32]
        {0x49, 0x8B, 0x7B}, // rdi  [r11+40]
        {0x49, 0x8B, 0x6B}, // rbp  [r11+48]
        {0x4D, 0x8B, 0x43}, // r8   [r11+64]
        {0x4D, 0x8B, 0x4B}, // r9   [r11+72]
        {0x4D, 0x8B, 0x53}, // r10  [r11+80]
        {0x4D, 0x8B, 0x63}, // r12  [r11+88]
        {0x4D, 0x8B, 0x6B}, // r13  [r11+96]
        {0x4D, 0x8B, 0x73}, // r14  [r11+104]
        {0x4D, 0x8B, 0x7B}, // r15  [r11+112]
        {0x4D, 0x8B, 0x5B}, // r11  [r11+120]  (rip — loaded last)
    };
    static const u8 kDisp[15] = {0, 8, 16, 24, 32, 40, 48, 64, 72, 80, 88, 96, 104, 112, 120};
    // Restore everything except rsp first; rsp second-to-last; rip into r11 last.
    for (int i = 0; i < 7; ++i) {
        c[n++] = kLoad[i][0]; c[n++] = kLoad[i][1]; c[n++] = kLoad[i][2]; c[n++] = kDisp[i];
    }
    for (int i = 7; i < 14; ++i) {
        c[n++] = kLoad[i][0]; c[n++] = kLoad[i][1]; c[n++] = kLoad[i][2]; c[n++] = kDisp[i];
    }
    // mov rsp, [r11+56]
    c[n++] = 0x49; c[n++] = 0x8B; c[n++] = 0x63; c[n++] = 56;
    // mov r11, [r11+120]  (landing pad rip)
    c[n++] = kLoad[14][0]; c[n++] = kLoad[14][1]; c[n++] = kLoad[14][2]; c[n++] = kDisp[14];
    // jmp r11
    c[n++] = 0x41; c[n++] = 0xFF; c[n++] = 0xE3;
    FlushInstructionCache(GetCurrentProcess(), c, n);
    return reinterpret_cast<u64>(c);
}

// Overwrites the guest return-address slot the dispatcher will `ret` through
// with a landing trampoline that resumes at ctx.gr[16] with ctx's registers.
bool InstallContext(const GuestUnwindContext& ctx, u64 ret_slot) {
    const u64 tramp = EmitLandingTrampoline(ctx);
    if (!tramp) return false;
    __try {
        *reinterpret_cast<u64*>(ret_slot) = tramp;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR(HLE, "unwind: cannot write return slot 0x%llx", ret_slot);
        return false;
    }
    LOG_DEBUG(HLE, "unwind: install context rip=0x%llx rsp=0x%llx via trampoline 0x%llx",
              ctx.gr[16], ctx.gr[7], tramp);
    return true;
}

// --- Native LSDA personality (imported __gxx_personality_v0) -----------------
// When the guest imports its personality routine instead of statically
// linking libc++abi, CIE personality pointers resolve to one of our HLE
// thunk stubs.  Calling that stub from inside the unwinder cannot do LSDA
// type matching (the stub only knows how to say "continue"), so we evaluate
// the gcc LSDA format natively here, mirroring __gxx_personality_v0
// (libstdc++ eh_personality.cc): scan the call-site table for the IP, walk
// the action records, and match catch types against the thrown type_info.
struct LsdaResult {
    bool has_landing_pad = false; // a call-site covers the IP and has an lp
    u64  landing_pad     = 0;
    u64  switch_value    = 0;     // matched catch filter (0 = cleanup only)
    bool handler_found   = false; // a catch clause matches the thrown type
};

// Type-table entry #i (1-based) from the gcc TType table, which grows
// backwards from the end of the class-info area.
u64 ReadTtypeEntry(u64 ttype_table_end, u8 ttype_enc, u64 i, bool* ok) {
    const u64 sz = EncodedSize(ttype_enc);
    if (sz == 0) {
        *ok = false;
        return 0;
    }
    u64 p = ttype_table_end - i * sz;
    return ReadEncodedPointer(&p, ttype_enc, ok);
}

// thrown_tinfo: std::type_info* of the exception being unwound (0 = unknown,
// in which case only catch(...) matches).
LsdaResult EvalLsda(u64 lsda, u64 ip, u64 region_start, bool is_signal, u64 thrown_tinfo) {
    LsdaResult res;
    if (!lsda) return res;
    bool ok = true;
    u64 p = lsda;
    const u8 lp_enc = Rd8(p++, &ok);
    u64 lp_base = 0;
    if (lp_enc != kEncOmit) {
        lp_base = ReadEncodedPointer(&p, lp_enc, &ok);
        if (!ok) return res;
    }
    if (lp_base == 0) lp_base = region_start;
    const u8 ttype_enc = Rd8(p++, &ok);
    u64 ttype_end = 0;
    if (ttype_enc != kEncOmit) {
        const u64 class_info = ReadUleb(&p, &ok);
        if (!ok) return res;
        ttype_end = p + class_info;
    }
    const u8 cs_enc = Rd8(p++, &ok);
    const u64 cs_table_len = ReadUleb(&p, &ok);
    if (!ok) return res;
    const u64 cs_table_end = p + cs_table_len;
    LOG_DEBUG(HLE, "unwind: lsda hdr: lp_enc=0x%02x lp_base=0x%llx ttype_enc=0x%02x ttype_end=0x%llx cs_enc=0x%02x cs_len=%llu",
             lp_enc, lp_base, ttype_enc, ttype_end, cs_enc, cs_table_len);
    u64 ip_adj = ip;
    if (!is_signal && ip_adj > 0) --ip_adj; // call-site ranges exclude the return addr
    while (p < cs_table_end) {
        const u64 cs_start  = ReadEncodedPointer(&p, cs_enc, &ok);
        const u64 cs_len    = ReadEncodedPointer(&p, cs_enc, &ok);
        const u64 cs_lp     = ReadEncodedPointer(&p, cs_enc, &ok);
        const u64 cs_action = ReadUleb(&p, &ok);
        if (!ok) return res;
        LOG_DEBUG(HLE, "unwind: lsda cs: [0x%llx..0x%llx) lp=0x%llx action=%llu (ip_adj=0x%llx lp_base=0x%llx)",
                 lp_base + cs_start, lp_base + cs_start + cs_len,
                 cs_lp ? lp_base + cs_lp : 0, cs_action, ip_adj, lp_base);
        if (ip_adj < lp_base + cs_start || ip_adj >= lp_base + cs_start + cs_len) continue;
        // This call-site covers the IP.
        if (cs_lp == 0) continue; // no landing pad in this range
        res.has_landing_pad = true;
        res.landing_pad = lp_base + cs_lp;
        if (cs_action == 0) return res; // pure cleanup (switch value 0)
        u64 ar = cs_table_end + cs_action - 1;
        for (int guard = 0; guard < 64; ++guard) {
            bool ok2 = true;
            const s64 filter = ReadSleb(&ar, &ok2);
            const s64 disp   = ReadSleb(&ar, &ok2);
            if (!ok2) break;
            if (filter > 0) {
                bool ok3 = true;
                const u64 entry = ttype_end ? ReadTtypeEntry(ttype_end, ttype_enc,
                                                             static_cast<u64>(filter), &ok3)
                                            : 0;
                LOG_DEBUG(HLE, "unwind: lsda action: filter=%lld ttype=0x%llx ok=%d thrown=0x%llx",
                         (long long)filter, entry, (int)ok3, thrown_tinfo);
                // entry == 0 in the type table is catch(...) — matches anything.
                if (ok3 && (entry == 0 || (thrown_tinfo != 0 && entry == thrown_tinfo))) {
                    res.handler_found = true;
                    res.switch_value = static_cast<u64>(filter);
                    return res;
                }
                if (ok3 && entry != 0) {
                    LOG_INFO(HLE, "unwind: lsda: catch type 0x%llx does not match thrown "
                             "tinfo 0x%llx — clause skipped", entry, thrown_tinfo);
                }
            }
            if (disp == 0) break;
            ar += static_cast<u64>(disp); // disp is relative to just-after the disp field
        }
        return res; // pad exists but no catch matched
    }
    return res;
}

// std::type_info* of a thrown C++ exception, from the __cxa_exception header
// (hdr = ue - 88; exceptionType at hdr + 8 — see the layout constants below).
u64 ReadThrownTinfo(u64 ue) {
    bool ok = true;
    const u64 t = Rd64(ue - 88 + 8, &ok);
    return ok ? t : 0;
}

// --- Itanium unwind state ----------------------------------------------------
constexpr u64 kUaSearchPhase   = 1;
constexpr u64 kUaCleanupPhase  = 2;
constexpr u64 kUaHandlerFrame  = 4;

constexpr u64 kUrcFatalPhase1    = 3;
constexpr u64 kUrcEndOfStack     = 5;
constexpr u64 kUrcHandlerFound   = 6;
constexpr u64 kUrcInstallContext = 7;
constexpr u64 kUrcContinueUnwind = 8;

constexpr u64 kCppExceptionClass = 0x474E5543432B2B00ULL; // "GNUCC++\0"

// SEH guard around the guest personality call.  POD-only body (C2712).
u64 CallGuestPersonalitySeh(u64 personality, u64* args6, bool* faulted) {
    u64 rc = 0;
    __try {
        rc = InvokeGuestFunction6(personality, args6);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return 0;
    }
    *faulted = false;
    return rc;
}

u64 CallPersonality(u64 personality, u64 actions, u64 exc_class, u64 ue,
                    GuestUnwindContext* ctx, bool* call_ok) {
    u64 a[6] = {1, actions, exc_class, ue, reinterpret_cast<u64>(ctx), 0};
    bool faulted = false;
    const u64 rc = CallGuestPersonalitySeh(personality, a, &faulted);
    if (faulted) {
        LOG_ERROR(HLE, "unwind: guest personality 0x%llx faulted", personality);
        *call_ok = false;
        return 0;
    }
    *call_ok = true;
    return rc;
}

// Per-thread phase-2 bookkeeping so _Unwind_Resume (re-entered from cleanup
// landing pads) can continue the in-flight unwind for a given exception.
struct Phase2Entry {
    u64 ue;
    u64 handler_cfa;
};
thread_local std::vector<Phase2Entry> g_phase2;

// Terminate-on-unhandled: matches std::terminate semantics for an exception
// that no frame catches (or a fatal unwind error).
[[noreturn]] void UnwindFatal(const char* why, u64 ue) {
    LOG_ERROR(HLE, "unwind: %s (ue=0x%llx) — terminating guest (std::terminate semantics)",
              why, ue);
    ExitGuestProcess(134); // 128+SIGABRT, like a native abort()
}

// Runs the phase-2 loop from `ctx`.  On success installs a context and never
// "returns to guest" in the usual sense (the handler returns 0 and the
// dispatcher rets into the landing trampoline).
void RunPhase2(u64 ue, u64 handler_cfa, GuestUnwindContext ctx, u64 ret_slot) {
    for (u64 depth = 0; depth < 4096; ++depth) {
        FrameEval fe;
        if (!EvalFrame(ctx, fe)) {
            UnwindFatal("phase 2 ran out of frames (FDE missing)", ue);
        }
        ctx.cfa = fe.cfa;
        ctx.lsda = fe.lsda;
        ctx.region_start = fe.pc_begin;
        const bool is_handler = (fe.cfa == handler_cfa);
        const u64 actions = kUaCleanupPhase | (is_handler ? kUaHandlerFrame : 0);
        if (fe.has_personality && HLE::IsHleThunkAddress(fe.personality)) {
            // Imported personality: evaluate the LSDA natively instead of
            // calling our own stub back as guest code.
            const LsdaResult lr = EvalLsda(fe.lsda, ctx.gr[16], fe.pc_begin,
                                           fe.signal_frame, ReadThrownTinfo(ue));
            LOG_INFO(HLE, "unwind: phase 2 native personality lsda=0x%llx -> lp=0x%llx switch=%llu%s",
                     fe.lsda, lr.landing_pad, lr.switch_value, is_handler ? " (handler)" : "");
            if (lr.has_landing_pad) {
                // x86-64 eh data registers: rax (gr[0]) = exception object,
                // rdx (gr[1]) = handler switch value.
                ctx.gr[0]  = ue;
                ctx.gr[1]  = lr.switch_value;
                ctx.gr[16] = lr.landing_pad;
                if (is_handler) {
                    for (size_t i = 0; i < g_phase2.size(); ++i) {
                        if (g_phase2[i].ue == ue) {
                            g_phase2.erase(g_phase2.begin() + static_cast<long long>(i));
                            break;
                        }
                    }
                }
                if (!InstallContext(ctx, ret_slot)) {
                    UnwindFatal("failed to install landing context", ue);
                }
                return;
            }
            if (is_handler) {
                UnwindFatal("handler frame has no landing pad in LSDA", ue);
            }
        } else if (fe.has_personality) {
            bool call_ok = false;
            const u64 rc = CallPersonality(fe.personality, actions,
                                           kCppExceptionClass, ue, &ctx, &call_ok);
            if (!call_ok) {
                UnwindFatal("personality faulted during cleanup phase", ue);
            }
            if (rc == kUrcInstallContext) {
                if (is_handler) {
                    for (size_t i = 0; i < g_phase2.size(); ++i) {
                        if (g_phase2[i].ue == ue) {
                            g_phase2.erase(g_phase2.begin() + static_cast<long long>(i));
                            break;
                        }
                    }
                }
                if (!InstallContext(ctx, ret_slot)) {
                    UnwindFatal("failed to install landing context", ue);
                }
                return;
            }
            if (rc != kUrcContinueUnwind) {
                UnwindFatal("personality returned fatal code in phase 2", ue);
            }
        } else if (is_handler) {
            UnwindFatal("handler frame has no personality", ue);
        }
        for (int i = 0; i < 17; ++i) ctx.gr[i] = fe.next_gr[i];
    }
    UnwindFatal("phase 2 exceeded 4096 frames (corrupt stack?)", ue);
}

// Full _Unwind_RaiseException: phase 1 (search) then phase 2 (cleanup).
// Returns an _Unwind_Reason_Code on failure (the ABI says the function
// returns to its caller only on error); on success the guest resumes at a
// landing pad and the HLE handler's return value is discarded.
u64 RaiseException(u64 ue, const GuestUnwindContext& initial, u64 ret_slot) {
    if (g_eh_tables.empty()) {
        LOG_ERROR(HLE, "unwind: no .eh_frame_hdr registered — cannot unwind");
        return kUrcEndOfStack;
    }
    GuestUnwindContext ctx = initial;
    u64 handler_cfa = 0;
    bool found = false;
    u64 depth1_rbp = 0, depth1_rbx = 0; // diagnostic: parser frame
    for (u64 depth = 0; depth < 4096; ++depth) {
        FrameEval fe;
        if (!EvalFrame(ctx, fe)) {
            LOG_INFO(HLE, "unwind: phase 1 frame walk ended at depth %llu (no FDE for rip=0x%llx)",
                     depth, ctx.gr[16]);
            break; // end of stack
        }
        if (depth == 1) { depth1_rbp = ctx.gr[6]; depth1_rbx = ctx.gr[3]; }
        ctx.cfa = fe.cfa;
        ctx.lsda = fe.lsda;
        ctx.region_start = fe.pc_begin;
        LOG_INFO(HLE, "unwind: phase 1 depth %llu rip=0x%llx cfa=0x%llx fde=[0x%llx..) personality=0x%llx lsda=0x%llx",
                 depth, ctx.gr[16], fe.cfa, fe.pc_begin,
                 fe.has_personality ? fe.personality : 0, fe.lsda);
        if (fe.has_personality && HLE::IsHleThunkAddress(fe.personality)) {
            // Imported personality (HLE thunk): evaluate the LSDA natively.
            const LsdaResult lr = EvalLsda(fe.lsda, ctx.gr[16], fe.pc_begin,
                                           fe.signal_frame, ReadThrownTinfo(ue));
            LOG_INFO(HLE, "unwind: phase 1 native personality lsda=0x%llx -> handler=%d lp=0x%llx switch=%llu (tid=%lu)",
                     fe.lsda, (int)lr.handler_found, lr.landing_pad, lr.switch_value,
                     GetCurrentThreadId());
            if (lr.handler_found) {
                handler_cfa = fe.cfa;
                found = true;
                break;
            }
        } else if (fe.has_personality) {
            bool call_ok = false;
            const u64 rc = CallPersonality(fe.personality, kUaSearchPhase,
                                           kCppExceptionClass, ue, &ctx, &call_ok);
            if (!call_ok) return kUrcFatalPhase1;
            LOG_INFO(HLE, "unwind: phase 1 personality 0x%llx -> rc=%llu (tid=%lu)",
                     fe.personality, rc, GetCurrentThreadId());
            if (rc == kUrcHandlerFound) {
                handler_cfa = fe.cfa;
                found = true;
                break;
            }
            if (rc != kUrcContinueUnwind) {
                LOG_ERROR(HLE, "unwind: personality returned %llu in search phase", rc);
                return kUrcFatalPhase1;
            }
        }
        for (int i = 0; i < 17; ++i) ctx.gr[i] = fe.next_gr[i];
    }
    if (!found) {
        // Diagnostic: independently scan the raw guest stack for any return
        // address whose frame LSDA WOULD catch this exception.  If this finds
        // a frame the CFI walk missed, the walk (not the game) is at fault.
        {
            const u64 var = initial.gr[3]; // throw-helper rbx = input variant
            bool vok = true;
            const u64 tag = Rd8(var, &vok);
            const u64 val = Rd64(var + 8, &vok);
            LOG_ERROR(HLE, "unwind: failing variant at 0x%llx tag=0x%llx val=0x%llx", var, tag, val);
        }
        const u64 tinfo = ReadThrownTinfo(ue);
        for (u64 sp = initial.gr[7]; sp < initial.gr[7] + 0x8000; sp += 8) {
            bool sok = true;
            const u64 cand = Rd64(sp, &sok);
            if (!sok || cand < 0x800000000ULL || cand >= 0x900000000ULL) continue;
            FdeInfo fde;
            if (!FindFde(cand, fde) || !fde.cie.has_personality || !fde.lsda) continue;
            const LsdaResult lr = EvalLsda(fde.lsda, cand, fde.pc_begin,
                                           fde.cie.signal_frame, tinfo);
            if (lr.handler_found) {
                LOG_ERROR(HLE, "unwind: stack scan found a CATCHING frame the CFI walk missed: "
                          "ret=0x%llx (sp+0x%llx) fde=[0x%llx..) lp=0x%llx switch=%llu",
                          cand, sp - initial.gr[7], fde.pc_begin, lr.landing_pad, lr.switch_value);
            }
        }
        LOG_ERROR(HLE, "unwind: no handler found for exception 0x%llx (end of stack)", ue);
        if (depth1_rbp) {
            bool rok = true;
            const u64 rec = Rd64(depth1_rbp - 0xc8, &rok);
            LOG_ERROR(HLE, "unwind: parser record ptr=0x%llx index*0x178 rbx=0x%llx", rec, depth1_rbx);
            if (rok && rec) {
                const u64 elem = rec + depth1_rbx * 0x178;
                for (u64 q = 0; q < 0x60; q += 8) {
                    bool vok = true;
                    const u64 slot = Rd64(elem + q, &vok);
                    LOG_ERROR(HLE, "unwind:   rec[+0x%02llx] = 0x%llx%s", q, slot, vok ? "" : " (unreadable)");
                }
                // Input array variant lives at [rbp-0x80] in the parser frame.
                for (u64 q = 0; q < 0x60; q += 8) {
                    bool vok = true;
                    const u64 slot = Rd64(depth1_rbp - 0x80 + q, &vok);
                    LOG_ERROR(HLE, "unwind:   arr[+0x%02llx] = 0x%llx%s", q, slot, vok ? "" : " (unreadable)");
                }
                bool aok = true;
                const u64 vec = Rd64(depth1_rbp - 0x80 + 8, &aok);
                for (u64 q = 0; q < 0x120; q += 8) {
                    bool vok = true;
                    const u64 slot = Rd64(depth1_rbp - 0x100 + q, &vok);
                    LOG_ERROR(HLE, "unwind:   frame[rbp-0x100+0x%03llx] = 0x%llx%s", q, slot, vok ? "" : " (unreadable)");
                }
                if (aok && vec) {
                    bool ok1 = true;
                    const u64 begin = Rd64(vec + 8, &ok1);
                    const u64 end   = Rd64(vec + 0x10, &ok1);
                    if (ok1 && begin && end >= begin) {
                        const u64 count = (end - begin) / 16;
                        LOG_ERROR(HLE, "unwind: array vec=0x%llx count=%llu", vec, count);
                        for (u64 e = 0; e < count && e < 20; ++e) {
                            bool ok2 = true, ok3 = true;
                            const u64 tag = Rd8(begin + e * 16, &ok2);
                            const u64 val = Rd64(begin + e * 16 + 8, &ok3);
                            LOG_ERROR(HLE, "unwind:   elem[%llu] tag=0x%llx val=0x%llx", e, tag, val);
                            if (tag == 3 && val) {
                                // tag-3 string: first qword points at the
                                // string object (len-prefixed data).
                                bool pok = true;
                                const u64 so = Rd64(val, &pok);
                                if (pok && so) {
                                    bool lok = true;
                                    const u64 len = Rd64(so, &lok);
                                    LOG_ERROR(HLE, "unwind:     strobj=0x%llx len/flags=0x%llx", so, len);
                                    char s[80] = {};
                                    for (int c = 0; c < 79; ++c) {
                                        bool cok = true;
                                        const char ch = static_cast<char>(Rd8(so + 8 + static_cast<u64>(c), &cok));
                                        if (!cok || ch == '\0') break;
                                        s[c] = (ch >= 32 && ch < 127) ? ch : '?';
                                    }
                                    LOG_ERROR(HLE, "unwind:     = \"%s\"", s);
                                }
                            }
                        }
                    }
                }
                for (u64 q = 0; q < 0x178; q += 8) {
                    bool vok = true;
                    const u64 slot = Rd64(elem + q, &vok);
                    if (!vok || slot < 0x10000 || slot >= (1ULL << 47)) continue;
                    char s[96] = {};
                    int n = 0;
                    for (; n < 95; ++n) {
                        bool cok = true;
                        const char c = static_cast<char>(Rd8(slot + static_cast<u64>(n), &cok));
                        if (!cok || c == '\0') break;
                        if (c < 32 || c >= 127) { n = 0; break; }
                        s[n] = c;
                    }
                    if (n >= 4) {
                        LOG_ERROR(HLE, "unwind:   rec[rbx+0x%llx] -> \"%s\"", q, s);
                    }
                }
            }
        }
        return kUrcEndOfStack;
    }
    Phase2Entry entry{ue, handler_cfa};
    g_phase2.push_back(entry);
    RunPhase2(ue, handler_cfa, initial, ret_slot);
    return kUrcContinueUnwind; // unreachable unless RunPhase2 returns
}

// --- __cxa_exception layout (libc++abi, LP64) --------------------------------
// Offsets from the header start (header sits 120 bytes before the user ptr):
constexpr u64 kCxaHdrSize         = 120;
constexpr u64 kOffExceptionType   = 8;
constexpr u64 kOffExceptionDtor   = 16;
constexpr u64 kOffHandlerCount    = 48; // int
constexpr u64 kOffAdjustedPtr     = 80;
constexpr u64 kOffUnwindHeader    = 88; // _Unwind_Exception (32 bytes), last
constexpr u64 kAllocPrefix        = 128; // user_ptr = heap_base + 128 (16-aligned)

struct CaughtEntry {
    u64 ue;
    u64 user;
};
thread_local std::vector<CaughtEntry> g_caught;
thread_local u64 g_uncaught_exceptions = 0;

u64 CxaAllocateException(u64 size) {
    const u64 aligned = (size + 15) & ~15ULL;
    const u64 base = HeapAlloc(aligned + kAllocPrefix, 16);
    if (!base) {
        LOG_ERROR(HLE, "__cxa_allocate_exception(%llu): out of guest heap", size);
        return 0;
    }
    // Zero the 120-byte header + 32-byte unwind header.
    bool ok = true;
    for (u64 off = 8; off < kAllocPrefix; off += 8) {
        __try {
            *reinterpret_cast<u64*>(base + off) = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; break; }
    }
    if (!ok) {
        HeapFree(base);
        return 0;
    }
    const u64 user = base + kAllocPrefix;
    // referenceCount (offset 0) = 1, matching libc++abi for a fresh exception.
    __try {
        *reinterpret_cast<u64*>(base + 8) = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        HeapFree(base);
        return 0;
    }
    LOG_DEBUG(HLE, "__cxa_allocate_exception(%llu) -> 0x%llx", size, user);
    return user;
}

void CxaFreeException(u64 user) {
    if (!user) return;
    HeapFree(user - kAllocPrefix);
}

// Dumps a guest memory range to a file (diagnostics; SEH-guarded per page).
void DebugDumpGuestMemory(u64 base, u64 size, const char* path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return;
    for (u64 off = 0; off < size; off += 0x1000) {
        char page[0x1000];
        __try {
            std::memcpy(page, reinterpret_cast<void*>(base + off), 0x1000);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::memset(page, 0, sizeof(page));
        }
        std::fwrite(page, 1, 0x1000, f);
    }
    std::fclose(f);
}

// --- __cxa_* / _Unwind_* HLE handlers ----------------------------------------

// Guard-variable state for __cxa_guard_*: thread-safe static-init interlock
// (same semantics as SharpEmu's CxaGuardExports and the Itanium ABI: first
// byte 1 = initialized; an in-progress init makes other threads wait).
std::mutex              g_guard_mutex;
std::condition_variable g_guard_cv;
std::unordered_map<u64, std::thread::id> g_guard_in_progress;

auto CxaGuardAcquireImpl = [](const GuestArgs& args) -> u64 {
    const u64 guard = args.arg1;
    if (!guard) {
        LOG_WARN(HLE, "__cxa_guard_acquire(null) -> 0");
        return 0;
    }
    std::unique_lock<std::mutex> lk(g_guard_mutex);
    for (;;) {
        bool ok = true;
        const u8 done = Rd8(guard, &ok);
        if (!ok) {
            LOG_ERROR(HLE, "__cxa_guard_acquire(0x%llx): unreadable guard", guard);
            return 0;
        }
        if (done != 0) return 0; // already initialized
        const auto it = g_guard_in_progress.find(guard);
        if (it == g_guard_in_progress.end()) {
            g_guard_in_progress[guard] = std::this_thread::get_id();
            LOG_DEBUG(HLE, "__cxa_guard_acquire(0x%llx) -> 1 (initializing)", guard);
            return 1;
        }
        if (it->second == std::this_thread::get_id()) {
            // Recursive init on the same thread: the ABI leaves this
            // undefined; report "already initializing" rather than deadlock.
            LOG_WARN(HLE, "__cxa_guard_acquire(0x%llx): recursive acquisition", guard);
            return 0;
        }
        g_guard_cv.wait(lk);
    }
};

// SEH-guarded single-byte write (POD-only so __try is legal, C2712).
bool SafeWrite8(u64 addr, u8 v) {
    __try {
        *reinterpret_cast<u8*>(addr) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

auto CxaGuardReleaseImpl = [](const GuestArgs& args) -> u64 {
    const u64 guard = args.arg1;
    if (!guard) {
        LOG_WARN(HLE, "__cxa_guard_release(null)");
        return 0;
    }
    {
        std::lock_guard<std::mutex> lk(g_guard_mutex);
        if (!SafeWrite8(guard, 1)) {
            LOG_ERROR(HLE, "__cxa_guard_release(0x%llx): unwritable guard", guard);
            return 0;
        }
        g_guard_in_progress.erase(guard);
    }
    g_guard_cv.notify_all();
    LOG_DEBUG(HLE, "__cxa_guard_release(0x%llx) -> done", guard);
    return 0;
};

auto CxaGuardAbortImpl = [](const GuestArgs& args) -> u64 {
    const u64 guard = args.arg1;
    if (!guard) {
        LOG_WARN(HLE, "__cxa_guard_abort(null)");
        return 0;
    }
    {
        std::lock_guard<std::mutex> lk(g_guard_mutex);
        if (!SafeWrite8(guard, 0)) {
            LOG_ERROR(HLE, "__cxa_guard_abort(0x%llx): unwritable guard", guard);
            return 0;
        }
        g_guard_in_progress.erase(guard);
    }
    g_guard_cv.notify_all();
    LOG_DEBUG(HLE, "__cxa_guard_abort(0x%llx) -> reset", guard);
    return 0;
};

// __cxa_allocate_exception(size) — user pointer with the libc++abi header
// living in the 120 bytes before it (see layout constants above).
auto CxaAllocateExceptionImpl = [](const GuestArgs& args) -> u64 {
    return CxaAllocateException(args.arg1);
};

auto CxaFreeExceptionImpl = [](const GuestArgs& args) -> u64 {
    CxaFreeException(args.arg1);
    return 0;
};

auto CxaAllocateDependentExceptionImpl = [](const GuestArgs& args) -> u64 {
    // Dependent exceptions (rethrown nested) share the primary object; a
    // same-shaped allocation is sufficient for the throw paths we support.
    LOG_DEBUG(HLE, "__cxa_allocate_dependent_exception(%llu)", args.arg1);
    return CxaAllocateException(args.arg1 ? args.arg1 : 16);
};

// __cxa_throw(thrown_exception, tinfo, dest) — never returns on success:
// the guest resumes at a landing pad via the landing trampoline.
auto CxaThrowImpl = [](const GuestArgs& args) -> u64 {
    const u64 user  = args.arg1;
    const u64 tinfo = args.arg2;
    const u64 dtor  = args.arg3;
    if (!user) {
        LOG_ERROR(HLE, "__cxa_throw(null, ...) — ignoring");
        return 0;
    }
    const u64 hdr = user - kCxaHdrSize;
    const u64 ue  = user - (kCxaHdrSize - kOffUnwindHeader); // == user - 32
    __try {
        *reinterpret_cast<u64*>(hdr + kOffExceptionType) = tinfo;
        *reinterpret_cast<u64*>(hdr + kOffExceptionDtor) = dtor;
        *reinterpret_cast<u64*>(hdr + kOffAdjustedPtr)   = user;
        *reinterpret_cast<u32*>(hdr + kOffHandlerCount)  = 0;
        *reinterpret_cast<u64*>(ue + 0)  = kCppExceptionClass;
        *reinterpret_cast<u64*>(ue + 8)  = 0; // exception_cleanup
        *reinterpret_cast<u64*>(ue + 16) = 0; // private_1
        *reinterpret_cast<u64*>(ue + 24) = 0; // private_2
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR(HLE, "__cxa_throw(0x%llx): exception header not writable (bad pointer?)", user);
        return 0;
    }
    ++g_uncaught_exceptions;
    LOG_DEBUG(HLE, "__cxa_throw(obj: 0x%llx, tinfo: 0x%llx, dtor: 0x%llx)", user, tinfo, dtor);
    static bool g_dumped_text = false;
    if (!g_dumped_text) {
        g_dumped_text = true;
        DebugDumpGuestMemory(0x800000000ULL, 0x600000, ".work/guest_text.bin");
    }
    {
        // Diagnostic: dump the thrown type name (type_info: [vtable][name]).
        bool tok = true;
        const u64 name_ptr = Rd64(tinfo + 8, &tok);
        if (tok && name_ptr) {
            char tname[64] = {};
            for (int i = 0; i < 63; ++i) {
                bool cok = true;
                const char c = static_cast<char>(Rd8(name_ptr + static_cast<u64>(i), &cok));
                if (!cok || c == '\0') break;
                tname[i] = (c >= 32 && c < 127) ? c : '?';
            }
            LOG_INFO(HLE, "__cxa_throw type: '%s' (tinfo=0x%llx)", tname, tinfo);
        } else {
            LOG_INFO(HLE, "__cxa_throw tinfo=0x%llx unreadable name (dtor=0x%llx)", tinfo, dtor);
        }
        for (int q = 0; q < 4; ++q) {
            bool qok = true;
            LOG_INFO(HLE, "  tinfo[+%d] = 0x%llx", q * 8, Rd64(tinfo + static_cast<u64>(q * 8), &qok));
        }
        for (int q = 0; q < 4; ++q) {
            bool qok = true;
            LOG_INFO(HLE, "  obj[+%d] = 0x%llx", q * 8, Rd64(user + static_cast<u64>(q * 8), &qok));
        }
        // Try to print any string pointers inside the exception object
        // (GameMaker-style error messages travel in the thrown object).
        for (int q = 0; q < 8; ++q) {
            bool vok = true;
            const u64 slot = Rd64(user + static_cast<u64>(q * 8), &vok);
            if (!vok || slot < 0x10000 || slot >= (1ULL << 47)) continue;
            char s[80] = {};
            int n = 0;
            for (; n < 79; ++n) {
                bool cok = true;
                const char c = static_cast<char>(Rd8(slot + static_cast<u64>(n), &cok));
                if (!cok || c == '\0') break;
                if (c < 32 || c >= 127) { n = 0; break; }
                s[n] = c;
            }
            if (n >= 4) {
                LOG_INFO(HLE, "  obj[+%d] -> string: \"%s\"", q * 8, s);
            }
        }
    }

    GuestUnwindContext ctx;
    if (!args.stack_args || !ReconstructContext(args, ctx)) {
        LOG_ERROR(HLE, "__cxa_throw: cannot reconstruct unwind context — throwing is not possible here");
        return 0;
    }
    const u64 rc = RaiseException(ue, ctx, args.stack_args - 8);
    if (rc == kUrcEndOfStack) {
        UnwindFatal("no catch handler found for thrown exception", ue);
    }
    // Fatal phase-1 error: already logged by RaiseException.
    return 0;
};

// __cxa_begin_catch(unwind_exception*) -> user object pointer.
auto CxaBeginCatchImpl = [](const GuestArgs& args) -> u64 {
    const u64 ue = args.arg1;
    if (!ue) {
        LOG_WARN(HLE, "__cxa_begin_catch(null) -> 0");
        return 0;
    }
    bool ok = true;
    const u64 cls = Rd64(ue, &ok);
    if (!ok) {
        LOG_ERROR(HLE, "__cxa_begin_catch(0x%llx): unreadable header", ue);
        return 0;
    }
    if ((cls & 0xFFFFFFFFFFFFFF00ULL) != kCppExceptionClass) {
        LOG_WARN(HLE, "__cxa_begin_catch(0x%llx): foreign exception class 0x%llx", ue, cls);
    }
    const u64 hdr_start = ue + 32 - kCxaHdrSize; // header start = ue - 88
    u64 user = 0;
    u32 handler_count = 0;
    __try {
        handler_count = *reinterpret_cast<u32*>(hdr_start + kOffHandlerCount);
        *reinterpret_cast<u32*>(hdr_start + kOffHandlerCount) = handler_count + 1;
        user = *reinterpret_cast<u64*>(hdr_start + kOffAdjustedPtr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR(HLE, "__cxa_begin_catch(0x%llx): header not writable", ue);
        return 0;
    }
    if (user == 0) user = ue + 32; // fallback for non-native layout
    g_caught.push_back({ue, user});
    if (g_uncaught_exceptions > 0) --g_uncaught_exceptions;
    LOG_DEBUG(HLE, "__cxa_begin_catch(0x%llx) -> 0x%llx (handlerCount=%u)", ue, user, handler_count + 1);
    return user;
};

// __cxa_end_catch() — pop the caught stack; free when the last handler exits.
auto CxaEndCatchImpl = [](const GuestArgs& /*args*/) -> u64 {
    if (g_caught.empty()) {
        LOG_WARN(HLE, "__cxa_end_catch: caught-exception stack is empty — ignoring");
        return 0;
    }
    const CaughtEntry entry = g_caught.back();
    g_caught.pop_back();
    const u64 hdr_start = entry.ue + 32 - kCxaHdrSize;
    u32 handler_count = 1;
    __try {
        handler_count = *reinterpret_cast<u32*>(hdr_start + kOffHandlerCount);
        if (handler_count > 0) {
            *reinterpret_cast<u32*>(hdr_start + kOffHandlerCount) = handler_count - 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR(HLE, "__cxa_end_catch: header 0x%llx unreadable", entry.ue);
        return 0;
    }
    if (handler_count <= 1) {
        // Last handler: destroy bookkeeping (the object's destructor runs in
        // guest code at the end of the catch clause) and free the allocation.
        LOG_DEBUG(HLE, "__cxa_end_catch: freeing exception ue=0x%llx", entry.ue);
        HeapFree(entry.ue + 32 - kAllocPrefix);
    } else {
        LOG_DEBUG(HLE, "__cxa_end_catch: ue=0x%llx handlerCount=%u (still caught)",
                  entry.ue, handler_count - 1);
    }
    return 0;
};

// __cxa_rethrow() — re-raise the exception on top of the caught stack.
auto CxaRethrowImpl = [](const GuestArgs& args) -> u64 {
    if (g_caught.empty()) {
        LOG_ERROR(HLE, "__cxa_rethrow with no caught exception — ignoring");
        return 0;
    }
    const u64 ue = g_caught.back().ue;
    LOG_DEBUG(HLE, "__cxa_rethrow(ue=0x%llx)", ue);
    GuestUnwindContext ctx;
    if (!args.stack_args || !ReconstructContext(args, ctx)) {
        LOG_ERROR(HLE, "__cxa_rethrow: cannot reconstruct unwind context");
        return 0;
    }
    ++g_uncaught_exceptions;
    const u64 rc = RaiseException(ue, ctx, args.stack_args - 8);
    if (rc == kUrcEndOfStack) {
        UnwindFatal("no catch handler found for rethrown exception", ue);
    }
    return 0;
};

auto CxaGetExceptionPtrImpl = [](const GuestArgs& args) -> u64 {
    const u64 ue = args.arg1;
    if (!ue) return 0;
    bool ok = true;
    const u64 user = Rd64(ue + 32 - kCxaHdrSize + kOffAdjustedPtr, &ok);
    if (!ok || !user) return ue + 32;
    return user;
};

auto CxaCurrentExceptionTypeImpl = [](const GuestArgs& /*args*/) -> u64 {
    if (g_caught.empty()) return 0;
    const u64 hdr_start = g_caught.back().ue + 32 - kCxaHdrSize;
    bool ok = true;
    const u64 tinfo = Rd64(hdr_start + kOffExceptionType, &ok);
    return ok ? tinfo : 0;
};

// __cxa_get_globals{,_fast} -> &__cxa_eh_globals {caughtExceptions,
// uncaughtExceptions}.  Per-thread storage carved from the guest heap so the
// guest may legitimately read/write it.
struct GuestEhGlobals {
    u64 caught_exceptions;
    u32 uncaught_exceptions;
    u32 pad;
};
auto CxaGetGlobalsImpl = [](const GuestArgs& /*args*/) -> u64 {
    thread_local u64 t_globals = 0;
    if (!t_globals) {
        t_globals = HeapAlloc(sizeof(GuestEhGlobals), 16);
        if (t_globals) {
            bool ok = true;
            __try {
                auto* g = reinterpret_cast<GuestEhGlobals*>(t_globals);
                g->caught_exceptions = 0;
                g->uncaught_exceptions = 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
            if (!ok) { HeapFree(t_globals); t_globals = 0; }
        }
    }
    return t_globals;
};

// std::terminate-style entry points: on a real system these print and abort.
auto CxaTerminateLikeImpl = [](const GuestArgs& args) -> u64 {
    LOG_ERROR(HLE, "guest called a terminate-path C++ ABI function (arg1=0x%llx) — aborting guest",
              args.arg1);
    ExitGuestProcess(134);
};

// __gxx_personality_v0 — only reachable if the guest imports the personality
// instead of statically linking libc++abi (its own CIEs then point at our
// thunk).  When the unwinder sees such a thunk personality it evaluates the
// frame LSDA natively (EvalLsda above) and never calls this stub; a direct
// guest call here reports "no handler" in both phases so unwinding continues
// sanely.
auto GxxPersonalityImpl = [](const GuestArgs& args) -> u64 {
    const u64 actions = args.arg2;
    if ((actions & kUaSearchPhase) != 0) {
        LOG_WARN(HLE, "__gxx_personality_v0 (HLE): search phase -> CONTINUE_UNWIND "
                 "(HLE personality does not parse LSDA; catches in this frame are skipped)");
    } else {
        LOG_WARN(HLE, "__gxx_personality_v0 (HLE): cleanup phase -> CONTINUE_UNWIND "
                 "(cleanups in this frame are skipped)");
    }
    return kUrcContinueUnwind;
};

// --- _Unwind_* handlers --------------------------------------------------------
auto UnwindRaiseExceptionImpl = [](const GuestArgs& args) -> u64 {
    const u64 ue = args.arg1;
    if (!ue) return kUrcFatalPhase1;
    GuestUnwindContext ctx;
    if (!args.stack_args || !ReconstructContext(args, ctx)) {
        LOG_ERROR(HLE, "_Unwind_RaiseException: cannot reconstruct context");
        return kUrcFatalPhase1;
    }
    return RaiseException(ue, ctx, args.stack_args - 8);
};

auto UnwindResumeImpl = [](const GuestArgs& args) -> u64 {
    const u64 ue = args.arg1;
    if (!ue) {
        LOG_WARN(HLE, "_Unwind_Resume(null) — ignoring");
        return 0;
    }
    u64 handler_cfa = 0;
    bool known = false;
    for (size_t i = 0; i < g_phase2.size(); ++i) {
        if (g_phase2[i].ue == ue) { handler_cfa = g_phase2[i].handler_cfa; known = true; break; }
    }
    if (!known) {
        LOG_ERROR(HLE, "_Unwind_Resume(0x%llx): no phase-2 state for this exception "
                  "(Resume without a prior RaiseException?)", ue);
        return 0;
    }
    GuestUnwindContext ctx;
    if (!args.stack_args || !ReconstructContext(args, ctx)) {
        LOG_ERROR(HLE, "_Unwind_Resume: cannot reconstruct context");
        return 0;
    }
    RunPhase2(ue, handler_cfa, ctx, args.stack_args - 8);
    return 0;
};

GuestUnwindContext* ValidateCtx(u64 addr, const char* fn) {
    if (!addr) {
        LOG_WARN(HLE, "%s: null context", fn);
        return nullptr;
    }
    bool ok = true;
    const u64 magic = Rd64(addr, &ok);
    if (!ok || magic != kUnwindCtxMagic) {
        LOG_WARN(HLE, "%s: context 0x%llx is not ours (bad magic)", fn, addr);
        return nullptr;
    }
    return reinterpret_cast<GuestUnwindContext*>(addr);
}

auto UnwindGetGRImpl = [](const GuestArgs& args) -> u64 {
    GuestUnwindContext* ctx = ValidateCtx(args.arg1, "_Unwind_GetGR");
    if (!ctx) return 0;
    const u64 idx = args.arg2;
    if (idx > 16) {
        LOG_WARN(HLE, "_Unwind_GetGR: index %llu out of range", idx);
        return 0;
    }
    return ctx->gr[idx];
};

auto UnwindSetGRImpl = [](const GuestArgs& args) -> u64 {
    GuestUnwindContext* ctx = ValidateCtx(args.arg1, "_Unwind_SetGR");
    if (!ctx) return 0;
    const u64 idx = args.arg2;
    if (idx > 16) {
        LOG_WARN(HLE, "_Unwind_SetGR: index %llu out of range", idx);
        return 0;
    }
    ctx->gr[idx] = args.arg3;
    return 0;
};

auto UnwindGetIPImpl = [](const GuestArgs& args) -> u64 {
    GuestUnwindContext* ctx = ValidateCtx(args.arg1, "_Unwind_GetIP");
    return ctx ? ctx->gr[16] : 0;
};

auto UnwindGetIPInfoImpl = [](const GuestArgs& args) -> u64 {
    GuestUnwindContext* ctx = ValidateCtx(args.arg1, "_Unwind_GetIPInfo");
    if (!ctx) return 0;
    if (args.arg2) {
        __try {
            *reinterpret_cast<u32*>(args.arg2) = 0; // ip_before_insn: unknown
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    return ctx->gr[16];
};

auto UnwindSetIPImpl = [](const GuestArgs& args) -> u64 {
    GuestUnwindContext* ctx = ValidateCtx(args.arg1, "_Unwind_SetIP");
    if (!ctx) return 0;
    ctx->gr[16] = args.arg2;
    return 0;
};

auto UnwindGetLSDAImpl = [](const GuestArgs& args) -> u64 {
    GuestUnwindContext* ctx = ValidateCtx(args.arg1, "_Unwind_GetLanguageSpecificData");
    return ctx ? ctx->lsda : 0;
};

auto UnwindGetRegionStartImpl = [](const GuestArgs& args) -> u64 {
    GuestUnwindContext* ctx = ValidateCtx(args.arg1, "_Unwind_GetRegionStart");
    return ctx ? ctx->region_start : 0;
};

auto UnwindGetCFAImpl = [](const GuestArgs& args) -> u64 {
    GuestUnwindContext* ctx = ValidateCtx(args.arg1, "_Unwind_GetCFA");
    return ctx ? ctx->cfa : 0;
};

auto UnwindGetTextRelBaseImpl = [](const GuestArgs& /*args*/) -> u64 {
    return g_eh_tables.empty() ? 0 : g_eh_tables.front().addr;
};

auto UnwindGetDataRelBaseImpl = [](const GuestArgs& /*args*/) -> u64 {
    return 0; // flat guest map: datarel pointers resolve against the hdr (see ReadEncodedPointer)
};

auto UnwindDeleteExceptionImpl = [](const GuestArgs& args) -> u64 {
    const u64 ue = args.arg1;
    if (!ue) return 0;
    bool ok = true;
    const u64 cls = Rd64(ue, &ok);
    if (ok && (cls & 0xFFFFFFFFFFFFFF00ULL) == kCppExceptionClass) {
        LOG_DEBUG(HLE, "_Unwind_DeleteException(0x%llx)", ue);
        HeapFree(ue + 32 - kAllocPrefix);
    } else {
        LOG_WARN(HLE, "_Unwind_DeleteException(0x%llx): not one of ours — ignoring", ue);
    }
    return 0;
};

// Log-and-survive stubs for the corners of the unwind API we do not implement.
auto UnwindUnsupportedImpl = [](const GuestArgs& args) -> u64 {
    LOG_WARN(HLE, "unsupported _Unwind_* call (ctx/arg 0x%llx, 0x%llx) -> 0",
             args.arg1, args.arg2);
    return 0;
};

void RegisterCxxAbiSymbols() {
    LOG_INFO(HLE, "Registering C++ ABI / unwind HLE symbols...");

    for (const char* module : {"libSceLibcInternal", "libc"}) {
        auto reg = [&](const char* name, const char* nid, const HleHandler& fn) {
            RegisterSymbol(module, name, fn);
            if (nid) {
                RegisterSymbol(module, std::string(nid) + "#T#T", fn);
                RegisterSymbol(module, nid, fn);
            }
        };

        reg("__cxa_allocate_exception", "cfAXurvfl5o", CxaAllocateExceptionImpl);
        reg("__cxa_free_exception", "nOIEswYD4Ig", CxaFreeExceptionImpl);
        reg("__cxa_allocate_dependent_exception", "IJKVjsmxxWI", CxaAllocateDependentExceptionImpl);
        reg("__cxa_free_dependent_exception", "kBxt5LwtLA4", CxaFreeExceptionImpl);
        reg("__cxa_throw", "vkuuLfhnSZI", CxaThrowImpl);
        reg("__cxa_rethrow", "ZL9FV4mJXxo", CxaRethrowImpl);
        reg("__cxa_begin_catch", "3cUUypQzMiI", CxaBeginCatchImpl);
        reg("__cxa_end_catch", "lX+4FNUklF0", CxaEndCatchImpl);
        reg("__cxa_get_exception_ptr", "Y6Sl4Xw7gfA", CxaGetExceptionPtrImpl);
        reg("__cxa_current_exception_type", "BxPeH9TTcs4", CxaCurrentExceptionTypeImpl);
        reg("__cxa_get_globals", "3rJJb81CDM4", CxaGetGlobalsImpl);
        reg("__cxa_get_globals_fast", "uCRed7SvX5E", CxaGetGlobalsImpl);
        reg("__gxx_personality_v0", "XwLA5cTHjt4", GxxPersonalityImpl);

        reg("__cxa_guard_acquire", "3GPpjQdAMTw", CxaGuardAcquireImpl);
        reg("__cxa_guard_release", "9rAeANT2tyE", CxaGuardReleaseImpl);
        reg("__cxa_guard_abort", "2emaaluWzUw", CxaGuardAbortImpl);

        // Terminate paths: abort-path functions must not silently return.
        reg("__cxa_pure_virtual", "zr094EQ39Ww", CxaTerminateLikeImpl);
        reg("__cxa_deleted_virtual", "Rr1kQN/xweU", CxaTerminateLikeImpl);
        reg("__cxa_call_unexpected", "usKbuvy2hQg", CxaTerminateLikeImpl);
        reg("__cxa_call_terminate", "1FXBkxtWpJE", CxaTerminateLikeImpl);
        reg("__cxa_bad_cast", "pBxafllkvt0", CxaTerminateLikeImpl);
        reg("__cxa_bad_typeid", "xcc6DTcL8QA", CxaTerminateLikeImpl);
        reg("__cxa_throw_bad_array_new_length", "XbAd9Ach9mo", CxaTerminateLikeImpl);

        // _Unwind_* level-1 ABI.
        reg("_Unwind_RaiseException", "wpNJwmDDtxw", UnwindRaiseExceptionImpl);
        reg("_Unwind_Resume", "f1zwJ3jAI2k", UnwindResumeImpl);
        reg("_Unwind_Resume_or_Rethrow", "xUsJSLsdv9I", UnwindResumeImpl);
        reg("_Unwind_GetGR", "oQpl5qCT014", UnwindGetGRImpl);
        reg("_Unwind_SetGR", "Xr4pio56GGQ", UnwindSetGRImpl);
        reg("_Unwind_GetIP", "sETNbyWsEHs", UnwindGetIPImpl);
        reg("_Unwind_GetIPInfo", "5/3wdp3FVtc", UnwindGetIPInfoImpl);
        reg("_Unwind_SetIP", "BWAtsvaXToA", UnwindSetIPImpl);
        reg("_Unwind_GetLanguageSpecificData", "gpc4YXZffkQ", UnwindGetLSDAImpl);
        reg("_Unwind_GetRegionStart", "p3BVovMPEE4", UnwindGetRegionStartImpl);
        reg("_Unwind_GetCFA", "MmTshn7gIgU", UnwindGetCFAImpl);
        reg("_Unwind_GetTextRelBase", "HPlmfRhMWwo", UnwindGetTextRelBaseImpl);
        reg("_Unwind_GetDataRelBase", "nqD9bBu/W6U", UnwindGetDataRelBaseImpl);
        reg("_Unwind_DeleteException", "GslDM6l8E7U", UnwindDeleteExceptionImpl);

        // Not implemented: forced unwind / backtrace / SjLj helpers.
        reg("_Unwind_Backtrace", "s62MgBhosjU", UnwindUnsupportedImpl);
        reg("_Unwind_ForcedUnwind", "zs817D8+hPA", UnwindUnsupportedImpl);
        reg("_Unwind_FindEnclosingFunction", "go1LaO7f15k", UnwindUnsupportedImpl);
        reg("_Unwind_Find_FDE", "Jhba7poZWZ0", UnwindUnsupportedImpl);
        reg("_Unwind_GetBSP", "uKwYpqeUkzo", UnwindUnsupportedImpl);
    }
}

} // namespace

s32* HLE::GuestErrnoPtr() { return &t_guest_errno; }
void HLE::SetGuestErrno(s32 value) { t_guest_errno = value; }

void HLE::SetGuestEhFrameHdr(guest_addr_t addr, u64 size) {
    if (addr == 0) {
        LOG_INFO(HLE, "Guest .eh_frame_hdr registration skipped (addr=0) — module without C++ exception unwinding");
        return;
    }
    for (const EhFrameTable& t : g_eh_tables) {
        if (t.addr == addr) return; // already registered
    }
    g_eh_tables.push_back({addr, size});
    LOG_INFO(HLE, "Guest .eh_frame_hdr registered at 0x%llx (%llu bytes, %zu table(s))",
             addr, size, g_eh_tables.size());
}

void RegisterLibLibc() {
    LOG_INFO(HLE, "Registering libc/libSceLibcInternal heap HLE symbols...");

    auto MallocImpl = [](const GuestArgs& args) -> u64 {
        const u64 size = args.arg1;
        const u64 mem  = HeapAlloc(size, kMinAlignment);
        LOG_DEBUG(HLE, "malloc(size: %llu) -> 0x%llx", size, mem);
        return mem;
    };
    auto FreeImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "free(ptr: 0x%llx)", args.arg1);
        HeapFree(args.arg1);
        return 0;
    };
    auto CallocImpl = [](const GuestArgs& args) -> u64 {
        const u64 nmemb = args.arg1;
        const u64 size  = args.arg2;
        // Overflow-safe multiply.
        if (size != 0 && nmemb > ~0ULL / size) {
            LOG_WARN(HLE, "calloc(%llu, %llu): size overflow -> null", nmemb, size);
            return 0;
        }
        const u64 total = nmemb * size;
        const u64 mem   = HeapAlloc(total, kMinAlignment);
        if (mem) {
            std::memset(reinterpret_cast<void*>(mem), 0, total ? total : 1);
        }
        LOG_DEBUG(HLE, "calloc(nmemb: %llu, size: %llu) -> 0x%llx", nmemb, size, mem);
        return mem;
    };
    auto ReallocImpl = [](const GuestArgs& args) -> u64 {
        const u64 old_ptr  = args.arg1;
        const u64 new_size = args.arg2;
        if (!old_ptr) {
            return HeapAlloc(new_size, kMinAlignment);
        }
        if (new_size == 0) {
            HeapFree(old_ptr);
            return 0;
        }
        u64 old_size = 0;
        if (!HeapValidate(old_ptr, &old_size)) {
            LOG_WARN(HLE, "realloc(0x%llx, %llu): bad header — allocating fresh without copy",
                     old_ptr, new_size);
            return HeapAlloc(new_size, kMinAlignment);
        }
        const u64 mem = HeapAlloc(new_size, kMinAlignment);
        if (mem) {
            std::memcpy(reinterpret_cast<void*>(mem), reinterpret_cast<const void*>(old_ptr),
                        old_size < new_size ? old_size : new_size);
            HeapFree(old_ptr);
        }
        LOG_DEBUG(HLE, "realloc(ptr: 0x%llx, size: %llu) -> 0x%llx", old_ptr, new_size, mem);
        return mem;
    };
    auto MemalignImpl = [](const GuestArgs& args) -> u64 {
        const u64 align = args.arg1;
        const u64 size  = args.arg2;
        const u64 mem   = HeapAlloc(size, align);
        LOG_DEBUG(HLE, "memalign(align: %llu, size: %llu) -> 0x%llx", align, size, mem);
        return mem;
    };
    auto AlignedAllocImpl = [](const GuestArgs& args) -> u64 {
        const u64 align = args.arg1;
        const u64 size  = args.arg2;
        // Lenient: unlike strict C11 we do not require size % align == 0.
        const u64 mem   = HeapAlloc(size, align);
        LOG_DEBUG(HLE, "aligned_alloc(align: %llu, size: %llu) -> 0x%llx", align, size, mem);
        return mem;
    };
    auto PosixMemalignImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t out_ptr = args.arg1;
        const u64 align = args.arg2;
        const u64 size  = args.arg3;
        if (!out_ptr) return 22; // EINVAL
        // Alignment must be a power of two and a multiple of sizeof(void*).
        if (align < sizeof(void*) || (align & (align - 1)) != 0) {
            LOG_WARN(HLE, "posix_memalign: invalid alignment %llu -> EINVAL", align);
            return 22; // EINVAL
        }
        const u64 mem = HeapAlloc(size, align);
        if (!mem) return 12; // ENOMEM
        Memory::Write<u64>(out_ptr, mem);
        LOG_DEBUG(HLE, "posix_memalign(align: %llu, size: %llu) -> 0x%llx", align, size, mem);
        return 0;
    };

    // Canonical module names (libSceLibcInternal is what retail games link
    // against; "libc" covers homebrew-style imports).
    for (const char* module : {"libSceLibcInternal", "libc"}) {
        RegisterSymbol(module, "malloc", MallocImpl);
        RegisterSymbol(module, "free", FreeImpl);
        RegisterSymbol(module, "calloc", CallocImpl);
        RegisterSymbol(module, "realloc", ReallocImpl);
        RegisterSymbol(module, "memalign", MemalignImpl);
        RegisterSymbol(module, "aligned_alloc", AlignedAllocImpl);
        RegisterSymbol(module, "posix_memalign", PosixMemalignImpl);

        // NIDs from assets/nid_db.txt.
        RegisterSymbol(module, "gQX+4GDQjpM#T#T", MallocImpl);       // malloc
        RegisterSymbol(module, "tIhsqj0qsFE#T#T", FreeImpl);         // free
        RegisterSymbol(module, "2X5agFjKxMc#T#T", CallocImpl);       // calloc
        RegisterSymbol(module, "Y7aJ1uydPMo#T#T", ReallocImpl);      // realloc
        RegisterSymbol(module, "Ujf3KzMvRmI#T#T", MemalignImpl);     // memalign
        RegisterSymbol(module, "2Btkg8k24Zg#T#T", AlignedAllocImpl); // aligned_alloc
        RegisterSymbol(module, "cVSk9y8URbc#T#T", PosixMemalignImpl);// posix_memalign
    }

    // Replace the leaky page-per-call libkernel registrations (malloc, free,
    // calloc, realloc incl. its alternate NID) with the real heap.
    RegisterSymbol("libkernel", "gQX+4GDQjpM#T#T", MallocImpl);
    RegisterSymbol("libkernel", "tIhsqj0qsFE#T#T", FreeImpl);
    RegisterSymbol("libkernel", "2X5agFjKxMc#T#T", CallocImpl);
    RegisterSymbol("libkernel", "0E5HFqWCBSA#T#T", ReallocImpl);
    RegisterSymbol("libkernel", "Y7aJ1uydPMo#T#T", ReallocImpl);
    RegisterSymbol("libkernel", "malloc#T#T", MallocImpl);
    RegisterSymbol("libkernel", "free#T#T", FreeImpl);
    RegisterSymbol("libkernel", "calloc#T#T", CallocImpl);
    RegisterSymbol("libkernel", "realloc#T#T", ReallocImpl);

    // __cxa_atexit (tsvEmnenz48) — record, return success.
    auto CxaAtexitImpl = [](const GuestArgs& args) -> u64 {
        std::lock_guard<std::mutex> lk(g_heap_mutex);
        g_atexit_handlers.push_back({args.arg1, args.arg2, args.arg3});
        LOG_DEBUG(HLE, "__cxa_atexit(func: 0x%llx, arg: 0x%llx, dso: 0x%llx) -> recorded (%zu total)",
                  args.arg1, args.arg2, args.arg3, g_atexit_handlers.size());
        return 0;
    };
    RegisterSymbol("libSceLibcInternal", "__cxa_atexit", CxaAtexitImpl);
    RegisterSymbol("libSceLibcInternal", "tsvEmnenz48#T#T", CxaAtexitImpl);
    RegisterSymbol("libkernel", "tsvEmnenz48#T#T", CxaAtexitImpl);

    // __cxa_finalize (H2e8t5ScQGc) — no-op.
    auto CxaFinalizeImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "__cxa_finalize(dso: 0x%llx) -> 0", args.arg1);
        return 0;
    };
    RegisterSymbol("libSceLibcInternal", "__cxa_finalize", CxaFinalizeImpl);
    RegisterSymbol("libSceLibcInternal", "H2e8t5ScQGc#T#T", CxaFinalizeImpl);

    // __stack_chk_guard — data symbol: the guest reads 8 bytes through the
    // resolved address as the canary.  The HLE thunk's own machine-code bytes
    // serve as a stable, process-constant canary value (consistent reads
    // across all functions, so stack checks never spuriously fail).
    auto StackChkFailImpl = [](const GuestArgs& /*args*/) -> u64 {
        LOG_ERROR(HLE, "__stack_chk_fail called! Guest stack corruption detected (or bad canary).");
        return 0;
    };
    for (const char* module : {"libSceLibcInternal", "libc"}) {
        RegisterSymbol(module, "__stack_chk_guard", [](const GuestArgs&) -> u64 { return 0; });
        RegisterSymbol(module, "__stack_chk_fail", StackChkFailImpl);
    }

    // sceLibcHeapGetTraceInfo — heap tracing is not implemented; report
    // success with no data written (the query is best-effort diagnostics).
    RegisterSymbol("libSceLibcInternal", "sceLibcHeapGetTraceInfo", [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceLibcHeapGetTraceInfo(0x%llx, 0x%llx) -> 0 (no trace data)", args.arg1, args.arg2);
        return 0;
    });

    // sceLibcInternalHeapErrorReportForGame (al3JzFI9MQ0) — log and succeed.
    auto HeapErrorReportImpl = [](const GuestArgs& args) -> u64 {
        LOG_WARN(HLE, "sceLibcInternalHeapErrorReportForGame(0x%llx, ...) called", args.arg1);
        return 0;
    };
    RegisterSymbol("libSceLibcInternal", "sceLibcInternalHeapErrorReportForGame", HeapErrorReportImpl);
    RegisterSymbol("libSceLibcInternal", "al3JzFI9MQ0#T#T", HeapErrorReportImpl);

    // -----------------------------------------------------------------------
    // printf family + string/memory functions.
    //
    // sprintf/snprintf take SysV varargs: the dispatcher captures rdi..r9 into
    // GuestArgs and the guest stack pointer as GuestArgs::stack_args, and
    // guest_printf.cpp synthesizes a va_list over them.  vsprintf/vsnprintf
    // receive a real guest va_list pointer.
    // -----------------------------------------------------------------------
    auto SprintfImpl  = [](const GuestArgs& args) -> u64 { return GuestSprintf(args); };
    auto SnprintfImpl = [](const GuestArgs& args) -> u64 { return GuestSnprintf(args); };
    auto VsprintfImpl = [](const GuestArgs& args) -> u64 { return GuestVsprintf(args); };
    auto VsnprintfImpl = [](const GuestArgs& args) -> u64 { return GuestVsnprintf(args); };

    auto MemcmpImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t a = args.arg1;
        const guest_addr_t b = args.arg2;
        const u64 n = args.arg3;
        if (n == 0) return 0;
        if (!a || !b || n > 0x10000000ULL) return (u64)(s64)-1;
        const int cmp = std::memcmp(reinterpret_cast<const void*>(a),
                                    reinterpret_cast<const void*>(b), n);
        return (u64)(s64)cmp;
    };

    // time() — real wall-clock time (a return-0 stub here breaks RNG seeding).
    auto TimeImpl = [](const GuestArgs& args) -> u64 {
        const s64 t = static_cast<s64>(std::time(nullptr));
        if (args.arg1) {
            Memory::Write<s64>(args.arg1, t);
        }
        return static_cast<u64>(t);
    };

    // _Getpctype — returns a pointer to the 256-entry u16 classification table
    // (MSVC-style flags; LOST EPIC's CRT indexes it directly for isalpha et
    // al. — a return-0 stub crashes with movzx from [char*2]).
    // The returned pointer allows index -1 (EOF), like the real CRT.
    auto GetpctypeImpl = [](const GuestArgs& /*args*/) -> u64 {
        constexpr u16 kUpper = 0x01, kLower = 0x02, kDigit = 0x04, kSpace = 0x08,
                      kPunct = 0x10, kControl = 0x20, kBlank = 0x40, kHex = 0x80,
                      kAlpha = 0x100;
        static u16 table[258] = {};
        static std::once_flag table_once;
        std::call_once(table_once, [] {
            for (int i = 0; i < 258; ++i) table[i] = 0;
            u16* t = &table[1]; // t[-1] == table[0] == 0 (EOF)
            for (int i = 0; i <= 0x1F; ++i) t[i] = kControl;
            t[0x7F] = kControl;
            t[0x20] = kSpace | kBlank;
            t[0x09] = kSpace | kBlank | kControl;
            for (int i = 0x0A; i <= 0x0D; ++i) t[i] = kSpace | kControl;
            for (int c = '0'; c <= '9'; ++c) t[c] = kDigit | kHex;
            for (int c = 'A'; c <= 'Z'; ++c) t[c] = kUpper | kAlpha;
            for (int c = 'a'; c <= 'z'; ++c) t[c] = kLower | kAlpha;
            for (int c = 'A'; c <= 'F'; ++c) t[c] |= kHex;
            for (int c = 'a'; c <= 'f'; ++c) t[c] |= kHex;
            for (int i = 0x21; i <= 0x7E; ++i) {
                if (t[i] == 0) t[i] = kPunct;
            }
        });
        return reinterpret_cast<u64>(&table[1]);
    };

    // __error() — per-thread errno location (PS5 libc: int* __error(void)).
    // Dreaming Sarah writes through the result right after a strtol-style
    // parse; a return-0 stub crashes with `mov dword [rax], 0` at a null
    // address.  A host thread_local address is fine (direct-pointer memory).
    auto ErrorImpl = [](const GuestArgs& /*args*/) -> u64 {
        return reinterpret_cast<u64>(GuestErrnoPtr());
    };

    // strstr() — plain libc semantics; the auto-stub returned NULL for every
    // query which silently breaks config/string parsing.
    auto StrstrImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1 || !args.arg2) return 0;
        const char* haystack = reinterpret_cast<const char*>(args.arg1);
        const char* needle   = reinterpret_cast<const char*>(args.arg2);
        const char* hit = std::strstr(haystack, needle);
        LOG_DEBUG(HLE, "strstr(0x%llx, 0x%llx) -> 0x%llx", args.arg1, args.arg2,
                  reinterpret_cast<u64>(hit));
        return hit ? reinterpret_cast<u64>(hit) : 0;
    };

    for (const char* module : {"libSceLibcInternal", "libc", "libkernel"}) {
        RegisterSymbol(module, "__error", ErrorImpl);
        RegisterSymbol(module, "9BcDykPmo1I", ErrorImpl); // __error
        RegisterSymbol(module, "strstr", StrstrImpl);
    }

    // strto* family — Dreaming Sarah parses its config with these; the
    // auto-stubs returned 0 for every value.  Guest pointers are host
    // pointers (direct-pointer memory), so the CRT implementations work
    // as-is.  strtod/strtof return their result in xmm0 — the dispatcher
    // mirrors rax into xmm0 (see dispatcher.asm), so returning the bit
    // pattern here is sufficient.
    auto StrtollImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1) return 0;
        char* end = nullptr;
        const long long v = std::strtoll(reinterpret_cast<const char*>(args.arg1),
                                         &end, static_cast<int>(args.arg3));
        if (args.arg2) Memory::Write<u64>(args.arg2, reinterpret_cast<u64>(end));
        return static_cast<u64>(v);
    };
    auto StrtoullImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1) return 0;
        char* end = nullptr;
        const unsigned long long v = std::strtoull(reinterpret_cast<const char*>(args.arg1),
                                                   &end, static_cast<int>(args.arg3));
        if (args.arg2) Memory::Write<u64>(args.arg2, reinterpret_cast<u64>(end));
        return static_cast<u64>(v);
    };
    auto StrtolImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1) return 0;
        char* end = nullptr;
        const long v = std::strtol(reinterpret_cast<const char*>(args.arg1),
                                   &end, static_cast<int>(args.arg3));
        if (args.arg2) Memory::Write<u64>(args.arg2, reinterpret_cast<u64>(end));
        return static_cast<u64>(static_cast<s64>(v));
    };
    auto StrtoulImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1) return 0;
        char* end = nullptr;
        const unsigned long v = std::strtoul(reinterpret_cast<const char*>(args.arg1),
                                             &end, static_cast<int>(args.arg3));
        if (args.arg2) Memory::Write<u64>(args.arg2, reinterpret_cast<u64>(end));
        return static_cast<u64>(v);
    };
    auto StrtodImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1) return 0;
        char* end = nullptr;
        static _locale_t c_locale = _create_locale(LC_ALL, "C");
        const double v = _strtod_l(reinterpret_cast<const char*>(args.arg1), &end, c_locale);
        if (args.arg2) Memory::Write<u64>(args.arg2, reinterpret_cast<u64>(end));
        u64 bits = 0;
        static_assert(sizeof(bits) == sizeof(v));
        std::memcpy(&bits, &v, sizeof(bits));
        {
            char preview[24] = {};
            const char* src = reinterpret_cast<const char*>(args.arg1);
            for (int i = 0; i < 23 && src[i]; ++i) preview[i] = src[i];
            LOG_INFO(HLE, "strtod('%s') -> %g (end+%ld)", preview, v,
                     static_cast<long>(end - reinterpret_cast<const char*>(args.arg1)));
        }
        return bits;
    };
    auto StrtofImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1) return 0;
        char* end = nullptr;
        static _locale_t c_locale = _create_locale(LC_ALL, "C");
        const float v = _strtof_l(reinterpret_cast<const char*>(args.arg1), &end, c_locale);
        if (args.arg2) Memory::Write<u64>(args.arg2, reinterpret_cast<u64>(end));
        u32 bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        return static_cast<u64>(bits);
    };
    auto MemchrImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1 || !args.arg3) return 0;
        const void* hit = std::memchr(reinterpret_cast<const void*>(args.arg1),
                                      static_cast<int>(args.arg2), args.arg3);
        return hit ? reinterpret_cast<u64>(hit) : 0;
    };
    auto GettimeofdayImpl = [](const GuestArgs& args) -> u64 {
        if (args.arg1) {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            Memory::Write<s64>(args.arg1,
                std::chrono::duration_cast<std::chrono::seconds>(now).count());
            Memory::Write<s64>(args.arg1 + 8,
                std::chrono::duration_cast<std::chrono::microseconds>(now).count() % 1000000);
        }
        return 0;
    };

    auto StrchrImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1) return 0;
        const char* hit = std::strchr(reinterpret_cast<const char*>(args.arg1), static_cast<int>(args.arg2));
        return hit ? reinterpret_cast<u64>(hit) : 0;
    };
    auto StrrchrImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1) return 0;
        const char* hit = std::strrchr(reinterpret_cast<const char*>(args.arg1), static_cast<int>(args.arg2));
        return hit ? reinterpret_cast<u64>(hit) : 0;
    };
    auto StrcatImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1 || !args.arg2) return args.arg1;
        char* dst = reinterpret_cast<char*>(args.arg1);
        const char* src = reinterpret_cast<const char*>(args.arg2);
        const size_t dst_len = std::strlen(dst);
        const size_t src_len = std::strlen(src);
        std::memcpy(dst + dst_len, src, src_len + 1);
        return args.arg1;
    };
    auto StrncatImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1 || !args.arg2 || !args.arg3) return args.arg1;
        char* dst = reinterpret_cast<char*>(args.arg1);
        const char* src = reinterpret_cast<const char*>(args.arg2);
        const size_t dst_len = std::strlen(dst);
        const size_t n = static_cast<size_t>(args.arg3);
        size_t src_len = std::strlen(src);
        if (src_len > n) src_len = n;
        std::memcpy(dst + dst_len, src, src_len);
        dst[dst_len + src_len] = '\0';
        return args.arg1;
    };

    for (const char* module : {"libSceLibcInternal", "libc", "libkernel"}) {
        RegisterSymbol(module, "strtoll", StrtollImpl);
        RegisterSymbol(module, "VOBg+iNwB-4", StrtollImpl);  // strtoll
        RegisterSymbol(module, "strtoull", StrtoullImpl);
        RegisterSymbol(module, "5OqszGpy7Mg", StrtoullImpl); // strtoull
        RegisterSymbol(module, "strtol", StrtolImpl);
        RegisterSymbol(module, "strtoul", StrtoulImpl);
        RegisterSymbol(module, "strtod", StrtodImpl);
        RegisterSymbol(module, "2vDqwBlpF-o", StrtodImpl);   // strtod
        RegisterSymbol(module, "strtof", StrtofImpl);
        RegisterSymbol(module, "memchr", MemchrImpl);
        RegisterSymbol(module, "8u8lPzUEq+U", MemchrImpl);   // memchr
        RegisterSymbol(module, "strchr", StrchrImpl);
        RegisterSymbol(module, "strrchr", StrrchrImpl);
        RegisterSymbol(module, "strcat", StrcatImpl);
        RegisterSymbol(module, "strncat", StrncatImpl);
        RegisterSymbol(module, "strstr", StrstrImpl);
        RegisterSymbol(module, "gettimeofday", GettimeofdayImpl);
        RegisterSymbol(module, "n88vx3C5nW8", GettimeofdayImpl); // gettimeofday
    }

    for (const char* module : {"libSceLibcInternal", "libc"}) {
        RegisterSymbol(module, "sprintf", SprintfImpl);
        RegisterSymbol(module, "snprintf", SnprintfImpl);
        RegisterSymbol(module, "vsprintf", VsprintfImpl);
        RegisterSymbol(module, "vsnprintf", VsnprintfImpl);
        RegisterSymbol(module, "memcmp", MemcmpImpl);
        RegisterSymbol(module, "time", TimeImpl);
        RegisterSymbol(module, "_Getpctype", GetpctypeImpl);

        // NIDs from assets/nid_db.txt.
        RegisterSymbol(module, "tcVi5SivF7Q#T#T", SprintfImpl);   // sprintf
        RegisterSymbol(module, "eLdDw6l0-bU#T#T", SnprintfImpl);  // snprintf
        RegisterSymbol(module, "jbz9I9vkqkk#T#T", VsprintfImpl);  // vsprintf
        RegisterSymbol(module, "Q2V+iqvjgC0#T#T", VsnprintfImpl); // vsnprintf
        RegisterSymbol(module, "DfivPArhucg#T#T", MemcmpImpl);    // memcmp
        RegisterSymbol(module, "wLlFkwG9UcQ#T#T", TimeImpl);      // time
        RegisterSymbol(module, "sUP1hBaouOw#T#T", GetpctypeImpl); // _Getpctype
    }

    // C++ ABI / unwind surface (__cxa_*, _Unwind_*, guard variables).
    RegisterCxxAbiSymbols();
}

} // namespace HLE
