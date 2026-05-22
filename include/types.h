#ifndef TYPES_H
#define TYPES_H

typedef unsigned char      uint8_t;     // exactly 1 byte, unsigned (0 to 255)
typedef unsigned short     uint16_t;    // exactly 2 bytes, unsigned (0 to 65535)
typedef unsigned int       uint32_t;    // exactly 4 bytes, unsigned (0 to ~4 billion)

typedef signed char        int8_t;      // 1 byte, signed (-128 to 127)
typedef signed short       int16_t;     // 2 bytes, signed
typedef signed int         int32_t;     // 4 bytes, signed

typedef uint32_t   size_t;
#define NULL       ((void *)0)

#endif