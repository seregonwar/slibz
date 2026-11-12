/* uint32_t slz_crc32_pclmul(uint32_t crc, const uint8_t *buf, size_t len)
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * CRC-32 by carry-less multiplication folding (Intel white paper "Fast CRC Computation
 * Using PCLMULQDQ"). len % 16 == 0 and len >= 64. Uses only xmm0-5 (volatile on Win64 too),
 * no stack, unaligned loads. Assembles with GCC/Clang for ELF, Mach-O and COFF. */
#if defined(_WIN32)
# define CRC %ecx
# define BUF %rdx
# define LEN %r8
#else
# define CRC %edi
# define BUF %rsi
# define LEN %rdx
#endif
#ifdef __APPLE__
# define SYM(x) _##x
#else
# define SYM(x) x
#endif

.text
.p2align 4
K12: .quad 0x0000000154442bd4, 0x00000001c6e41596 /* x^(4*128+32), x^(4*128-32) mod P */
K34: .quad 0x00000001751997d0, 0x00000000ccaa009e /* x^(128+32),   x^(128-32)   mod P */
K5:  .quad 0x0000000163cd6124, 0
M32: .quad 0x00000000ffffffff, 0
KP:  .quad 0x00000001db710641, 0x00000001f7011641 /* P', mu (Barrett) */

.globl SYM(slz_crc32_pclmul)
.p2align 4
SYM(slz_crc32_pclmul):
    movdqu  (BUF), %xmm1
    movdqu  16(BUF), %xmm2
    movdqu  32(BUF), %xmm3
    movdqu  48(BUF), %xmm4
    movd    CRC, %xmm0
    pxor    %xmm0, %xmm1
    add     $64, BUF
    sub     $64, LEN
    movdqa  K12(%rip), %xmm0
    cmp     $64, LEN
    jb      2f
1:  /* fold 4 x 128 bits */
.irp r, 1, 2, 3, 4
    movdqa  %xmm\r, %xmm5
    pclmulqdq $0x00, %xmm0, %xmm\r
    pclmulqdq $0x11, %xmm0, %xmm5
    pxor    %xmm5, %xmm\r
.endr
    movdqu  (BUF), %xmm5
    pxor    %xmm5, %xmm1
    movdqu  16(BUF), %xmm5
    pxor    %xmm5, %xmm2
    movdqu  32(BUF), %xmm5
    pxor    %xmm5, %xmm3
    movdqu  48(BUF), %xmm5
    pxor    %xmm5, %xmm4
    add     $64, BUF
    sub     $64, LEN
    cmp     $64, LEN
    jae     1b
2:  /* fold 4 -> 1 */
    movdqa  K34(%rip), %xmm0
.irp r, 2, 3, 4
    movdqa  %xmm1, %xmm5
    pclmulqdq $0x00, %xmm0, %xmm1
    pclmulqdq $0x11, %xmm0, %xmm5
    pxor    %xmm5, %xmm1
    pxor    %xmm\r, %xmm1
.endr
    cmp     $16, LEN
    jb      4f
3:  /* fold remaining 16-byte blocks */
    movdqa  %xmm1, %xmm5
    pclmulqdq $0x00, %xmm0, %xmm1
    pclmulqdq $0x11, %xmm0, %xmm5
    pxor    %xmm5, %xmm1
    movdqu  (BUF), %xmm5
    pxor    %xmm5, %xmm1
    add     $16, BUF
    sub     $16, LEN
    cmp     $16, LEN
    jae     3b
4:  /* 128 -> 64 bits */
    pclmulqdq $0x01, %xmm1, %xmm0
    psrldq  $8, %xmm1
    pxor    %xmm0, %xmm1
    /* 64 -> 32 bits */
    movdqa  %xmm1, %xmm2
    movdqa  K5(%rip), %xmm0
    movdqa  M32(%rip), %xmm3
    psrldq  $4, %xmm2
    pand    %xmm3, %xmm1
    pclmulqdq $0x00, %xmm0, %xmm1
    pxor    %xmm2, %xmm1
    /* Barrett reduction */
    movdqa  KP(%rip), %xmm0
    movdqa  %xmm1, %xmm2
    pand    %xmm3, %xmm1
    pclmulqdq $0x10, %xmm0, %xmm1
    pand    %xmm3, %xmm1
    pclmulqdq $0x00, %xmm0, %xmm1
    pxor    %xmm2, %xmm1
    pextrd  $1, %xmm1, %eax
    ret

#if defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif
