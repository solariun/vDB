// vDB configuration: platform switches and size limits (spec 4.3, 8).
#ifndef VDB_CONFIG_HPP
#define VDB_CONFIG_HPP

#include <stddef.h>
#include <stdint.h>

#define VDB_VERSION "1.0.0"

// AVR copies string literals into RAM; tables and messages live in flash there (spec A17).
#if defined(__AVR__)
#include <avr/pgmspace.h>
#define VDB_FLASH PROGMEM
#define VDB_F(text) PSTR(text)
#define VDB_SMALL 1
#else
#define VDB_FLASH
#define VDB_F(text) (text)
#endif

// Bump-pool chunk for statement trees and temporaries.
#ifndef VDB_POOL_CHUNK
#ifdef VDB_SMALL
#define VDB_POOL_CHUNK 256
#else
#define VDB_POOL_CHUNK 4096
#endif
#endif

// Per-row scratch chunk (computed text of one row).
#ifndef VDB_SCRATCH_CHUNK
#ifdef VDB_SMALL
#define VDB_SCRATCH_CHUNK 128
#else
#define VDB_SCRATCH_CHUNK 4096
#endif
#endif

// CSV reader input buffer.
#ifndef VDB_CSV_CHUNK
#ifdef VDB_SMALL
#define VDB_CSV_CHUNK 64
#else
#define VDB_CSV_CHUNK 65536
#endif
#endif

// Nesting bound for the parser, expressions and subqueries: an error, never a stack overflow (spec 9).
#ifndef VDB_MAX_DEPTH
#ifdef VDB_SMALL
#define VDB_MAX_DEPTH 12
#else
#define VDB_MAX_DEPTH 200
#endif
#endif

// Largest compiled regular expression, in instructions (spec A6).
#ifndef VDB_REGEX_MAX_PROGRAM
#ifdef VDB_SMALL
#define VDB_REGEX_MAX_PROGRAM 128
#else
#define VDB_REGEX_MAX_PROGRAM 8192
#endif
#endif

// Room for one error message (fixed: reporting never allocates).
#ifndef VDB_ERROR_SIZE
#ifdef VDB_SMALL
#define VDB_ERROR_SIZE 72
#else
#define VDB_ERROR_SIZE 256
#endif
#endif

namespace vdb {

inline char flash_char(const char* p) {
#if defined(__AVR__)
    return static_cast<char>(pgm_read_byte(p));
#else
    return *p;
#endif
}

inline uint8_t flash_byte(const uint8_t* p) {
#if defined(__AVR__)
    return pgm_read_byte(p);
#else
    return *p;
#endif
}

}  // namespace vdb

#endif
