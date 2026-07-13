#pragma once
#include "../common/types.h"

namespace Memory {

    // Initializer
    bool Initialize();
    void Shutdown();

    // Map guest virtual memory range.
    // If address is 0, the OS will choose a free virtual address range.
    guest_addr_t Map(guest_addr_t address, u64 size, u32 protection);
    guest_addr_t Reserve(guest_addr_t address, u64 size);
    guest_addr_t Commit(guest_addr_t address, u64 size, u32 protection);
    bool Unmap(guest_addr_t address, u64 size);
    bool Protect(guest_addr_t address, u64 size, u32 protection);

    // Read/Write helpers (using direct pointers since Guest VM = Host VM)
    template <typename T>
    inline T Read(guest_addr_t addr) {
        return *reinterpret_cast<T*>(addr);
    }

    template <typename T>
    inline void Write(guest_addr_t addr, T value) {
        *reinterpret_cast<T*>(addr) = value;
    }

    void ReadBuffer(guest_addr_t addr, void* dest, u64 size);
    void WriteBuffer(guest_addr_t addr, const void* src, u64 size);

    // Virtual to host pointer translation
    inline void* Translate(guest_addr_t addr) {
        return reinterpret_cast<void*>(addr);
    }

    inline guest_addr_t GetGuestAddress(void* host_ptr) {
        return reinterpret_cast<guest_addr_t>(host_ptr);
    }

    // PS5/FreeBSD memory protection flags
    constexpr u32 PROT_NONE  = 0x0;
    constexpr u32 PROT_READ  = 0x1;
    constexpr u32 PROT_WRITE = 0x2;
    constexpr u32 PROT_EXEC  = 0x4;
}
