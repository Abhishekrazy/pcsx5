# ADR 0003: Isolate Media Decoders

## Status

Accepted

## Decision

FFmpeg, Bink2, and future media decoders must sit behind a narrow media abstraction.

## Rationale

The core should not depend on a specific media library. Bink2 also introduces distribution/licensing constraints.

