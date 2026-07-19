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
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace HLE {

namespace {

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
        return aligned;
    }

    // Bump-allocate, growing the arena as needed.
    for (;;) {
        const u64 aligned = (g_heap_bump + kHeaderSize + align - 1) & ~(align - 1);
        const u64 total   = (aligned - g_heap_bump) + size;
        if (g_heap_bump != 0 && g_heap_bump + total <= g_heap_end) {
            g_heap_bump += total;
            WriteHeader(aligned, size);
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

} // namespace

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
        static bool initialized = false;
        if (!initialized) {
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
            initialized = true;
        }
        return reinterpret_cast<u64>(&table[1]);
    };

    // __error() — per-thread errno location (PS5 libc: int* __error(void)).
    // Dreaming Sarah writes through the result right after a strtol-style
    // parse; a return-0 stub crashes with `mov dword [rax], 0` at a null
    // address.  A host thread_local address is fine (direct-pointer memory).
    auto ErrorImpl = [](const GuestArgs& /*args*/) -> u64 {
        static thread_local s32 t_guest_errno = 0;
        return reinterpret_cast<u64>(&t_guest_errno);
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
        const double v = std::strtod(reinterpret_cast<const char*>(args.arg1), &end);
        if (args.arg2) Memory::Write<u64>(args.arg2, reinterpret_cast<u64>(end));
        u64 bits = 0;
        static_assert(sizeof(bits) == sizeof(v));
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    };
    auto StrtofImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1) return 0;
        char* end = nullptr;
        const float v = std::strtof(reinterpret_cast<const char*>(args.arg1), &end);
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
}

} // namespace HLE
