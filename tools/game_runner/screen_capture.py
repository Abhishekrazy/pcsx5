import os
import time
import ctypes
import ctypes.wintypes
import hashlib
from PIL import ImageGrab, Image
import math

user32 = ctypes.windll.user32

def find_window_by_title(window_title_substring):
    """Return the HWND of the first visible window whose title contains the
    given substring, or 0 if none matches."""
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
    return hwnd_found[0]


def find_window_by_pid(pid):
    """Return the HWND of the first visible top-level window owned by `pid`, or
    0 if the process has none.

    Title matching is not good enough here: the repository directory is named
    pcsx5, so an editor or terminal window matches "pcsx5" and would be captured
    as though it were the emulator's output.  Process ownership is exact."""
    found = [0]

    def enum_handler(hwnd, ctx):
        if not user32.IsWindowVisible(hwnd):
            return True
        owner = ctypes.wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            found[0] = hwnd
            return False
        return True

    EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_int, ctypes.c_int)
    user32.EnumWindows(EnumWindowsProc(enum_handler), 0)
    return found[0]


def get_hwnd_rect(hwnd):
    """Screen rect of an explicit HWND, or None if the handle is not a live
    window.  Preferred over title matching: pcsx5 prints its HWND on stdout as
    PCSX5_WINDOW_HANDLE=<n>, which is unambiguous when several windows exist."""
    if not hwnd or not user32.IsWindow(hwnd):
        return None
    rect = ctypes.wintypes.RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        return None
    return (rect.left, rect.top, rect.right, rect.bottom)


def capture_rect(rect, output_path):
    """Grab a screen rectangle to output_path.  Returns the PIL image, or None
    if the rect is off-screen or degenerate (a minimised window reports
    left < -32000)."""
    try:
        if not rect:
            img = ImageGrab.grab()
        else:
            left, top, right, bottom = rect
            if left < -32000:
                return None
            if right <= left or bottom <= top:
                return None
            img = ImageGrab.grab(bbox=rect)
        img.save(output_path)
        return img
    except Exception as e:
        print(f"Screen capture failed: {e}")
        return None


def capture_hwnd(hwnd, output_path):
    """Capture an explicit window handle.  Returns None when the window is not
    on screen, which the caller must distinguish from 'captured a black frame'."""
    rect = get_hwnd_rect(hwnd)
    if rect is None:
        return None
    return capture_rect(rect, output_path)


def frame_diff_ratio(img1, img2, size=(128, 128)):
    """Fraction of pixels that changed between two frames, computed on a
    downscaled grayscale pair.

    compare_images() below walks every pixel of a full-resolution frame, which
    costs seconds per comparison at 1080p and is unusable inside a sampling
    loop.  This variant is bounded by `size` and is what the runtime session
    driver uses for frame-change detection.  compare_images() is left untouched
    for its existing callers."""
    a = img1.convert("L").resize(size, Image.Resampling.BILINEAR).tobytes()
    b = img2.convert("L").resize(size, Image.Resampling.BILINEAR).tobytes()
    changed = sum(1 for x, y in zip(a, b) if abs(x - y) > 10)
    return changed / float(len(a))


def get_window_rect(window_title_substring):
    """Screen rect of the first visible window matching a title substring."""
    hwnd = find_window_by_title(window_title_substring)
    if hwnd == 0:
        return None
    return get_hwnd_rect(hwnd)


def capture_window(window_title, output_path):
    """Capture a window found by title, falling back to a full-screen grab when
    no window matches (unchanged behaviour, now sharing one capture path)."""
    return capture_rect(get_window_rect(window_title), output_path)


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
