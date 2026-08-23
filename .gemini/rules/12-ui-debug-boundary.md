# UI / Debug Boundary

WPF is the desktop product shell.

Dear ImGui is developer/debug tooling unless an ADR explicitly expands its role.

The UI must not become emulator-state authority.

Debug UI may observe and request operations through explicit debug/runtime APIs.

No UI object should be passed deep into CPU/Kernel/HLE/GPU code.

Headless execution must remain possible without WPF.

