# CPU / Guest Execution Governance

PCSX5 currently uses x86_64 guest execution and a controlled dispatcher/ABI bridge.

Do not introduce JIT/recompiler architecture without measured evidence.

Before CPU changes:
- identify guest ABI
- identify host ABI boundary
- identify register preservation
- identify exception/fault semantics
- identify TLS interaction
- identify self-modifying/code-patch behavior
- identify thread interaction
- characterize hot paths
- benchmark before optimization

MASM:
- must have a documented responsibility
- must have a portable/testable fallback where practical
- must not spread assembly through unrelated subsystems

Never turn a guest semantic problem into an assembly workaround without evidence.

