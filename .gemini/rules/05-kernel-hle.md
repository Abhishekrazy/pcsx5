# PCSX5 Rule

Kernel provides OS semantics; HLE implements guest libraries/services. Avoid kernel/HLE/CPU cycles. Move shared concepts into narrow independent types. Unknown called NIDs must be observable, not unexplained NULL behavior.
