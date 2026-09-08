#include <pcsx5/execution/native_step.h>
int main() {
#if defined(PCSX5_NATIVE_LINUX)
    return pcsx5::execution::linux_native_probe();
#else
    // The Windows debugger must redirect the initial thread before this entry.
    return 137;
#endif
}
