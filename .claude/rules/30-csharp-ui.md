# Rule: C# / WPF

The WPF application is a shell and orchestration layer, not the emulator.

- Keep emulator state out of code-behind.
- Use MVVM where it reduces UI coupling.
- PInvoke only through a small native interop layer.
- Treat the native DLL ABI as a versioned contract.
- Do not pass C++ STL types, exceptions, ownership-bearing objects, or C++ class layouts across PInvoke.
- Prefer opaque handles, fixed-layout structs, UTF-8/UTF-16 conversion at the boundary, and explicit buffer ownership.
- UI must remain responsive during emulation.
- Long-running emulator work must not block the WPF dispatcher thread.
