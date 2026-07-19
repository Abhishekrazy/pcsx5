#include "memory.h"
#include "kernel.h"
#include <windows.h>
#include <algorithm>
#include <mutex>

namespace Kernel {

// Guest memory allocator state
static u64 g_guest_alloc_base = 0x400000000000;  // 4TB base address for guest allocations
static u64 g_guest_alloc_size = 0x10000000000;   // 1TB allocation space
static u64 g_guest_alloc_current = 0;
static std::mutex g_guest_alloc_mutex;

// Initialize guest memory allocator
void InitializeGuestMemory() {
    std::lock_guard<std::mutex> lock(g_guest_alloc_mutex);
    g_guest_alloc_current = g_guest_alloc_base;
}

// Shutdown guest memory allocator
void ShutdownGuestMemory() {
    std::lock_guard<std::mutex> lock(g_guest_alloc_mutex);
    // Free all allocated guest memory
    // In a real implementation, we'd track all allocations and free them
    g_guest_alloc_current = g_guest_alloc_base;
}

// Allocate guest memory
guest_addr_t AllocGuestMemory(u64 size, u64 alignment, int prot, int flags, int fd, s64 offset) {
    std::lock_guard<std::mutex> lock(g_guest_alloc_mutex);
    (void)flags;
    (void)fd;
    (void)offset;
    
    // Align the current pointer
    u64 aligned_current = (g_guest_alloc_current + alignment - 1) & ~(alignment - 1);
    
    // Check if we have enough space
    if (aligned_current + size > g_guest_alloc_base + g_guest_alloc_size) {
        return 0;  // Out of memory
    }
    
    guest_addr_t addr = aligned_current;
    g_guest_alloc_current = aligned_current + size;
    
    // Reserve the memory in the host process
    void* host_addr = VirtualAlloc(
        reinterpret_cast<void*>(addr),
        static_cast<SIZE_T>(size),
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE
    );
    
    if (host_addr == nullptr) {
        // Rollback allocation
        g_guest_alloc_current = aligned_current;
        return 0;
    }
    
    // Apply protection
    DWORD protect = 0;
    if (prot & PROT_READ) protect |= PAGE_READONLY;
    if (prot & PROT_WRITE) protect |= PAGE_READWRITE;
    if (prot & PROT_EXEC) protect |= PAGE_EXECUTE_READ;
    if ((prot & PROT_READ) && (prot & PROT_WRITE)) protect = PAGE_READWRITE;
    if ((prot & PROT_READ) && (prot & PROT_WRITE) && (prot & PROT_EXEC)) protect = PAGE_EXECUTE_READWRITE;
    
    DWORD old_protect;
    if (!VirtualProtect(host_addr, static_cast<SIZE_T>(size), protect, &old_protect)) {
        VirtualFree(host_addr, 0, MEM_RELEASE);
        g_guest_alloc_current = aligned_current;
        return 0;
    }
    
    return addr;
}

// Free guest memory
bool FreeGuestMemory(guest_addr_t addr, u64 size) {
    std::lock_guard<std::mutex> lock(g_guest_alloc_mutex);
    (void)size;
    
    // Release the memory in the host process
    BOOL result = VirtualFree(
        reinterpret_cast<void*>(static_cast<uintptr_t>(addr)),
        0,
        MEM_RELEASE
    );
    
    // Note: We don't actually reclaim the address space in this simple allocator
    // A more sophisticated allocator would track free blocks
    
    return result != FALSE;
}

// Change memory protection
bool ProtectGuestMemory(guest_addr_t addr, u64 size, int prot) {
    // Map guest R/W/X combinations to the exact host page protection;
    // OR-ing PAGE_* constants together is not a valid translation.
    const bool r = (prot & PROT_READ)  != 0;
    const bool w = (prot & PROT_WRITE) != 0;
    const bool x = (prot & PROT_EXEC)  != 0;
    DWORD protect;
    if (x)      protect = w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    else if (w) protect = PAGE_READWRITE;
    else if (r) protect = PAGE_READONLY;
    else        protect = PAGE_NOACCESS;
    
    DWORD old_protect;
    BOOL result = VirtualProtect(
        reinterpret_cast<void*>(static_cast<uintptr_t>(addr)),
        static_cast<SIZE_T>(size),
        protect,
        &old_protect
    );
    
    return result != FALSE;
}

// Get memory info for a guest address
bool GetGuestMemoryInfo(guest_addr_t addr, MEMORY_BASIC_INFORMATION* info) {
    return VirtualQuery(
        reinterpret_cast<void*>(static_cast<uintptr_t>(addr)),
        info,
        sizeof(MEMORY_BASIC_INFORMATION)
    ) != 0;
}

// Check if guest address is valid
bool IsValidGuestAddress(guest_addr_t addr, u64 size) {
    return addr >= g_guest_alloc_base && 
           addr + size <= g_guest_alloc_base + g_guest_alloc_size;
}

// Map guest memory (for mmap syscall)
guest_addr_t MapGuestMemory(guest_addr_t addr, u64 length, int prot, int flags, int fd, s64 offset) {
    // If addr is 0, let the allocator choose
    if (addr == 0) {
        return AllocGuestMemory(length, 0x1000, prot, flags, fd, offset);
    }
    
    // Check if the requested address is available
    if (!IsValidGuestAddress(addr, length)) {
        return 0;
    }
    
    // Try to reserve at the specific address
    void* host_addr = VirtualAlloc(
        reinterpret_cast<void*>(static_cast<uintptr_t>(addr)),
        static_cast<SIZE_T>(length),
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE
    );
    
    if (host_addr == nullptr) {
        return 0;
    }
    
    // Apply protection
    DWORD protect = 0;
    if (prot & PROT_READ) protect |= PAGE_READONLY;
    if (prot & PROT_WRITE) protect |= PAGE_READWRITE;
    if (prot & PROT_EXEC) protect |= PAGE_EXECUTE_READ;
    if ((prot & PROT_READ) && (prot & PROT_WRITE)) protect = PAGE_READWRITE;
    if ((prot & PROT_READ) && (prot & PROT_WRITE) && (prot & PROT_EXEC)) protect = PAGE_EXECUTE_READWRITE;
    
    DWORD old_protect;
    if (!VirtualProtect(host_addr, static_cast<SIZE_T>(length), protect, &old_protect)) {
        VirtualFree(host_addr, 0, MEM_RELEASE);
        return 0;
    }
    
    return addr;
}

// Unmap guest memory (for munmap syscall)
bool UnmapGuestMemory(guest_addr_t addr, u64 length) {
    return FreeGuestMemory(addr, length);
}

// Get current break (for brk syscall)
guest_addr_t GetBreak() {
    std::lock_guard<std::mutex> lock(g_guest_alloc_mutex);
    return g_guest_alloc_current;
}

// Set break (for brk syscall)
guest_addr_t SetBreak(guest_addr_t new_break) {
    std::lock_guard<std::mutex> lock(g_guest_alloc_mutex);
    
    if (new_break < g_guest_alloc_base) {
        return g_guest_alloc_current;
    }
    
    if (new_break > g_guest_alloc_base + g_guest_alloc_size) {
        return g_guest_alloc_current;
    }
    
    if (new_break > g_guest_alloc_current) {
        // Expand the break - allocate more memory
        u64 size = new_break - g_guest_alloc_current;
        void* host_addr = VirtualAlloc(
            reinterpret_cast<void*>(static_cast<uintptr_t>(g_guest_alloc_current)),
            static_cast<SIZE_T>(size),
            MEM_COMMIT,
            PAGE_READWRITE
        );
        
        if (host_addr == nullptr) {
            return g_guest_alloc_current;
        }
        
        g_guest_alloc_current = new_break;
    } else if (new_break < g_guest_alloc_current) {
        // Shrink the break - decommit memory
        u64 size = g_guest_alloc_current - new_break;
        VirtualFree(
            reinterpret_cast<void*>(static_cast<uintptr_t>(new_break)),
            static_cast<SIZE_T>(size),
            MEM_DECOMMIT
        );
        
        g_guest_alloc_current = new_break;
    }
    
    return g_guest_alloc_current;
}

} // namespace Kernel