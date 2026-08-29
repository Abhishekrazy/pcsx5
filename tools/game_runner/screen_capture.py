import os
import time
import ctypes
import ctypes.wintypes
import hashlib
from PIL import ImageGrab, Image
import math

user32 = ctypes.windll.user32

def get_window_rect(window_title_substring):
    hwnd_found = [0]
    
    def enum_handler(hwnd, ctx):
        if user32.IsWindowVisible(hwnd):
            length = user32.GetWindowTextLengthW(hwnd)
            if length > 0:
                buff = ctypes.create_unicode_buffer(length + 1)
                user32.GetWindowTextW(hwnd, buff, length + 1)
                if window_title_substring.lower() in buff.value.lower():
                    hwnd_found[0] = hwnd
        return True
    
    EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_int, ctypes.c_int)
    user32.EnumWindows(EnumWindowsProc(enum_handler), 0)
    
    if hwnd_found[0] == 0:
        return None
    
    rect = ctypes.wintypes.RECT()
    user32.GetWindowRect(hwnd_found[0], ctypes.byref(rect))
    return (rect.left, rect.top, rect.right, rect.bottom)

def capture_window(window_title, output_path):
    rect = get_window_rect(window_title)
    try:
        if not rect:
            img = ImageGrab.grab()
        else:
            if rect[0] < -32000:
                return None
            
            # Ensure bbox has positive width and height
            left, top, right, bottom = rect
            if right <= left or bottom <= top:
                return None
                
            img = ImageGrab.grab(bbox=rect)
            
        img.save(output_path)
        return img
    except Exception as e:
        print(f"Screen capture failed: {e}")
        return None

def image_hash(img):
    img_gray = img.convert("L").resize((32, 32), Image.Resampling.LANCZOS)
    return hashlib.md5(img_gray.tobytes()).hexdigest()

def compare_images(img1, img2):
    if img1.size != img2.size:
        img2 = img2.resize(img1.size)
    
    diff = 0
    d1 = img1.getdata()
    d2 = img2.getdata()
    
    for c1, c2 in zip(d1, d2):
        if isinstance(c1, int):
            if abs(c1 - c2) > 10:
                diff += 1
        else:
            if sum(abs(a - b) for a, b in zip(c1, c2)) > 30:
                diff += 1
                
    return diff / (img1.width * img1.height)
