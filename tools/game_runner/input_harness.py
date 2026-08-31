import ctypes
import time

VK_CODE = {
    'space': 0x20,
    'enter': 0x0D,
    'esc': 0x1B,
    'up': 0x26,
    'down': 0x28,
    'left': 0x25,
    'right': 0x27,
    'a': 0x41,
    'b': 0x42,
    'x': 0x58,
    'y': 0x59,
    # Face-button and shoulder letters. KytyPS5 binds J/I/K/L to
    # Cross/Triangle/Square/Circle and Q/E to L1/R1, so a keyboard script can
    # drive either emulator through the same names.
    'j': 0x4A,
    'i': 0x49,
    'k': 0x4B,
    'l': 0x4C,
    'q': 0x51,
    'e': 0x45,
}

PUL = ctypes.POINTER(ctypes.c_ulong)
class KeyBdInput(ctypes.Structure):
    _fields_ = [("wVk", ctypes.c_ushort),
                ("wScan", ctypes.c_ushort),
                ("dwFlags", ctypes.c_ulong),
                ("time", ctypes.c_ulong),
                ("dwExtraInfo", PUL)]

class HardwareInput(ctypes.Structure):
    _fields_ = [("uMsg", ctypes.c_ulong),
                ("wParamL", ctypes.c_short),
                ("wParamH", ctypes.c_ushort)]

class MouseInput(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_long),
                ("dy", ctypes.c_long),
                ("mouseData", ctypes.c_ulong),
                ("dwFlags", ctypes.c_ulong),
                ("time", ctypes.c_ulong),
                ("dwExtraInfo", PUL)]

class Input_I(ctypes.Union):
    _fields_ = [("ki", KeyBdInput),
                ("mi", MouseInput),
                ("hi", HardwareInput)]

class Input(ctypes.Structure):
    _fields_ = [("type", ctypes.c_ulong),
                ("ii", Input_I)]

def press_key(hexKeyCode):
    extra = ctypes.c_ulong(0)
    ii_ = Input_I()
    ii_.ki = KeyBdInput(hexKeyCode, 0x48, 0, 0, ctypes.pointer(extra))
    x = Input(ctypes.c_ulong(1), ii_)
    ctypes.windll.user32.SendInput(1, ctypes.pointer(x), ctypes.sizeof(x))

def release_key(hexKeyCode):
    extra = ctypes.c_ulong(0)
    ii_ = Input_I()
    ii_.ki = KeyBdInput(hexKeyCode, 0x48, 0x0002, 0, ctypes.pointer(extra))
    x = Input(ctypes.c_ulong(1), ii_)
    ctypes.windll.user32.SendInput(1, ctypes.pointer(x), ctypes.sizeof(x))

def send_key_press(key_name, duration=0.1):
    key_name = key_name.lower()
    vk = VK_CODE.get(key_name)
    if not vk:
        if len(key_name) == 1:
            vk = ord(key_name.upper())
        else:
            return
            
    press_key(vk)
    time.sleep(duration)
    release_key(vk)


# --- Mouse -------------------------------------------------------------------
# UI verification needs pointer input: many shell affordances are reachable only
# by click until keyboard/gamepad navigation is complete, and a verification run
# must be able to drive them without a human at the machine.

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


def _abs(x, y):
    """Convert screen pixels to the 0..65535 absolute range SendInput wants."""
    user32 = ctypes.windll.user32
    user32.SetProcessDPIAware()
    w = user32.GetSystemMetrics(0)
    h = user32.GetSystemMetrics(1)
    return int(x * 65535 / (w - 1)), int(y * 65535 / (h - 1))


def _mouse_event(flags, ax=0, ay=0):
    extra = ctypes.c_ulong(0)
    ii_ = Input_I()
    ii_.mi = MouseInput(ax, ay, 0, flags, 0, ctypes.pointer(extra))
    inp = Input(ctypes.c_ulong(0), ii_)
    ctypes.windll.user32.SendInput(1, ctypes.pointer(inp), ctypes.sizeof(inp))


def move_to(x, y):
    ax, ay = _abs(x, y)
    _mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, ax, ay)


def click_at(x, y, settle=0.12):
    """Move to a screen coordinate and left-click it."""
    move_to(x, y)
    time.sleep(settle)
    _mouse_event(MOUSEEVENTF_LEFTDOWN)
    time.sleep(0.05)
    _mouse_event(MOUSEEVENTF_LEFTUP)
    time.sleep(settle)


def double_click_at(x, y, settle=0.12):
    click_at(x, y, settle)
    _mouse_event(MOUSEEVENTF_LEFTDOWN)
    time.sleep(0.04)
    _mouse_event(MOUSEEVENTF_LEFTUP)
    time.sleep(settle)
