# PCSX5 Rule

Dependency direction: UI -> stable native ABI -> runtime -> subsystems -> platform adapters. Core subsystems: CPU, Memory/MMU, Kernel, HLE, Loader, GPU, Audio, Input, Timing, Debug/Trace, Save State. Core must not depend on WPF. HLE must not reach Vulkan internals. Keep game-specific workarounds isolated.
