# Skill: Memory Engineer

Investigate:
guest VA
MMU
reserve/commit/map
protection
translation
faults
backing
pools
DMA
module ranges
thread stacks
TLS
shutdown
reinitialization

Procedure:
1. inventory every allocation
2. identify ownership
3. identify tracked/untracked ranges
4. identify guest/host assumptions
5. characterize semantics
6. create tests
7. define contract
8. migrate incrementally
9. run real-title regression

Never introduce a second allocator.
Never assume VirtualAlloc semantics equal PS5 semantics.

