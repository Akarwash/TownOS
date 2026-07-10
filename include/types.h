#ifndef TYPES_H
#define TYPES_H

typedef unsigned char      uint8_t;     // exactly 1 byte, unsigned (0 to 255)
typedef unsigned short     uint16_t;    // exactly 2 bytes, unsigned (0 to 65535)
typedef unsigned int       uint32_t;    // exactly 4 bytes, unsigned (0 to ~4 billion)

typedef unsigned long      uint64_t;    // exactly 8 bytes, unsigned (LP64: long is 64-bit)

typedef signed char        int8_t;      // 1 byte, signed (-128 to 127)
typedef signed short       int16_t;     // 2 bytes, signed
typedef signed int         int32_t;     // 4 bytes, signed
typedef signed long        int64_t;     // 8 bytes, signed

typedef uint64_t   size_t;              // 64-bit: matches pointer width on x86-64
#define NULL       ((void *)0)

#endif