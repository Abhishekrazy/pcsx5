# Release / Update Governance

Release pipeline:

source
-> reproducible build
-> tests
-> package
-> artifact hash
-> signature
-> release manifest
-> channel
-> update client
-> staged install
-> health check
-> rollback

Never execute arbitrary downloaded payloads.

Update system must verify:
- version
- platform
- architecture
- artifact integrity
- signature
- compatibility

Core/UI ABI compatibility must be explicit.

Channels should support stable/preview where justified.

