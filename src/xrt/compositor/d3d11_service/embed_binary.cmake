# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Emit a C source file containing the bytes of INPUT_FILE as a named array.
# Invoked at build time via:
#   cmake -DINPUT_FILE=<path> -DOUTPUT_FILE=<path> -DSYMBOL=<name> -P embed_binary.cmake

if(NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "embed_binary.cmake requires INPUT_FILE, OUTPUT_FILE, SYMBOL")
endif()

file(READ "${INPUT_FILE}" HEX_CONTENTS HEX)
string(LENGTH "${HEX_CONTENTS}" HEX_LEN)
math(EXPR BYTE_LEN "${HEX_LEN} / 2")

# "aabbcc..." -> "0xaa,0xbb,0xcc,..."
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTE_ARRAY "${HEX_CONTENTS}")
# Insert a newline every 16 bytes (96 chars of "0xNN," each) to keep line lengths sane.
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " BYTE_ARRAY "${BYTE_ARRAY}")
string(REGEX REPLACE ",$" "" BYTE_ARRAY "${BYTE_ARRAY}")

file(WRITE "${OUTPUT_FILE}" "/* Auto-generated from ${INPUT_FILE} by embed_binary.cmake — do not edit. */
#include <stdint.h>
#include <stddef.h>

const uint8_t ${SYMBOL}[${BYTE_LEN}] = {
    ${BYTE_ARRAY}
};
const size_t ${SYMBOL}_size = ${BYTE_LEN};
")
