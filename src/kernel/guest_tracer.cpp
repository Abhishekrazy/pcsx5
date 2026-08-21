//
// guest_tracer.cpp — focused guest CPU tracer for Dreaming Sarah (PPSA02929).
// See guest_tracer.h.
//
// Logs every call to the Construct JSON string reader (guest 0x81012f790) to
// guest_trace.log: thread id + parser token start/cursor/length + string bytes.
//
// Mechanism: permanently overwrite the first byte of 0x81012f790 with INT3
// (0xCC).  On each trap: log, emulate the skipped "push rbp" by writing rbp to
// [rsp-8] (guarded), advance RIP past the 1-byte instruction, and leave 0xCC in
// place so every subsequent call re-traps.
//
// Inert unless PCSX5_GUEST_TRACE is set.
//

#include "guest_tracer.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <fstream>
#include <cstring>

#include <windows.h>

#include "../memory/memory.h"

namespace Kernel {

namespace {

constexpr u64 kStringReader = 0x81012f790;

struct TraceState {
    std::mutex    mtx;
    bool          armed      = false;
    unsigned long long hit_count = 0;
    std::ofstream file;
};

bool TraceEnabled() {
    char buf[2] = {0};
    size_t n = 0;
    return (::getenv_s(&n, buf, sizeof(buf), "PCSX5_GUEST_TRACE") == 0 && n > 0);
}

TraceState& State() { static TraceState st; return st; }

// arm the INT3 breakpoint once
void ArmBreakpoint() {
    TraceState& st = State();
    if (st.armed) return;
    DWORD oldProt = 0;
    const u64 page = kStringReader & ~0xFFFull;
    ::VirtualProtect(reinterpret_cast<void*>(page), 0x2000, PAGE_EXECUTE_READWRITE, &oldProt);
    *reinterpret_cast<u8*>(kStringReader) = 0xCC;
    ::VirtualProtect(reinterpret_cast<void*>(page), 0x2000, oldProt, &oldProt);
    st.armed = true;
    std::fprintf(stderr, "[GT] armed 0x%llx\n", static_cast<unsigned long long>(kStringReader));
}

// Read a u64 from guest memory via Memory::ReadBuffer (safe, no guest-stack SEH).
bool ReadGuest64(u64 addr, u64* out) {
    if (addr < 0x10000 || addr >= (1ULL << 47)) return false;
    if (!Memory::IsReadable(addr, 8)) return false;
    *out = Memory::Read<u64>(addr);
    return true;
}

void LogStringReader(PCONTEXT ctx) {
    TraceState& st = State();
    const u64 parser = ctx->Rsi;   // arg2 = parser object
    u64 start = 0, cursor = 0;
    ReadGuest64(parser + 0x60, &start);
    ReadGuest64(parser + 0x70, &cursor);
    char s[96] = {0};
    int n = 0;
    if (start && cursor > start + 1) {
        for (u64 a = start + 1; a < cursor && n < 90 && Memory::IsReadable(a, 1); ++a) {
            const char c = static_cast<char>(Memory::Read<u8>(a));
            s[n++] = (c >= 32 && c < 127) ? c : '.';
        }
    }
    const unsigned long tid = ::GetCurrentThreadId();
    if (st.file) {
        st.file << "tid=" << tid << " parser=0x" << std::hex << parser
                << " start=0x" << start << " cursor=0x" << cursor
                << " len=" << std::dec << (cursor > start ? cursor - start - 2 : 0)
                << " \"" << s << "\"\n";
        st.file.flush();
    }
}

} // namespace

bool GuestTracer::Enabled() {
    if (!TraceEnabled()) return false;
    ArmBreakpoint();
    return true;
}

bool GuestTracer::HandleTrap(DWORD exception_code, PCONTEXT ctx) {
    if (!Enabled()) return false;

    std::lock_guard<std::mutex> lock(State().mtx);
    TraceState& st = State();

    if (exception_code != EXCEPTION_BREAKPOINT) return false;
    const u64 rip = ctx->Rip;
    if (rip != kStringReader && rip != kStringReader + 1) return false;

    if (!st.file.is_open()) {
        st.file.open("guest_trace.log", std::ios::out | std::ios::trunc);
    }

    ++st.hit_count;
    LogStringReader(ctx);

    // Emulate the skipped "push rbp" (1 byte) safely, then advance.
    const u64 new_rsp = ctx->Rsp - 8;
    if (new_rsp >= 0x10000 && new_rsp < (1ULL << 47) && Memory::IsWritable(new_rsp, 8)) {
        Memory::Write<u64>(new_rsp, ctx->Rbp);
        ctx->Rsp = new_rsp;
    }
    ctx->Rip = kStringReader + 1;
    return true;
}

void GuestTracer::NotifyGuestRip(u64 /*rip*/, PCONTEXT /*ctx*/) {}

} // namespace Kernel
