# Input / Platform Governance

Guest input semantics belong above platform adapters.

Target direction:

Guest Input
-> Input Service
-> Device Abstraction
-> Win32/HID/XInput/DualSense backend

Do not let XInput assumptions leak into guest controller semantics.

Platform-specific hotplug/device behavior belongs in platform adapters.

