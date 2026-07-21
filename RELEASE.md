# Releasing PCSX5

This document describes the release channels, the version naming
convention, and the exact steps to publish a release through GitHub
Actions.

---

## 1. Release channels

PCSX5 uses [semantic versioning](https://semver.org/) with prerelease
suffixes.  There are three channels:

| Channel | Tag format | Example | GitHub Release type |
| ------- | ---------- | ------- | ------------------- |
| **Alpha** | `v<version>-alpha` | `v0.0.1-alpha` | Full release |
| **Beta**  | `v<version>-beta`  | `v0.2.0-beta`  | Full release |
| **Stable** | `v<version>` | `v1.0.0` | Full release |

Notes:

* Every tag is published as a **full release** on GitHub — the
  alpha/beta/rc suffix is part of the version name only, no
  pre-release flag is set.
* Iterations within a channel can add a counter:
  `v0.0.1-alpha.2`, `v0.1.0-beta.3`, `v1.0.0-rc.1`.
* Every push to `main` and every pull request still builds and tests,
  but only `v*` tags publish a GitHub Release.

---

## 2. What a release contains

Each published release attaches:

* `PCSX5_<version>_Release.zip` — portable build, extract and run.
* `PCSX5-<version>-win64-Setup.exe` — Inno Setup installer.

Both are produced by [`build_and_package.ps1`](build_and_package.ps1)
on the CI runner (`windows-latest`, MSVC + Ninja, Release config).
Release notes are auto-generated from the commits since the previous
tag.

---

## 3. How to publish a release

> **Important:** the tag points at a commit, so make sure everything
> you want in the release (license changes, fixes, docs) is committed
> and pushed **before** tagging.

From the repository root, on an up-to-date `main`:

```bash
# 1. Make sure your working tree is committed and pushed
git status
git push

# 2. Create the tag (example: first alpha)
git tag v0.0.1-alpha

# 3. Push the tag — this triggers the release pipeline
git push origin v0.0.1-alpha
```

Then watch the run under the repository's **Actions** tab.  When the
`build` job succeeds, the `release` job publishes the pre-release with
the ZIP and installer attached.

### Subsequent releases

```bash
git tag v0.0.1-alpha.2     # next alpha iteration
git tag v0.1.0-beta        # promote to beta
git tag v1.0.0             # first stable release
git push origin <tag>
```

### Deleting a bad tag

If you pushed a wrong tag and need to redo it:

```bash
git tag -d v0.0.1-alpha            # delete locally
git push origin :refs/tags/v0.0.1-alpha   # delete on GitHub
```

Also delete the corresponding GitHub Release (Releases page → Edit →
Delete) before re-pushing the corrected tag.

---

## 4. Release checklist

1. All intended changes are committed and pushed to `main`.
2. CI on `main` is green.
3. Version/tag name follows the convention in §1.
4. Tag created and pushed (`git push origin <tag>`).
5. Verify the published release: correct pre-release/stable flag,
   both artifacts attached, release notes sane.
