/* The only font of the program: DejaVu Sans, 14 px, 1 bit per pixel, printable ASCII 0x20-0x7E only (no symbols,
 * no kerning). Generated, not written by hand (see src/ui/font/README.md). 1 bit per pixel means no anti-aliasing,
 * so text adds no colours to the 16-colour palette. */
#include "lvgl.h"

#ifndef P99_FONT
#define P99_FONT 1
#endif

#if P99_FONT

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xfe, 0xc0,

    /* U+0022 "\"" */
    0x99, 0x99,

    /* U+0023 "#" */
    0x9, 0x2, 0x40, 0x91, 0xff, 0x12, 0x4, 0x8f,
    0xf8, 0x90, 0x24, 0x9, 0x0,

    /* U+0024 "$" */
    0x10, 0x47, 0xe5, 0x93, 0x47, 0x85, 0x16, 0x5f,
    0x84, 0x10,

    /* U+0025 "%" */
    0x70, 0x88, 0x88, 0x89, 0x8, 0xb0, 0x72, 0xe0,
    0x51, 0xd, 0x10, 0x91, 0x11, 0x11, 0xe,

    /* U+0026 "&" */
    0x3c, 0x10, 0x4, 0x1, 0x80, 0x70, 0x26, 0x28,
    0xca, 0x1c, 0xc3, 0xf, 0x20,

    /* U+0027 "'" */
    0xf0,

    /* U+0028 "(" */
    0x69, 0x49, 0x24, 0x91, 0x22,

    /* U+0029 ")" */
    0xc9, 0x12, 0x49, 0x2d, 0x28,

    /* U+002A "*" */
    0x11, 0x25, 0xf1, 0xcd, 0x62, 0x0,

    /* U+002B "+" */
    0x8, 0x4, 0x2, 0x1, 0xf, 0xf8, 0x40, 0x20,
    0x10, 0x8, 0x0,

    /* U+002C "," */
    0x58,

    /* U+002D "-" */
    0xf0,

    /* U+002E "." */
    0xc0,

    /* U+002F "/" */
    0x18, 0x84, 0x62, 0x10, 0x88, 0x42, 0x21, 0x0,

    /* U+0030 "0" */
    0x38, 0x8a, 0xc, 0x18, 0x30, 0x60, 0xc1, 0x44,
    0x70,

    /* U+0031 "1" */
    0xf2, 0x41, 0x4, 0x10, 0x41, 0x4, 0x13, 0xf0,

    /* U+0032 "2" */
    0xfa, 0x30, 0x41, 0xc, 0x63, 0x18, 0xc3, 0xf0,

    /* U+0033 "3" */
    0xf8, 0x10, 0x41, 0x70, 0x30, 0x41, 0xf, 0xe0,

    /* U+0034 "4" */
    0xc, 0x18, 0x51, 0x26, 0x48, 0xbf, 0x82, 0x4,
    0x8,

    /* U+0035 "5" */
    0x7c, 0x81, 0x3, 0xc0, 0x40, 0x40, 0x81, 0x85,
    0xf0,

    /* U+0036 "6" */
    0x3e, 0x82, 0x5, 0xcc, 0x50, 0x60, 0xc1, 0x44,
    0x70,

    /* U+0037 "7" */
    0xfe, 0xc, 0x10, 0x60, 0x83, 0x6, 0x8, 0x30,
    0x40,

    /* U+0038 "8" */
    0x7a, 0x18, 0x61, 0x7b, 0x38, 0x61, 0xcd, 0xe0,

    /* U+0039 "9" */
    0x79, 0x8a, 0xc, 0x1c, 0x6f, 0x40, 0x83, 0x85,
    0xf0,

    /* U+003A ":" */
    0xc6,

    /* U+003B ";" */
    0x50, 0x5, 0x80,

    /* U+003C "<" */
    0x1, 0x87, 0x9e, 0x18, 0x7, 0x80, 0x70, 0x6,

    /* U+003D "=" */
    0xff, 0x80, 0x0, 0x1f, 0xf0,

    /* U+003E ">" */
    0xc0, 0x3c, 0x3, 0xc0, 0x30, 0xf1, 0xc3, 0x0,

    /* U+003F "?" */
    0x7a, 0x10, 0x42, 0x10, 0x82, 0x0, 0x20, 0x80,

    /* U+0040 "@" */
    0xf, 0x83, 0xc, 0x60, 0x24, 0x3, 0x8f, 0x99,
    0x99, 0x90, 0x99, 0x9, 0x99, 0xa4, 0xec, 0x60,
    0x3, 0xc, 0xf, 0x80,

    /* U+0041 "A" */
    0xc, 0xe, 0x5, 0x82, 0x42, 0x21, 0x19, 0xfc,
    0x83, 0x40, 0xe0, 0x40,

    /* U+0042 "B" */
    0xfd, 0x6, 0xc, 0x3f, 0xd0, 0xe0, 0xc1, 0x87,
    0xf8,

    /* U+0043 "C" */
    0x3e, 0x61, 0xc0, 0x80, 0x80, 0x80, 0x80, 0xc0,
    0x61, 0x3e,

    /* U+0044 "D" */
    0xfc, 0x86, 0x83, 0x81, 0x81, 0x81, 0x81, 0x83,
    0x86, 0xfc,

    /* U+0045 "E" */
    0xfe, 0x8, 0x20, 0xfe, 0x8, 0x20, 0x83, 0xf0,

    /* U+0046 "F" */
    0xfc, 0x21, 0xf, 0xc2, 0x10, 0x84, 0x0,

    /* U+0047 "G" */
    0x1f, 0x30, 0x50, 0x10, 0x8, 0x7c, 0x6, 0x3,
    0x81, 0x60, 0x8f, 0x80,

    /* U+0048 "H" */
    0x81, 0x81, 0x81, 0x81, 0xff, 0x81, 0x81, 0x81,
    0x81, 0x81,

    /* U+0049 "I" */
    0xff, 0xc0,

    /* U+004A "J" */
    0x24, 0x92, 0x49, 0x24, 0x9c,

    /* U+004B "K" */
    0x86, 0x8c, 0x98, 0xb0, 0xc0, 0xe0, 0xb0, 0x98,
    0x8c, 0x86,

    /* U+004C "L" */
    0x82, 0x8, 0x20, 0x82, 0x8, 0x20, 0x83, 0xf0,

    /* U+004D "M" */
    0xc1, 0xe0, 0xe8, 0xf4, 0x5b, 0x6c, 0xa6, 0x53,
    0x11, 0x80, 0xc0, 0x40,

    /* U+004E "N" */
    0xc1, 0xe1, 0xa1, 0xb1, 0x99, 0x99, 0x8d, 0x85,
    0x87, 0x83,

    /* U+004F "O" */
    0x3e, 0x31, 0xb0, 0x70, 0x18, 0xc, 0x6, 0x3,
    0x83, 0x63, 0x1f, 0x0,

    /* U+0050 "P" */
    0xfa, 0x38, 0x61, 0x8f, 0xe8, 0x20, 0x82, 0x0,

    /* U+0051 "Q" */
    0x3e, 0x31, 0xb0, 0x70, 0x18, 0xc, 0x6, 0x3,
    0x83, 0x63, 0x1f, 0x0, 0x80, 0x20,

    /* U+0052 "R" */
    0xf9, 0x1a, 0x14, 0x28, 0xdf, 0x23, 0x42, 0x87,
    0x4,

    /* U+0053 "S" */
    0x7f, 0x86, 0x4, 0x7, 0x83, 0x80, 0x81, 0x87,
    0xf8,

    /* U+0054 "T" */
    0xff, 0x84, 0x2, 0x1, 0x0, 0x80, 0x40, 0x20,
    0x10, 0x8, 0x4, 0x0,

    /* U+0055 "U" */
    0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81,
    0x42, 0x3c,

    /* U+0056 "V" */
    0xc0, 0xa0, 0xd8, 0x6c, 0x22, 0x31, 0x90, 0x48,
    0x2c, 0x1c, 0x6, 0x0,

    /* U+0057 "W" */
    0x43, 0xa, 0x18, 0x51, 0xc6, 0xcb, 0x32, 0x49,
    0x12, 0x48, 0xf2, 0xc7, 0x1c, 0x18, 0x60, 0xc3,
    0x0,

    /* U+0058 "X" */
    0x61, 0x91, 0x84, 0x83, 0xc0, 0xc0, 0xe0, 0x58,
    0x64, 0x61, 0x20, 0xc0,

    /* U+0059 "Y" */
    0x41, 0x31, 0x88, 0x82, 0x81, 0xc0, 0x40, 0x20,
    0x10, 0x8, 0x4, 0x0,

    /* U+005A "Z" */
    0xff, 0x3, 0x6, 0xc, 0x8, 0x10, 0x30, 0x60,
    0xc0, 0xff,

    /* U+005B "[" */
    0xf2, 0x49, 0x24, 0x92, 0x4e,

    /* U+005C "\\" */
    0x86, 0x10, 0x86, 0x10, 0x84, 0x10, 0x84, 0x10,

    /* U+005D "]" */
    0xe4, 0x92, 0x49, 0x24, 0x9e,

    /* U+005E "^" */
    0xc, 0xf, 0x8, 0xc8, 0x30,

    /* U+005F "_" */
    0xfe,

    /* U+0060 "`" */
    0xc8, 0x80,

    /* U+0061 "a" */
    0xfa, 0x30, 0x5f, 0x86, 0x18, 0xdd,

    /* U+0062 "b" */
    0x81, 0x2, 0x5, 0xcc, 0x50, 0x60, 0xc1, 0x83,
    0x8a, 0xe0,

    /* U+0063 "c" */
    0x3d, 0x8, 0x20, 0x82, 0x4, 0xf,

    /* U+0064 "d" */
    0x2, 0x4, 0x9, 0xd4, 0x70, 0x60, 0xc1, 0x82,
    0x8c, 0xe8,

    /* U+0065 "e" */
    0x3c, 0x8e, 0xf, 0xf8, 0x10, 0x10, 0x9f,

    /* U+0066 "f" */
    0x3a, 0x11, 0xf4, 0x21, 0x8, 0x42, 0x10,

    /* U+0067 "g" */
    0x3a, 0x8e, 0xc, 0x18, 0x30, 0x51, 0x9d, 0x2,
    0x9, 0xe0,

    /* U+0068 "h" */
    0x82, 0x8, 0x2e, 0xce, 0x18, 0x61, 0x86, 0x18,
    0x40,

    /* U+0069 "i" */
    0x9f, 0xe0,

    /* U+006A "j" */
    0x20, 0x12, 0x49, 0x24, 0x93, 0x80,

    /* U+006B "k" */
    0x81, 0x2, 0x4, 0x69, 0x96, 0x38, 0x70, 0xb1,
    0x32, 0x30,

    /* U+006C "l" */
    0xff, 0xe0,

    /* U+006D "m" */
    0xb9, 0xd8, 0xc6, 0x10, 0xc2, 0x18, 0x43, 0x8,
    0x61, 0xc, 0x21,

    /* U+006E "n" */
    0xbb, 0x38, 0x61, 0x86, 0x18, 0x61,

    /* U+006F "o" */
    0x38, 0x8a, 0xc, 0x18, 0x30, 0x51, 0x1c,

    /* U+0070 "p" */
    0xb9, 0x8a, 0xc, 0x18, 0x30, 0x71, 0x5c, 0x81,
    0x2, 0x0,

    /* U+0071 "q" */
    0x3a, 0x8e, 0xc, 0x18, 0x30, 0x51, 0x9d, 0x2,
    0x4, 0x8,

    /* U+0072 "r" */
    0xbc, 0x88, 0x88, 0x88,

    /* U+0073 "s" */
    0x7e, 0x8, 0x3c, 0x1c, 0x10, 0x7e,

    /* U+0074 "t" */
    0x42, 0x3e, 0x84, 0x21, 0x8, 0x41, 0xc0,

    /* U+0075 "u" */
    0x86, 0x18, 0x61, 0x86, 0x1c, 0xdd,

    /* U+0076 "v" */
    0x43, 0x42, 0x62, 0x26, 0x24, 0x34, 0x1c, 0x18,

    /* U+0077 "w" */
    0x8c, 0xe3, 0x29, 0x4b, 0x52, 0x57, 0x94, 0xc6,
    0x31, 0x8c,

    /* U+0078 "x" */
    0x62, 0x26, 0x3c, 0x18, 0x18, 0x34, 0x66, 0x42,

    /* U+0079 "y" */
    0x43, 0x42, 0x66, 0x24, 0x24, 0x3c, 0x18, 0x18,
    0x10, 0x10, 0x60,

    /* U+007A "z" */
    0xfc, 0x31, 0x84, 0x21, 0x8c, 0x3f,

    /* U+007B "{" */
    0x1c, 0x82, 0x8, 0x20, 0x8c, 0x8, 0x20, 0x82,
    0x8, 0x1c,

    /* U+007C "|" */
    0xff, 0xfe,

    /* U+007D "}" */
    0xc0, 0x82, 0x8, 0x20, 0x81, 0xc8, 0x20, 0x82,
    0x8, 0xc0,

    /* U+007E "~" */
    0x78, 0xc7, 0x80
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 71, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 90, .box_w = 1, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 3, .adv_w = 103, .box_w = 4, .box_h = 4, .ofs_x = 1, .ofs_y = 6},
    {.bitmap_index = 5, .adv_w = 188, .box_w = 10, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 18, .adv_w = 143, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 28, .adv_w = 213, .box_w = 12, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 43, .adv_w = 175, .box_w = 10, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 56, .adv_w = 62, .box_w = 1, .box_h = 4, .ofs_x = 1, .ofs_y = 6},
    {.bitmap_index = 57, .adv_w = 87, .box_w = 3, .box_h = 13, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 62, .adv_w = 87, .box_w = 3, .box_h = 13, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 67, .adv_w = 112, .box_w = 7, .box_h = 6, .ofs_x = 0, .ofs_y = 4},
    {.bitmap_index = 73, .adv_w = 188, .box_w = 9, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 84, .adv_w = 71, .box_w = 2, .box_h = 3, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 85, .adv_w = 81, .box_w = 4, .box_h = 1, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 86, .adv_w = 71, .box_w = 1, .box_h = 2, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 87, .adv_w = 75, .box_w = 5, .box_h = 12, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 95, .adv_w = 143, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 104, .adv_w = 143, .box_w = 6, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 112, .adv_w = 143, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 120, .adv_w = 143, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 128, .adv_w = 143, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 137, .adv_w = 143, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 146, .adv_w = 143, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 155, .adv_w = 143, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 164, .adv_w = 143, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 172, .adv_w = 143, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 181, .adv_w = 75, .box_w = 1, .box_h = 7, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 182, .adv_w = 75, .box_w = 2, .box_h = 9, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 185, .adv_w = 188, .box_w = 9, .box_h = 7, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 193, .adv_w = 188, .box_w = 9, .box_h = 4, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 198, .adv_w = 188, .box_w = 9, .box_h = 7, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 206, .adv_w = 119, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 214, .adv_w = 224, .box_w = 12, .box_h = 13, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 234, .adv_w = 153, .box_w = 9, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 246, .adv_w = 154, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 255, .adv_w = 156, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 265, .adv_w = 172, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 275, .adv_w = 142, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 283, .adv_w = 129, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 290, .adv_w = 174, .box_w = 9, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 302, .adv_w = 168, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 312, .adv_w = 66, .box_w = 1, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 314, .adv_w = 66, .box_w = 3, .box_h = 13, .ofs_x = -1, .ofs_y = -3},
    {.bitmap_index = 319, .adv_w = 147, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 329, .adv_w = 125, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 337, .adv_w = 193, .box_w = 9, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 349, .adv_w = 168, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 359, .adv_w = 176, .box_w = 9, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 371, .adv_w = 135, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 379, .adv_w = 176, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 393, .adv_w = 156, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 402, .adv_w = 142, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 411, .adv_w = 137, .box_w = 9, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 423, .adv_w = 164, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 433, .adv_w = 153, .box_w = 9, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 445, .adv_w = 221, .box_w = 13, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 462, .adv_w = 153, .box_w = 9, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 474, .adv_w = 137, .box_w = 9, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 486, .adv_w = 153, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 496, .adv_w = 87, .box_w = 3, .box_h = 13, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 501, .adv_w = 75, .box_w = 5, .box_h = 12, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 509, .adv_w = 87, .box_w = 3, .box_h = 13, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 514, .adv_w = 188, .box_w = 9, .box_h = 4, .ofs_x = 1, .ofs_y = 7},
    {.bitmap_index = 519, .adv_w = 112, .box_w = 7, .box_h = 1, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 520, .adv_w = 112, .box_w = 3, .box_h = 3, .ofs_x = 1, .ofs_y = 9},
    {.bitmap_index = 522, .adv_w = 137, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 528, .adv_w = 142, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 538, .adv_w = 123, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 544, .adv_w = 142, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 554, .adv_w = 138, .box_w = 7, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 561, .adv_w = 79, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 568, .adv_w = 142, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 578, .adv_w = 142, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 587, .adv_w = 62, .box_w = 1, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 589, .adv_w = 62, .box_w = 3, .box_h = 14, .ofs_x = -1, .ofs_y = -3},
    {.bitmap_index = 595, .adv_w = 130, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 605, .adv_w = 62, .box_w = 1, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 607, .adv_w = 218, .box_w = 11, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 618, .adv_w = 142, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 624, .adv_w = 137, .box_w = 7, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 631, .adv_w = 142, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 641, .adv_w = 142, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 651, .adv_w = 92, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 655, .adv_w = 117, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 661, .adv_w = 88, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 668, .adv_w = 142, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 674, .adv_w = 133, .box_w = 8, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 682, .adv_w = 183, .box_w = 10, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 692, .adv_w = 133, .box_w = 8, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 700, .adv_w = 133, .box_w = 8, .box_h = 11, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 711, .adv_w = 118, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 717, .adv_w = 143, .box_w = 6, .box_h = 13, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 727, .adv_w = 75, .box_w = 1, .box_h = 15, .ofs_x = 2, .ofs_y = -4},
    {.bitmap_index = 729, .adv_w = 143, .box_w = 6, .box_h = 13, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 739, .adv_w = 188, .box_w = 9, .box_h = 2, .ofs_x = 1, .ofs_y = 4}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t p99_font = {
#else
lv_font_t p99_font = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 4,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if P99_FONT*/

