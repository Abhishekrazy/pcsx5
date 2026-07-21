// Freestanding guest ELF test program using System V syscall conventions
// Compile with: clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -o test_guest.elf main.cpp
//
// Syscall arguments are placed with register constraints ("a", "D", "S", "d")
// instead of hardcoded register names in the asm string: clang in MSVC mode
// parses inline asm with Intel dialect and rejects AT&T-style %rax names.

extern "C" void _start() {
    // 1. Issue syscall #4: sys_write(fd = 1 (stdout), buf = "Hello Guest\n", len = 12)
    // System V syscall register layout:
    // rax = syscall number (4)
    // rdi = arg1 (1)
    // rsi = arg2 (pointer to message)
    // rdx = arg3 (12)
    const char* msg = "Hello Guest\n";
    long nr_write = 4;
    const long fd_stdout = 1;
    const long msg_len = 12;
    asm volatile (
        "syscall\n"
        : "+a"(nr_write)
        : "D"(fd_stdout), "S"(msg), "d"(msg_len)
        : "rcx", "r11", "memory"
    );

    // 2. Issue syscall #1: sys_exit(code = 0)
    // rax = syscall number (1)
    // rdi = arg1 (0)
    long nr_exit = 1;
    const long exit_code = 0;
    asm volatile (
        "syscall\n"
        : "+a"(nr_exit)
        : "D"(exit_code)
        : "rcx", "r11", "memory"
    );
}
