# Legacy Migration Governance

Existing code is not automatically bad because it is old.

Migration:

Inventory
-> characterize
-> map behavior
-> define boundary
-> compatibility/adaptor
-> migrate
-> validate
-> remove obsolete path

Never delete a legacy path merely because a new path exists.

Remove only after:
- callers migrated
- tests cover behavior
- runtime behavior compared
- rollback/containment understood

