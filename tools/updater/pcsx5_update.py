import os
import sys
import hashlib
import shutil
import time
import urllib.request
import json
import logging

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")

# In a real environment, this would point to GitHub Releases or an update server
UPDATE_URL = "https://api.github.com/repos/Abhishekrazy/pcsx5/releases/latest"

def get_file_hash(filepath):
    if not os.path.exists(filepath):
        return None
    hasher = hashlib.sha256()
    with open(filepath, 'rb') as f:
        while chunk := f.read(8192):
            hasher.update(chunk)
    return hasher.hexdigest()

def check_for_updates():
    logging.info("Checking for incremental updates...")
    # Mocking the update check for now since there's no actual release payload yet
    # In reality, this would fetch the latest release JSON and download the DLL asset.
    # To demonstrate the "small module update" requirement:
    logging.info("Core emulator is fully encapsulated in pcsx5_core.dll.")
    logging.info("Downloading new pcsx5_core.dll (mocked for demo)...")
    
    # We simulate an update by verifying the hashes
    target_dll = os.path.join("build", "bin", "Release", "pcsx5_core.dll")
    backup_dll = target_dll + ".backup"
    
    if not os.path.exists(target_dll):
        logging.error(f"{target_dll} not found.")
        return

    current_hash = get_file_hash(target_dll)
    logging.info(f"Current core hash: {current_hash}")
    
    # Normally we download the new dll to a temp file, check hash, then swap
    temp_dll = target_dll + ".new"
    
    # MOCK: just copy the current one to temp to simulate download
    shutil.copy2(target_dll, temp_dll)
    
    # ATOMIC SWAP
    logging.info("Applying update via atomic swap...")
    
    # Wait for the emulator to release the lock on the DLL if it's currently running
    max_retries = 10
    for i in range(max_retries):
        try:
            if os.path.exists(backup_dll):
                os.remove(backup_dll)
            # Rename running DLL to .backup (Windows allows renaming open files!)
            os.rename(target_dll, backup_dll)
            # Move the new DLL into place
            os.rename(temp_dll, target_dll)
            logging.info("Update applied successfully! The new core will be used on next restart.")
            break
        except PermissionError:
            logging.warning("Emulator is currently using the DLL. Waiting 1 second...")
            time.sleep(1)
    else:
        logging.error("Failed to apply update. Close the emulator and try again.")
        if os.path.exists(temp_dll):
            os.remove(temp_dll)

if __name__ == "__main__":
    check_for_updates()
