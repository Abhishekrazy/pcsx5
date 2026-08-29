Goal: Implement Next Boundary: Patch Dreaming Sarah JSON Serialization Bug

We have perfectly reconstructed the bug mechanism:
1. The engine attempts to extract the directory from a quoted asset path.
2. The asset path is e.g. "images/precious_stones-sheet1.png".
3. The native value-reader incorrectly calculates a length of 6.
4. It copies the BOM (EF BB BF) + 6 bytes from the quoted string: EF BB BF " i m a g e.
5. It writes a null terminator.
6. The JSON deserializer is called. It unconditionally skips the 3-byte BOM (dd rsi, 3, dd ebx, -3).
7. The JSON deserializer is given "image\0.
8. strlen returns 6. GetNextToken parses "image and throws std::invalid_argument because it hits EOF without a closing quote.

We need to find the exact function that calculates the length of 6 and copies the string, and patch it.
