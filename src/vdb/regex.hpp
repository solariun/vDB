// Regular expressions on a Pike VM: linear time in text x pattern, never exponential (spec 3.5, A6).
#ifndef VDB_REGEX_HPP
#define VDB_REGEX_HPP

#include "text.hpp"
#include "vec.hpp"

namespace vdb {

enum RegexFlag {
    k_regex_icase = 1,      // i
    k_regex_multiline = 2,  // m: ^ and $ also at line ends
    k_regex_dotall = 4,     // s: . matches newline
    k_regex_first = 8       // 1: REGEXP_REPLACE replaces only the first match
};

const size_t k_regex_max_groups = 10;  // group 0 and \1..\9
const size_t k_regex_unset = static_cast<size_t>(-1);

// Parses "ims1" into flags; false on an unknown letter.
bool regex_flags(const char* text, size_t length, unsigned& flags);

// Thread lists reused across matches (one per query).
class RegexWork {
public:
    explicit RegexWork(Allocator& allocator);
    bool prepare(size_t program, size_t slots);

    Vec<uint16_t> pcs_a;
    Vec<uint16_t> pcs_b;
    Vec<uint32_t> caps_a;
    Vec<uint32_t> caps_b;
    Vec<uint16_t> mark;
    Vec<uint32_t> stack;
    Vec<uint32_t> current;
    uint16_t generation;
};

class Regex {
public:
    // Compiles into `pool`; the result lives as long as the pool. Returns 0 and fills `status` on error.
    static Regex* compile(Pool& pool, const char* pattern, size_t length, unsigned flags, Status& status);

    // Leftmost-first search from byte `from`. `captures` gets 2 * `groups` offsets (k_regex_unset when a group
    // did not take part). Returns 1 on a match, 0 on none, -1 when memory ran out.
    int search(const char* text, size_t length, size_t from, size_t* captures, size_t groups, RegexWork& work) const;

    size_t group_count() const { return groups_; }  // including group 0
    unsigned flags() const { return flags_; }

    struct Inst {
        uint8_t op;
        uint8_t arg;
        uint16_t x;
        uint16_t y;
        uint32_t c;
    };
    struct Range {
        uint32_t low;
        uint32_t high;
    };
    struct Class {
        const Range* ranges;
        uint16_t count;
        uint8_t negate;
        uint8_t sets;  // bit flags of \d \D \w \W \s \S inside the class
    };

private:
    bool class_has(const Class& c, uint32_t cp) const;
    bool step_matches(const Inst& inst, uint32_t cp) const;
    bool assertion_holds(const Inst& inst, const char* text, size_t length, size_t at) const;

    const Inst* program_;
    size_t size_;
    const Class* classes_;
    size_t groups_;
    unsigned flags_;

    friend class RegexCompiler;
};

}  // namespace vdb

#endif
