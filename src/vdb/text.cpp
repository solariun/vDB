#include "text.hpp"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

namespace vdb {

// ── Text ─────────────────────────────────────────────────────────────────────

Text::Text(Allocator& allocator) : allocator_(&allocator), data_(0), size_(0), capacity_(0), failed_(false) {}

Text::~Text() {
    if (data_) allocator_->release(data_);
}

Text::Text(Text&& other)
    : allocator_(other.allocator_),
      data_(other.data_),
      size_(other.size_),
      capacity_(other.capacity_),
      failed_(other.failed_) {
    other.data_ = 0;
    other.size_ = other.capacity_ = 0;
}

bool Text::reserve(size_t capacity) {
    if (failed_) return false;
    if (capacity + 1 <= capacity_) return true;
    size_t grown = capacity_ ? capacity_ * 2 : 32;
    if (grown < capacity + 1) grown = capacity + 1;
    if (grown < capacity) {
        failed_ = true;
        return false;
    }
    char* data = static_cast<char*>(allocator_->allocate(grown));
    if (!data && grown > capacity + 1) {
        grown = capacity + 1;
        data = static_cast<char*>(allocator_->allocate(grown));
    }
    if (!data) {
        failed_ = true;
        return false;
    }
    if (data_) {
        memcpy(data, data_, size_ + 1);
        allocator_->release(data_);
    } else {
        data[0] = 0;
    }
    data_ = data;
    capacity_ = grown;
    return true;
}

bool Text::append(const char* text, size_t length) {
    if (!reserve(size_ + length)) return false;
    if (length) memcpy(data_ + size_, text, length);
    size_ += length;
    data_[size_] = 0;
    return true;
}

bool Text::append(const char* text) { return append(text, strlen(text)); }

bool Text::append_char(char c) { return append(&c, 1); }

bool Text::append_flash(const char* flash_text) {
    for (const char* p = flash_text;; ++p) {
        char c = flash_char(p);
        if (!c) return ok();
        if (!append_char(c)) return false;
    }
}

bool Text::append_int(int64_t value) {
    char buffer[k_number_text];
    return append(buffer, format_int(value, buffer));
}

bool Text::append_real(double value, char decimal, int digits) {
    char buffer[k_number_text];
    return append(buffer, format_real(value, decimal, digits, buffer));
}

void Text::clear() { truncate(0); }

void Text::truncate(size_t length) {
    if (length < size_) {
        size_ = length;
        data_[size_] = 0;
    }
}

// ── Status ───────────────────────────────────────────────────────────────────

namespace {

void put(char* out, size_t& at, char c) {
    if (at + 1 < VDB_ERROR_SIZE) out[at++] = c;
}

void put_text(char* out, size_t& at, const char* text, size_t length) {
    for (size_t i = 0; i < length && text[i]; ++i) put(out, at, text[i]);
}

void vformat(char* out, size_t at, const char* format, va_list args) {
    for (const char* p = format;; ++p) {
        char c = flash_char(p);
        if (!c) break;
        if (c != '%') {
            put(out, at, c);
            continue;
        }
        char spec = flash_char(++p);
        char buffer[k_number_text];
        if (spec == 's') {
            const char* text = va_arg(args, const char*);
            put_text(out, at, text ? text : "(null)", static_cast<size_t>(-1));
        } else if (spec == '.' && flash_char(p + 1) == '*' && flash_char(p + 2) == 's') {
            p += 2;
            int length = va_arg(args, int);
            const char* text = va_arg(args, const char*);
            put_text(out, at, text, length < 0 ? 0 : static_cast<size_t>(length));
        } else if (spec == 'd') {
            put_text(out, at, buffer, format_int(va_arg(args, int), buffer));
        } else if (spec == 'l' && flash_char(p + 1) == 'd') {
            ++p;
            put_text(out, at, buffer, format_int(va_arg(args, long), buffer));
        } else if (spec == 'u') {
            put_text(out, at, buffer, format_int(static_cast<int64_t>(va_arg(args, unsigned)), buffer));
        } else if (spec == 'c') {
            put(out, at, static_cast<char>(va_arg(args, int)));
        } else if (spec == '%') {
            put(out, at, '%');
        } else if (!spec) {
            break;
        }
    }
    out[at] = 0;
}

}  // namespace

Status& Status::fail(int code, const char* flash_format, ...) {
    code_ = code;
    va_list args;
    va_start(args, flash_format);
    vformat(message_, 0, flash_format, args);
    va_end(args);
    return *this;
}

Status& Status::add(const char* flash_format, ...) {
    va_list args;
    va_start(args, flash_format);
    vformat(message_, strlen(message_), flash_format, args);
    va_end(args);
    return *this;
}

Status& Status::fail_memory() { return fail(k_error_memory, VDB_F("out of memory")); }

// ── numbers ──────────────────────────────────────────────────────────────────

bool parse_number(const char* text, size_t length, char decimal, Number& out) {
    const size_t k_max_digits = 48;
    size_t i = 0;
    while (i < length && is_space(text[i])) ++i;
    char buffer[k_max_digits + 16];
    size_t b = 0;
    bool negative = false;
    if (i < length && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    if (negative) buffer[b++] = '-';
    size_t digits = 0;
    bool overflow = false;
    uint64_t magnitude = 0;
    const uint64_t k_limit = static_cast<uint64_t>(INT64_MAX);
    while (i < length && is_digit(text[i])) {
        uint64_t d = static_cast<uint64_t>(text[i] - '0');
        if (magnitude > (k_limit + 1 - d) / 10) overflow = true;
        if (!overflow) magnitude = magnitude * 10 + d;
        if (b >= k_max_digits) return false;
        buffer[b++] = text[i];
        ++digits;
        ++i;
    }
    bool is_int = true;
    if (i < length && text[i] == decimal) {
        is_int = false;
        ++i;
        buffer[b++] = '.';
        while (i < length && is_digit(text[i])) {
            if (b >= k_max_digits) return false;
            buffer[b++] = text[i];
            ++digits;
            ++i;
        }
    }
    if (!digits) return false;
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        size_t j = i + 1;
        char exponent[8];
        size_t e = 0;
        if (j < length && (text[j] == '+' || text[j] == '-')) exponent[e++] = text[j++];
        size_t exponent_digits = 0;
        while (j < length && is_digit(text[j])) {
            if (e < sizeof(exponent) - 1) exponent[e++] = text[j];
            ++exponent_digits;
            ++j;
        }
        if (!exponent_digits) return false;
        is_int = false;
        buffer[b++] = 'e';
        memcpy(buffer + b, exponent, e);
        b += e;
        i = j;
    }
    while (i < length && is_space(text[i])) ++i;
    if (i != length) return false;
    buffer[b] = 0;
    if (is_int && !overflow) {
        out.is_int = true;
        if (negative) {
            out.i = magnitude > k_limit ? INT64_MIN : -static_cast<int64_t>(magnitude);
        } else {
            if (magnitude > k_limit) {
                out.is_int = false;
                out.r = static_cast<double>(magnitude);
                return true;
            }
            out.i = static_cast<int64_t>(magnitude);
        }
        out.r = static_cast<double>(out.i);
        return true;
    }
    out.is_int = false;
    out.r = strtod(buffer, 0);
    out.i = 0;
    return true;
}

size_t format_int(int64_t value, char* out) {
    char reversed[24];
    size_t n = 0;
    uint64_t magnitude = value < 0 ? static_cast<uint64_t>(-(value + 1)) + 1 : static_cast<uint64_t>(value);
    do {
        reversed[n++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    size_t length = 0;
    if (value < 0) out[length++] = '-';
    while (n) out[length++] = reversed[--n];
    out[length] = 0;
    return length;
}

bool real_is_integral(double value, int64_t& out) {
    const double k_int64_bound = 9223372036854775808.0;  // 2^63, exact in float and double
    if (!(value >= -k_int64_bound && value < k_int64_bound)) return false;
    double whole = floor(value);
    if (whole != value) return false;
    out = static_cast<int64_t>(whole);
    return true;
}

namespace {
size_t format_real_adjusted(double value, char decimal, int digits, int adjust, char* out) {
    size_t length = 0;
    if (value != value) {
        memcpy(out, "nan", 4);
        return 3;
    }
    if (value < 0) {
        out[length++] = '-';
        value = -value;
    }
    if (!finite(value)) {
        memcpy(out + length, "inf", 4);
        return length + 3;
    }
    if (value == 0) {
        out[0] = '0';
        out[1] = 0;
        return 1;
    }
    if (digits < 1) digits = 1;
    if (digits > 17) digits = 17;
    int exponent = static_cast<int>(floor(log10(value)));
    double top = pow(10.0, digits);
    double scaled = 0;
    const int k_max_power = 300;  // 10^300 fits a double; larger shifts go in two steps
    for (int attempt = 0; attempt < 3; ++attempt) {
        int shift = digits - 1 - exponent;
        double v = value;
        while (shift > k_max_power) {
            v *= pow(10.0, k_max_power);
            shift -= k_max_power;
        }
        while (shift < -k_max_power) {
            v /= pow(10.0, k_max_power);
            shift += k_max_power;
        }
        scaled = shift >= 0 ? v * pow(10.0, shift) : v / pow(10.0, -shift);
        scaled = floor(scaled + 0.5);
        if (scaled >= top) {
            ++exponent;
        } else if (scaled < top / 10) {
            --exponent;
        } else {
            break;
        }
    }
    char mantissa[24];
    uint64_t m = static_cast<uint64_t>(scaled + adjust);
    for (int i = digits - 1; i >= 0; --i) {
        mantissa[i] = static_cast<char>('0' + m % 10);
        m /= 10;
    }
    int significant = digits;
    while (significant > 1 && mantissa[significant - 1] == '0') --significant;
    const int k_plain_low = -5;
    if (exponent >= k_plain_low && exponent < digits) {
        if (exponent < 0) {
            out[length++] = '0';
            out[length++] = decimal;
            for (int i = -1; i > exponent; --i) out[length++] = '0';
            for (int i = 0; i < significant; ++i) out[length++] = mantissa[i];
        } else {
            for (int i = 0; i <= exponent; ++i) out[length++] = i < significant ? mantissa[i] : '0';
            if (significant > exponent + 1) {
                out[length++] = decimal;
                for (int i = exponent + 1; i < significant; ++i) out[length++] = mantissa[i];
            }
        }
    } else {
        out[length++] = mantissa[0];
        if (significant > 1) {
            out[length++] = decimal;
            for (int i = 1; i < significant; ++i) out[length++] = mantissa[i];
        }
        out[length++] = 'e';
        out[length++] = exponent < 0 ? '-' : '+';
        length += format_int(exponent < 0 ? -exponent : exponent, out + length);
    }
    out[length] = 0;
    return length;
}

}  // namespace

size_t format_real(double value, char decimal, int digits, char* out) {
    if (digits < k_real_exact_digits) return format_real_adjusted(value, decimal, digits, 0, out);
    // Shortest text that reads back as the same double: scaling may miss the last digit by one or two.
    const int k_adjustments[] = {0, -1, 1, -2, 2};
    for (int d = k_real_digits; d <= k_real_exact_digits; ++d) {
        for (size_t a = 0; a < sizeof(k_adjustments) / sizeof(k_adjustments[0]); ++a) {
            size_t n = format_real_adjusted(value, decimal, d, k_adjustments[a], out);
            Number back;
            if (parse_number(out, n, decimal, back) && back.real() == value) return n;
        }
    }
    return format_real_adjusted(value, decimal, k_real_exact_digits, 0, out);
}

size_t format_number(const Number& number, char decimal, char* out) {
    return number.is_int ? format_int(number.i, out) : format_real(number.r, decimal, k_real_digits, out);
}

// ── UTF-8 and case ───────────────────────────────────────────────────────────

size_t utf8_decode(const char* text, size_t length, uint32_t& code_point) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(text);
    unsigned char c = s[0];
    size_t need = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : 4;
    if (need == 0 || need == 4 || need >= length) {
        code_point = c;
        return 1;
    }
    uint32_t cp = c & (0x3F >> need);
    for (size_t i = 1; i <= need; ++i) {
        if ((s[i] & 0xC0) != 0x80) {
            code_point = c;
            return 1;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    code_point = cp;
    return need + 1;
}

size_t utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

size_t utf8_count(const char* text, size_t length) {
    size_t count = 0;
    for (size_t i = 0; i < length;) {
        uint32_t cp;
        i += utf8_decode(text + i, length - i, cp);
        ++count;
    }
    return count;
}

size_t utf8_offset(const char* text, size_t length, size_t characters) {
    size_t i = 0;
    while (characters && i < length) {
        uint32_t cp;
        i += utf8_decode(text + i, length - i, cp);
        --characters;
    }
    return i;
}

namespace {
// Latin Extended-A pairs: true when cp is the upper case of the pair (cp, cp + 1).
bool extended_a_upper(uint32_t cp) {
    if (cp >= 0x100 && cp <= 0x137) return (cp & 1) == 0;
    if (cp >= 0x139 && cp <= 0x148) return (cp & 1) == 1;
    if (cp >= 0x14A && cp <= 0x177) return (cp & 1) == 0;
    if (cp >= 0x179 && cp <= 0x17E) return (cp & 1) == 1;
    return false;
}
}  // namespace

uint32_t fold_lower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp < 0xC0) return cp;
    if (cp <= 0xDE && cp != 0xD7) return cp + 32;
    if (extended_a_upper(cp)) return cp + 1;
    if (cp == 0x178) return 0xFF;
    return cp;
}

uint32_t fold_upper(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp < 0xE0) return cp;
    if (cp <= 0xFE && cp != 0xF7) return cp - 32;
    if (cp == 0xFF) return 0x178;
    if (cp > 0x100 && extended_a_upper(cp - 1)) return cp - 1;
    return cp;
}

bool names_equal(const char* a, const char* b) {
    for (;; ++a, ++b) {
        if (ascii_lower(*a) != ascii_lower(*b)) return false;
        if (!*a) return true;
    }
}

bool names_equal(const char* a, size_t a_length, const char* b) {
    for (size_t i = 0; i < a_length; ++i) {
        if (!b[i] || ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return b[a_length] == 0;
}

bool flash_names_equal(const char* text, size_t length, const char* flash_name) {
    for (size_t i = 0; i < length; ++i) {
        char c = flash_char(flash_name + i);
        if (!c || ascii_lower(text[i]) != ascii_lower(c)) return false;
    }
    return flash_char(flash_name + length) == 0;
}

size_t flash_length(const char* flash_text) {
    size_t n = 0;
    while (flash_char(flash_text + n)) ++n;
    return n;
}

int compare_bytes(const char* a, size_t a_length, const char* b, size_t b_length) {
    size_t n = a_length < b_length ? a_length : b_length;
    int c = n ? memcmp(a, b, n) : 0;
    if (c) return c < 0 ? -1 : 1;
    return a_length < b_length ? -1 : a_length > b_length ? 1 : 0;
}

size_t edit_distance(const char* a, const char* b, size_t limit) {
    const size_t k_max = 32;
    size_t la = strlen(a), lb = strlen(b);
    if (la > k_max || lb > k_max) return limit;
    size_t row[k_max + 1];
    for (size_t j = 0; j <= lb; ++j) row[j] = j;
    for (size_t i = 1; i <= la; ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        size_t best = row[0];
        for (size_t j = 1; j <= lb; ++j) {
            size_t above = row[j];
            size_t cost = ascii_lower(a[i - 1]) == ascii_lower(b[j - 1]) ? 0 : 1;
            size_t value = diagonal + cost;
            if (row[j - 1] + 1 < value) value = row[j - 1] + 1;
            if (above + 1 < value) value = above + 1;
            row[j] = value;
            diagonal = above;
            if (value < best) best = value;
        }
        if (best >= limit) return limit;
    }
    return row[lb] < limit ? row[lb] : limit;
}

// ── varint ───────────────────────────────────────────────────────────────────

size_t varint_size(uint32_t value) {
    size_t n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

uint8_t* varint_write(uint8_t* out, uint32_t value) {
    while (value >= 0x80) {
        *out++ = static_cast<uint8_t>(value | 0x80);
        value >>= 7;
    }
    *out++ = static_cast<uint8_t>(value);
    return out;
}

const uint8_t* varint_read(const uint8_t* in, uint32_t& value) {
    uint32_t result = 0;
    unsigned shift = 0;
    for (;;) {
        uint8_t byte = *in++;
        result |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }
    value = result;
    return in;
}

}  // namespace vdb
