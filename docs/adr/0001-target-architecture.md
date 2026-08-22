# ADR 0001: Layered Emulator Architecture

## Status

Accepted

## Decision

Use a layered architecture with a native emulator core and a thin WPF desktop shell.

The native core owns emulation. The UI owns presentation, configuration editing, update orchestration, and user interaction.

A narrow versioned C ABI separates managed and native code.

## Rationale

The current project already works but has tangled responsibilities. A clean boundary enables:
- headless testing
- safer refactoring
- independent debugging
- better compatibility work
- fewer UI/core dependencies

## Consequences

Positive:
- clear ownership
- testable core
- reduced coupling

Negative:
- initial interop design cost
- explicit data marshaling
- more modules

## Rejected

A single C# application owning emulator logic was rejected because it would tightly couple platform UI and emulation internals.

