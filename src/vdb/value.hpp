// Values in expressions and the rules that compare them (spec 2.2 – 2.4, D3, A3).
#ifndef VDB_VALUE_HPP
#define VDB_VALUE_HPP

#include "text.hpp"

namespace vdb {

enum ValueType { k_null = 0, k_integer = 1, k_real = 2, k_text = 3 };

enum ValueFlag {
    k_strict = 1,   // quoted literal or CAST AS TEXT: compared as text
    k_boolean = 2,  // TRUE, FALSE or a comparison: compared by truth
    k_has_text = 4  // a number whose text form is in s/n too
};

struct Value {
    uint8_t type;
    uint8_t flags;
    union {
        int64_t i;
        double r;
    };
    const char* s;
    size_t n;

    static Value null() {
        Value v;
        v.type = k_null;
        v.flags = 0;
        v.i = 0;
        v.s = "";
        v.n = 0;
        return v;
    }
    static Value integer(int64_t value) {
        Value v = null();
        v.type = k_integer;
        v.i = value;
        return v;
    }
    static Value real(double value) {
        Value v = null();
        v.type = k_real;
        v.r = value;
        return v;
    }
    static Value boolean(bool value) {
        Value v = integer(value ? 1 : 0);
        v.flags = k_boolean;
        return v;
    }
    static Value text(const char* s, size_t n, uint8_t flags = 0) {
        Value v = null();
        v.type = k_text;
        v.flags = flags;
        v.s = s;
        v.n = n;
        return v;
    }
    bool is_null() const { return type == k_null; }
    bool is_number() const { return type == k_integer || type == k_real; }
};

enum Truth { k_false = 0, k_true = 1, k_unknown = -1 };

// A number from any value (text read per spec 2.2). False for NULL and text that is not a number.
bool value_number(const Value& v, char decimal, Number& out);
Value number_value(const Number& n);

// Truth of a value: NULL is UNKNOWN; numbers are true when not 0; text "true/yes/on/t/y" and "false/no/off/f/n"
// (any case) and numeric text as numbers; any other text is false.
int value_truth(const Value& v, char decimal);

// SQL comparison (spec 2.3). Returns false when the result is UNKNOWN; otherwise `result` is -1, 0 or 1.
bool compare_sql(const Value& a, const Value& b, char decimal, int& result);

// The one total order of spec 2.4: NULL < numbers < text.
int compare_order(const Value& a, const Value& b, char decimal);

// Appends a key equal for values that compare_order calls equal (grouping, DISTINCT, joins, sets).
bool key_append(Text& key, const Value& v, char decimal);

// Text form of a value: numbers formatted into `scratch` (A2). Returns false on out of memory.
bool value_text(const Value& v, char decimal, Pool& scratch, const char*& s, size_t& n);

// The value with its text form filled (numbers formatted), for sinks and records.
bool textify(Value& v, char decimal, Pool& scratch);

uint32_t hash_bytes(const char* data, size_t length);

}  // namespace vdb

#endif
