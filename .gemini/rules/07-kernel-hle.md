# Kernel / HLE Governance

Kernel:
- guest OS semantics
- handles
- scheduling/thread semantics
- memory services
- synchronization
- lifecycle

HLE:
- guest library/service implementations
- NID/import resolution
- service-facing contracts

Avoid:
CPU <-> HLE <-> Kernel cycles
HLE -> Vulkan internals
HLE -> random host global state
Kernel -> UI

Unknown imports must not silently become plausible success.

Stub policy:
- observable
- categorized
- counted
- traceable
- removable

If a stub is required for compatibility, record:
title
function/NID
observed call pattern
current behavior
why it is safe
next implementation evidence

