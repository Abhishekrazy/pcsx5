#!/usr/bin/env python3
"""
PS5 PRX NID Extractor
Parses decrypted PS5 .prx / .esbak ELF files to extract NID symbol mappings.
Outputs a C++ header with RegisterSymbol stubs for every exported function.
"""

import struct
import sys
import os
import hashlib
import base64
from pathlib import Path

# ELF constants
ET_NONE, ET_REL, ET_EXEC, ET_DYN = 0, 1, 2, 3
ELFMAG = b'\x7fELF'
EM_X86_64 = 62

SHT_NULL    = 0
SHT_PROGBITS= 1
SHT_SYMTAB  = 2
SHT_STRTAB  = 3
SHT_RELA    = 4
SHT_HASH    = 5
SHT_DYNAMIC = 6
SHT_NOTE    = 7
SHT_DYNSYM  = 11

def read_elf64_header(data):
    if data[:4] != ELFMAG:
        return None
    # ELF64 header: ident(16) + type(2) + machine(2) + version(4) + entry(8) +
    #               phoff(8) + shoff(8) + flags(4) + ehsize(2) + phentsize(2) +
    #               phnum(2) + shentsize(2) + shnum(2) + shstrndx(2) = 64 bytes
    if len(data) < 64:
        return None
    e_ident    = data[0:16]
    e_type     = struct.unpack_from('<H', data, 16)[0]
    e_machine  = struct.unpack_from('<H', data, 18)[0]
    e_version  = struct.unpack_from('<I', data, 20)[0]
    e_entry    = struct.unpack_from('<Q', data, 24)[0]
    e_phoff    = struct.unpack_from('<Q', data, 32)[0]
    e_shoff    = struct.unpack_from('<Q', data, 40)[0]
    e_flags    = struct.unpack_from('<I', data, 48)[0]
    e_ehsize   = struct.unpack_from('<H', data, 52)[0]
    e_phentsize= struct.unpack_from('<H', data, 54)[0]
    e_phnum    = struct.unpack_from('<H', data, 56)[0]
    e_shentsize= struct.unpack_from('<H', data, 58)[0]
    e_shnum    = struct.unpack_from('<H', data, 60)[0]
    e_shstrndx = struct.unpack_from('<H', data, 62)[0]
    return {
        'e_ident': e_ident, 'e_type': e_type, 'e_machine': e_machine,
        'e_version': e_version, 'e_entry': e_entry,
        'e_phoff': e_phoff, 'e_shoff': e_shoff, 'e_flags': e_flags,
        'e_ehsize': e_ehsize, 'e_phentsize': e_phentsize, 'e_phnum': e_phnum,
        'e_shentsize': e_shentsize, 'e_shnum': e_shnum, 'e_shstrndx': e_shstrndx,
    }

def read_shdr64(data, offset):
    fmt = '<IIQQQQIIQQ'
    size = struct.calcsize(fmt)
    f = struct.unpack(fmt, data[offset:offset+size])
    return {
        'sh_name':      f[0],
        'sh_type':      f[1],
        'sh_flags':     f[2],
        'sh_addr':      f[3],
        'sh_offset':    f[4],
        'sh_size':      f[5],
        'sh_link':      f[6],
        'sh_info':      f[7],
        'sh_addralign': f[8],
        'sh_entsize':   f[9],
    }

def read_sym64(data, offset):
    fmt = '<IBBHQQ'
    size = struct.calcsize(fmt)
    f = struct.unpack(fmt, data[offset:offset+size])
    return {
        'st_name':  f[0],
        'st_info':  f[1],
        'st_other': f[2],
        'st_shndx': f[3],
        'st_value': f[4],
        'st_size':  f[5],
    }

def get_cstr(data, offset):
    end = data.index(b'\x00', offset)
    return data[offset:end].decode('utf-8', errors='replace')

def nid_for_name(name: str) -> str:
    """Compute the PS4/PS5 NID for a symbol name using SHA1-based scheme."""
    # PS4/PS5 NID = first 8 bytes of SHA1(name + "\x00" + "SCE-PS5") base64url-encoded
    suffix = b"SCE-PS5"
    h = hashlib.sha1(name.encode() + b"\x51" + suffix).digest()
    # Take first 8 bytes, base64url encode without padding
    nid = base64.b64encode(h[:8]).decode().rstrip('=').replace('+', '+').replace('/', '/')
    return nid

def extract_symbols(filepath: str):
    with open(filepath, 'rb') as f:
        data = f.read()

    if data[:4] != ELFMAG:
        # Maybe it's an ESBAK (sharpemu backup) - try directly
        print(f"  [!] Not a standard ELF: {filepath}")
        return []

    ehdr = read_elf64_header(data)
    if not ehdr:
        return []

    if ehdr['e_shoff'] == 0 or ehdr['e_shnum'] == 0:
        print(f"  [!] No section headers in {filepath}")
        return []

    # Read section headers
    sections = []
    for i in range(ehdr['e_shnum']):
        off = ehdr['e_shoff'] + i * ehdr['e_shentsize']
        sections.append(read_shdr64(data, off))

    # Read section name string table
    shstrtab = sections[ehdr['e_shstrndx']]
    shstrtab_data = data[shstrtab['sh_offset']:shstrtab['sh_offset'] + shstrtab['sh_size']]

    # Find .dynsym and its string table
    dynsym_sec = None
    dynstr_sec = None
    for sec in sections:
        name = get_cstr(shstrtab_data, sec['sh_name'])
        if sec['sh_type'] == SHT_DYNSYM:
            dynsym_sec = sec
        if name == '.dynstr':
            dynstr_sec = sec

    if not dynsym_sec or not dynstr_sec:
        print(f"  [!] No .dynsym/.dynstr in {filepath}")
        return []

    dynstr_data = data[dynstr_sec['sh_offset']:dynstr_sec['sh_offset'] + dynstr_sec['sh_size']]
    
    symbols = []
    num_syms = dynsym_sec['sh_size'] // dynsym_sec['sh_entsize']
    for i in range(num_syms):
        off = dynsym_sec['sh_offset'] + i * dynsym_sec['sh_entsize']
        sym = read_sym64(data, off)
        if sym['st_name'] == 0:
            continue
        stype = sym['st_info'] & 0xF
        sbind = (sym['st_info'] >> 4) & 0xF
        # Only global/weak functions or objects
        if stype not in (1, 2) or sbind not in (1, 2):
            continue
        name = get_cstr(dynstr_data, sym['st_name'])
        symbols.append({
            'name': name,
            'value': sym['st_value'],
            'size': sym['st_size'],
            'type': stype,
            'bind': sbind,
        })

    return symbols

def process_prx(filepath: str, module_name: str, out_lines: list):
    print(f"Processing: {filepath} as module '{module_name}'")
    syms = extract_symbols(filepath)
    if not syms:
        return 0

    count = 0
    out_lines.append(f"\n        // ===== Module: {module_name} ({os.path.basename(filepath)}) =====")
    for sym in syms:
        name = sym['name']
        # Skip internal/mangled
        if name.startswith('__') and not name.startswith('__cxa'):
            continue
        nid = nid_for_name(name)
        safe_name = name.replace('"', '\\"')
        out_lines.append(
            f'        // {safe_name}\n'
            f'        RegisterSymbol("{module_name}", "{nid}#j#j", []'
            f'(const GuestArgs& /*a*/) -> u64 {{ return 0; }});'
        )
        count += 1
        print(f"  {name} -> NID:{nid}")

    print(f"  Extracted {count} symbols from {module_name}")
    return count

def main():
    search_roots = [
        r"I:\Installed Games\sharpemu-win64-fbf2c2d\Games",
    ]

    all_prx = []
    for root in search_roots:
        for dirpath, _, files in os.walk(root):
            for fname in files:
                if fname.endswith('.esbak') and '.prx' in fname:
                    all_prx.append(os.path.join(dirpath, fname))
                elif fname.endswith('.prx') and not fname.endswith('.esbak'):
                    all_prx.append(os.path.join(dirpath, fname))

    if not all_prx:
        print("No PRX files found. Run from the project root or specify paths.")
        return

    print(f"Found {len(all_prx)} PRX files:")
    for p in all_prx:
        print(f"  {p}")

    out_lines = [
        "// AUTO-GENERATED by extract_nids.py",
        "// PRX symbol stubs — edit handlers as needed",
        "#pragma once",
    ]

    total = 0
    seen_modules = set()
    for prx_path in all_prx:
        fname = os.path.basename(prx_path)
        # Derive module name: libc.prx.esbak -> libc, libSceJobManager.prx -> libSceJobManager
        mod = fname.replace('.prx.esbak', '').replace('.prx', '')
        if mod in seen_modules:
            continue
        seen_modules.add(mod)
        total += process_prx(prx_path, mod, out_lines)

    out_path = Path(r"I:\Personal\Windows\pcsx5\src\hle\generated_stubs.h")
    with open(out_path, 'w') as f:
        f.write('\n'.join(out_lines))

    print(f"\nDone! Extracted {total} total symbols.")
    print(f"Output written to: {out_path}")

if __name__ == '__main__':
    main()
