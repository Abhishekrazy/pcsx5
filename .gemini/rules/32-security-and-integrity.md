# Security and Integrity

Although this is an emulator, developer infrastructure and update mechanisms are security-sensitive.

Protect:
- downloaded update artifacts
- plugin loading
- scripts
- save/config data
- crash dumps
- paths
- external process execution

Never:
- execute arbitrary downloaded scripts
- trust unsigned plugin code
- allow path traversal in tooling
- run repository scripts with elevated privileges unnecessarily
- store secrets in source/config

