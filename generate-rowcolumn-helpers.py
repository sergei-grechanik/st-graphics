#!/usr/bin/env python3

# Generate functions to convert row/column numbers encoded as diacritics to
# actual numbers.
# This script reads the file rowcolumn-diacritics.txt from the currend directory
# and produces the following files:
# - rowcolumn_diacritics_helpers.c - helper functions to be used by the terminal
# - cell-image-placeholder.txt - cell image placeholder for the largest possible
#   image.
#
# Note: the file rowcolumn-diacritics.txt lists the diacritics that we want to
# use to indicate row/column numbers. It is derived from UnicodeData.txt for the
# version 6.0.0 of Unicode (chosen somewhat arbitrarily: it's old enough, but
# still contains more than 255 suitable combining chars) using the following
# command:
#
#     cat UnicodeData.txt | grep "Mn;230;NSM;;"
#
# That is, we use combining chars of the same combining class 230 (above the
# base character) that do not have decomposition mappings.

import unicodedata

codes = []

with open("./rowcolumn-diacritics.txt", "r") as file:
    for line in file.readlines():
        code = int(line.split(";")[0], 16)
        char = chr(code)
        assert unicodedata.combining(char) == 230
        codes.append(code)

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

    print("int diacritic_to_num(uint32_t code)\n{", file=file)
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

with open("./cell-image-placeholder.txt", "w") as file:
    img_char = chr(0xEEEE)
    for row_code in codes:
        row_char = chr(row_code)
        for col_code in codes:
            col_char = chr(col_code)
            cell = img_char + row_char + col_char
            print(cell, file=file, end='')
            for nf in ["NFC", "NFKC", "NFD", "NFKD"]:
                if not unicodedata.is_normalized(nf, cell):
                    print(cell)
                    print("unnormalized!", nf, [hex(ord(img_char)), hex(row_code), hex(col_code)])
                    normalized = unicodedata.normalize(nf, cell)
                    print("normalized:", [hex(ord(c)) for c in normalized])
                    exit(1)
        print(file=file)
