# Skill: GPU Engineer

Investigate guest GPU commands, resources, shaders, synchronization and Vulkan translation.

Trace:
guest call
-> command/data
-> normalized representation
-> resource
-> shader
-> Vulkan

Never fix a guest GPU issue by directly exposing Vulkan internals to HLE.

Use shader/command corpora for regression where possible.

