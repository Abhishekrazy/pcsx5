# PCSX5 Update System

## Goal

Users should be able to update PCSX5 without manually downloading and replacing executables.

## Recommended architecture

```text
pcsx5.exe
  |
  +-- UpdateService
        |
        +-- ReleaseManifestClient
        +-- VersionPolicy
        +-- SignatureVerifier
        +-- DownloadManager
        +-- StagingArea
        +-- Installer/Updater helper
        +-- RollbackManager
```

## Release manifest

The manifest should contain:

- product
- channel
- version
- minimum supported updater version
- release notes URL
- artifact URL
- SHA-256
- signature
- signing key identifier
- supported architecture
- minimum Windows version

Example:

```json
{
  "product": "PCSX5",
  "channel": "stable",
  "version": "0.1.0",
  "minimumUpdaterVersion": "0.1.0",
  "architecture": "x64",
  "artifact": {
    "url": "configured release URL",
    "sha256": "..."
  },
  "signature": {
    "algorithm": "configured signing scheme",
    "value": "...",
    "keyId": "..."
  }
}
```

The example is a schema illustration, not a production signing implementation.

## Update flow

1. Read local version.
2. Fetch manifest over HTTPS.
3. Validate schema.
4. Validate channel and architecture.
5. Compare versions.
6. Ask user or follow configured policy.
7. Download to a temporary directory.
8. Verify hash.
9. Verify signature.
10. Stage update.
11. Close the application cleanly.
12. Run a small updater/helper that is not locked by the application.
13. Replace the application files atomically where possible.
14. Preserve the previous version.
15. Start the new version.
16. Record successful startup.
17. Roll back if the new version fails the post-update health check.

## Important

GitHub Releases can be the distribution source, but do not make raw Git tags or arbitrary repository files the trust boundary.

Prefer a release artifact plus a signed manifest.

## Update channels

Support:
- stable
- preview/nightly

Do not mix compatibility promises between channels.

## Core/UI compatibility

The updater must treat the UI and core as a versioned unit unless a stable independent ABI is explicitly implemented.

Never update `pcsx5_core.dll` independently from an incompatible UI.
