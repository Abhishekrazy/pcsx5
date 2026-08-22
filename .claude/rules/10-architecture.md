# Rule: Dependency Direction

Allowed dependency direction:

UI -> Core public API
Debug frontend -> Core debug API
Core -> internal emulator subsystems
Subsystem -> lower-level services
Platform adapter -> platform API
Third-party wrapper -> third-party library

Forbidden:
- Core -> WPF
- CPU -> UI
- GPU -> WPF
- Kernel -> WPF
- Game-specific compatibility code -> generic CPU internals unless the behavior is truly architectural
- Third-party library calls scattered through unrelated subsystems

Use interfaces only at meaningful seams. Do not create interfaces for every class.
Prefer concrete types internally and narrow abstractions at platform/public boundaries.
