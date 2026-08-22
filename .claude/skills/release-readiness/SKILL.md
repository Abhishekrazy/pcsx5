# Release Readiness Skill

Check:

- clean source build
- tests
- packaging
- version consistency
- core/UI compatibility
- symbols/debug information policy
- update manifest
- artifact hash
- signature
- installer/updater behavior
- rollback
- release notes
- compatibility matrix

Never call a release ready if an artifact is unsigned when signing is required by policy.
