# Development Workflow

## Before coding

Run:

```text
git status
inspect architecture docs
locate subsystem
locate tests
locate call sites
```

## During coding

Keep changes small.

After each meaningful migration:

```text
cmake configure
cmake build
ctest
targeted regression
git diff
```

## Pull request / merge checklist

- [ ] Scope is narrow.
- [ ] Architecture boundary is explicit.
- [ ] No undocumented emulator behavior was invented.
- [ ] Tests or characterization evidence exist.
- [ ] No unnecessary dependency was added.
- [ ] Logs remain useful.
- [ ] Performance impact was measured when relevant.
- [ ] Compatibility matrix was updated if applicable.
- [ ] ADR added for durable architecture decisions.
- [ ] Build artifacts are not committed.

## AI workflow

Claude must first produce a short plan for non-trivial changes.

The plan must identify:
- files to inspect
- subsystem boundary
- expected behavior to preserve
- tests
- risks

Claude should not ask for approval for every tiny edit, but must stop before a destructive rewrite, dependency replacement, ABI break, or broad cross-subsystem refactor.
