# Debugging and Observability

## Logging

Use structured categories:

- CORE
- CPU
- MMU
- KERNEL
- HLE
- LOADER
- GPU
- SHADER
- AUDIO
- INPUT
- MEDIA
- SAVE_STATE
- COMPAT
- UPDATE
- UI

Levels:
TRACE, DEBUG, INFO, WARN, ERROR, FATAL

Do not log high-frequency hot-path events at INFO.

## Crash context

A crash report should capture, where available:
- emulator version
- git commit
- game title/version
- CPU PC
- thread
- current subsystem
- recent log ring buffer
- loaded modules
- GPU backend
- renderer configuration
- relevant exception code
- stack trace

Never include user secrets or arbitrary file contents.

## Debug tools

Prioritize:
1. pause/resume
2. guest PC
3. CPU register view
4. memory viewer
5. syscall trace
6. module list
7. GPU command trace
8. shader inspection
9. frame timing
10. compatibility profile inspection

## Deterministic traces

When a bug is hard to reproduce, create a bounded trace fixture rather than dumping unbounded logs.

A trace fixture should have:
- version
- source commit
- input
- expected result
- reproduction command
