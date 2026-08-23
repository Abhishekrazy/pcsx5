# Configuration and State Lifecycle Governance

Configuration has one authoritative initialization owner.

Consumers read configuration; they do not silently replace global configuration context.

Initialization must define:
- owner
- order
- idempotency
- reinitialization semantics
- shutdown
- persistence
- override precedence

CLI overrides must not be silently clobbered by Lua or another consumer.

For global state, document:
owner
lifetime
thread safety
initialization
shutdown
reset/test isolation

