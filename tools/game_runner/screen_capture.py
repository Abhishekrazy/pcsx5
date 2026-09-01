import os
import time
import ctypes
import threading
import ctypes.wintypes
import hashlib
from PIL import ImageGrab, Image
import math

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32


class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                ("biClrImportant", ctypes.c_uint32)]


# WindowFromPoint takes POINT by value and returns a handle; without these the
# handle is truncated on 64-bit Windows and every occlusion check misreports.
user32.WindowFromPoint.argtypes = [_POINT]
user32.WindowFromPoint.restype = ctypes.wintypes.HWND
user32.GetAncestor.argtypes = [ctypes.wintypes.HWND, ctypes.c_uint]
user32.GetAncestor.restype = ctypes.wintypes.HWND

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

    # HWND and LPARAM are pointer-sized. Declaring them c_int truncates them on
    # 64-bit Windows and the handle no longer resolves, so a window that does
    # exist is reported as absent -- indistinguishable from an emulator that
    # never opened one.
    EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.wintypes.HWND,
                                         ctypes.wintypes.LPARAM)
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

    # HWND and LPARAM are pointer-sized. Declaring them c_int truncates them on
    # 64-bit Windows and the handle no longer resolves, so a window that does
    # exist is reported as absent -- indistinguishable from an emulator that
    # never opened one.
    EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.wintypes.HWND,
                                         ctypes.wintypes.LPARAM)
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


def focus_window(hwnd):
    """Bring a window to the foreground so injected input reaches it.

    SendInput delivers to whatever is focused, not to a chosen window, so a
    verification run that does not do this silently sends its keys and clicks
    into another application and reports a UI that "did not respond".
    AttachThreadInput is needed because SetForegroundWindow is refused for a
    process that does not own the current foreground window."""
    if not hwnd or not user32.IsWindow(hwnd):
        return False
    kernel32 = ctypes.windll.kernel32
    SW_RESTORE = 9
    try:
        if user32.IsIconic(hwnd):
            user32.ShowWindow(hwnd, SW_RESTORE)
        fg = user32.GetForegroundWindow()
        if fg == hwnd:
            return True
        target_thread = user32.GetWindowThreadProcessId(hwnd, None)
        this_thread = kernel32.GetCurrentThreadId()
        attached = False
        if target_thread and target_thread != this_thread:
            attached = bool(user32.AttachThreadInput(this_thread, target_thread, True))
        user32.BringWindowToTop(hwnd)
        ok = bool(user32.SetForegroundWindow(hwnd))
        if attached:
            user32.AttachThreadInput(this_thread, target_thread, False)
        return ok
    except Exception:
        return False


def capture_rect(rect, output_path):
    """Grab a screen rectangle to output_path.  Returns the PIL image, or None
    when there is nothing legitimate to capture: no rect, an off-screen window
    (a minimised one reports left < -32000), or a degenerate rect.

    There is deliberately no whole-desktop fallback.  Capturing the desktop when
    the emulator window cannot be found does not produce a picture of the game --
    it produces a picture of whatever the developer is doing, which then feeds
    frame hashing and change ratios and reports a title as rendering and
    progressing when nothing was ever drawn.  A run with no capturable window
    must be classified as having no frames, not given fabricated ones."""
    try:
        if not rect:
            return None
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


def _window_is_unobstructed(hwnd, rect):
    """True when `hwnd` (or one of its children) is the top-level window at
    every sampled point of its own rectangle.

    Used only to decide whether a screen grab would photograph the emulator or
    whatever happens to be lying on top of it.  Sampling five points rather than
    the whole rect keeps this cheap; a window covered only in a corner still
    reads as obstructed at one of the samples in practice, and the cost of a
    false "obstructed" is a skipped frame, which is safe."""
    left, top, right, bottom = rect
    w, h = right - left, bottom - top
    if w <= 2 or h <= 2:
        return False
    xs = (left + w // 4, left + w // 2, right - w // 4)
    ys = (top + h // 4, top + h // 2, bottom - h // 4)
    points = ((xs[1], ys[1]), (xs[0], ys[0]), (xs[2], ys[0]),
              (xs[0], ys[2]), (xs[2], ys[2]))
    GA_ROOT = 2
    for x, y in points:
        pt = _POINT(x, y)
        top_hwnd = user32.WindowFromPoint(pt)
        if not top_hwnd:
            return False
        root = user32.GetAncestor(top_hwnd, GA_ROOT) or top_hwnd
        if root != hwnd:
            return False
    return True


_surface_capture_disabled = [False]


def surface_capture_available():
    """False once PrintWindow has hung on this run, so callers can report why
    they fell back."""
    return not _surface_capture_disabled[0]


def _capture_window_surface(hwnd, width, height, timeout_s=4.0):
    """Run the surface capture on a worker thread and give up after
    `timeout_s`.

    PrintWindow is synchronous with the target window's message loop: it blocks
    until that window services the request.  An emulator that has stopped
    pumping messages therefore blocks the caller for as long as it stays stuck --
    measured at 23 and 38 seconds against this build.  A harness whose whole
    purpose is detecting hangs must not itself hang on one, so the call is made
    on a worker and abandoned on timeout.

    The abandoned thread cannot be killed, so after the first timeout this path
    is disabled for the rest of the run rather than leaking a thread per sample."""
    if _surface_capture_disabled[0]:
        return None
    result = [None]

    def work():
        result[0] = _capture_window_surface_blocking(hwnd, width, height)

    t = threading.Thread(target=work, daemon=True)
    t.start()
    t.join(timeout_s)
    if t.is_alive():
        _surface_capture_disabled[0] = True
        print("[capture] PrintWindow did not return within %.1fs -- the window is "
              "not servicing messages; surface capture disabled for this run"
              % timeout_s)
        return None
    return result[0]


def _capture_window_surface_blocking(hwnd, width, height):
    """Ask the window to render itself into a bitmap via PrintWindow, and return
    a PIL image, or None if it declines.

    This reads the window's own surface rather than the screen, so an overlapping
    window cannot contaminate the result.  It was assumed this would fail for a
    Vulkan-presented window and return black; measured on this emulator it does
    not -- PrintWindow returns the drawn frame, non-client frame included, which
    is the same framing the previous screen-rect grab produced."""
    hdc = user32.GetDC(hwnd)
    if not hdc:
        return None
    mem_dc = bmp = None
    try:
        mem_dc = gdi32.CreateCompatibleDC(hdc)
        bmp = gdi32.CreateCompatibleBitmap(hdc, width, height)
        if not mem_dc or not bmp:
            return None
        gdi32.SelectObject(mem_dc, bmp)
        # PW_RENDERFULLCONTENT (2) is required for windows whose content is
        # composited by the GPU; without it some of them print blank.
        if not user32.PrintWindow(hwnd, mem_dc, 2):
            return None
        buf = ctypes.create_string_buffer(width * height * 4)
        info = _BITMAPINFOHEADER()
        info.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
        info.biWidth = width
        info.biHeight = -height          # negative: top-down rows
        info.biPlanes = 1
        info.biBitCount = 32
        info.biCompression = 0           # BI_RGB
        if not gdi32.GetDIBits(mem_dc, bmp, 0, height, buf,
                               ctypes.byref(info), 0):
            return None
        return Image.frombuffer("RGBA", (width, height), buf,
                                "raw", "BGRA", 0, 1).convert("RGB")
    except Exception:
        return None
    finally:
        if bmp:
            gdi32.DeleteObject(bmp)
        if mem_dc:
            gdi32.DeleteDC(mem_dc)
        user32.ReleaseDC(hwnd, hdc)


def capture_hwnd(hwnd, output_path):
    """Capture an explicit window handle.  Returns None when the window cannot
    be photographed, which the caller must distinguish from 'captured a black
    frame'.

    The window's own surface is read first.  A screen grab is used only when
    that fails AND the window is verified to be the top-level window across its
    rectangle, because a screen grab of a covered window returns the covering
    window's pixels.  That is not a hypothetical: a 20-minute run recorded an
    unrelated editor window as emulator frames, and its frozen/unique-frame
    figures were computed from them.

    A frame is never invented.  If neither route can produce the emulator's own
    pixels, the caller sees None and the run is reported as having no frame at
    that sample, which is the honest outcome."""
    if not hwnd or not user32.IsWindow(hwnd):
        return None
    if user32.IsIconic(hwnd):
        return None
    rect = get_hwnd_rect(hwnd)
    if rect is None:
        return None
    left, top, right, bottom = rect
    if left < -32000 or right <= left or bottom <= top:
        return None

    img = _capture_window_surface(hwnd, right - left, bottom - top)
    if img is None:
        if not _window_is_unobstructed(hwnd, rect):
            return None
        img = capture_rect(rect, output_path)
        return img
    try:
        img.save(output_path)
    except Exception as e:
        print(f"Screen capture failed: {e}")
        return None
    return img


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
    """Capture a window found by title.  Returns None when no window matches,
    rather than substituting the desktop -- see capture_rect."""
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
