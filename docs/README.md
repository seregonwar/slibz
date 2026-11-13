# slz documentation

The details behind the [README](../README.md): how to use and build slz, how in-place extraction and the
hardening work, how it is verified and measured, and what comes next.

| page | contents |
|---|---|
| [Usage](usage.md) | every command-line option and the C library API |
| [In-place (destructive) extraction](in-place-extraction.md) | hole punching, checkpoint order, journal, resume, disk bound |
| [Security model](security-model.md) | threat model, paths, links, overlaps and bombs, parsers, decoders |
| [Verification](verification.md) | round trips, hostile archives, sanitizers, fuzzing, forced kills, injected I/O failures, CI |
| [Benchmarks](benchmarks.md) | continuous benchmarks on every push, full performance tables, methodology, how to reproduce |
| [Platforms](platforms.md) | what is tested where, assembly CRC-32, the WebAssembly build |
| [Building and integration](building.md) | CMake build and tests, install, `find_package`, vendoring, options |
| [Project layout](project-layout.md) | source files and how the code is shared |
| [Limits and roadmap](roadmap.md) | what is not supported yet and what is planned |
