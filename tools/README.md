# PCSX5 Dreaming Sarah (PPSA02929) — Python Automation & Brute-Force Suite

All tools live in `tools/`.  Python 3.14 + capstone + numpy + **PyTorch CUDA
(RTX 5070 Ti, 17 GB, sm_120)** are available.  Run from the repo root.

## The harness (build -> run -> capture -> analyze -> repeat)

```
python tools/autorun.py run                     # one build + boot, print verdict
python tools/autorun.py loop --max 50           # rebuild + rerun 50x
python tools/autorun.py loop --max 50 --until menus   # stop when a marker appears
python tools/autorun.py loop --max 100 --trace        # with guest INT3 tracer
python tools/autorun.py loop --max 100 --probe        # with HLE memcpy string probe
python tools/autorun.py history                 # print run history (JSONL)
python tools/autorun.py grep <regex> [n]        # grep the latest captured log
```

- Auto-kills the lingering `pcsx5_cli.exe` that locks `pcsx5_core.dll` (the
  cause of intermittent `LNK1104` build failures).
- Captures the full log + stdout to `.work/autologs/run_NNNN_<ts>.log`.
- Classifies outcome: `uncaught-exception` (with thrown C++ type + message),
  `guest-crash`, or progress marker (`first-draw` / `shaders` / `pthreads`).
- Appends each verdict to `.work/autorun_history.jsonl` (JSONL, machine-readable).

## Static reverse engineering (read-only)

```
python tools/dream_tool.py disasm <gva> [n]   # disassemble (SELF->ELF correct)
python tools/dream_tool.py segments           # eboot LOAD/TLS map
python tools/dream_tool.py tls                # PT_TLS info
python tools/dre_re.py strings [minlen]       # engine strings
python tools/dre_re.py callgraph <gva> [depth]
python tools/dre_re.py xrefs <gva>            # RIP-relative xrefs (needs detail flag)
python tools/dre_xref.py <gva>                # robust raw RIP xref scan
python tools/gpu_analyze.py search <hex>      # GPU memchr for a byte pattern
python tools/gpu_analyze.py cctable <addr>    # char-class table classifier
python tools/gpu_analyze.py refs <addr>       # vectorized xref scan
python tools/gpu_analyze.py scan4 <u32>       # find a numeric immediate
```

## GPU brute-force (CUDA)

`gpu_analyze.py` uses PyTorch CUDA for the O(NxM) scan jobs that are slow in
pure Python (byte-pattern search, immediate scan, xref scan over the 7.7 MB
reconstructed ELF).  The backend auto-selects `cuda` when available and prints
it on every run (verified: `backend = cuda` on the RTX 5070 Ti).

To add a new brute-force task, drop a function that builds a torch tensor on the
`cuda` device and parallelizes the independent work items; see `search()` /
`scan4()` as templates.

## Current crash (what all of this is driving at)

Deterministic `std::invalid_argument("parse error - unexpected '"'")` on a
Construct-runtime `P.Worker` thread, from a guest-native truncation of
`"images/precious_stones-sheet0.png"` -> `"image"`.  See
`.work/investigation_state.md` and `.work/datajs_parse_crash_findings.md` for
the full root-cause narrowing (HLE, thread race, TLS, tokenizer all ruled out).
