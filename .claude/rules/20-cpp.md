# Rule: C++20 Core

- Use C++20.
- Prefer RAII, value semantics, explicit ownership, and narrow interfaces.
- Avoid raw owning pointers.
- Avoid global mutable state.
- Avoid exceptions crossing the C ABI boundary.
- Use fixed-width integer types for guest-visible data.
- Do not assume host pointer size equals guest pointer size.
- Keep guest addresses and host pointers distinct types.
- Treat guest memory as untrusted input.
- Define endianness explicitly where guest-visible formats are involved.
- Do not rely on compiler-specific layout for guest ABI structures without static assertions.
- Keep hot paths measurable. Do not prematurely optimize.
- Use sanitizers where applicable to host-side tests.
- Avoid hidden allocations in CPU/GPU hot loops unless measured and accepted.
