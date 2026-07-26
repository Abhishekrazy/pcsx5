# PCSX5 — Session Summary

## 2026-07-26 — Full sprint

### Completed (all pushed to `origin/main`)
| # | Task | Status |
|---|------|--------|
| B1.1 | Content-load crash diagnosed (memcpy AV in VCRUNTIME140.dll) | ✅ |
| B1.2 | Fixed via Memory::IsReadable/IsWritable validation | ✅ |
| B1.2b | DLL staging: both `dist/` root and `dist/plugins/` | ✅ |
| I1.1 | JSON replay format defined | ✅ |
| I1.2 | InputBotBackend reads JSON → ControllerState | ✅ |
| I1.3 | InputRecorder wired into GPU::PollEvents() + core_api | ✅ |
| I3.2 | Memory pool: 1 GB at 0x4000000000 | ✅ |
| I3.3 | Descriptor pool (confirmed pre-existing) | ✅ |
| I3.4 | shared_mutex for HLE dispatch (concurrent reads) | ✅ |
| I3.5 | VRR frame pacing (confirmed pre-existing) | ✅ |
| I3.6 | Shader warmup (persistent VkPipelineCache) | ✅ |
| I5.1 | Audio timing (kMaxBuffersInFlight pacing) | ✅ |
| I6.1 | Boot-status timeline in VEH crash dump | ✅ |
| I6.2 | Top-10 stub heat map in --report JSON | ✅ |
| UI | Copy + Raw Logs buttons on crash dialog | ✅ |

### Remaining (mostly reactive / blocked)
| Section | Items | Why blocked |
|---------|-------|-------------|
| B1.3 | Verify menu renders | Game boot intermittent |
| B2 | GPU pipeline gaps | Needs game to reach new draws |
| B3 | HLE stub sweep | Needs game to call different stubs |
| I1.4 | Session recording for regression | Needs I2 pipeline |
| I2.x | Find-fix-rerun pipeline | Needs I1 finished |
| I4.x | Video decoders | Needs Bink SDK / FFmpeg headers |
| I5.2-3 | Audio optimizations | Low priority |
| I6.3-4 | Golden frames, hang snapshots | Needs game progression |

### Key commits
```
f4cd242 progress: mark I6.2 done
ea9fdd6 feat: I1.3 recording wired, I6.2 stub heat map, I6.1 done
2e0cf5a progress: I3.5, I3.6 done, update summary
85254ed feat: I6.1 boot-status timeline in crash dump
1683b72 feat: shared_mutex HLE dispatch + InputRecorder
c329165 feat: InputBotBackend + --play-input/--record-input CLI args
```
