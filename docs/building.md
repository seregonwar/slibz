# Building and integration

[slz](../README.md) > [docs](README.md) > Building and integration

## Build and test

```bash
cmake -S . -B build && cmake --build build --config Release
```

```bash
ctest --test-dir build -C Release --output-on-failure -j 8
```

The end-to-end suite (`tests/run_tests.py`) is registered as several `archives-*` tests that share one
prepared corpus, so `-j` runs them in parallel; `python tests/run_tests.py <slz> <slz-test>` still runs
everything in one process.

## Integration

```bash
cmake -S . -B build -DSLZ_TESTS=OFF && cmake --build build --config Release
```

```bash
cmake --install build --prefix /usr/local
```

```cmake
find_package(slz 0.1 REQUIRED)
target_link_libraries(app PRIVATE slz::slz)
```

Or vendor it with `add_subdirectory(slz)` and link `slz::slz`.

| CMake option | default | |
|---|---|---|
| `SLZ_ASM` | ON | assembly CRC-32 (GCC/Clang) |
| `SLZ_TESTS` | ON when top level | tests and `ctest` targets |
| `SLZ_SANITIZE` | OFF | ASan + UBSan |
| `SLZ_FAULT_INJECTION` | OFF | test builds only: `SLZ_FAULT_AT=N` fails the N-th I/O operation |
| `SLZ_INSTALL` | ON | install rules and the `slz::slz` package |

The WebAssembly build is described in [Platforms](platforms.md).
