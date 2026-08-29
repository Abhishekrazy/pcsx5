import os
import subprocess
import time
import sys
import psutil

def run_game(pcsx5_path, eboot_path, timeout_seconds=120):
    print(f"Starting emulator: {pcsx5_path}")
    print(f"Target: {eboot_path}")
    
    # Run the emulator in a separate process, redirect output to run.log
    run_log_path = os.path.join(os.path.dirname(pcsx5_path), "run.log")
    
    process = subprocess.Popen(
        [pcsx5_path, "--headless", eboot_path],
        cwd=os.path.dirname(pcsx5_path),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT
    )
    
    # Open log for writing
    log_file = open(run_log_path, 'wb')
    
    start_time = time.time()
    crashed = False
    timed_out = False
    
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
    
    try:
        while True:
            if time.time() - start_time > timeout_seconds:
                timed_out = True
                break
            if process.poll() is not None:
                break
            time.sleep(0.5)
    finally:
        # Terminate if still running
        if process.poll() is None:
            try:
                parent = psutil.Process(process.pid)
                for child in parent.children(recursive=True):
                    child.kill()
                parent.kill()
            except:
                process.kill()
                
        # Dump remaining stdout
        out, _ = process.communicate()
        if out:
            log_file.write(out)
        log_file.close()

    print(f"Run completed. Timed out: {timed_out}. Exit code: {process.returncode}")
    return run_log_path

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python runner.py <pcsx5_cli.exe> <eboot.bin>")
        sys.exit(1)
    run_game(sys.argv[1], sys.argv[2])
