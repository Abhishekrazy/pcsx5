import os
import sys
import time
import json
import subprocess
import psutil
import datetime
from screen_capture import capture_window, image_hash, compare_images
from crash_analyzer import analyze_crash
from input_harness import send_key_press

def get_run_id(eboot_name):
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    return f"{eboot_name}_{timestamp}"

def run_loop(pcsx5_path, eboot_path, timeout_seconds=120, capture_intervals=[5, 15, 30, 60, 120]):
    pcsx5_path = os.path.abspath(pcsx5_path)
    eboot_path = os.path.abspath(eboot_path)
    print(f"Starting emulator: {pcsx5_path}")
    print(f"Target: {eboot_path}")
    
    eboot_name = os.path.basename(eboot_path).replace('.bin', '')
    run_id = get_run_id(eboot_name)
    
    # pcsx5_path might be dist\pcsx5_cli.exe
    project_root = os.path.dirname(os.path.dirname(pcsx5_path))
    if os.path.basename(project_root) == "build" or os.path.basename(project_root) == "dist":
        project_root = os.path.dirname(project_root)
        
    artifacts_dir = os.path.join(project_root, "artifacts", "runtime", run_id)
    os.makedirs(artifacts_dir, exist_ok=True)
    
    run_log_path = os.path.join(artifacts_dir, "run.log")
    
    process = subprocess.Popen(
        [pcsx5_path, eboot_path],
        cwd=os.path.dirname(pcsx5_path),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT
    )
    
    log_file = open(run_log_path, 'wb')
    start_time = time.time()
    
    import threading
    def stream_output():
        while True:
            line = process.stdout.readline()
            if not line:
                break
            log_file.write(line)
            log_file.flush()
            
    t = threading.Thread(target=stream_output, daemon=True)
    t.start()
    
    captures = []
    last_img = None
    frames_changing = False
    
    try:
        while True:
            elapsed = time.time() - start_time
            if elapsed > timeout_seconds:
                break
            if process.poll() is not None:
                break
                
            if len(capture_intervals) > 0 and elapsed >= capture_intervals[0]:
                interval = capture_intervals.pop(0)
                img_path = os.path.join(artifacts_dir, f"capture_{interval}s.png")
                img = capture_window("PCSX5", img_path)
                if img:
                    if last_img:
                        diff = compare_images(last_img, img)
                        if diff > 0.01:
                            frames_changing = True
                    last_img = img
                    captures.append({
                        "time": interval,
                        "hash": image_hash(img),
                        "path": img_path
                    })
                    
            time.sleep(1)
    finally:
        if process.poll() is None:
            try:
                parent = psutil.Process(process.pid)
                for child in parent.children(recursive=True):
                    child.kill()
                parent.kill()
            except:
                process.kill()
                
        out, _ = process.communicate()
        if out:
            log_file.write(out)
        log_file.close()

    # The core writes crash dumps into its diagnostics bundle directory.
    # This script passes no --crash-dir, so the core uses its default
    # ("pcsx5_crash"), relative to the cwd set on the Popen call above.
    dump_path = os.path.join(os.path.dirname(pcsx5_path), "pcsx5_crash",
                             "crash_rip_dump.bin")
    crash_info = analyze_crash(run_log_path, dump_path)
    
    result = {
        "run_id": run_id,
        "process_alive_at_end": process.returncode is None,
        "exit_code": process.returncode,
        "duration": time.time() - start_time,
        "captures": captures,
        "frames_changing": frames_changing,
        "crash_info": crash_info
    }
    
    with open(os.path.join(artifacts_dir, "result.json"), "w") as f:
        json.dump(result, f, indent=4)
        
    print(json.dumps(result, indent=4))
    return result

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python autonomous_loop.py <pcsx5.exe> <eboot.bin>")
        sys.exit(1)
    # create copy of capture intervals so it's not exhausted across multiple runs if we were to loop
    intervals = [5, 15, 30, 60, 120]
    run_loop(sys.argv[1], sys.argv[2], capture_intervals=intervals)
