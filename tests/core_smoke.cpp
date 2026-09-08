#include <pcsx5/core/version.h>

int main() {
    return pcsx5::core::architecture_name() == "host-independent-core" ? 0 : 1;
}
