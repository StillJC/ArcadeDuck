#ifndef H8_TYPES_H
#define H8_TYPES_H

#include "config.h"

#define h8_u8 unsigned char
#define h8_s8 signed char
#define h8_u16 unsigned short
#define h8_s16 signed short
#define h8_u32 unsigned int
#define h8_s32 signed int
#define h8_bool unsigned char

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0
#endif

#if H8_REVERSE_BITFIELDS
#define H8_BITFIELD_2(a, b) struct \
{ \
  b; \
  a; \
}
#define H8_BITFIELD_3(a, b, c) struct \
{ \
  c; \
  b; \
  a; \
}
#define H8_BITFIELD_4(a, b, c, d) struct \
{ \
  d; \
  c; \
  b; \
  a; \
}
#define H8_BITFIELD_5(a, b, c, d, e) struct \
{ \
  e; \
  d; \
  c; \
  b; \
  a; \
}
#define H8_BITFIELD_6(a, b, c, d, e, f) struct \
{ \
  f; \
  e; \
  d; \
  c; \
  b; \
  a; \
}
#define H8_BITFIELD_7(a, b, c, d, e, f, g) struct \
{ \
  g; \
  f; \
  e; \
  d; \
  c; \
  b; \
  a; \
}
#define H8_BITFIELD_8(a, b, c, d, e, f, g, h) struct \
{ \
  h; \
  g; \
  f; \
  e; \
  d; \
  c; \
  b; \
  a; \
}
#else
#define H8_BITFIELD_2(a, b) struct \
{ \
  a; \
  b; \
}
#define H8_BITFIELD_3(a, b, c) struct \
{ \
  a; \
  b; \
  c; \
}
#define H8_BITFIELD_4(a, b, c, d) struct \
{ \
  a; \
  b; \
  c; \
  d; \
}
#define H8_BITFIELD_5(a, b, c, d, e) struct \
{ \
  a; \
  b; \
  c; \
  d; \
  e; \
}
#define H8_BITFIELD_6(a, b, c, d, e, f) struct \
{ \
  a; \
  b; \
  c; \
  d; \
  e; \
  f; \
}
#define H8_BITFIELD_7(a, b, c, d, e, f, g) struct \
{ \
  a; \
  b; \
  c; \
  d; \
  e; \
  f; \
  g; \
}
#define H8_BITFIELD_8(a, b, c, d, e, f, g, h) struct \
{ \
  a; \
  b; \
  c; \
  d; \
  e; \
  f; \
  g; \
  h; \
}
#endif

/**
 * An 8-bit value.
 */
typedef union
{
  H8_BITFIELD_2
  (
    h8_u8 l : 4,
    h8_u8 h : 4
  );
  h8_u8 u;
  h8_s8 i;
} h8_byte_t;

/**
 * A 16-bit value in native endianness.
 */
typedef union
{
  struct
  {
#if H8_BIG_ENDIAN
    h8_byte_t h;
    h8_byte_t l;
#else
    h8_byte_t l;
    h8_byte_t h;
#endif
  };
  h8_u16 u;
  h8_s16 i;
} h8_word_t;

/**
 * A 16-bit value in explicit big endianness.
 */
typedef struct
{
  h8_byte_t h;
  h8_byte_t l;
} h8_word_be_t;

/**
 * A 32-bit value in native endianness.
 */
typedef union
{
  struct
  {
#if H8_BIG_ENDIAN
    h8_byte_t a;
    h8_byte_t b;
    h8_byte_t c;
    h8_byte_t d;
#else
    h8_byte_t d;
    h8_byte_t c;
    h8_byte_t b;
    h8_byte_t a;
#endif
  };
  struct
  {
#if H8_BIG_ENDIAN
    h8_word_t h;
    h8_word_t l;
#else
    h8_word_t l;
    h8_word_t h;
#endif
  };
  h8_u32 u;
  h8_s32 i;
} h8_long_t;

/**
 * A 32-bit value in explicit big endianness.
 */
typedef struct
{
  h8_byte_t a;
  h8_byte_t b;
  h8_byte_t c;
  h8_byte_t d;
} h8_long_be_t;

typedef union
{
  struct
  {
#if H8_BIG_ENDIAN
    union
    {
      struct
      {
        h8_u8 ah : 4;
        h8_u8 al : 4;
      };
      h8_byte_t a;
    };
    union
    {
      struct
      {
        h8_u8 bh : 4;
        h8_u8 bl : 4;
      };
      h8_byte_t b;
    };
#else
    union
    {
      struct
      {
        h8_u8 bl : 4;
        h8_u8 bh : 4;
      };
      h8_byte_t b;
    };
    union
    {
      struct
      {
        h8_u8 al : 4;
        h8_u8 ah : 4;
      };
      h8_byte_t a;
    };
#endif
  };
  h8_word_t bits;
} h8_instruction_t;

/* Used to silence warning for unused variables */
#define H8_UNUSED(a) (void)a

/* 2-bit binary constants */
#define B00 0x0
#define B01 0x1
#define B10 0x2
#define B11 0x3

/* 4-bit binary constants */
#define B0000 0x0
#define B0001 0x1
#define B0010 0x2
#define B0011 0x3
#define B0100 0x4
#define B0101 0x5
#define B0110 0x6
#define B0111 0x7
#define B1000 0x8
#define B1001 0x9
#define B1010 0xa
#define B1011 0xb
#define B1100 0xc
#define B1101 0xd
#define B1110 0xe
#define B1111 0xf

/* 8-bit binary constants */
#define B00000000 0x0
#define B00000001 0x1
#define B00000010 0x2
#define B00000011 0x3
#define B00000100 0x4
#define B00000101 0x5
#define B00000110 0x6
#define B00000111 0x7
#define B00001000 0x8
#define B00001001 0x9
#define B00001010 0xa
#define B00001011 0xb
#define B00001100 0xc
#define B00001101 0xd
#define B00001110 0xe
#define B00001111 0xf
#define B00010000 0x10
#define B00010001 0x11
#define B00010010 0x12
#define B00010011 0x13
#define B00010100 0x14
#define B00010101 0x15
#define B00010110 0x16
#define B00010111 0x17
#define B00011000 0x18
#define B00011001 0x19
#define B00011010 0x1a
#define B00011011 0x1b
#define B00011100 0x1c
#define B00011101 0x1d
#define B00011110 0x1e
#define B00011111 0x1f
#define B00100000 0x20
#define B00100001 0x21
#define B00100010 0x22
#define B00100011 0x23
#define B00100100 0x24
#define B00100101 0x25
#define B00100110 0x26
#define B00100111 0x27
#define B00101000 0x28
#define B00101001 0x29
#define B00101010 0x2a
#define B00101011 0x2b
#define B00101100 0x2c
#define B00101101 0x2d
#define B00101110 0x2e
#define B00101111 0x2f
#define B00110000 0x30
#define B00110001 0x31
#define B00110010 0x32
#define B00110011 0x33
#define B00110100 0x34
#define B00110101 0x35
#define B00110110 0x36
#define B00110111 0x37
#define B00111000 0x38
#define B00111001 0x39
#define B00111010 0x3a
#define B00111011 0x3b
#define B00111100 0x3c
#define B00111101 0x3d
#define B00111110 0x3e
#define B00111111 0x3f
#define B01000000 0x40
#define B01000001 0x41
#define B01000010 0x42
#define B01000011 0x43
#define B01000100 0x44
#define B01000101 0x45
#define B01000110 0x46
#define B01000111 0x47
#define B01001000 0x48
#define B01001001 0x49
#define B01001010 0x4a
#define B01001011 0x4b
#define B01001100 0x4c
#define B01001101 0x4d
#define B01001110 0x4e
#define B01001111 0x4f
#define B01010000 0x50
#define B01010001 0x51
#define B01010010 0x52
#define B01010011 0x53
#define B01010100 0x54
#define B01010101 0x55
#define B01010110 0x56
#define B01010111 0x57
#define B01011000 0x58
#define B01011001 0x59
#define B01011010 0x5a
#define B01011011 0x5b
#define B01011100 0x5c
#define B01011101 0x5d
#define B01011110 0x5e
#define B01011111 0x5f
#define B01100000 0x60
#define B01100001 0x61
#define B01100010 0x62
#define B01100011 0x63
#define B01100100 0x64
#define B01100101 0x65
#define B01100110 0x66
#define B01100111 0x67
#define B01101000 0x68
#define B01101001 0x69
#define B01101010 0x6a
#define B01101011 0x6b
#define B01101100 0x6c
#define B01101101 0x6d
#define B01101110 0x6e
#define B01101111 0x6f
#define B01110000 0x70
#define B01110001 0x71
#define B01110010 0x72
#define B01110011 0x73
#define B01110100 0x74
#define B01110101 0x75
#define B01110110 0x76
#define B01110111 0x77
#define B01111000 0x78
#define B01111001 0x79
#define B01111010 0x7a
#define B01111011 0x7b
#define B01111100 0x7c
#define B01111101 0x7d
#define B01111110 0x7e
#define B01111111 0x7f
#define B10000000 0x80
#define B10000001 0x81
#define B10000010 0x82
#define B10000011 0x83
#define B10000100 0x84
#define B10000101 0x85
#define B10000110 0x86
#define B10000111 0x87
#define B10001000 0x88
#define B10001001 0x89
#define B10001010 0x8a
#define B10001011 0x8b
#define B10001100 0x8c
#define B10001101 0x8d
#define B10001110 0x8e
#define B10001111 0x8f
#define B10010000 0x90
#define B10010001 0x91
#define B10010010 0x92
#define B10010011 0x93
#define B10010100 0x94
#define B10010101 0x95
#define B10010110 0x96
#define B10010111 0x97
#define B10011000 0x98
#define B10011001 0x99
#define B10011010 0x9a
#define B10011011 0x9b
#define B10011100 0x9c
#define B10011101 0x9d
#define B10011110 0x9e
#define B10011111 0x9f
#define B10100000 0xa0
#define B10100001 0xa1
#define B10100010 0xa2
#define B10100011 0xa3
#define B10100100 0xa4
#define B10100101 0xa5
#define B10100110 0xa6
#define B10100111 0xa7
#define B10101000 0xa8
#define B10101001 0xa9
#define B10101010 0xaa
#define B10101011 0xab
#define B10101100 0xac
#define B10101101 0xad
#define B10101110 0xae
#define B10101111 0xaf
#define B10110000 0xb0
#define B10110001 0xb1
#define B10110010 0xb2
#define B10110011 0xb3
#define B10110100 0xb4
#define B10110101 0xb5
#define B10110110 0xb6
#define B10110111 0xb7
#define B10111000 0xb8
#define B10111001 0xb9
#define B10111010 0xba
#define B10111011 0xbb
#define B10111100 0xbc
#define B10111101 0xbd
#define B10111110 0xbe
#define B10111111 0xbf
#define B11000000 0xc0
#define B11000001 0xc1
#define B11000010 0xc2
#define B11000011 0xc3
#define B11000100 0xc4
#define B11000101 0xc5
#define B11000110 0xc6
#define B11000111 0xc7
#define B11001000 0xc8
#define B11001001 0xc9
#define B11001010 0xca
#define B11001011 0xcb
#define B11001100 0xcc
#define B11001101 0xcd
#define B11001110 0xce
#define B11001111 0xcf
#define B11010000 0xd0
#define B11010001 0xd1
#define B11010010 0xd2
#define B11010011 0xd3
#define B11010100 0xd4
#define B11010101 0xd5
#define B11010110 0xd6
#define B11010111 0xd7
#define B11011000 0xd8
#define B11011001 0xd9
#define B11011010 0xda
#define B11011011 0xdb
#define B11011100 0xdc
#define B11011101 0xdd
#define B11011110 0xde
#define B11011111 0xdf
#define B11100000 0xe0
#define B11100001 0xe1
#define B11100010 0xe2
#define B11100011 0xe3
#define B11100100 0xe4
#define B11100101 0xe5
#define B11100110 0xe6
#define B11100111 0xe7
#define B11101000 0xe8
#define B11101001 0xe9
#define B11101010 0xea
#define B11101011 0xeb
#define B11101100 0xec
#define B11101101 0xed
#define B11101110 0xee
#define B11101111 0xef
#define B11110000 0xf0
#define B11110001 0xf1
#define B11110010 0xf2
#define B11110011 0xf3
#define B11110100 0xf4
#define B11110101 0xf5
#define B11110110 0xf6
#define B11110111 0xf7
#define B11111000 0xf8
#define B11111001 0xf9
#define B11111010 0xfa
#define B11111011 0xfb
#define B11111100 0xfc
#define B11111101 0xfd
#define B11111110 0xfe
#define B11111111 0xff

#endif
