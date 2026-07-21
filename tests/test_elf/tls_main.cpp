// Freestanding guest ELF test program that exercises the FS-relative TLS
// trap path.  The host kernel does not actually load the FS segment base;
// instead, the Vectored Exception Handler emulates `fs:` reads and writes
// by translating the displacement through Kernel::GuestTlsContext.  This
// test:
//
//   1. Writes a sentinel value (0xCAFEBABE_DEADBEEF) to fs:0.
//   2. Reads it back through fs:0 and compares.
//   3. Writes the value 0 to a negative-offset slot (fs:-8).
//   4. Reads it back through fs:-8 and compares.
//   5. Reports the per-step outcome via sys_write (syscall #4).
//   6. Exits via sys_exit (syscall #1).
//
// Compile with:
//   clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib \
//         -Wl,--entry=_start -o tls_guest.elf tls_main.cpp
//
// Syscall arguments are placed with register constraints ("a", "D", "S",
// "d") and the fs: accesses are prefixed with .att_syntax: clang in MSVC
// mode parses inline asm with Intel dialect and rejects AT&T-style %rax /
// %%fs:0 syntax.

static const char kOk[]   = "TLS OK\n";
static const char kFail[] = "TLS FAIL\n";
static const char kPart[] = "TLS PARTIAL\n";

static void sys_write(const char* s, unsigned long len) {
    long nr = 4;
    const long fd = 1;
    asm volatile (
        "syscall\n"
        : "+a"(nr)
        : "D"(fd), "S"(s), "d"(len)
        : "rcx", "r11", "memory"
    );
}

static void sys_exit(int code) {
    long nr = 1;
    const long c = code;
    asm volatile (
        "syscall\n"
        : "+a"(nr)
        : "D"(c)
        : "rcx", "r11", "memory"
    );
}

extern "C" void _start() {
    // --- Step 1: write 0xCAFEBABE_DEADBEEF to fs:0 -------------------------
    unsigned long long written = 0xCAFEBABEDEADBEEFULL;
    asm volatile (
        ".att_syntax\n"
        "movq %0, %%fs:0\n"
        :
        : "r"(written)
        : "memory"
    );

    // --- Step 2: read it back and compare ---------------------------------
    unsigned long long read_back = 0;
    asm volatile (
        ".att_syntax\n"
        "movq %%fs:0, %0\n"
        : "=r"(read_back)
        :
        : "memory"
    );
    bool step12_ok = (read_back == 0xCAFEBABEDEADBEEFULL);

    // --- Step 3: write to a negative-displacement TLS slot -----------------
    unsigned long long neg_value = 0x1122334455667788ULL;
    asm volatile (
        ".att_syntax\n"
        "movq %0, %%fs:-8\n"
        :
        : "r"(neg_value)
        : "memory"
    );

    // --- Step 4: read back from the negative offset -----------------------
    unsigned long long neg_read = 0;
    asm volatile (
        ".att_syntax\n"
        "movq %%fs:-8, %0\n"
        : "=r"(neg_read)
        :
        : "memory"
    );
    bool step34_ok = (neg_read == 0x1122334455667788ULL);

    // --- Step 5: report ---------------------------------------------------
    if (step12_ok && step34_ok) {
        sys_write(kOk, sizeof(kOk) - 1);
    } else if (step12_ok || step34_ok) {
        sys_write(kPart, sizeof(kPart) - 1);
    } else {
        sys_write(kFail, sizeof(kFail) - 1);
    }

    // --- Step 6: exit ----------------------------------------------------
    sys_exit(0);
}
