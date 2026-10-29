#include "sql_names.hpp"

#include <string.h>

#include "text.hpp"

namespace vdb {

namespace {

#define VDB_KW_TEXT(name, reserved) #name "\0"
const char k_keyword_names[] VDB_FLASH = VDB_KEYWORDS(VDB_KW_TEXT);
#undef VDB_KW_TEXT

#define VDB_KW_RESERVED(name, reserved) reserved,
const uint8_t k_keyword_reserved[] VDB_FLASH = {VDB_KEYWORDS(VDB_KW_RESERVED)};
#undef VDB_KW_RESERVED

#define VDB_FN_TEXT(name, min, max, flags) #name "\0"
const char k_function_names[] VDB_FLASH = VDB_FUNCTIONS(VDB_FN_TEXT);
#undef VDB_FN_TEXT

#define VDB_FN_INFO(name, min, max, flags) min, max, flags,
const int8_t k_function_info[] VDB_FLASH = {VDB_FUNCTIONS(VDB_FN_INFO)};
#undef VDB_FN_INFO

const size_t k_info_fields = 3;

// Index (from 1) of `text` in a flash list of NUL-separated names, or 0.
int find_in(const char* list, const char* text, size_t length) {
    int index = 1;
    for (const char* name = list; flash_char(name); ++index) {
        size_t n = flash_length(name);
        if (n == length && flash_names_equal(text, length, name)) return index;
        name += n + 1;
    }
    return 0;
}

}  // namespace

Keyword find_keyword(const char* text, size_t length) {
    return static_cast<Keyword>(find_in(k_keyword_names, text, length));
}

bool keyword_reserved(Keyword keyword) {
    return keyword > kw_none && keyword < kw_count && flash_byte(&k_keyword_reserved[keyword - 1]) != 0;
}

Function find_function(const char* text, size_t length) {
    return static_cast<Function>(find_in(k_function_names, text, length));
}

FunctionInfo function_info(Function function) {
    FunctionInfo info = {0, 0, 0};
    if (function <= fn_none || function >= fn_count) return info;
    const int8_t* p = &k_function_info[(function - 1) * k_info_fields];
    info.min_args = static_cast<int8_t>(flash_byte(reinterpret_cast<const uint8_t*>(p)));
    info.max_args = static_cast<int8_t>(flash_byte(reinterpret_cast<const uint8_t*>(p + 1)));
    info.flags = flash_byte(reinterpret_cast<const uint8_t*>(p + 2));
    return info;
}

const char* function_name(Function function, char* buffer, size_t size) {
    buffer[0] = 0;
    int index = 1;
    for (const char* name = k_function_names; flash_char(name); ++index) {
        size_t n = flash_length(name);
        if (index == function) {
            size_t i = 0;
            for (; i < n && i + 1 < size; ++i) buffer[i] = flash_char(name + i);
            buffer[i] = 0;
            break;
        }
        name += n + 1;
    }
    return buffer;
}

void each_function(void (*visit)(const char* name, const FunctionInfo& info, void* user), void* user) {
    const size_t k_name_size = 24;
    char buffer[k_name_size];
    for (int f = fn_none + 1; f < fn_count; ++f) {
        Function function = static_cast<Function>(f);
        visit(function_name(function, buffer, sizeof(buffer)), function_info(function), user);
    }
}

}  // namespace vdb
