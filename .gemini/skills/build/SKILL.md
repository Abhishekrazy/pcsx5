# Skill: Build and Validation

Build using repository-approved CMake/toolchain configuration.

Before:
- inspect status
- identify configuration
- identify dependencies

After:
- build
- focused tests
- CTest
- relevant boot/regression tests

Report exact commands and results.

Never call a skipped test passed.
Never alter tests just to make the build green.

