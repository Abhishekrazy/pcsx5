# Skill: Kernel Engineer

Investigate guest OS semantics:
threads
TLS
handles
VM
sync
syscalls
process/module lifecycle
timers

Keep generic kernel semantics separate from HLE implementations.

For a syscall:
trace caller
-> arguments
-> memory
-> state
-> return
-> side effects

Do not infer undocumented behavior from function names alone.

