# Retail SELF decryption

## Status today

Fake-signed (fPKG) SELF executables load today: `LoadSelf` in
`src/loader/elf.cpp` parses the SELF container and maps the inner ELF
normally. Fake-signed packages carry **plaintext** segments, so no
decryption is involved.

Retail SELF executables (from disc or PSN PKGs) have **encrypted
segments**. These are detected and rejected in the segment-copy loop of
`ExtractInnerElf` (src/loader/elf.cpp:853), at the
`seg.encrypted()` / `seg.compressed()` checks (src/loader/elf.cpp:875-881):

- `seg.encrypted()` → segment is AES-encrypted; loading is refused.
- `seg.compressed()` → segment is additionally zlib-compressed; refused as
  well (decompression would be trivial once decryption exists).

## Plugin point

Decryption plugs into `ExtractInnerElf` / `ParseSelfImage`, where each
SELF segment is copied from the container into the inner ELF image. The
SELF header table tells you, per segment, which key index applies; the
decrypted segments are then written in place of the ciphertext before the
existing ELF path takes over. No other loader code needs to change.

## Why it is not implemented

Retail SELF decryption requires key material derived by the console's
SAMU (secure co-processor) — per-title/per-segment keys unwrapped by
hardware-fused secrets. That key material is **not publicly available**
and cannot be reproduced in software:

- The [psdevwiki PS5 "Keys" page](https://www.psdevwiki.com/ps5/Keys)
  documents SAMU-side keyseeds and ROM keys. These describe the SAMU's
  internal key hierarchy; they do **not** enable software retail SELF
  decryption on a PC.
- Fake-signed packages work without keys because the scene passcode
  derivation (double-SHA256 key schedule → AES-128-CBC per PKG entry, see
  `src/loader/pkg.h/.cpp`) is fully public.

## Legal model

Same rule as firmware modules: key material must come from the **user's
own console**, never shipped with the emulator. When user-supplied SAMU
key dumps become practical, the config layer gains a key-file path option
and `ExtractInnerElf` consumes it. Until then, retail SELF loading stays
blocked by design, not by accident.
