Add the Reporting Governance rule to the PCSX5 Gemini harness.

Then retroactively normalize the current lifecycle workstream into the new reporting system.

Read the existing lifecycle completion report and repository state.

Create:

docs/audits/AUDIT-2026-08-23-subsystem-lifecycle.md
docs/tasks/TASK-2026-08-23-subsystem-lifecycle.md

Do not change emulator source code.

The audit must preserve the findings from the completed lifecycle investigation, including:
- lifecycle inventory
- initialization graph
- teardown graph
- FD table deadlock
- Kernel state retention
- HLE physical pool leak
- GPU XInput stale state
- detached Vblank thread
- partial reinitialization status
- characterization test
- exact test results
- real-title regression
- remaining risks

The task report must record the actual implementation that occurred:
- fd_table.cpp lock-discipline fix
- locked helpers
- bounds checks
- lifecycle characterization test
- 45/45 CTest
- PPSA02929 and PPSA21564 regression
- architectural impact
- remaining lifecycle risks

Do not claim full reinitialization support.

After creating the files, inspect them for consistency and report:
1. files created
2. evidence status
3. any discrepancy between the old completion report and repository state
4. recommended next task

Do not start the recommended next task.
