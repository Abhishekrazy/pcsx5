//
// guest_tracer.cpp - diagnostic CPU tracer.
//

#include "guest_tracer.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <fstream>
#include <cstring>
#include <unordered_map>

#include <windows.h>

#include "../memory/memory.h"

namespace Kernel {

namespace {

struct Breakpoint {
    u64 address;
    std::string name;
    u8 original_byte;
    bool is_active;
};

struct TraceState {
    std::mutex    mtx;
    std::unordered_map<u64, Breakpoint> breakpoints;
    std::ofstream file;
    
    // For single step re-arming
    u64 pending_rearm_address = 0;
};

bool TraceEnabled() {
    return true;
}

TraceState& State() { static TraceState st; return st; }

// Write a byte to memory bypassing protection
void WriteByteSafe(u64 address, u8 value) {
    DWORD oldProt = 0;
    const u64 page = address & ~0xFFFull;
    ::VirtualProtect(reinterpret_cast<void*>(page), 0x2000, PAGE_EXECUTE_READWRITE, &oldProt);
    *reinterpret_cast<u8*>(address) = value;
    ::VirtualProtect(reinterpret_cast<void*>(page), 0x2000, oldProt, &oldProt);
}

// Read a byte from memory safely
u8 ReadByteSafe(u64 address) {
    if (!Memory::IsReadable(address, 1)) return 0;
    return Memory::Read<u8>(address);
}

void LogState(PCONTEXT ctx, const std::string& name) {
    TraceState& st = State();
    if (!st.file.is_open()) {
        st.file.open("guest_trace.log", std::ios::out | std::ios::app);
    }
    
    const unsigned long tid = ::GetCurrentThreadId();
    st.file << "\n[TRACE] " << name << " hit! (TID=" << tid << ")\n";
    st.file << std::hex;
    st.file << "  RIP=" << ctx->Rip << " RSP=" << ctx->Rsp << " RBP=" << ctx->Rbp << "\n";
    st.file << "  RDI=" << ctx->Rdi << " RSI=" << ctx->Rsi << " RDX=" << ctx->Rdx << "\n";
    st.file << "  RCX=" << ctx->Rcx << " R8 =" << ctx->R8  << " R9 =" << ctx->R9  << "\n";
    
    // Dump 32 QWORDs of stack
    st.file << "  Stack dump:\n";
    for (int i = 0; i < 32; ++i) {
        u64 addr = ctx->Rsp + (i * 8);
        if (Memory::IsReadable(addr, 8)) {
            st.file << "    [" << addr << "] = " << Memory::Read<u64>(addr) << "\n";
        }
    }
    st.file << std::dec;
    st.file.flush();
}

} // namespace

bool GuestTracer::Enabled() {
    return TraceEnabled();
}

void GuestTracer::AddBreakpoint(u64 address, const std::string& name) {
    if (!TraceEnabled()) return;
    
    std::lock_guard<std::mutex> lock(State().mtx);
    TraceState& st = State();
    
    if (st.breakpoints.count(address) > 0) return;
    
    Breakpoint bp;
    bp.address = address;
    bp.name = name;
    bp.original_byte = ReadByteSafe(address);
    bp.is_active = true;
    
    st.breakpoints[address] = bp;
    
    // Write INT3
    WriteByteSafe(address, 0xCC);
    if (!st.file.is_open()) {
        st.file.open("guest_trace.log", std::ios::out | std::ios::app);
    }
    st.file << "[GT] Breakpoint added: " << name << " at 0x" << std::hex << address << " (orig 0x" << (int)bp.original_byte << ")\n" << std::dec;
    st.file.flush();
}

bool GuestTracer::HandleTrap(DWORD exception_code, PCONTEXT ctx) {
    if (!TraceEnabled()) return false;

    std::lock_guard<std::mutex> lock(State().mtx);
    TraceState& st = State();

    if (exception_code == EXCEPTION_SINGLE_STEP) {
        // Did we just step over a breakpoint?
        if (st.pending_rearm_address != 0) {
            u64 addr = st.pending_rearm_address;
            st.pending_rearm_address = 0;
            
            // Re-arm it
            if (st.breakpoints.count(addr)) {
                WriteByteSafe(addr, 0xCC);
                st.breakpoints[addr].is_active = true;
            }
            
            // Clear TF
            ctx->EFlags &= ~0x100;
            return true;
        }
        return false;
    }

    if (exception_code == EXCEPTION_BREAKPOINT) {
        u64 bp_addr = 0;
        if (st.breakpoints.count(ctx->Rip)) bp_addr = ctx->Rip;
        else if (st.breakpoints.count(ctx->Rip - 1)) {
            bp_addr = ctx->Rip - 1;
            ctx->Rip = bp_addr; // Rewind RIP to execute the original instruction
        }
        
        if (bp_addr != 0) {
            Breakpoint& bp = st.breakpoints[bp_addr];
            if (bp.is_active) {
                LogState(ctx, bp.name);
                
                // Disarm and restore original byte
                WriteByteSafe(bp_addr, bp.original_byte);
                bp.is_active = false;
                
                // Set TF to single step
                ctx->EFlags |= 0x100;
                st.pending_rearm_address = bp_addr;
                
                return true;
            }
        }
        return false;
    }

    return false;
}

void GuestTracer::NotifyGuestRip(u64 /*rip*/, PCONTEXT /*ctx*/) {}

} // namespace Kernel
