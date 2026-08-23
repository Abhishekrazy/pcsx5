# Skill: Shader Reverse Engineering

Analyze guest shader representations and translated SPIR-V.

Workflow:
capture
-> disassemble/inspect
-> identify semantics
-> compare multiple shaders
-> reproduce
-> modify translator
-> compile
-> compare output
-> regression

Never assume a shader opcode's semantics from naming alone.

