// Text buffers, status messages, numbers as text, UTF-8 and case folding (spec 2.2, 2.5, A12).
#ifndef VDB_TEXT_HPP
#define VDB_TEXT_HPP

#include "memory.hpp"

namespace vdb {

// A growable NUL-terminated byte buffer. A failed append is sticky: check ok() once at the end.
class Text {
public:
    explicit Text(Allocator& allocator);
    ~Text();
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;
    Text(Text&& other);

    bool append(const char* text, size_t length);
    bool append(const char* text);
    bool append_char(char c);
    bool append_flash(const char* flash_text);
    bool append_int(int64_t value);
    bool append_real(double value, char decimal, int digits);
    bool reserve(size_t capacity);
    void clear();
    void truncate(size_t length);

    const char* c_str() const { return data_ ? data_ : ""; }
    char* data() { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    bool ok() const { return !failed_; }
    Allocator& allocator() const { return *allocator_; }

private:
    Allocator* allocator_;
    char* data_;
    size_t size_;
    size_t capacity_;
    bool failed_;
};

enum ErrorCode {
    k_ok = 0,
    k_error_syntax,
    k_error_name,
    k_error_memory,
    k_error_runtime,
    k_error_csv,
    k_error_misuse,
    k_error_limit,
    k_error_unsupported,
    k_error_io
};

// An outcome with a fixed-size message: reporting never allocates (it must work when memory is gone).
class Status {
public:
    Status() : code_(k_ok) { message_[0] = 0; }
    bool ok() const { return code_ == k_ok; }
    int code() const { return code_; }
    const char* message() const { return message_; }
    explicit operator bool() const { return ok(); }

    // printf-like with a format in flash: %s, %.*s, %d, %ld, %u, %c and %%.
    Status& fail(int code, const char* flash_format, ...);
    Status& add(const char* flash_format, ...);  // appends to the message
    Status& fail_memory();
    void clear() {
        code_ = k_ok;
        message_[0] = 0;
    }

private:
    int code_;
    char message_[VDB_ERROR_SIZE];
};

// ── numbers ──────────────────────────────────────────────────────────────────

const size_t k_number_text = 40;  // room for any formatted number
#if defined(__AVR__)
const int k_real_digits = 6;
#else
const int k_real_digits = 15;
#endif
const int k_real_exact_digits = k_real_digits + 2;  // round-trips a double

struct Number {
    bool is_int;
    int64_t i;
    double r;
    double real() const { return is_int ? static_cast<double>(i) : r; }
};

// Reads text as a number (spec 2.2): optional spaces, sign, digits, decimal separator, exponent.
bool parse_number(const char* text, size_t length, char decimal, Number& out);
size_t format_int(int64_t value, char* out);
size_t format_real(double value, char decimal, int digits, char* out);
size_t format_number(const Number& number, char decimal, char* out);
bool real_is_integral(double value, int64_t& out);
// Not NaN and not infinite, on every double width (AVR's is 32-bit).
inline bool finite(double v) { return v - v == 0; }

// ── UTF-8 and case ───────────────────────────────────────────────────────────

size_t utf8_decode(const char* text, size_t length, uint32_t& code_point);  // >= 1 byte; invalid bytes pass alone
size_t utf8_encode(uint32_t code_point, char* out);                         // 1..4 bytes
size_t utf8_count(const char* text, size_t length);
size_t utf8_offset(const char* text, size_t length, size_t characters);  // byte offset of a character index
uint32_t fold_lower(uint32_t code_point);                                // ASCII, Latin-1, Latin Extended-A
uint32_t fold_upper(uint32_t code_point);

inline char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
inline char ascii_upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool names_equal(const char* a, const char* b);                    // ASCII case-insensitive
bool names_equal(const char* a, size_t a_length, const char* b);  // b NUL-terminated
bool flash_names_equal(const char* text, size_t length, const char* flash_name);
size_t flash_length(const char* flash_text);
int compare_bytes(const char* a, size_t a_length, const char* b, size_t b_length);
size_t edit_distance(const char* a, const char* b, size_t limit);  // ASCII case-insensitive, capped at limit

// ── varint (records) ─────────────────────────────────────────────────────────

size_t varint_size(uint32_t value);
uint8_t* varint_write(uint8_t* out, uint32_t value);
const uint8_t* varint_read(const uint8_t* in, uint32_t& value);

}  // namespace vdb

#endif
