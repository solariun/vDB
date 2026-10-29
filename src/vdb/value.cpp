#include "value.hpp"

#include <string.h>

namespace vdb {

bool value_number(const Value& v, char decimal, Number& out) {
    switch (v.type) {
        case k_integer:
            out.is_int = true;
            out.i = v.i;
            out.r = static_cast<double>(v.i);
            return true;
        case k_real:
            out.is_int = false;
            out.i = 0;
            out.r = v.r;
            return true;
        case k_text: return parse_number(v.s, v.n, decimal, out);
        default: return false;
    }
}

Value number_value(const Number& n) { return n.is_int ? Value::integer(n.i) : Value::real(n.r); }

namespace {

bool word_is(const Value& v, const char* word) {
    size_t start = 0, end = v.n;
    while (start < end && is_space(v.s[start])) ++start;
    while (end > start && is_space(v.s[end - 1])) --end;
    return names_equal(v.s + start, end - start, word);
}

int compare_numbers(const Number& a, const Number& b) {
    if (a.is_int && b.is_int) return a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
    double x = a.real(), y = b.real();
    return x < y ? -1 : x > y ? 1 : 0;
}

}  // namespace

int value_truth(const Value& v, char decimal) {
    if (v.type == k_null) return k_unknown;
    if (v.type == k_integer) return v.i != 0 ? k_true : k_false;
    if (v.type == k_real) return v.r != 0 ? k_true : k_false;
    Number n;
    if (parse_number(v.s, v.n, decimal, n)) return n.real() != 0 ? k_true : k_false;
    if (word_is(v, "true") || word_is(v, "yes") || word_is(v, "on") || word_is(v, "t") || word_is(v, "y")) return k_true;
    return k_false;
}

bool compare_sql(const Value& a, const Value& b, char decimal, int& result) {
    if (a.type == k_null || b.type == k_null) return false;
    if ((a.flags & k_boolean) || (b.flags & k_boolean)) {
        int x = value_truth(a, decimal), y = value_truth(b, decimal);
        if (x == k_unknown || y == k_unknown) return false;
        result = x < y ? -1 : x > y ? 1 : 0;
        return true;
    }
    Number x, y;
    if (a.is_number() || b.is_number()) {
        if (!value_number(a, decimal, x) || !value_number(b, decimal, y)) return false;  // D3
        result = compare_numbers(x, y);
        return true;
    }
    bool a_flexible = !(a.flags & k_strict), b_flexible = !(b.flags & k_strict);
    if (a_flexible && b_flexible && parse_number(a.s, a.n, decimal, x) && parse_number(b.s, b.n, decimal, y)) {
        result = compare_numbers(x, y);
        return true;
    }
    result = compare_bytes(a.s, a.n, b.s, b.n);
    return true;
}

int compare_order(const Value& a, const Value& b, char decimal) {
    if (a.type == k_null || b.type == k_null) return a.type == b.type ? 0 : a.type == k_null ? -1 : 1;
    Number x, y;
    bool a_number = value_number(a, decimal, x);
    bool b_number = value_number(b, decimal, y);
    if (a_number && b_number) return compare_numbers(x, y);
    if (a_number != b_number) return a_number ? -1 : 1;
    return compare_bytes(a.s, a.n, b.s, b.n);
}

bool key_append(Text& key, const Value& v, char decimal) {
    Number n;
    if (v.type == k_null) return key.append_char('N');
    if (value_number(v, decimal, n)) {
        int64_t whole;
        if (!n.is_int && real_is_integral(n.r, whole)) {
            n.is_int = true;
            n.i = whole;
        }
        unsigned char bytes[sizeof(int64_t) > sizeof(double) ? sizeof(int64_t) : sizeof(double)];
        if (n.is_int) {
            key.append_char('I');
            for (size_t i = 0; i < sizeof(int64_t); ++i) bytes[i] = static_cast<unsigned char>(static_cast<uint64_t>(n.i) >> (8 * i));
            return key.append(reinterpret_cast<const char*>(bytes), sizeof(int64_t));
        }
        double r = n.r == 0 ? 0.0 : n.r;
        memcpy(bytes, &r, sizeof(double));
        key.append_char('R');
        return key.append(reinterpret_cast<const char*>(bytes), sizeof(double));
    }
    char length[k_number_text];
    key.append_char('T');
    key.append(length, format_int(static_cast<int64_t>(v.n), length));
    key.append_char(':');
    return key.append(v.s, v.n);
}

bool value_text(const Value& v, char decimal, Pool& scratch, const char*& s, size_t& n) {
    if (v.type == k_text || (v.flags & k_has_text)) {
        s = v.s;
        n = v.n;
        return true;
    }
    if (v.type == k_null) {
        s = "";
        n = 0;
        return true;
    }
    char buffer[k_number_text];
    n = v.type == k_integer ? format_int(v.i, buffer) : format_real(v.r, decimal, k_real_digits, buffer);
    char* copy = scratch.copy(buffer, n);
    if (!copy) return false;
    s = copy;
    return true;
}

bool textify(Value& v, char decimal, Pool& scratch) {
    if (v.type == k_text || v.type == k_null || (v.flags & k_has_text)) return true;
    if (!value_text(v, decimal, scratch, v.s, v.n)) return false;
    v.flags |= k_has_text;
    return true;
}

uint32_t hash_bytes(const char* data, size_t length) {
    const uint32_t k_fnv_offset = 2166136261u;
    const uint32_t k_fnv_prime = 16777619u;
    uint32_t h = k_fnv_offset;
    for (size_t i = 0; i < length; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= k_fnv_prime;
    }
    return h;
}

}  // namespace vdb
