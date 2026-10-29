// Keyword and function tables (X-macros keep the enums and the flash name lists in step, spec A17).
#ifndef VDB_SQL_NAMES_HPP
#define VDB_SQL_NAMES_HPP

#include "config.hpp"

namespace vdb {

// X(name, reserved): a reserved word cannot be a bare table or column name (quote it: "order").
#define VDB_KEYWORDS(X)                                                                                             \
    X(ALL, 1) X(AND, 1) X(AS, 1) X(ASC, 1) X(BETWEEN, 1) X(BY, 1) X(CASE, 1) X(CAST, 1) X(CREATE, 1) X(CROSS, 1)       \
    X(DELETE, 1) X(DESC, 1) X(DISTINCT, 1) X(DROP, 1) X(ELSE, 1) X(END, 1) X(ESCAPE, 1) X(EXCEPT, 1) X(EXISTS, 1)     \
    X(FALSE, 1) X(FROM, 1) X(FULL, 1) X(GLOB, 1) X(GROUP, 1) X(HAVING, 1) X(ILIKE, 1) X(IN, 1) X(INNER, 1)            \
    X(INSERT, 1) X(INTERSECT, 1) X(INTO, 1) X(IS, 1) X(JOIN, 1) X(LEFT, 1) X(LIKE, 1) X(LIMIT, 1) X(NOT, 1)          \
    X(NULL, 1) X(OFFSET, 1) X(ON, 1) X(OR, 1) X(ORDER, 1) X(OUTER, 1) X(REGEXP, 1) X(RIGHT, 1) X(RLIKE, 1)            \
    X(SELECT, 1) X(SET, 1) X(SIMILAR, 1) X(TABLE, 1) X(THEN, 1) X(TRUE, 1) X(UNION, 1) X(UPDATE, 1) X(USING, 1)      \
    X(VALUES, 1) X(WHEN, 1) X(WHERE, 1) X(WITH, 1) X(BOTH, 0) X(FETCH, 0) X(FIRST, 0) X(FOR, 0) X(IF, 0)              \
    X(LAST, 0) X(LATERAL, 0) X(LEADING, 0) X(NATURAL, 1) X(NEXT, 0) X(NULLS, 0) X(ONLY, 0) X(OVER, 0)                 \
    X(RECURSIVE, 0) X(ROW, 0) X(ROWS, 0) X(TO, 0) X(TRAILING, 0) X(ISNULL, 0) X(NOTNULL, 0) X(PRIMARY, 0)            \
    X(UNIQUE, 0) X(DEFAULT, 0) X(REFERENCES, 0) X(CHECK, 0) X(CONSTRAINT, 0) X(INDEX, 0) X(VIEW, 0) X(ALTER, 0)      \
    X(TRIGGER, 0) X(WINDOW, 0)

enum Keyword {
    kw_none = 0,
#define VDB_KW_ENUM(name, reserved) kw_##name,
    VDB_KEYWORDS(VDB_KW_ENUM)
#undef VDB_KW_ENUM
        kw_count
};

// X(id, "NAME", min args, max args (-1: any), flags)
enum FunctionFlag { fn_aggregate = 1, fn_takes_null = 2 };

#define VDB_FUNCTIONS(X)                                                                                            \
    X(COALESCE, 1, -1, 2) X(IFNULL, 2, 2, 2) X(NVL, 2, 2, 2) X(NULLIF, 2, 2, 2) X(IIF, 3, 3, 2) X(GREATEST, 1, -1, 2) \
    X(LEAST, 1, -1, 2) X(TYPEOF, 1, 1, 2) X(IS_NUMBER, 1, 1, 2) X(ISNUMERIC, 1, 1, 2) X(LENGTH, 1, 1, 0)             \
    X(LEN, 1, 1, 0) X(CHAR_LENGTH, 1, 1, 0) X(CHARACTER_LENGTH, 1, 1, 0) X(OCTET_LENGTH, 1, 1, 0) X(UPPER, 1, 1, 0)  \
    X(UCASE, 1, 1, 0) X(LOWER, 1, 1, 0) X(LCASE, 1, 1, 0) X(TRIM, 1, 2, 0) X(LTRIM, 1, 2, 0) X(RTRIM, 1, 2, 0)       \
    X(SUBSTR, 2, 3, 0) X(SUBSTRING, 2, 3, 0) X(LEFT, 2, 2, 0) X(RIGHT, 2, 2, 0) X(REPLACE, 3, 3, 0) X(INSTR, 2, 2, 0) \
    X(LPAD, 2, 3, 0) X(RPAD, 2, 3, 0) X(REVERSE, 1, 1, 0) X(REPEAT, 2, 2, 0) X(CONCAT, 1, -1, 2)                     \
    X(CONCAT_WS, 2, -1, 2) X(SPLIT_PART, 3, 3, 0) X(INITCAP, 1, 1, 0) X(ASCII, 1, 1, 0) X(CHAR, 1, -1, 0)            \
    X(CHR, 1, -1, 0) X(CONTAINS, 2, 2, 0) X(STARTS_WITH, 2, 2, 0) X(ENDS_WITH, 2, 2, 0) X(PRINTF, 1, -1, 2)          \
    X(FORMAT, 1, -1, 2) X(REGEXP_LIKE, 2, 3, 0) X(REGEXP_SUBSTR, 2, 4, 0) X(REGEXP_EXTRACT, 2, 4, 0)                  \
    X(REGEXP_REPLACE, 3, 4, 0) X(REGEXP_COUNT, 2, 3, 0) X(REGEXP_INSTR, 2, 3, 0) X(ABS, 1, 1, 0) X(SIGN, 1, 1, 0)    \
    X(ROUND, 1, 2, 0) X(TRUNC, 1, 2, 0) X(TRUNCATE, 1, 2, 0) X(FLOOR, 1, 1, 0) X(CEIL, 1, 1, 0) X(CEILING, 1, 1, 0)  \
    X(MOD, 2, 2, 0) X(POWER, 2, 2, 0) X(POW, 2, 2, 0) X(SQRT, 1, 1, 0) X(EXP, 1, 1, 0) X(LN, 1, 1, 0)                 \
    X(LOG, 1, 2, 0) X(LOG10, 1, 1, 0) X(LOG2, 1, 1, 0) X(PI, 0, 0, 0) X(SIN, 1, 1, 0) X(COS, 1, 1, 0)                 \
    X(TAN, 1, 1, 0) X(ASIN, 1, 1, 0) X(ACOS, 1, 1, 0) X(ATAN, 1, 1, 0) X(ATAN2, 2, 2, 0) X(DEGREES, 1, 1, 0)          \
    X(RADIANS, 1, 1, 0) X(COUNT, 0, 1, 1) X(SUM, 1, 1, 1) X(TOTAL, 1, 1, 1) X(AVG, 1, 1, 1) X(MIN, 1, -1, 1)          \
    X(MAX, 1, -1, 1) X(GROUP_CONCAT, 1, 2, 1) X(STRING_AGG, 2, 2, 1) X(STDDEV, 1, 1, 1) X(STDDEV_SAMP, 1, 1, 1)       \
    X(STDDEV_POP, 1, 1, 1) X(VARIANCE, 1, 1, 1) X(VAR_SAMP, 1, 1, 1) X(VAR_POP, 1, 1, 1) X(MEDIAN, 1, 1, 1)

enum Function {
    fn_none = 0,
#define VDB_FN_ENUM(name, min, max, flags) fn_##name,
    VDB_FUNCTIONS(VDB_FN_ENUM)
#undef VDB_FN_ENUM
        fn_count
};

struct FunctionInfo {
    int8_t min_args;
    int8_t max_args;
    uint8_t flags;
};

Keyword find_keyword(const char* text, size_t length);
bool keyword_reserved(Keyword keyword);
Function find_function(const char* text, size_t length);
FunctionInfo function_info(Function function);
const char* function_name(Function function, char* buffer, size_t size);  // copies out of flash
// Calls `visit` with each function name (for --functions and hints).
void each_function(void (*visit)(const char* name, const FunctionInfo& info, void* user), void* user);

}  // namespace vdb

#endif
