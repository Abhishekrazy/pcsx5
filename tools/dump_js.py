import sys
f = r"Games/PPSA02929-app0/data.js"
data = open(f, "rb").read()
# find all occurrences of "image" (lowercase, no leading slash) as literal
import re
idxs = [m.start() for m in re.finditer(rb'image', data)]
print("occurrences of 'image':", len(idxs))
# find "precious_stones"
ps = data.find(b"precious_stones")
print("precious_stones at file offset", ps)
print()
# dump 80 bytes before and 120 after precious_stones
lo = max(0, ps - 80)
hi = min(len(data), ps + 120)
chunk = data[lo:hi]
print("hex dump around precious_stones:")
for i in range(0, len(chunk), 16):
    row = chunk[i:i+16]
    hexs = " ".join("%02x" % b for b in row)
    asc = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
    print("  %08x  %-47s  %s" % (lo + i, hexs, asc))
