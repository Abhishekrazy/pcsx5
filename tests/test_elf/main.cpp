// Freestanding guest ELF test program using System V syscall conventions
// Compile with: clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -o test_guest.elf main.cpp

extern "C" void _start() {
    // 1. Issue syscall #4: sys_write(fd = 1 (stdout), buf = "Hello Guest\n", len = 12)
    // System V syscall register layout:
    // rax = syscall number (4)
    // rdi = arg1 (1)
    // rsi = arg2 (pointer to message)
    // rdx = arg3 (12)
    const char* msg = "Hello Guest\n";
    
    asm volatile (
        "mov $4, %%rax\n"
        "mov $1, %%rdi\n"
        "mov %0, %%rsi\n"
        "mov $12, %%rdx\n"
        "syscall\n"
        :
        : "r"(msg)
        : "rax", "rdi", "rsi", "rdx"
    );

    // 2. Issue syscall #1: sys_exit(code = 0)
    // rax = syscall number (1)
    // rdi = arg1 (0)
    asm volatile (
        "mov $1, %%rax\n"
        "mov $0, %%rdi\n"
        "syscall\n"
    );
}
