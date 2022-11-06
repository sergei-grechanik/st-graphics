#!/usr/bin/env python3

# This script generates functions to convert row/column numbers encoded as
# diacritics to actual numbers.
# It reads the file rowcolumn-diacritics.txt from the currend directory and
# produces the following files:
# - rowcolumn_diacritics_helpers.c - contains a helper function to convert from
#   diacritics to row/column numbers.
# - rowcolumn_diacritics.sh - contains an array of row/column diacritics (can be
#   used by shell scripts to generate image placeholders).
#
# The script also checks some desirable properties of row/column diacritics,
# e.g. that image placeholders are in normal form.

import unicodedata
import sys

# codes of all row/column diacritics
codes = []

with open("./rowcolumn-diacritics.txt", "r") as file:
    for line in file.readlines():
        if line.startswith('#'):
            continue
        code = int(line.split(";")[0], 16)
        char = chr(code)
        assert unicodedata.combining(char) == 230
        codes.append(code)

print("Generating ./rowcolumn_diacritics_helpers.c")
with open("./rowcolumn_diacritics_helpers.c", "w") as file:
    range_start_num = 1
    range_start = 0
    range_end = 0

    def print_range():
        if range_start >= range_end:
            return
        for code in range(range_start, range_end):
            print("\tcase " + hex(code) + ":", file=file)
        print("\t\treturn code - " + hex(range_start) + " + " +
              str(range_start_num) + ";",
              file=file)

    print("#include <stdint.h>\n", file=file)
    print("uint16_t diacritic_to_num(uint32_t code)\n{", file=file)
    print("\tswitch (code) {", file=file)

    for code in codes:
        if range_end == code:
            range_end += 1
        else:
            print_range()
            range_start_num += range_end - range_start
            range_start = code
            range_end = code + 1
    print_range()

    print("\t}", file=file)
    print("\treturn 0;", file=file)
    print("}", file=file)

print("Generating ./rowcolumn_diacritics.sh")
with open("./rowcolumn_diacritics.sh", "w") as file:
    print("ROWCOLUMN_DIACRITICS=(", file=file, end="")
    for code in codes:
        print('"\\U' + format(code, 'x') + '" ', file=file, end="")
    print(")", file=file)

print("Checking that image placeholder cannot be normalized further")

img_char = chr(0x10EEEE)
for row_code in codes:
    row_char = chr(row_code)
    for col_code in codes:
        col_char = chr(col_code)
        cell = img_char + row_char + col_char
        for nf in ["NFC", "NFKC", "NFD", "NFKD"]:
            if not unicodedata.is_normalized(nf, cell):
                print(cell)
                print("unnormalized!", nf, [hex(ord(img_char)), hex(row_code), hex(col_code)])
                normalized = unicodedata.normalize(nf, cell)
                print("normalized:", [hex(ord(c)) for c in normalized])
                exit(1)

print("Checking that the row/column marks are not fused with anything "
      "letter-like during normalization")

# Collect somewhat normal characters.
normal_symbols = []
for i in range(sys.maxunicode):
    string = chr(i)
    if unicodedata.category(string)[0] not in ['L', 'P', 'N', 'S']:
        continue
    is_normalized = True
    for nf in ["NFC", "NFKC", "NFD", "NFKD"]:
        if not unicodedata.is_normalized(nf, string):
            is_normalized = False
    if is_normalized:
        normal_symbols.append(i)

for code in codes:
    print("Checking " + hex(code), end="\r")
    for num in normal_symbols:
        string = chr(num) + chr(code)
        for nf in ["NFC", "NFKC", "NFD", "NFKD"]:
            if not unicodedata.is_normalized(nf, string):
                normalized = unicodedata.normalize(nf, string)
                print("WARNING: " + hex(num) + " + " + hex(code) +
                      " is normalized to " + normalized,
                      " ".join(hex(ord(c)) for c in normalized))
