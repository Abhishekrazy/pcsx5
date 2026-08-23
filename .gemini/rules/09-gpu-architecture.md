# GPU Architecture Governance

Required direction:

Guest GPU API
  ->
Command Decode
  ->
Normalized GPU representation
  ->
Resource tracking
  ->
Shader translation
  ->
Vulkan backend

HLE must not manipulate Vulkan objects directly.

Do not expose Vulkan types through generic guest-facing interfaces.

GPU resources need explicit ownership/lifetime.

Shader translation must distinguish:
- guest shader representation
- translated representation
- Vulkan pipeline state

GPU compatibility fixes require reproducible evidence.

