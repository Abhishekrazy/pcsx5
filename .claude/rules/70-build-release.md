# Rule: Build and Release

CMake is the source of truth for native targets.

Required build concepts:
- Debug
- RelWithDebInfo
- Release
- tests

Release artifacts must be reproducible from source and versioned.

The update system must:
- check a signed/validated release manifest
- compare semantic versions
- download only from the configured release source
- verify artifact integrity
- stage the update
- replace files safely on restart
- support rollback to the previous known-good version
- never overwrite a running core DLL
- never execute an unverified downloaded binary

Do not implement auto-update by blindly downloading and executing a URL.
