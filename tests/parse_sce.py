import struct
import sys

def parse_elf(filepath):
    print(f"Parsing ELF: {filepath}")
    with open(filepath, "rb") as f:
        data = f.read()

    # ELF64 Header format
    # e_ident: 16s, e_type: H, e_machine: H, e_version: I, e_entry: Q, e_phoff: Q, e_shoff: Q, e_flags: I, e_ehsize: H, e_phentsize: H, e_phnum: H, e_shentsize: H, e_shnum: H, e_shstrndx: H
    header_fmt = "<16sHHIQQQIHHHHHH"
    header_size = struct.calcsize(header_fmt)
    
    if len(data) < header_size:
        print("File too small to be an ELF64")
        return

    (e_ident, e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, 
     e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_fmt__deprecated = struct.unpack(header_fmt, data[:header_size])

    print("--- ELF Header ---")
    print(f"e_ident: {e_ident[:4]}")
    print(f"e_type: 0x{e_type:X}")
    print(f"e_machine: 0x{e_machine:X}")
    print(f"e_entry: 0x{e_entry:X}")
    print(f"e_phoff: 0x{e_phoff:X} (num: {e_phnum})")
    print(f"e_shoff: 0x{e_shoff:X} (num: {e_shnum})")
    print(f"e_shstrndx: {e_shstrndx}")

    # Read Program Headers
    print("\n--- Program Headers ---")
    phdr_fmt = "<IIQQQQQQ"
    phdr_size = struct.calcsize(phdr_fmt)
    for i in range(e_phnum):
        offset = e_phoff + i * phdr_size
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack(phdr_fmt, data[offset:offset+phdr_size])
        print(f"PH #{i}: Type: {p_type}, Flags: 0x{p_flags:X}, Offset: 0x{p_offset:X}, VAddr: 0x{p_vaddr:X}, FileSz: {p_filesz}, MemSz: {p_memsz}")

    # Read Section Headers to get names
    print("\n--- Section Headers ---")
    shdr_fmt = "<IIQQQQIIQQ"
    shdr_size = struct.calcsize(shdr_fmt)
    
    # First get string table offset
    shstr_offset = e_shoff + e_shstrndx * shdr_size
    _, _, _, _, shstr_file_offset, shstr_size, _, _, _, _ = struct.unpack(shdr_fmt, data[shstr_offset:shstr_offset+shdr_size])
    shstrtab = data[shstr_file_offset:shstr_file_offset+shstr_size]

    sections = []
    for i in range(e_shnum):
        offset = e_shoff + i * shdr_size
        sh_name_idx, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack(shdr_fmt, data[offset:offset+shdr_size])
        
        # Get section name
        name_end = shstrtab.find(b"\0", sh_name_idx)
        name = shstrtab[sh_name_idx:name_end].decode("utf-8") if name_end != -1 else ""
        print(f"Section #{i}: Name: '{name}', Type: {sh_type}, Addr: 0x{sh_addr:X}, Offset: 0x{sh_offset:X}, Size: {sh_size}")
        
        sections.append({
            "name": name,
            "addr": sh_addr,
            "offset": sh_offset,
            "size": sh_size,
            "type": sh_type
        })

    # Search for symbol tables (.symtab or .dynsym) to list symbols
    for sec in sections:
        if sec["type"] in (2, 11): # SHT_SYMTAB=2, SHT_DYNSYM=11
            # Find string table linked to it
            str_sec = sections[sec["name"] == ".symtab" and sections.index(next(s for s in sections if s["name"] == ".strtab")) or sections.index(next(s for s in sections if s["name"] == ".dynstr"))]
            strtab = data[str_sec["offset"]:str_sec["offset"]+str_sec["size"]]
            
            sym_fmt = "<IQQ" # Simple read, actual size is 24 bytes in ELF64
            # Elf64_Sym: st_name: I, st_info: B, st_other: B, st_shndx: H, st_value: Q, st_size: Q
            sym_fmt_full = "<IBBHQQ"
            sym_size = struct.calcsize(sym_fmt_full)
            
            print(f"\n--- Symbols in {sec['name']} ---")
            num_syms = sec["size"] // sym_size
            for j in range(num_syms):
                offset = sec["offset"] + j * sym_size
                st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack(sym_fmt_full, data[offset:offset+sym_size])
                if st_name < len(strtab):
                    name_end = strtab.find(b"\0", st_name)
                    sym_name = strtab[st_name:name_end].decode("utf-8") if name_end != -1 else ""
                    if sym_name and ("start" in sym_name or "entry" in sym_name or "main" in sym_name or "sce" in sym_name):
                        print(f"Symbol: '{sym_name}' -> Value: 0x{st_value:X}, Size: {st_size}, Bind/Type: {st_info}")

if __name__ == "__main__":
    parse_elf("I:\\Installed Games\\sharpemu-win64-fbf2c2d\\Games\\PPSA02929-app0\\eboot.bin.esbak")
