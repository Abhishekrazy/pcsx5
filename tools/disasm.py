import sys
with open('I:/Personal/Windows/pcsx5/Games/PPSA21564-app/eboot.bin', 'rb') as f:
    eboot = f.read()

import struct
from capstone import *

# Assuming standard eboot base is 0x800000000, but there might be a header.
# Actually, the file offset might differ from guest offset.
# Let's search for the bytes around 0x807046a15 in the eboot if we don't know the offset.
# Or better, I can just dump memory from the crash dump if I had one. 
# But I can also just run capstone on the eboot if I find the exact bytes.
