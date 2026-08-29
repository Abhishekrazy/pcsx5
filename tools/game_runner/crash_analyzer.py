import os
import re
import struct

def analyze_crash(run_log_path, dump_path=None):
    if not os.path.exists(run_log_path):
        return {"status": "NO_LOG"}
    
    with open(run_log_path, 'r', encoding='utf-8', errors='ignore') as f:
        log_lines = f.readlines()
        
    result = {
        "status": "PROCESS_EXITED",
        "last_hle_trace": None,
        "veh_exception": None,
        "assertion": None,
        "unresolved_nid": None,
        "crash_type": "UNKNOWN"
    }
    
    # Heuristics
    hle_traces = []
    
    for line in log_lines:
        # Strip ANSI escape codes
        line = re.sub(r'\x1b\[[0-9;]*m', '', line).strip()
        
        if "VEH Exception Triggered" in line:
            result["veh_exception"] = line
            result["crash_type"] = "ACCESS VIOLATION"
        elif "Assertion failed" in line or "ASSERTION" in line:
            result["assertion"] = line
            result["crash_type"] = "ASSERTION"
        elif "Unimplemented stub called" in line or "Unresolved symbol requested" in line:
            result["unresolved_nid"] = line
        elif "HLE trace" in line:
            hle_traces.append(line)
        elif "pcsx5 shutdown cleanly" in line:
            result["status"] = "CLEAN_SHUTDOWN"
            result["crash_type"] = "NONE"
            
    if hle_traces:
        # Avoid getting the cut-off trace
        if len(hle_traces[-1]) < 30 and len(hle_traces) > 1:
            result["last_hle_trace"] = hle_traces[-2]
        else:
            result["last_hle_trace"] = hle_traces[-1]
        
    # Analyze dump
    if dump_path and os.path.exists(dump_path) and result["crash_type"] != "NONE":
        try:
            import capstone
            md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
            dump = open(dump_path, 'rb').read()
            # The dump is exactly [RIP-0x8000, RIP+0x8000]
            # Disassemble at offset 0x8000
            rip_offset = 0x8000
            if len(dump) > rip_offset:
                code = dump[rip_offset:rip_offset+64]
                disasm_lines = []
                for i in md.disasm(code, 0x0):
                    disasm_lines.append(f"{i.mnemonic} {i.op_str}")
                result["rip_disasm"] = disasm_lines
        except Exception as e:
            result["dump_error"] = str(e)
            
    return result

if __name__ == "__main__":
    import sys
    if len(sys.argv) < 2:
        print("Usage: python crash_analyzer.py <run.log> [crash_rip_dump.bin]")
        sys.exit(1)
    
    res = analyze_crash(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    import json
    print(json.dumps(res, indent=4))
