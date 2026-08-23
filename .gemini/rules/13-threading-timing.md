# Threading and Timing Governance

Thread ownership must be explicit.

For each thread:
- owner
- purpose
- lifecycle
- synchronization
- shutdown behavior
- guest-visible effect

Never add a thread merely to solve a race without identifying the underlying synchronization contract.

Timing fixes require measurement.

Do not use arbitrary sleeps as synchronization.

If timing is uncertain:
instrument before changing.

