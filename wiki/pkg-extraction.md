# PKG Extraction Workflow

This guide explains how to extract PS4 and PS5 PKG files using PCSX5's
built-in extraction tools.

## Quick Start

```powershell
# Extract any PKG (auto-detects PS4 vs PS5 by magic byte)
.\build\bin\Release\pcsx5_cli.exe --extract-pkg game.pkg output_dir/
```

For a PS5 PKG, this will:
1. Parse the PS5 PKG header (magic `0x7F464948`)
2. Find and extract the PFS (PlayStation File System) image
3. Mount the PFS and extract `eboot.bin` + all game assets

## Supported Formats

| Format | Magic | Status |
|--------|-------|--------|
| PS4 fPKG | `0x7F434E54` | ✅ Full extraction |
| PS5 fPKG | `0x7F464948` | ✅ PFS → eboot.bin extraction |
| PS4/PS5 Retail NPDRM | `drm_type=3` | ⚠️ Encrypted entries skipped |

## What Gets Extracted

### PS4 PKG
Individual PKG entries are extracted by their entry ID:
- `eboot.bin` — main executable (if inside a data entry)
- `param.sfo` — title metadata
- `icon0.png`, `pic0.png` — game art
- `snd0.at9` — background audio
- `trophy/trophy00.trp` — trophy data
- `license.dat`, `npbind.dat` — DRM metadata

### PS5 PKG
The PFS image is extracted and mounted, then all files are extracted:
- `eboot.bin` — main executable (ELF or SELF-wrapped)
- `sce_sys/param.json` — title metadata (JSON format)
- `sce_sys/icon0.png` — game icon
- `sce_sys/pic0.png`, `sce_sys/pic1.png` — screenshots
- `sce_sys/snd0.at9` — background audio
- `sce_sys/trophy/trophy00.trp` — trophy data

## Fake-Signed (fPKG) vs Retail

### fPKG (Fake-Signed)
Fake-signed PKGs use a known scene passcode
(`00000000000000000000000000000000`  — 32 ASCII zeros).
These extract fully — both plaintext and encrypted entries are handled.

### Retail NPDRM
Retail PKGs use console-specific keys. PCSX5:
- Detects retail PKGs by `drm_type == 3`
- Logs a warning
- Extracts only non-encrypted entries (metadata, icons)
- Skips encrypted data entries with a clear message

## Manual PFS Extraction

For advanced use, you can extract the PFS image manually and mount it:

```powershell
# 1. Extract the PFS image entry from the PKG
#    (PS5 PKGs have a PFS entry with type 0x01)

# 2. Mount and explore the PFS
#    The existing PFS parser handles both PS4 and PS5 formats.
```

## Loading Extracted Executables

After extraction, load the game:

```powershell
# Load extracted eboot.bin
.\build\bin\Release\pcsx5_cli.exe "output_dir/eboot.bin"

# fPKG SELFs are decrypted automatically by the SELF loader
```

## Troubleshooting

| Symptom | Cause | Solution |
|---------|-------|----------|
| `retail NPDRM PKG` warning | Retail PKG with encrypted entries | Can't extract — need console keys |
| `No PFS image entry found` | Not a PS5 PKG, or corrupted | Check magic byte |
| `Failed to mount PFS` | Corrupted or unsupported PFS version | Check PFS version (PS5 uses v2) |
| `eboot.bin not found` | Game stores ELF elsewhere | Extract all files and search |

## Technical Details

### PS5 PKG Header Layout

```
Offset  | Size | Field
--------|------|------------------
0x00    | 4    | Magic (0x7F464948)
0x04    | 2    | Package type
0x06    | 2    | Revision
0x0C    | 4    | File entry count
0x10    | 4    | Entry table offset
0x14    | 4    | Entry table size
0x18    | 8    | Body offset
0x20    | 8    | Body size
0x30    | 36   | Content ID (ASCII)
0x60    | 4    | DRM type
0x64    | 4    | Content type
0x400   | 64   | Layout table (FIH/PFS/SC/SI offsets+sizes)
```

### PS5 Entry Table

Each entry is 32 bytes:

```
Offset | Size | Field
-------|------|------------------
0x00   | 4    | Entry ID
0x04   | 4    | Type/flags (bit 31 = encrypted)
0x08   | 8    | Offset (absolute file offset)
0x10   | 8    | Size
0x18   | 8    | Padding
```

File names are stored in the first 256 bytes of each entry's data.

### fPKG Key Derivation

For fake-signed packages, the decryption key is:
```
key = SHA256(SHA256(be32(index)) || SHA256(content_id padded to 0x30) || passcode)
```
Where `index = 3` for data entries ("dk3") and the standard scene passcode
is 32 ASCII `'0'` characters.
