# PCSX5 Rule

Separate guest GPU API -> command decoding -> normalized representation -> resource tracking -> shader translation -> Vulkan backend. HLE must not access Vulkan objects or GPU globals directly. Shader changes require corpus/regression testing where feasible.
