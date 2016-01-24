/* Copyright 2016 Peter Goodman (peter@trailofbits.com), all rights reserved. */

/* Should not affect any flags */
TEST_BEGIN(SHLr8i8_0, 1)
TEST_INPUTS(
    0,
    1,
    0x80,
    0x55,
    0xAA,
    0x0F,
    0xF7,
    0xFE)

    mov eax, ARG1_32
    shl al, 0
TEST_END

/* Mask 0x1F of 0xFF is still bigger than 8 bits. */
TEST_BEGIN(SHLr8i8_255, 1)
TEST_IGNORE_FLAGS(OF CF AF)
TEST_INPUTS(
    0,
    1,
    0x80,
    0x55,
    0xAA,
    0x0F,
    0xF7,
    0xFE)

    mov eax, ARG1_32
    shl al, 0xFF
TEST_END

/* Special encoding of SHLr8 with i8 = 1, also has special behaviour
 * with AF and OF */
TEST_BEGIN(SHLr8i8_1, 1)
TEST_IGNORE_FLAGS(AF)
TEST_INPUTS(
    0,
    1,
    0x80,
    0x55,
    0xAA,
    0x0F,
    0xF7,
    0xFE)

    mov eax, ARG1_32
    shl al, 1
TEST_END

/* REX-prefix encoded SHLr8i8 */
TEST_BEGIN_64(SHLr8i8_1, 1)
TEST_IGNORE_FLAGS(AF)
TEST_INPUTS(
    0,
    1,
    0x80,
    0x55,
    0xAA,
    0x0F,
    0xF7,
    0xFE)

    mov r8, ARG1_64
    shl r8b, 1
TEST_END_64

/* Special encoding with `cl` as the reg containing the number of bits */
TEST_BEGIN(SHLr8cl_1, 2)
TEST_IGNORE_FLAGS(AF)
TEST_INPUTS(
    0, 1,
    1, 1,
    0x80, 1,
    0x55, 1,
    0xAA, 1,
    0x0F, 1,
    0xF7, 1,
    0xFE, 1)

    mov eax, ARG1_32
    mov ecx, ARG2_32
    shl al, cl
TEST_END

/* Special encoding with `cl` as the reg containing the number of bits, using
 * a REX prefix to access a 64-bit dest reg. */
TEST_BEGIN_64(SHLr8cl_1, 2)
TEST_IGNORE_FLAGS(AF)
TEST_INPUTS(
    0, 1,
    1, 1,
    0x80, 1,
    0x55, 1,
    0xAA, 1,
    0x0F, 1,
    0xF7, 1,
    0xFE, 1)

    mov r8, ARG1_64
    mov rcx, ARG2_64
    shl r8b, cl
TEST_END_64

/* Right shift where the number of bits is greater than the number of bits
 * in the src/dst reg */
TEST_BEGIN(SHLr8i8_9, 1)
TEST_IGNORE_FLAGS(OF CF AF)
TEST_INPUTS(
    0,
    1,
    0x80,
    0x55,
    0xAA,
    0x0F,
    0xF7,
    0xFE)

    mov eax, ARG1_32
    shl al, 9
TEST_END

TEST_BEGIN(SHLr8i8_2, 1)
TEST_IGNORE_FLAGS(OF AF)
TEST_INPUTS(
    0,
    1,
    0x80,
    0x55,
    0xAA,
    0x0F,
    0xF7,
    0xFE)

    mov eax, ARG1_32
    shl al, 2
TEST_END

TEST_BEGIN(SHLr16cl_1, 2)
TEST_IGNORE_FLAGS(AF)
TEST_INPUTS(
    0, 1,
    1, 1,
    0x8000, 1,
    0x5500, 1,
    0xAA00, 1,
    0x0F0F, 1,
    0xF7F7, 1,
    0xFEFE, 1)

    mov eax, ARG1_32
    mov ecx, ARG2_32
    shl ax, cl
TEST_END

TEST_BEGIN(SHLr16i8_1, 1)
TEST_IGNORE_FLAGS(AF)
TEST_INPUTS(
    0,
    1,
    0x8000,
    0x5500,
    0xAA00,
    0x0F0F,
    0xF7F7,
    0xFEFE)

    mov eax, ARG1_32
    shl ax, 1
TEST_END

TEST_BEGIN(SHLr16i8_8, 1)
TEST_IGNORE_FLAGS(OF AF)
TEST_INPUTS(
    0,
    1,
    0x8000,
    0x5500,
    0xAA00,
    0x0F0F,
    0xF7F7,
    0xFEFE)

    mov eax, ARG1_32
    shl ax, 8
TEST_END

TEST_BEGIN(SHLr16i8_15, 1)
TEST_IGNORE_FLAGS(OF AF)
TEST_INPUTS(
    0,
    1,
    0x8000,
    0x5500,
    0xAA00,
    0x0F0F,
    0xF7F7,
    0xFEFE)

    mov eax, ARG1_32
    shl ax, 15
TEST_END

TEST_BEGIN(SHLr16i8_255, 1)
TEST_IGNORE_FLAGS(OF CF AF)
TEST_INPUTS(
    0,
    1,
    0x8000,
    0x5500,
    0xAA00,
    0x0F0F,
    0xF7F7,
    0xFEFE)

    mov eax, ARG1_32
    shl ax, 0xFF
TEST_END

TEST_BEGIN(SHLr32i8_1, 1)
TEST_IGNORE_FLAGS(AF)
TEST_INPUTS(
    0,
    1,
    0x80000000,
    0x55000000,
    0xAA000000,
    0x0F0F0F0F,
    0xF7F7F7F7,
    0xFEFEFEFE)

    mov eax, ARG1_32
    shl eax, 1
TEST_END

TEST_BEGIN(SHLr32i8_16, 1)
TEST_IGNORE_FLAGS(OF AF)
TEST_INPUTS(
    0,
    1,
    0x80000000,
    0x55000000,
    0xAA000000,
    0x0F0F0F0F,
    0xF7F7F7F7,
    0xFEFEFEFE)

    mov eax, ARG1_32
    shl eax, 16
TEST_END

TEST_BEGIN(SHLr32i8_31, 1)
TEST_IGNORE_FLAGS(OF AF)
TEST_INPUTS(
    0,
    1,
    0x80000000,
    0x55000000,
    0xAA000000,
    0x0F0F0F0F,
    0xF7F7F7F7,
    0xFEFEFEFE)

    mov eax, ARG1_32
    shl eax, 31
TEST_END

TEST_BEGIN(SHLr32i8_255, 1)
TEST_IGNORE_FLAGS(OF CF AF)
TEST_INPUTS(
    0,
    1,
    0x80000000,
    0x55000000,
    0xAA000000,
    0x0F0F0F0F,
    0xF7F7F7F7,
    0xFEFEFEFE)

    mov eax, ARG1_32
    shl eax, 0xFF
TEST_END

TEST_BEGIN_64(SHLr64i8_1, 1)
TEST_IGNORE_FLAGS(AF)
TEST_INPUTS(
    0,
    1,
    0x8000000000000000,
    0x5500000000000000,
    0xAA00000000000000,
    0x0F0F0F0F0F0F0F0F,
    0xF7F7F7F7F7F7F7F7,
    0xFEFEFEFEFEFEFEFE)

    mov rax, ARG1_64
    shl rax, 1
TEST_END_64

TEST_BEGIN_64(SHLr64i8_32, 1)
TEST_IGNORE_FLAGS(OF AF)
TEST_INPUTS(
    0,
    1,
    0x8000000000000000,
    0x5500000000000000,
    0xAA00000000000000,
    0x0F0F0F0F0F0F0F0F,
    0xF7F7F7F7F7F7F7F7,
    0xFEFEFEFEFEFEFEFE)

    mov rax, ARG1_64
    shl rax, 32
TEST_END_64

TEST_BEGIN_64(SHLr64i8_63, 1)
TEST_IGNORE_FLAGS(OF AF)
TEST_INPUTS(
    0,
    1,
    0x8000000000000000,
    0x5500000000000000,
    0xAA00000000000000,
    0x0F0F0F0F0F0F0F0F,
    0xF7F7F7F7F7F7F7F7,
    0xFEFEFEFEFEFEFEFE)

    mov rax, ARG1_64
    shl rax, 63
TEST_END_64

TEST_BEGIN_64(SHLr64i8_255, 1)
TEST_IGNORE_FLAGS(OF CF AF)
TEST_INPUTS(
    0,
    1,
    0x8000000000000000,
    0x5500000000000000,
    0xAA00000000000000,
    0x0F0F0F0F0F0F0F0F,
    0xF7F7F7F7F7F7F7F7,
    0xFEFEFEFEFEFEFEFE)

    mov rax, ARG1_64
    shl rax, 0xFF
TEST_END_64