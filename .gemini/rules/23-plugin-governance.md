# Plugin Governance

Plugins may provide optional:
- debugger
- profiler
- RE tools
- compatibility analyzers
- shader tools
- release tooling
- automation
- external integrations

Plugin requirements:
- manifest
- version
- capabilities
- dependencies
- permissions
- compatibility range
- lifecycle
- failure behavior
- removal procedure

Plugins must use public contracts.

A plugin must not access another plugin's private implementation.

The emulator must not require optional developer plugins to boot.

