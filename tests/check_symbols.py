#!/usr/bin/env python3
# slz - Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
"""Every global symbol the slz library defines must start with slz_ (public API) or slz_i_
(internal, renamed in src/internal.h), so linking slz next to other code never clashes.

    check_symbols.py <nm> <libslz.a | libslz.so | libslz.dylib>

Exits 1 and lists the offending symbols otherwise. Mach-O names carry a leading underscore,
which is stripped."""
import subprocess, sys

nm, lib = sys.argv[1], sys.argv[2]
out = subprocess.run([nm, "-g", lib], capture_output=True, text=True, check=True).stdout
names = set()
for line in out.splitlines():
    f = line.split()
    # "<address> <type> <name>"; undefined symbols ("U <name>", no address) are references, not ours
    if len(f) == 3 and f[1] not in ("U", "w", "v"):
        names.add(f[2])
if names and all(n.startswith("_") for n in names):  # Mach-O
    names = {n[1:] for n in names}
names -= {"_init", "_fini", "_edata", "_end", "__bss_start"}  # ELF shared objects: defined by the linker
bad = sorted(n for n in names if not n.startswith("slz_"))
if not names:
    sys.exit(f"{lib}: no global symbols found (wrong nm output format?)")
if bad:
    print(f"{lib}: {len(bad)} global symbol(s) outside the slz_ namespace (add them to src/internal.h):")
    for n in bad:
        print("  " + n)
    sys.exit(1)
print(f"{lib}: {len(names)} global symbols, all prefixed slz_")
