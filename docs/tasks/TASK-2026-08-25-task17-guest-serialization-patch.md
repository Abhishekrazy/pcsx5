# Task: Patch Dreaming Sarah JSON Serialization Bug

## Objective
Fix the guest-level string truncation bug in `PPSA02929` (Dreaming Sarah) where the HTML5/Construct engine's native value-reader incorrectly calculates or writes a string length of `6` for asset paths (like `"images/precious_stones-sheet1.png"`), causing a fatal `std::invalid_argument` parse error on the worker thread.

## Background
In previous tasks, we ensured that `libc` string functions (`memcpy`, `strncpy`, etc.) are correctly routed to HLE implementations. However, tracing reveals that the guest engine passes an explicit `count=6` to `memcpy` when extracting the image path string. Because this is a logic bug within the guest executable itself (specifically the serialization payload for worker threads), the standard HLE string functions correctly obey the `count=6` instruction, resulting in a truncated, invalid string (`"image"` without a null terminator). The JSON tokenizer subsequently crashes and invokes `std::terminate`.

## Requirements
1. Identify the exact hook point in the `PPSA02929` executable (e.g., the `expectValue` switch block or the worker thread serialization routine).
2. Implement an in-memory runtime patch (via the `HLE` layer or the `ElfLoader`) that intercepts the buggy serialization routine.
3. Overwrite or intercept the routine to supply the correct string length (e.g., `34` for the image path) or widen the tokenizer's string-field reader so it consumes the entire string.
4. Prove from runtime logs that `memcpy` or the string copying function is subsequently invoked with the full, correct length.
5. Verify the guest engine successfully parses the frame record and bypasses the `std::invalid_argument` exception.

**Do NOT implement this task yet.** This document defines the boundary and requirements for the next architectural phase.
