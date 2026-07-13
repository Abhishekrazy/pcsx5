#include "kernel/tls.h"

#include <iostream>

int main() {
    Kernel::GuestTlsContext tls;
    if (!tls.Configure(0x100000, 0x2000, 0x1000)) {
        return 1;
    }

    guest_addr_t address = 0;
    if (!tls.Translate(0, sizeof(u64), address) || address != 0x101000 ||
        !tls.Translate(-8, sizeof(u64), address) || address != 0x100ff8 ||
        !tls.Translate(0xff8, sizeof(u64), address) || address != 0x101ff8 ||
        tls.Translate(-0x1001, sizeof(u64), address) ||
        tls.Translate(0x1000, sizeof(u64), address)) {
        std::cerr << "Guest TLS address translation test failed.\n";
        return 1;
    }

    std::cout << "Guest TLS address translation tests passed.\n";
    return 0;
}
