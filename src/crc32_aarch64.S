/* uint32_t slz_crc32_armv8(uint32_t crc, const uint8_t *buf, size_t len)  -- ARMv8 CRC32 ext.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * len % 16 == 0. AAPCS64 (Linux, FreeBSD, macOS, Windows ARM64). */
#ifdef __APPLE__
# define SYM(x) _##x
#else
# define SYM(x) x
#endif
.arch armv8-a+crc
.text
.globl SYM(slz_crc32_armv8)
.p2align 4
SYM(slz_crc32_armv8):
    cbz     x2, 2f
1:  ldp     x3, x4, [x1], #16
    crc32x  w0, w0, x3
    crc32x  w0, w0, x4
    subs    x2, x2, #16
    b.ne    1b
2:  ret
#if defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif
