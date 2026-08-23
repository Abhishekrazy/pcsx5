# Loader / Module Governance

Loader owns module-format parsing and module-loading policy.

Memory/MMU owns guest address-space authority.

Loader may request:
- preferred address
- alignment
- size
- mapping permissions

Memory decides allocation validity.

Module lifetime must be explicit.

Every relocation should be diagnosable:
preferred
allocated
size
reason
module
title/version

Do not silently relocate because an allocator "knows better".

Do not implement module unload without evidence that lifecycle semantics require it.

