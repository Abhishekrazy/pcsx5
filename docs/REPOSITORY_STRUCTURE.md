# Recommended Repository Structure

```text
/
├── CLAUDE.md
├── CMakeLists.txt
├── cmake/
├── src/
│   ├── core/
│   ├── cpu/
│   ├── memory/
│   ├── kernel/
│   ├── hle/
│   ├── loader/
│   ├── gpu/
│   ├── audio/
│   ├── input/
│   ├── debug/
│   ├── runtime/
│   ├── platform/
│   └── abi/
├── ui/
│   └── Pcsx5.Ui/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── compatibility/
│   └── fixtures/
├── tools/
├── third_party/
├── docs/
│   ├── adr/
│   └── compatibility/
├── scripts/
├── packaging/
└── .claude/
    ├── rules/
    ├── skills/
    ├── commands/
    └── settings.json
```

This is the target direction, not a command to immediately move every existing file.

Migration should be incremental.
