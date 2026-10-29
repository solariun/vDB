// Scalar functions and text matching (spec 3.4 – 3.6).
#include <math.h>
#include <string.h>

#include "exec.hpp"

namespace vdb {

#ifndef VDB_MAX_TEXT
#ifdef VDB_SMALL
#define VDB_MAX_TEXT 1024
#else
#define VDB_MAX_TEXT (64u * 1024u * 1024u)
#endif
#endif

namespace {

const double k_pi = 3.14159265358979323846;

bool same_cp(uint32_t a, uint32_t b, bool icase) { return a == b || (icase && fold_lower(a) == fold_lower(b)); }

bool text_of(Executor& ex, const Value& v, const char*& s, size_t& n) {
    if (!value_text(v, ex.decimal(), ex.scratch(), s, n)) return ex.memory();
    return true;
}

bool too_large(Executor& ex, size_t n) {
    if (n <= VDB_MAX_TEXT) return false;
    ex.status().fail(k_error_limit, VDB_F("text result too large (limit %ld bytes)"), static_cast<long>(VDB_MAX_TEXT));
    return true;
}

bool set_text(Executor& ex, Text& t, Value& out) {
    if (!t.ok()) return ex.memory();
    char* s = ex.text(t.size());
    if (!s) return false;
    memcpy(s, t.c_str(), t.size());
    out = Value::text(s, t.size());
    return true;
}

bool copy_text(Executor& ex, const char* s, size_t n, Value& out) {
    char* p = ex.text(n);
    if (!p) return false;
    if (n) memcpy(p, s, n);
    out = Value::text(p, n);
    return true;
}

bool number_arg(Executor& ex, const Value& v, Number& n) { return value_number(v, ex.decimal(), n); }

Value real_value(double r) {
    if (!finite(r)) return Value::null();
    return Value::real(r);
}

Value whole_value(double r) {
    int64_t i;
    if (real_is_integral(r, i)) return Value::integer(i);
    return real_value(r);
}

bool int_arg(Executor& ex, const Value& v, int64_t& out) {
    Number n;
    if (!number_arg(ex, v, n)) return false;
    out = n.is_int ? n.i : static_cast<int64_t>(n.r);
    return true;
}

bool contains_cp(const char* set, size_t n, uint32_t cp) {
    for (size_t i = 0; i < n;) {
        uint32_t c;
        i += utf8_decode(set + i, n - i, c);
        if (c == cp) return true;
    }
    return false;
}

const char* find_bytes(const char* s, size_t n, const char* needle, size_t m) {
    if (m == 0) return s;
    if (m > n) return 0;
    for (size_t i = 0; i + m <= n; ++i) {
        if (s[i] == needle[0] && memcmp(s + i, needle, m) == 0) return s + i;
    }
    return 0;
}

bool fold_text(Executor& ex, const char* s, size_t n, bool upper, Value& out) {
    char* p = ex.text(n * 2);
    if (!p) return false;
    size_t length = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += utf8_decode(s + i, n - i, cp);
        length += utf8_encode(upper ? fold_upper(cp) : fold_lower(cp), p + length);
    }
    p[length] = 0;
    out = Value::text(p, length);
    return true;
}

// Unsigned decimal or hex digits of `value` into `out`, returns the length.
size_t digits_of(uint64_t value, unsigned base, bool upper, char* out) {
    char reversed[24];
    size_t n = 0;
    do {
        unsigned d = static_cast<unsigned>(value % base);
        reversed[n++] = static_cast<char>(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        value /= base;
    } while (value);
    for (size_t i = 0; i < n; ++i) out[i] = reversed[n - 1 - i];
    return n;
}

// Fixed notation with `precision` decimals.
size_t format_fixed(double v, int precision, char decimal, char* out) {
    const double k_fixed_limit = 1e17;
    const int k_max_precision = 15;
    if (precision > k_max_precision) precision = k_max_precision;
    if (v != v || v >= k_fixed_limit || v <= -k_fixed_limit) return format_real(v, decimal, k_real_digits, out);
    size_t length = 0;
    if (v < 0) {
        out[length++] = '-';
        v = -v;
    }
    double scale = pow(10.0, precision);
    double scaled = floor(v * scale + 0.5);
    double whole = floor(scaled / scale);
    double fraction = scaled - whole * scale;
    length += digits_of(static_cast<uint64_t>(whole), 10, false, out + length);
    if (precision > 0) {
        out[length++] = decimal;
        char digits[24];
        size_t n = digits_of(static_cast<uint64_t>(fraction), 10, false, digits);
        for (size_t i = n; i < static_cast<size_t>(precision); ++i) out[length++] = '0';
        memcpy(out + length, digits, n);
        length += n;
    }
    out[length] = 0;
    return length;
}

bool printf_text(Executor& ex, Value* args, size_t count, Value& out) {
    const char* fmt;
    size_t fn;
    if (args[0].is_null()) {
        out = Value::null();
        return true;
    }
    if (!text_of(ex, args[0], fmt, fn)) return false;
    Text t(ex.allocator());
    size_t next = 1;
    for (size_t i = 0; i < fn; ++i) {
        if (fmt[i] != '%') {
            t.append_char(fmt[i]);
            continue;
        }
        ++i;
        bool left = false, zero = false, plus = false;
        for (; i < fn && (fmt[i] == '-' || fmt[i] == '0' || fmt[i] == '+' || fmt[i] == ' '); ++i) {
            if (fmt[i] == '-') left = true;
            if (fmt[i] == '0') zero = true;
            if (fmt[i] == '+') plus = true;
        }
        size_t width = 0;
        while (i < fn && is_digit(fmt[i])) width = width * 10 + static_cast<size_t>(fmt[i++] - '0');
        int precision = -1;
        if (i < fn && fmt[i] == '.') {
            precision = 0;
            ++i;
            while (i < fn && is_digit(fmt[i])) precision = precision * 10 + (fmt[i++] - '0');
        }
        if (i >= fn) break;
        char conv = fmt[i];
        if (conv == '%') {
            t.append_char('%');
            continue;
        }
        Value arg = next < count ? args[next] : Value::null();
        ++next;
        char buffer[64];
        const char* piece = buffer;
        size_t length = 0;
        Number num;
        bool numeric = number_arg(ex, arg, num);
        switch (conv) {
            case 'd':
            case 'i':
            case 'u': {
                int64_t v = numeric ? (num.is_int ? num.i : static_cast<int64_t>(num.r)) : 0;
                if (plus && v >= 0) buffer[length++] = '+';
                length += format_int(v, buffer + length);
                break;
            }
            case 'f':
            case 'F':
                if (plus && numeric && num.real() >= 0) buffer[length++] = '+';
                length += format_fixed(numeric ? num.real() : 0, precision < 0 ? 6 : precision, ex.decimal(), buffer + length);
                break;
            case 'e':
            case 'E':
            case 'g':
            case 'G':
                length = format_real(numeric ? num.real() : 0, ex.decimal(), precision < 0 ? k_real_digits : precision + 1, buffer);
                break;
            case 'x':
            case 'X': {
                int64_t v = numeric ? (num.is_int ? num.i : static_cast<int64_t>(num.r)) : 0;
                length = digits_of(static_cast<uint64_t>(v), 16, conv == 'X', buffer);
                break;
            }
            case 'c':
                if (numeric) {
                    length = utf8_encode(static_cast<uint32_t>(num.is_int ? num.i : static_cast<int64_t>(num.r)), buffer);
                } else if (arg.type != k_null) {
                    uint32_t cp = 0;
                    if (arg.n) length = utf8_decode(arg.s, arg.n, cp);
                    piece = arg.s;
                }
                break;
            default:  // 's' and anything else
                if (arg.type != k_null && !text_of(ex, arg, piece, length)) return false;
                if (precision >= 0) length = utf8_offset(piece, length, static_cast<size_t>(precision));
                break;
        }
        size_t chars = utf8_count(piece, length);
        size_t pad = width > chars ? width - chars : 0;
        if (!left) {
            bool zeros = zero && conv != 's' && conv != 'c';
            if (zeros && length && (piece[0] == '-' || piece[0] == '+')) {
                t.append_char(piece[0]);
                ++piece;
                --length;
            }
            for (size_t p = 0; p < pad; ++p) t.append_char(zeros ? '0' : ' ');
        }
        t.append(piece, length);
        if (left) {
            for (size_t p = 0; p < pad; ++p) t.append_char(' ');
        }
        if (too_large(ex, t.size())) return false;
    }
    return set_text(ex, t, out);
}

// Flags of a regex function from the argument at `index` ("i", "m", "s", "1").
bool regex_flags_arg(Executor& ex, Value* args, size_t count, size_t index, unsigned& flags) {
    flags = 0;
    if (index >= count || args[index].is_null()) return true;
    const char* s;
    size_t n;
    if (!text_of(ex, args[index], s, n)) return false;
    if (!regex_flags(s, n, flags)) {
        ex.status().fail(k_error_misuse, VDB_F("regex flags take only i, m, s, 1 (got '%.*s')"), static_cast<int>(n), s);
        return false;
    }
    return true;
}

bool constant_args(Expr* e, size_t from) {
    size_t i = 0;
    for (Expr* a = e->args; a; a = a->next, ++i) {
        if (i >= from && a->kind != e_literal && a->kind != e_param) return false;
    }
    return true;
}

bool regex_function(Executor& ex, Expr* e, Function f, Value* args, size_t count, Value& out) {
    size_t flag_index = f == fn_REGEXP_REPLACE ? 3 : (f == fn_REGEXP_SUBSTR || f == fn_REGEXP_EXTRACT) ? 3 : 2;
    unsigned flags;
    if (!regex_flags_arg(ex, args, count, flag_index, flags)) return false;
    const Regex* re = pattern_regex(ex, e, constant_args(e, 1), args[1], flags);
    if (!re) return false;
    const char* s;
    size_t n;
    if (!text_of(ex, args[0], s, n)) return false;
    size_t caps[2 * k_regex_max_groups];
    RegexWork& work = ex.regex_work();
    switch (f) {
        case fn_REGEXP_LIKE: {
            int r = re->search(s, n, 0, 0, 0, work);
            if (r < 0) return ex.memory();
            out = Value::boolean(r == 1);
            return true;
        }
        case fn_REGEXP_INSTR: {
            int r = re->search(s, n, 0, caps, 1, work);
            if (r < 0) return ex.memory();
            out = Value::integer(r == 1 ? static_cast<int64_t>(utf8_count(s, caps[0]) + 1) : 0);
            return true;
        }
        case fn_REGEXP_COUNT: {
            int64_t total = 0;
            for (size_t pos = 0; pos <= n;) {
                int r = re->search(s, n, pos, caps, 1, work);
                if (r < 0) return ex.memory();
                if (r == 0) break;
                ++total;
                if (caps[1] == caps[0]) {
                    if (caps[0] >= n) break;
                    uint32_t cp;
                    pos = caps[0] + utf8_decode(s + caps[0], n - caps[0], cp);
                } else {
                    pos = caps[1];
                }
            }
            out = Value::integer(total);
            return true;
        }
        case fn_REGEXP_SUBSTR:
        case fn_REGEXP_EXTRACT: {
            int64_t group = 0;
            if (count > 2 && !args[2].is_null() && !int_arg(ex, args[2], group)) group = 0;
            if (group < 0 || static_cast<size_t>(group) >= re->group_count()) {
                ex.status().fail(k_error_misuse, VDB_F("the pattern has no group %d"), static_cast<int>(group));
                return false;
            }
            int r = re->search(s, n, 0, caps, static_cast<size_t>(group) + 1, work);
            if (r < 0) return ex.memory();
            size_t a = caps[2 * group], b = caps[2 * group + 1];
            if (r == 0 || a == k_regex_unset || b == k_regex_unset) {
                out = Value::null();
                return true;
            }
            return copy_text(ex, s + a, b - a, out);
        }
        default: {  // REGEXP_REPLACE
            const char* rep;
            size_t rn;
            if (!text_of(ex, args[2], rep, rn)) return false;
            size_t groups = re->group_count();
            Text t(ex.allocator());
            size_t pos = 0;
            while (pos <= n) {
                int r = re->search(s, n, pos, caps, groups, work);
                if (r < 0) return ex.memory();
                if (r == 0) break;
                t.append(s + pos, caps[0] - pos);
                for (size_t i = 0; i < rn; ++i) {
                    char c = rep[i];
                    if ((c == '\\' || c == '$') && i + 1 < rn && is_digit(rep[i + 1])) {
                        size_t g = static_cast<size_t>(rep[++i] - '0');
                        if (g < groups && caps[2 * g] != k_regex_unset) t.append(s + caps[2 * g], caps[2 * g + 1] - caps[2 * g]);
                    } else if ((c == '\\' || c == '$') && i + 1 < rn && rep[i + 1] == c) {
                        t.append_char(c);
                        ++i;
                    } else {
                        t.append_char(c);
                    }
                }
                if (too_large(ex, t.size())) return false;
                if (caps[1] == caps[0]) {
                    if (caps[0] >= n) {
                        pos = n + 1;
                        break;
                    }
                    uint32_t cp;
                    size_t step = utf8_decode(s + caps[0], n - caps[0], cp);
                    t.append(s + caps[0], step);
                    pos = caps[0] + step;
                } else {
                    pos = caps[1];
                }
                if (flags & k_regex_first) break;
            }
            if (pos < n) t.append(s + pos, n - pos);
            return set_text(ex, t, out);
        }
    }
}

bool pad(Executor& ex, Value* args, size_t count, bool left, Value& out) {
    const char *s, *fill = " ";
    size_t n, fn = 1;
    int64_t width;
    if (!text_of(ex, args[0], s, n)) return false;
    if (!int_arg(ex, args[1], width)) {
        out = Value::null();
        return true;
    }
    if (count > 2 && !text_of(ex, args[2], fill, fn)) return false;
    if (width < 0) width = 0;
    if (too_large(ex, static_cast<size_t>(width) * 4)) return false;
    size_t chars = utf8_count(s, n);
    if (chars >= static_cast<size_t>(width)) return copy_text(ex, s, utf8_offset(s, n, static_cast<size_t>(width)), out);
    Text t(ex.allocator());
    size_t missing = static_cast<size_t>(width) - chars;
    Text filler(ex.allocator());
    if (fn) {
        size_t fill_chars = utf8_count(fill, fn);
        for (size_t i = 0; i < missing; ++i) {
            size_t at = utf8_offset(fill, fn, i % fill_chars);
            uint32_t cp;
            filler.append(fill + at, utf8_decode(fill + at, fn - at, cp));
        }
    }
    if (left) t.append(filler.c_str(), filler.size());
    t.append(s, n);
    if (!left) t.append(filler.c_str(), filler.size());
    return set_text(ex, t, out);
}

bool trim(Executor& ex, Value* args, size_t count, bool left, bool right, Value& out) {
    const char *s, *set = " \t\r\n";
    size_t n, sn = 4;
    if (!text_of(ex, args[0], s, n)) return false;
    if (count > 1 && !text_of(ex, args[1], set, sn)) return false;
    size_t start = 0, end = n;
    while (left && start < end) {
        uint32_t cp;
        size_t step = utf8_decode(s + start, end - start, cp);
        if (!contains_cp(set, sn, cp)) break;
        start += step;
    }
    while (right && end > start) {
        size_t at = end - 1;
        while (at > start && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) --at;
        uint32_t cp;
        utf8_decode(s + at, end - at, cp);
        if (!contains_cp(set, sn, cp)) break;
        end = at;
    }
    return copy_text(ex, s + start, end - start, out);
}

bool substring(Executor& ex, Value* args, size_t count, Value& out) {
    const char* s;
    size_t n;
    int64_t start, length = INT64_MAX;
    if (!text_of(ex, args[0], s, n)) return false;
    if (!int_arg(ex, args[1], start) || (count > 2 && !int_arg(ex, args[2], length))) {
        out = Value::null();
        return true;
    }
    int64_t chars = static_cast<int64_t>(utf8_count(s, n));
    int64_t begin;
    if (start > 0) {
        begin = start - 1;
    } else if (start == 0) {
        begin = 0;
        if (length != INT64_MAX) --length;
    } else {
        begin = chars + start;
        if (begin < 0) {
            if (length != INT64_MAX) length += begin;
            begin = 0;
        }
    }
    if (length < 0) length = 0;
    if (begin > chars) begin = chars;
    int64_t end = length > chars - begin ? chars : begin + length;
    size_t a = utf8_offset(s, n, static_cast<size_t>(begin));
    size_t b = utf8_offset(s, n, static_cast<size_t>(end));
    return copy_text(ex, s + a, b - a, out);
}

}  // namespace

bool like_match(const char* s, size_t n, const char* p, size_t m, uint32_t escape, bool icase) {
    const size_t k_none = static_cast<size_t>(-1);
    size_t si = 0, pi = 0, star_p = k_none, star_s = 0;
    while (si < n) {
        if (pi < m) {
            uint32_t pc;
            size_t at = pi;
            size_t pl = utf8_decode(p + at, m - at, pc);
            bool literal = false;
            if (pc == escape && at + pl < m) {
                at += pl;
                pl = utf8_decode(p + at, m - at, pc);
                literal = true;
            }
            if (!literal && pc == '%') {
                pi = at + pl;
                star_p = pi;
                star_s = si;
                continue;
            }
            uint32_t sc;
            size_t sl = utf8_decode(s + si, n - si, sc);
            if ((!literal && pc == '_') || same_cp(pc, sc, icase)) {
                pi = at + pl;
                si += sl;
                continue;
            }
        }
        if (star_p != k_none) {
            uint32_t sc;
            star_s += utf8_decode(s + star_s, n - star_s, sc);
            si = star_s;
            pi = star_p;
            continue;
        }
        return false;
    }
    while (pi < m) {
        uint32_t pc;
        size_t pl = utf8_decode(p + pi, m - pi, pc);
        if (pc != '%' || pc == escape) return false;
        pi += pl;
    }
    return true;
}

namespace {
// Matches a [class] at p[*pi]; returns false when it is no class (then '[' is literal).
bool glob_class(const char* p, size_t m, size_t& pi, uint32_t c, bool& in) {
    size_t at = pi + 1;
    bool negate = false;
    if (at < m && (p[at] == '^' || p[at] == '!')) {
        negate = true;
        ++at;
    }
    bool first = true;
    in = false;
    while (at < m && (p[at] != ']' || first)) {
        first = false;
        uint32_t low, high;
        at += utf8_decode(p + at, m - at, low);
        high = low;
        if (at + 1 < m && p[at] == '-' && p[at + 1] != ']') {
            ++at;
            at += utf8_decode(p + at, m - at, high);
        }
        if (c >= low && c <= high) in = true;
    }
    if (at >= m) return false;
    pi = at + 1;
    if (negate) in = !in;
    return true;
}
}  // namespace

bool glob_match(const char* s, size_t n, const char* p, size_t m) {
    const size_t k_none = static_cast<size_t>(-1);
    size_t si = 0, pi = 0, star_p = k_none, star_s = 0;
    while (si < n) {
        if (pi < m) {
            uint32_t pc;
            size_t pl = utf8_decode(p + pi, m - pi, pc);
            if (pc == '*') {
                pi += pl;
                star_p = pi;
                star_s = si;
                continue;
            }
            uint32_t sc;
            size_t sl = utf8_decode(s + si, n - si, sc);
            if (pc == '[') {
                size_t next = pi;
                bool in;
                if (glob_class(p, m, next, sc, in)) {
                    if (in) {
                        pi = next;
                        si += sl;
                        continue;
                    }
                } else if (sc == '[') {
                    pi += pl;
                    si += sl;
                    continue;
                }
            } else if (pc == '?' || pc == sc) {
                pi += pl;
                si += sl;
                continue;
            }
        }
        if (star_p != k_none) {
            uint32_t sc;
            star_s += utf8_decode(s + star_s, n - star_s, sc);
            si = star_s;
            pi = star_p;
            continue;
        }
        return false;
    }
    while (pi < m && p[pi] == '*') ++pi;
    return pi == m;
}

bool similar_to_regex(const char* p, size_t m, uint32_t escape, Text& out) {
    out.append("^(?:");
    for (size_t i = 0; i < m;) {
        uint32_t cp;
        size_t step = utf8_decode(p + i, m - i, cp);
        if (cp == escape && i + step < m) {
            i += step;
            step = utf8_decode(p + i, m - i, cp);
            if (cp < 0x80 && !is_alpha(static_cast<char>(cp)) && !is_digit(static_cast<char>(cp))) out.append_char('\\');
            out.append(p + i, step);
            i += step;
            continue;
        }
        if (cp == '%') out.append(".*");
        else if (cp == '_') out.append_char('.');
        else if (cp == '.' || cp == '^' || cp == '$' || cp == '\\') {
            out.append_char('\\');
            out.append_char(static_cast<char>(cp));
        } else {
            out.append(p + i, step);
        }
        i += step;
    }
    out.append(")$");
    return out.ok();
}

bool call_function(Executor& ex, Expr* e, Frame*, Value* args, size_t count, Value& out) {
    Function f = static_cast<Function>(e->function);
    const char *s, *t;
    size_t n, m;
    Number x, y;
    switch (f) {
        case fn_COALESCE:
        case fn_IFNULL:
        case fn_NVL:
            for (size_t i = 0; i < count; ++i) {
                if (!args[i].is_null()) {
                    out = args[i];
                    return true;
                }
            }
            out = Value::null();
            return true;
        case fn_NULLIF: {
            int c;
            out = (!args[0].is_null() && compare_sql(args[0], args[1], ex.decimal(), c) && c == 0) ? Value::null() : args[0];
            return true;
        }
        case fn_IIF: out = value_truth(args[0], ex.decimal()) == k_true ? args[1] : args[2]; return true;
        case fn_GREATEST:
        case fn_LEAST: {
            out = Value::null();
            for (size_t i = 0; i < count; ++i) {
                if (args[i].is_null()) continue;
                int c = out.is_null() ? 0 : compare_order(args[i], out, ex.decimal());
                if (out.is_null() || (f == fn_GREATEST ? c > 0 : c < 0)) out = args[i];
            }
            return true;
        }
        case fn_TYPEOF: {
            static const char k_null_name[] = "null";
            static const char k_integer_name[] = "integer";
            static const char k_real_name[] = "real";
            static const char k_text_name[] = "text";
            const char* name = args[0].type == k_null ? k_null_name : args[0].type == k_integer ? k_integer_name
                             : args[0].type == k_real ? k_real_name : k_text_name;
            out = Value::text(name, strlen(name));
            return true;
        }
        case fn_IS_NUMBER:
        case fn_ISNUMERIC: out = Value::boolean(value_number(args[0], ex.decimal(), x)); return true;
        case fn_LENGTH:
        case fn_LEN:
        case fn_CHAR_LENGTH:
        case fn_CHARACTER_LENGTH:
            if (!text_of(ex, args[0], s, n)) return false;
            out = Value::integer(static_cast<int64_t>(utf8_count(s, n)));
            return true;
        case fn_OCTET_LENGTH:
            if (!text_of(ex, args[0], s, n)) return false;
            out = Value::integer(static_cast<int64_t>(n));
            return true;
        case fn_UPPER:
        case fn_UCASE:
        case fn_LOWER:
        case fn_LCASE:
            if (!text_of(ex, args[0], s, n)) return false;
            return fold_text(ex, s, n, f == fn_UPPER || f == fn_UCASE, out);
        case fn_INITCAP: {
            if (!text_of(ex, args[0], s, n)) return false;
            char* p = ex.text(n * 2);
            if (!p) return false;
            size_t length = 0;
            bool word = false;
            for (size_t i = 0; i < n;) {
                uint32_t cp;
                i += utf8_decode(s + i, n - i, cp);
                bool letter = fold_lower(cp) != fold_upper(cp) || (cp < 0x80 && is_digit(static_cast<char>(cp)));
                length += utf8_encode(letter ? (word ? fold_lower(cp) : fold_upper(cp)) : cp, p + length);
                word = letter || cp == '\'';
            }
            p[length] = 0;
            out = Value::text(p, length);
            return true;
        }
        case fn_TRIM: return trim(ex, args, count, true, true, out);
        case fn_LTRIM: return trim(ex, args, count, true, false, out);
        case fn_RTRIM: return trim(ex, args, count, false, true, out);
        case fn_SUBSTR:
        case fn_SUBSTRING: return substring(ex, args, count, out);
        case fn_LEFT:
        case fn_RIGHT: {
            int64_t k;
            if (!text_of(ex, args[0], s, n)) return false;
            if (!int_arg(ex, args[1], k)) {
                out = Value::null();
                return true;
            }
            int64_t chars = static_cast<int64_t>(utf8_count(s, n));
            if (k < 0) k = chars + k < 0 ? 0 : chars + k;
            if (k > chars) k = chars;
            size_t at = f == fn_LEFT ? 0 : utf8_offset(s, n, static_cast<size_t>(chars - k));
            size_t end = f == fn_LEFT ? utf8_offset(s, n, static_cast<size_t>(k)) : n;
            return copy_text(ex, s + at, end - at, out);
        }
        case fn_REPLACE: {
            const char* to;
            size_t tn;
            if (!text_of(ex, args[0], s, n) || !text_of(ex, args[1], t, m) || !text_of(ex, args[2], to, tn)) return false;
            if (!m) return copy_text(ex, s, n, out);
            Text r(ex.allocator());
            size_t pos = 0;
            for (const char* hit; (hit = find_bytes(s + pos, n - pos, t, m)) != 0;) {
                size_t at = static_cast<size_t>(hit - s);
                r.append(s + pos, at - pos);
                r.append(to, tn);
                pos = at + m;
                if (too_large(ex, r.size())) return false;
            }
            r.append(s + pos, n - pos);
            return set_text(ex, r, out);
        }
        case fn_INSTR: {
            if (!text_of(ex, args[0], s, n) || !text_of(ex, args[1], t, m)) return false;
            const char* hit = find_bytes(s, n, t, m);
            out = Value::integer(hit ? static_cast<int64_t>(utf8_count(s, static_cast<size_t>(hit - s)) + 1) : 0);
            return true;
        }
        case fn_CONTAINS:
        case fn_STARTS_WITH:
        case fn_ENDS_WITH: {
            if (!text_of(ex, args[0], s, n) || !text_of(ex, args[1], t, m)) return false;
            bool r;
            if (f == fn_CONTAINS) r = find_bytes(s, n, t, m) != 0;
            else if (f == fn_STARTS_WITH) r = m <= n && memcmp(s, t, m) == 0;
            else r = m <= n && memcmp(s + n - m, t, m) == 0;
            out = Value::boolean(r);
            return true;
        }
        case fn_LPAD: return pad(ex, args, count, true, out);
        case fn_RPAD: return pad(ex, args, count, false, out);
        case fn_REVERSE: {
            if (!text_of(ex, args[0], s, n)) return false;
            char* p = ex.text(n);
            if (!p) return false;
            size_t end = n;
            for (size_t i = 0; i < n;) {
                uint32_t cp;
                size_t step = utf8_decode(s + i, n - i, cp);
                end -= step;
                memcpy(p + end, s + i, step);
                i += step;
            }
            out = Value::text(p, n);
            return true;
        }
        case fn_REPEAT: {
            int64_t times;
            if (!text_of(ex, args[0], s, n)) return false;
            if (!int_arg(ex, args[1], times) || times < 0) times = 0;
            if (n && too_large(ex, n * static_cast<size_t>(times > static_cast<int64_t>(VDB_MAX_TEXT) ? VDB_MAX_TEXT : times)))
                return false;
            char* p = ex.text(n * static_cast<size_t>(times));
            if (!p) return false;
            for (int64_t i = 0; i < times; ++i) memcpy(p + static_cast<size_t>(i) * n, s, n);
            out = Value::text(p, n * static_cast<size_t>(times));
            return true;
        }
        case fn_CONCAT:
        case fn_CONCAT_WS: {
            const char* sep = "";
            size_t sn = 0;
            size_t first = 0;
            if (f == fn_CONCAT_WS) {
                if (args[0].is_null()) {
                    out = Value::null();
                    return true;
                }
                if (!text_of(ex, args[0], sep, sn)) return false;
                first = 1;
            }
            Text r(ex.allocator());
            bool any = false;
            for (size_t i = first; i < count; ++i) {
                if (args[i].is_null()) continue;
                if (!text_of(ex, args[i], s, n)) return false;
                if (any) r.append(sep, sn);
                r.append(s, n);
                any = true;
                if (too_large(ex, r.size())) return false;
            }
            return set_text(ex, r, out);
        }
        case fn_SPLIT_PART: {
            int64_t index;
            if (!text_of(ex, args[0], s, n) || !text_of(ex, args[1], t, m)) return false;
            if (!int_arg(ex, args[2], index) || index == 0) {
                out = Value::null();
                return true;
            }
            size_t fields = 1;
            if (m) {
                for (const char* hit = find_bytes(s, n, t, m); hit; hit = find_bytes(hit + m, n - static_cast<size_t>(hit + m - s), t, m)) ++fields;
            }
            int64_t want = index > 0 ? index - 1 : static_cast<int64_t>(fields) + index;
            if (want < 0 || want >= static_cast<int64_t>(fields)) return copy_text(ex, "", 0, out);
            size_t pos = 0;
            for (int64_t k = 0; k < want; ++k) {
                const char* hit = find_bytes(s + pos, n - pos, t, m);
                pos = static_cast<size_t>(hit - s) + m;
            }
            const char* hit = m ? find_bytes(s + pos, n - pos, t, m) : 0;
            size_t end = hit ? static_cast<size_t>(hit - s) : n;
            return copy_text(ex, s + pos, end - pos, out);
        }
        case fn_ASCII: {
            if (!text_of(ex, args[0], s, n)) return false;
            uint32_t cp = 0;
            if (n) utf8_decode(s, n, cp);
            out = Value::integer(cp);
            return true;
        }
        case fn_CHAR:
        case fn_CHR: {
            char* p = ex.text(count * 4);
            if (!p) return false;
            size_t length = 0;
            for (size_t i = 0; i < count; ++i) {
                int64_t cp;
                if (int_arg(ex, args[i], cp) && cp >= 0 && cp <= 0x10FFFF) length += utf8_encode(static_cast<uint32_t>(cp), p + length);
            }
            p[length] = 0;
            out = Value::text(p, length);
            return true;
        }
        case fn_PRINTF:
        case fn_FORMAT: return printf_text(ex, args, count, out);
        case fn_REGEXP_LIKE:
        case fn_REGEXP_SUBSTR:
        case fn_REGEXP_EXTRACT:
        case fn_REGEXP_REPLACE:
        case fn_REGEXP_COUNT:
        case fn_REGEXP_INSTR: return regex_function(ex, e, f, args, count, out);
        case fn_PI: out = Value::real(k_pi); return true;
        default: break;
    }
    // Math: every argument must read as a number, or the result is NULL (spec 3.6).
    if (!number_arg(ex, args[0], x) || (count > 1 && !number_arg(ex, args[1], y))) {
        out = Value::null();
        return true;
    }
    double a = x.real();
    switch (f) {
        case fn_ABS:
            out = x.is_int && x.i != INT64_MIN ? Value::integer(x.i < 0 ? -x.i : x.i) : real_value(fabs(a));
            return true;
        case fn_SIGN: out = Value::integer(a > 0 ? 1 : a < 0 ? -1 : 0); return true;
        case fn_ROUND:
        case fn_TRUNC:
        case fn_TRUNCATE: {
            int64_t digits = 0;
            if (count > 1) digits = y.is_int ? y.i : static_cast<int64_t>(y.r);
            const int64_t k_max_digits = 30;
            if (digits > k_max_digits) digits = k_max_digits;
            if (digits < -k_max_digits) digits = -k_max_digits;
            if (x.is_int && digits >= 0) {
                out = Value::integer(x.i);
                return true;
            }
            double scale = pow(10.0, static_cast<double>(digits));
            double v = a * scale;
            v = f == fn_ROUND ? (v >= 0 ? floor(v + 0.5) : ceil(v - 0.5)) : (v >= 0 ? floor(v) : ceil(v));
            v /= scale;
            out = (x.is_int || digits <= 0) ? whole_value(v) : real_value(v);
            return true;
        }
        case fn_FLOOR: out = x.is_int ? Value::integer(x.i) : whole_value(floor(a)); return true;
        case fn_CEIL:
        case fn_CEILING: out = x.is_int ? Value::integer(x.i) : whole_value(ceil(a)); return true;
        case fn_MOD:
            if (x.is_int && y.is_int) {
                out = y.i == 0 ? Value::null() : Value::integer(y.i == -1 ? 0 : x.i % y.i);
            } else {
                out = y.real() == 0 ? Value::null() : real_value(fmod(a, y.real()));
            }
            return true;
        case fn_POWER:
        case fn_POW: {
            double r = pow(a, y.real());
            int64_t i;
            out = (x.is_int && y.is_int && y.i >= 0 && real_is_integral(r, i) && fabs(r) < 9.0e15) ? Value::integer(i) : real_value(r);
            return true;
        }
        case fn_SQRT: out = a < 0 ? Value::null() : real_value(sqrt(a)); return true;
        case fn_EXP: out = real_value(exp(a)); return true;
        case fn_LN: out = a <= 0 ? Value::null() : real_value(log(a)); return true;
        case fn_LOG:
            if (count > 1) {
                double v = y.real();
                out = (a <= 0 || a == 1 || v <= 0) ? Value::null() : real_value(log(v) / log(a));
            } else {
                out = a <= 0 ? Value::null() : real_value(log10(a));
            }
            return true;
        case fn_LOG10: out = a <= 0 ? Value::null() : real_value(log10(a)); return true;
        case fn_LOG2: out = a <= 0 ? Value::null() : real_value(log(a) / log(2.0)); return true;
        case fn_SIN: out = real_value(sin(a)); return true;
        case fn_COS: out = real_value(cos(a)); return true;
        case fn_TAN: out = real_value(tan(a)); return true;
        case fn_ASIN: out = (a < -1 || a > 1) ? Value::null() : real_value(asin(a)); return true;
        case fn_ACOS: out = (a < -1 || a > 1) ? Value::null() : real_value(acos(a)); return true;
        case fn_ATAN: out = real_value(atan(a)); return true;
        case fn_ATAN2: out = real_value(atan2(a, y.real())); return true;
        case fn_DEGREES: out = real_value(a * 180.0 / k_pi); return true;
        case fn_RADIANS: out = real_value(a * k_pi / 180.0); return true;
        default:
            ex.status().fail(k_error_misuse, VDB_F("aggregate function used as a value outside a query"));
            return false;
    }
}

}  // namespace vdb
