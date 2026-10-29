#include "regex.hpp"

#include <string.h>

namespace vdb {

namespace {

enum Op { k_op_char, k_op_any, k_op_class, k_op_match, k_op_jmp, k_op_split, k_op_save, k_op_assert };
enum Assertion { k_bol, k_eol, k_word_boundary, k_not_word_boundary };
enum Set { k_set_digit = 1, k_set_not_digit = 2, k_set_word = 4, k_set_not_word = 8, k_set_space = 16, k_set_not_space = 32 };
enum NodeKind { k_empty, k_char, k_any, k_class, k_cat, k_alt, k_repeat, k_group, k_assert };

const int k_unbounded = -1;
const int k_max_repeat = 1000;
const uint32_t k_restore = 0x80000000u;

struct Node {
    uint8_t kind;
    uint8_t greedy;
    uint16_t index;  // class index, group number or assertion
    uint32_t c;
    int min;
    int max;
    Node* a;
    Node* b;
};

bool is_word_cp(uint32_t cp) {
    return (cp < 0x80 && (is_alpha(static_cast<char>(cp)) || is_digit(static_cast<char>(cp)) || cp == '_')) ||
           (cp >= 0xC0 && cp != 0xD7 && cp != 0xF7);
}
bool is_space_cp(uint32_t cp) { return cp < 0x80 && is_space(static_cast<char>(cp)); }
bool is_digit_cp(uint32_t cp) { return cp >= '0' && cp <= '9'; }

uint32_t previous_cp(const char* text, size_t at) {
    if (at == 0) return 0;
    size_t start = at - 1;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80 && at - start < 4) --start;
    uint32_t cp;
    utf8_decode(text + start, at - start, cp);
    return cp;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool regex_flags(const char* text, size_t length, unsigned& flags) {
    flags = 0;
    for (size_t i = 0; i < length; ++i) {
        switch (text[i]) {
            case 'i': flags |= k_regex_icase; break;
            case 'm': flags |= k_regex_multiline; break;
            case 's': flags |= k_regex_dotall; break;
            case '1': flags |= k_regex_first; break;
            case 'g': break;  // global is the default (spec A13)
            case 'c': flags &= ~static_cast<unsigned>(k_regex_icase); break;
            default: return false;
        }
    }
    return true;
}

// ── compiler ─────────────────────────────────────────────────────────────────

class RegexCompiler {
public:
    RegexCompiler(Pool& pool, const char* pattern, size_t length, unsigned flags, Status& status)
        : pool_(pool),
          p_(pattern),
          n_(length),
          at_(0),
          flags_(flags),
          status_(status),
          groups_(1),
          depth_(0),
          code_(pool.allocator()),
          classes_(pool.allocator()),
          ranges_(pool.allocator()) {}

    Regex* run() {
        Node* root = parse_alt();
        if (!root) return 0;
        if (at_ < n_) return fail(VDB_F("regex: unmatched ')'"));
        emit(k_op_save, 0, 0, 0, 0);
        if (!gen(root)) return 0;
        emit(k_op_save, 0, 1, 0, 0);
        emit(k_op_match, 0, 0, 0, 0);
        if (!code_ok()) return 0;
        Regex* regex = make<Regex>(pool_);
        Regex::Inst* program = static_cast<Regex::Inst*>(pool_.allocate(code_.size() * sizeof(Regex::Inst)));
        Regex::Class* classes = static_cast<Regex::Class*>(pool_.allocate((classes_.size() + 1) * sizeof(Regex::Class)));
        if (!regex || !program || !classes) return memory();
        memcpy(program, code_.data(), code_.size() * sizeof(Regex::Inst));
        for (size_t i = 0; i < classes_.size(); ++i) classes[i] = classes_[i];
        regex->program_ = program;
        regex->size_ = code_.size();
        regex->classes_ = classes;
        regex->groups_ = groups_;
        regex->flags_ = flags_;
        return regex;
    }

private:
    decltype(nullptr) fail(const char* flash_message) {
        if (status_.ok()) status_.fail(k_error_runtime, flash_message);
        return nullptr;
    }
    decltype(nullptr) memory() {
        if (status_.ok()) status_.fail_memory();
        return nullptr;
    }
    bool code_ok() {
        if (code_.size() > VDB_REGEX_MAX_PROGRAM) {
            fail(VDB_F("regex: pattern too large"));
            return false;
        }
        return status_.ok();
    }

    Node* node(uint8_t kind) {
        Node* n = make<Node>(pool_);
        if (!n) {
            memory();
            return 0;
        }
        n->kind = kind;
        n->greedy = 1;
        return n;
    }

    bool more() const { return at_ < n_; }
    char peek() const { return p_[at_]; }

    Node* parse_alt() {
        if (++depth_ > VDB_MAX_DEPTH) return fail(VDB_F("regex: nested too deeply"));
        Node* left = parse_cat();
        while (left && more() && peek() == '|') {
            ++at_;
            Node* right = parse_cat();
            if (!right) return 0;
            Node* alt = node(k_alt);
            if (!alt) return 0;
            alt->a = left;
            alt->b = right;
            left = alt;
        }
        --depth_;
        return left;
    }

    Node* parse_cat() {
        Node* result = node(k_empty);
        while (result && more() && peek() != '|' && peek() != ')') {
            Node* item = parse_repeat();
            if (!item) return 0;
            if (result->kind == k_empty) {
                result = item;
            } else {
                Node* cat = node(k_cat);
                if (!cat) return 0;
                cat->a = result;
                cat->b = item;
                result = cat;
            }
        }
        return result;
    }

    bool read_count(int& value) {
        size_t start = at_;
        long v = 0;
        while (more() && is_digit(peek())) {
            v = v * 10 + (peek() - '0');
            if (v > k_max_repeat) v = k_max_repeat + 1;
            ++at_;
        }
        value = static_cast<int>(v);
        return at_ > start;
    }

    Node* parse_repeat() {
        Node* atom = parse_atom();
        while (atom && more()) {
            char c = peek();
            int min = 0, max = 0;
            if (c == '*') {
                min = 0;
                max = k_unbounded;
                ++at_;
            } else if (c == '+') {
                min = 1;
                max = k_unbounded;
                ++at_;
            } else if (c == '?') {
                min = 0;
                max = 1;
                ++at_;
            } else if (c == '{') {
                size_t save = at_;
                ++at_;
                if (!read_count(min)) {
                    at_ = save;
                    break;
                }
                max = min;
                if (more() && peek() == ',') {
                    ++at_;
                    if (!read_count(max)) max = k_unbounded;
                }
                if (!more() || peek() != '}') {
                    at_ = save;
                    break;
                }
                ++at_;
                if (min > k_max_repeat || max > k_max_repeat) return fail(VDB_F("regex: repeat count above 1000"));
                if (max != k_unbounded && max < min) return fail(VDB_F("regex: bad repeat {m,n}"));
            } else {
                break;
            }
            if (atom->kind == k_assert) return fail(VDB_F("regex: nothing to repeat"));
            Node* rep = node(k_repeat);
            if (!rep) return 0;
            rep->a = atom;
            rep->min = min;
            rep->max = max;
            if (more() && peek() == '?') {
                rep->greedy = 0;
                ++at_;
            }
            atom = rep;
        }
        return atom;
    }

    Node* char_node(uint32_t cp) {
        Node* n = node(k_char);
        if (n) n->c = (flags_ & k_regex_icase) ? fold_lower(cp) : cp;
        return n;
    }

    Node* set_node(uint8_t sets) {
        size_t first = ranges_.size();
        Node* n = node(k_class);
        if (!n) return 0;
        Regex::Class c;
        c.ranges = 0;
        c.count = 0;
        c.negate = 0;
        c.sets = sets;
        return finish_class(n, c, first);
    }

    Node* finish_class(Node* n, Regex::Class& c, size_t first) {
        size_t count = ranges_.size() - first;
        Regex::Range* ranges = static_cast<Regex::Range*>(pool_.allocate((count + 1) * sizeof(Regex::Range)));
        if (!ranges) return memory();
        for (size_t i = 0; i < count; ++i) ranges[i] = ranges_[first + i];
        ranges_.resize(first);
        c.ranges = ranges;
        c.count = static_cast<uint16_t>(count);
        n->index = static_cast<uint16_t>(classes_.size());
        if (classes_.size() >= 0xFFFF || !classes_.push(c)) return memory();
        return n;
    }

    // Reads one escaped code point after '\' (not a class letter). Returns false on an unknown escape.
    bool escape_cp(uint32_t& cp) {
        char c = p_[at_++];
        switch (c) {
            case 't': cp = '\t'; return true;
            case 'n': cp = '\n'; return true;
            case 'r': cp = '\r'; return true;
            case 'f': cp = '\f'; return true;
            case 'v': cp = '\v'; return true;
            case '0': cp = 0; return true;
            case 'x':
            case 'u': {
                size_t digits = c == 'x' ? 2 : 4;
                uint32_t v = 0;
                for (size_t i = 0; i < digits; ++i) {
                    if (!more() || hex_value(peek()) < 0) return false;
                    v = v * 16 + static_cast<uint32_t>(hex_value(p_[at_++]));
                }
                cp = v;
                return true;
            }
            default:
                if (is_alpha(c) || is_digit(c)) return false;
                cp = static_cast<unsigned char>(c);
                if (cp >= 0x80) {
                    --at_;
                    at_ += utf8_decode(p_ + at_, n_ - at_, cp);
                }
                return true;
        }
    }

    uint8_t set_of(char c) {
        switch (c) {
            case 'd': return k_set_digit;
            case 'D': return k_set_not_digit;
            case 'w': return k_set_word;
            case 'W': return k_set_not_word;
            case 's': return k_set_space;
            case 'S': return k_set_not_space;
            default: return 0;
        }
    }

    bool add_range(uint32_t low, uint32_t high) {
        Regex::Range r;
        r.low = low;
        r.high = high;
        if (!ranges_.push(r)) {
            memory();
            return false;
        }
        return true;
    }

    // [:name:] inside a class; at_ is on the first ':'.
    bool posix_class(Regex::Class& c) {
        static const char k_names[] VDB_FLASH = "alpha\0digit\0alnum\0space\0upper\0lower\0punct\0xdigit\0";
        size_t start = at_ + 1;
        size_t end = start;
        while (end < n_ && p_[end] != ':') ++end;
        if (end + 1 >= n_ || p_[end + 1] != ']') return false;
        int which = -1;
        int index = 0;
        for (const char* name = k_names; flash_char(name); name += flash_length(name) + 1, ++index) {
            if (flash_names_equal(p_ + start, end - start, name)) which = index;
        }
        if (which < 0) return false;
        at_ = end + 2;
        switch (which) {
            case 0: return add_range('a', 'z') && add_range('A', 'Z') && add_range(0xC0, 0xD6) && add_range(0xD8, 0xF6) &&
                           add_range(0xF8, 0x24F);
            case 1: c.sets |= k_set_digit; return true;
            case 2: c.sets |= k_set_digit; return add_range('a', 'z') && add_range('A', 'Z') && add_range(0xC0, 0xD6) &&
                           add_range(0xD8, 0xF6) && add_range(0xF8, 0x24F);
            case 3: c.sets |= k_set_space; return true;
            case 4: return add_range('A', 'Z') && add_range(0xC0, 0xD6) && add_range(0xD8, 0xDE);
            case 5: return add_range('a', 'z') && add_range(0xDF, 0xF6) && add_range(0xF8, 0xFF);
            case 6: return add_range('!', '/') && add_range(':', '@') && add_range('[', '`') && add_range('{', '~');
            default: return add_range('0', '9') && add_range('a', 'f') && add_range('A', 'F');
        }
    }

    Node* parse_class() {
        size_t first = ranges_.size();
        Node* n = node(k_class);
        if (!n) return 0;
        Regex::Class c;
        c.ranges = 0;
        c.count = 0;
        c.negate = 0;
        c.sets = 0;
        if (more() && peek() == '^') {
            c.negate = 1;
            ++at_;
        }
        bool first_item = true;
        for (;;) {
            if (!more()) return fail(VDB_F("regex: missing ']'"));
            char ch = peek();
            if (ch == ']' && !first_item) {
                ++at_;
                break;
            }
            first_item = false;
            if (ch == '[' && at_ + 1 < n_ && p_[at_ + 1] == ':') {
                ++at_;
                if (!posix_class(c)) return fail(VDB_F("regex: unknown [:class:]"));
                continue;
            }
            uint32_t low;
            if (ch == '\\') {
                ++at_;
                if (!more()) return fail(VDB_F("regex: trailing '\\'"));
                uint8_t set = set_of(peek());
                if (set) {
                    c.sets |= set;
                    ++at_;
                    continue;
                }
                if (peek() == 'b') {
                    ++at_;
                    low = '\b';
                } else if (!escape_cp(low)) {
                    return fail(VDB_F("regex: unknown escape"));
                }
            } else {
                at_ += utf8_decode(p_ + at_, n_ - at_, low);
            }
            uint32_t high = low;
            if (more() && peek() == '-' && at_ + 1 < n_ && p_[at_ + 1] != ']') {
                ++at_;
                if (peek() == '\\') {
                    ++at_;
                    if (!more() || !escape_cp(high)) return fail(VDB_F("regex: bad range"));
                } else {
                    at_ += utf8_decode(p_ + at_, n_ - at_, high);
                }
                if (high < low) return fail(VDB_F("regex: bad range"));
            }
            if (!add_range(low, high)) return 0;
        }
        return finish_class(n, c, first);
    }

    Node* parse_atom() {
        char c = peek();
        if (c == '(') {
            ++at_;
            uint16_t group = 0;
            if (at_ + 1 < n_ && peek() == '?' && p_[at_ + 1] == ':') {
                at_ += 2;
            } else if (more() && peek() == '?') {
                return fail(VDB_F("regex: only (?: ) groups are supported"));
            } else {
                group = static_cast<uint16_t>(groups_++);
            }
            Node* inner = parse_alt();
            if (!inner) return 0;
            if (!more() || peek() != ')') return fail(VDB_F("regex: missing ')'"));
            ++at_;
            if (!group) return inner;
            Node* g = node(k_group);
            if (!g) return 0;
            g->a = inner;
            g->index = group;
            return g;
        }
        if (c == '[') {
            ++at_;
            return parse_class();
        }
        if (c == '.') {
            ++at_;
            return node(k_any);
        }
        if (c == '^' || c == '$') {
            ++at_;
            Node* n = node(k_assert);
            if (n) n->index = c == '^' ? k_bol : k_eol;
            return n;
        }
        if (c == '*' || c == '+' || c == '?') return fail(VDB_F("regex: nothing to repeat"));
        if (c == '\\') {
            ++at_;
            if (!more()) return fail(VDB_F("regex: trailing '\\'"));
            char e = peek();
            uint8_t set = set_of(e);
            if (set) {
                ++at_;
                return set_node(set);
            }
            if (e == 'b' || e == 'B') {
                ++at_;
                Node* n = node(k_assert);
                if (n) n->index = e == 'b' ? k_word_boundary : k_not_word_boundary;
                return n;
            }
            uint32_t cp;
            if (is_digit(e) && e != '0') return fail(VDB_F("regex: back-references are not supported"));
            if (!escape_cp(cp)) return fail(VDB_F("regex: unknown escape"));
            return char_node(cp);
        }
        uint32_t cp;
        at_ += utf8_decode(p_ + at_, n_ - at_, cp);
        return char_node(cp);
    }

    size_t emit(uint8_t op, uint8_t arg, uint16_t x, uint16_t y, uint32_t c) {
        Regex::Inst inst;
        inst.op = op;
        inst.arg = arg;
        inst.x = x;
        inst.y = y;
        inst.c = c;
        if (code_.size() > VDB_REGEX_MAX_PROGRAM || !code_.push(inst)) {
            if (code_.size() <= VDB_REGEX_MAX_PROGRAM) memory();
            return 0;
        }
        return code_.size() - 1;
    }
    uint16_t here() const { return static_cast<uint16_t>(code_.size()); }

    bool gen(Node* n) {
        if (!code_ok()) return false;
        switch (n->kind) {
            case k_empty: return true;
            case k_char: emit(k_op_char, 0, 0, 0, n->c); return code_ok();
            case k_any: emit(k_op_any, 0, 0, 0, 0); return code_ok();
            case k_class: emit(k_op_class, 0, n->index, 0, 0); return code_ok();
            case k_assert: emit(k_op_assert, static_cast<uint8_t>(n->index), 0, 0, 0); return code_ok();
            case k_cat: return gen(n->a) && gen(n->b);
            case k_group:
                emit(k_op_save, 0, static_cast<uint16_t>(n->index * 2), 0, 0);
                if (!gen(n->a)) return false;
                emit(k_op_save, 0, static_cast<uint16_t>(n->index * 2 + 1), 0, 0);
                return code_ok();
            case k_alt: {
                size_t split = emit(k_op_split, 0, 0, 0, 0);
                if (!code_ok()) return false;
                code_[split].x = here();
                if (!gen(n->a)) return false;
                size_t jump = emit(k_op_jmp, 0, 0, 0, 0);
                if (!code_ok()) return false;
                code_[split].y = here();
                if (!gen(n->b)) return false;
                code_[jump].x = here();
                return true;
            }
            default: return gen_repeat(n);
        }
    }

    void set_split(size_t split, uint16_t body, uint16_t out, bool greedy) {
        code_[split].x = greedy ? body : out;
        code_[split].y = greedy ? out : body;
    }

    bool gen_repeat(Node* n) {
        bool greedy = n->greedy != 0;
        for (int i = 0; i < n->min; ++i) {
            if (!gen(n->a)) return false;
        }
        if (n->max == k_unbounded) {
            uint16_t loop = here();
            size_t split = emit(k_op_split, 0, 0, 0, 0);
            if (!code_ok()) return false;
            uint16_t body = here();
            if (!gen(n->a)) return false;
            emit(k_op_jmp, 0, loop, 0, 0);
            if (!code_ok()) return false;
            set_split(split, body, here(), greedy);
            return true;
        }
        Vec<uint16_t> pending(pool_.allocator());
        for (int i = n->min; i < n->max; ++i) {
            size_t split = emit(k_op_split, 0, 0, 0, 0);
            if (!code_ok()) return false;
            if (!pending.push(static_cast<uint16_t>(split))) {
                memory();
                return false;
            }
            code_[split].x = here();
            if (!gen(n->a)) return false;
        }
        uint16_t out = here();
        for (size_t i = 0; i < pending.size(); ++i) set_split(pending[i], static_cast<uint16_t>(pending[i] + 1), out, greedy);
        return true;
    }

    Pool& pool_;
    const char* p_;
    size_t n_;
    size_t at_;
    unsigned flags_;
    Status& status_;
    size_t groups_;
    int depth_;
    Vec<Regex::Inst> code_;
    Vec<Regex::Class> classes_;
    Vec<Regex::Range> ranges_;
};

Regex* Regex::compile(Pool& pool, const char* pattern, size_t length, unsigned flags, Status& status) {
    RegexCompiler compiler(pool, pattern, length, flags, status);
    Regex* regex = compiler.run();
    if (regex && regex->groups_ > k_regex_max_groups) regex->groups_ = k_regex_max_groups;
    return regex;
}

// ── VM ───────────────────────────────────────────────────────────────────────

RegexWork::RegexWork(Allocator& allocator)
    : pcs_a(allocator),
      pcs_b(allocator),
      caps_a(allocator),
      caps_b(allocator),
      mark(allocator),
      stack(allocator),
      current(allocator),
      generation(0) {}

bool RegexWork::prepare(size_t program, size_t slots) {
    if (mark.size() < program) {
        if (!mark.resize(program)) return false;
        generation = 0;
        for (size_t i = 0; i < mark.size(); ++i) mark[i] = 0;
    }
    return pcs_a.resize(program) && pcs_b.resize(program) && caps_a.resize(program * slots + 1) &&
           caps_b.resize(program * slots + 1) && stack.resize(program * 3 + 4) && current.resize(slots + 1);
}

bool Regex::class_has(const Class& c, uint32_t cp) const {
    bool in = false;
    uint32_t lower = cp, upper = cp;
    if (flags_ & k_regex_icase) {
        lower = fold_lower(cp);
        upper = fold_upper(cp);
    }
    for (uint16_t i = 0; i < c.count && !in; ++i) {
        const Range& r = c.ranges[i];
        in = (cp >= r.low && cp <= r.high) || (lower >= r.low && lower <= r.high) || (upper >= r.low && upper <= r.high);
    }
    if (!in && c.sets) {
        in = ((c.sets & k_set_digit) && is_digit_cp(cp)) || ((c.sets & k_set_not_digit) && !is_digit_cp(cp)) ||
             ((c.sets & k_set_word) && is_word_cp(cp)) || ((c.sets & k_set_not_word) && !is_word_cp(cp)) ||
             ((c.sets & k_set_space) && is_space_cp(cp)) || ((c.sets & k_set_not_space) && !is_space_cp(cp));
    }
    return in != (c.negate != 0);
}

bool Regex::step_matches(const Inst& inst, uint32_t cp) const {
    switch (inst.op) {
        case k_op_char: return ((flags_ & k_regex_icase) ? fold_lower(cp) : cp) == inst.c;
        case k_op_any: return cp != '\n' || (flags_ & k_regex_dotall);
        case k_op_class: return class_has(classes_[inst.x], cp);
        default: return false;
    }
}

bool Regex::assertion_holds(const Inst& inst, const char* text, size_t length, size_t at) const {
    switch (inst.arg) {
        case k_bol: return at == 0 || ((flags_ & k_regex_multiline) && text[at - 1] == '\n');
        case k_eol: return at == length || ((flags_ & k_regex_multiline) && text[at] == '\n');
        default: {
            uint32_t next = 0;
            if (at < length) utf8_decode(text + at, length - at, next);
            bool before = at > 0 && is_word_cp(previous_cp(text, at));
            bool after = at < length && is_word_cp(next);
            return (before != after) == (inst.arg == k_word_boundary);
        }
    }
}

int Regex::search(const char* text, size_t length, size_t from, size_t* captures, size_t groups, RegexWork& w) const {
    size_t slots = captures ? (groups < groups_ ? groups : groups_) * 2 : 0;
    if (!w.prepare(size_, slots)) return -1;
    uint16_t* clist = w.pcs_a.data();
    uint16_t* nlist = w.pcs_b.data();
    uint32_t* ccaps = w.caps_a.data();
    uint32_t* ncaps = w.caps_b.data();
    uint16_t* mark = w.mark.data();
    uint32_t* stack = w.stack.data();
    uint32_t* current = w.current.data();
    size_t ccount = 0, ncount = 0;
    bool matched = false;
    const uint32_t k_unset32 = 0xFFFFFFFFu;

    if (++w.generation == 0) {
        for (size_t i = 0; i < w.mark.size(); ++i) mark[i] = 0;
        w.generation = 1;
    }

    // Follows the empty-width instructions from `pc` and lists the threads that consume or match.
    struct Adder {
        const Regex* re;
        const char* text;
        size_t length;
        uint16_t* mark;
        uint32_t* stack;
        uint32_t* current;
        size_t slots;

        void run(uint16_t pc0, size_t at, uint16_t gen, uint16_t* list, uint32_t* caps, size_t& count) {
            size_t top = 0;
            stack[top++] = pc0;
            while (top) {
                uint32_t entry = stack[--top];
                if (entry & k_restore) {
                    current[entry & ~k_restore] = stack[--top];
                    continue;
                }
                uint16_t pc = static_cast<uint16_t>(entry);
                if (mark[pc] == gen) continue;
                mark[pc] = gen;
                const Inst& inst = re->program_[pc];
                switch (inst.op) {
                    case k_op_jmp: stack[top++] = inst.x; break;
                    case k_op_split:
                        stack[top++] = inst.y;
                        stack[top++] = inst.x;
                        break;
                    case k_op_save:
                        if (inst.x < slots) {
                            stack[top++] = current[inst.x];
                            stack[top++] = k_restore | inst.x;
                            current[inst.x] = static_cast<uint32_t>(at);
                        }
                        stack[top++] = static_cast<uint32_t>(pc + 1);
                        break;
                    case k_op_assert:
                        if (re->assertion_holds(inst, text, length, at)) stack[top++] = static_cast<uint32_t>(pc + 1);
                        break;
                    default:
                        list[count] = pc;
                        for (size_t s = 0; s < slots; ++s) caps[count * slots + s] = current[s];
                        ++count;
                        break;
                }
            }
        }
    };
    Adder adder = {this, text, length, mark, stack, current, slots};
    uint16_t gen = w.generation;

    size_t pos = from;
    for (;;) {
        if (!matched && pos <= length) {
            for (size_t s = 0; s < slots; ++s) current[s] = k_unset32;
            adder.run(0, pos, gen, clist, ccaps, ccount);
        }
        uint32_t cp = 0;
        size_t step = 1;
        if (pos < length) step = utf8_decode(text + pos, length - pos, cp);
        if (!ccount) {
            // No thread alive here: a match is final, otherwise try from the next position.
            if (matched || pos >= length) break;
            pos += step;
            if (++w.generation == 0) {
                for (size_t i = 0; i < w.mark.size(); ++i) mark[i] = 0;
                w.generation = 1;
            }
            gen = w.generation;
            continue;
        }
        if (++w.generation == 0) {
            for (size_t i = 0; i < w.mark.size(); ++i) mark[i] = 0;
            w.generation = 1;
        }
        gen = w.generation;
        ncount = 0;
        for (size_t t = 0; t < ccount; ++t) {
            const Inst& inst = program_[clist[t]];
            if (inst.op == k_op_match) {
                matched = true;
                if (captures) {
                    for (size_t s = 0; s < slots; ++s) {
                        uint32_t v = ccaps[t * slots + s];
                        captures[s] = v == k_unset32 ? k_regex_unset : v;
                    }
                    for (size_t s = slots; s < groups * 2; ++s) captures[s] = k_regex_unset;
                }
                if (!captures) return 1;
                break;  // lower-priority threads are cut
            }
            if (pos < length && step_matches(inst, cp)) {
                for (size_t s = 0; s < slots; ++s) current[s] = ccaps[t * slots + s];
                adder.run(static_cast<uint16_t>(clist[t] + 1), pos + step, gen, nlist, ncaps, ncount);
            }
        }
        uint16_t* swap_list = clist;
        clist = nlist;
        nlist = swap_list;
        uint32_t* swap_caps = ccaps;
        ccaps = ncaps;
        ncaps = swap_caps;
        ccount = ncount;
        if (pos >= length) break;
        pos += step;
    }
    return matched ? 1 : 0;
}

}  // namespace vdb
