#include "memory.h"
#include "../common/log.h"
#include <windows.h>
#include <cstring>

namespace Memory {

    static DWORD TranslateProtection(u32 protection) {
        bool r = (protection & PROT_READ) != 0;
        bool w = (protection & PROT_WRITE) != 0;
        bool x = (protection & PROT_EXEC) != 0;

        if (x) {
            if (w) return PAGE_EXECUTE_READWRITE;
            if (r) return PAGE_EXECUTE_READ;
            return PAGE_EXECUTE;
        } else {
            if (w) return PAGE_READWRITE;
            if (r) return PAGE_READONLY;
            return PAGE_NOACCESS;
        }
    }

    bool Initialize() {
        LOG_INFO(Memory, "Initializing guest memory manager...");
        return true;
    }

    void Shutdown() {
        LOG_INFO(Memory, "Shutting down guest memory manager...");
    }

    guest_addr_t Map(guest_addr_t address, u64 size, u32 protection) {
        DWORD win_protection = TranslateProtection(protection);
        
        // Ensure size is aligned to system page size
        u64 aligned_size = ALIGN_UP(size, PAGE_SIZE);

        void* requested_ptr = reinterpret_cast<void*>(address);
        void* allocated_ptr = VirtualAlloc(requested_ptr, aligned_size, MEM_RESERVE | MEM_COMMIT, win_protection);

        if (!allocated_ptr) {
            LOG_ERROR(Memory, "Failed to allocate memory at guest address 0x%llx (size: %llu bytes, protection: %u). Win32 Error: %d", 
                      address, aligned_size, protection, GetLastError());
            return 0;
        }

        guest_addr_t guest_addr = GetGuestAddress(allocated_ptr);
        LOG_DEBUG(Memory, "Mapped memory range: [0x%llx - 0x%llx] (size: %llu bytes)", 
                  guest_addr, guest_addr + aligned_size, aligned_size);

        return guest_addr;
    }

    bool Unmap(guest_addr_t address, u64 /*size*/) {
        void* ptr = reinterpret_cast<void*>(address);
        // VirtualFree requires dwSize to be 0 when using MEM_RELEASE
        if (!VirtualFree(ptr, 0, MEM_RELEASE)) {
            LOG_ERROR(Memory, "Failed to unmap guest memory at 0x%llx. Win32 Error: %d", address, GetLastError());
            return false;
        }
        LOG_DEBUG(Memory, "Unmapped memory range starting at 0x%llx", address);
        return true;
    }

    bool Protect(guest_addr_t address, u64 size, u32 protection) {
        DWORD win_protection = TranslateProtection(protection);
        DWORD old_protection = 0;
        void* ptr = reinterpret_cast<void*>(address);

        if (!VirtualProtect(ptr, size, win_protection, &old_protection)) {
            LOG_ERROR(Memory, "Failed to change protection for guest memory at 0x%llx (size: %llu) to %u. Win32 Error: %d", 
                      address, size, protection, GetLastError());
            return false;
        }
        return true;
    }

    guest_addr_t Reserve(guest_addr_t address, u64 size) {
        u64 aligned_size = ALIGN_UP(size, 65536); // Windows allocation granularity is 64KB
        void* requested_ptr = reinterpret_cast<void*>(address);
        void* allocated_ptr = VirtualAlloc(requested_ptr, aligned_size, MEM_RESERVE, PAGE_NOACCESS);
        if (!allocated_ptr) {
            LOG_ERROR(Memory, "Failed to reserve virtual memory at 0x%llx (size: %llu). Win32 Error: %d", 
                      address, aligned_size, GetLastError());
            return 0;
        }
        guest_addr_t guest_addr = GetGuestAddress(allocated_ptr);
        LOG_DEBUG(Memory, "Reserved virtual address range: [0x%llx - 0x%llx] (size: %llu bytes)", 
                  guest_addr, guest_addr + aligned_size, aligned_size);
        return guest_addr;
    }

    guest_addr_t Commit(guest_addr_t address, u64 size, u32 protection) {
        DWORD win_protection = TranslateProtection(protection);
        u64 aligned_size = ALIGN_UP(size, PAGE_SIZE); // Commit at page granularity (4KB/16KB)
        void* ptr = reinterpret_cast<void*>(address);
        void* allocated_ptr = VirtualAlloc(ptr, aligned_size, MEM_COMMIT, win_protection);
        if (!allocated_ptr) {
            LOG_ERROR(Memory, "Failed to commit virtual memory at 0x%llx (size: %llu). Win32 Error: %d", 
                      address, aligned_size, GetLastError());
            return 0;
        }
        guest_addr_t guest_addr = GetGuestAddress(allocated_ptr);
        LOG_DEBUG(Memory, "Committed virtual address range: [0x%llx - 0x%llx] (size: %llu bytes)", 
                  guest_addr, guest_addr + aligned_size, aligned_size);
        return guest_addr;
    }

    void ReadBuffer(guest_addr_t addr, void* dest, u64 size) {
        std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
    }

    void WriteBuffer(guest_addr_t addr, const void* src, u64 size) {
        std::memcpy(reinterpret_cast<void*>(addr), src, size);
    }
}
// namespace Memory
