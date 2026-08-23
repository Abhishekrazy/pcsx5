# Plugin Architecture

Optional tools may provide:
- debugger
- profiler
- compatibility analyzer
- trace analyzer
- binary/RE analyzer
- shader analyzer
- release/update tooling
- automation

Each plugin defines name, version, capabilities, inputs, outputs, permissions, dependencies, failure behavior and removal procedure.

External APIs are isolated behind adapters.

The emulator core must remain functional without optional developer plugins.
