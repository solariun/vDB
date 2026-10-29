// Regular expressions (spec 3.5, A6).
#include "test_harness.hpp"
#include "cli.hpp"
#include "vdb/regex.hpp"

namespace {

// Returns the first match as "start-end" or "-" for none, or "error: ..." for a bad pattern.
struct Probe {
    char out[128];
    Probe(const char* pattern, const char* text, unsigned flags = 0, size_t group = 0) {
        vdb::Pool pool(vdb::heap_allocator());
        vdb::Status status;
        const vdb::Regex* re = vdb::Regex::compile(pool, pattern, strlen(pattern), flags, status);
        if (!re) {
            snprintf(out, sizeof(out), "error: %s", status.message());
            return;
        }
        vdb::RegexWork work(vdb::heap_allocator());
        size_t caps[2 * vdb::k_regex_max_groups];
        int r = re->search(text, strlen(text), 0, caps, group + 1, work);
        if (r != 1) {
            snprintf(out, sizeof(out), "-");
            return;
        }
        if (caps[2 * group] == vdb::k_regex_unset) snprintf(out, sizeof(out), "unset");
        else snprintf(out, sizeof(out), "%.*s", static_cast<int>(caps[2 * group + 1] - caps[2 * group]), text + caps[2 * group]);
    }
};

}  // namespace

#define MATCH(pattern, text, expected) CHECK_STR(Probe(pattern, text).out, expected)
#define MATCH_F(pattern, text, flags, expected) CHECK_STR(Probe(pattern, text, flags).out, expected)
#define GROUP(pattern, text, group, expected) CHECK_STR(Probe(pattern, text, 0, group).out, expected)

TEST(regex_literals_and_classes) {
    MATCH("abc", "xxabcxx", "abc");
    MATCH("a.c", "abc", "abc");
    MATCH("a.c", "a\nc", "-");
    MATCH_F("a.c", "a\nc", vdb::k_regex_dotall, "a\nc");
    MATCH("[0-9]+", "ab123cd", "123");
    MATCH("[^a-z]+", "abcDEF1g", "DEF1");
    MATCH("[]a]+", "x]a]y", "]a]");
    MATCH("[a\\-z]+", "q-az", "-az");
    MATCH("\\d+\\.\\d+", "v 3.14!", "3.14");
    MATCH("\\w+", "  hello_1 ", "hello_1");
    MATCH("\\s+", "a \t b", " \t ");
    MATCH("[[:alpha:]]+", "12abC3", "abC");
    MATCH("[[:digit:][:space:]]+", "ab1 2c", "1 2");
    MATCH("[[:upper:]]+", "abCDe", "CD");
    MATCH("[[:xdigit:]]+", "zz0aFgg", "0aF");
    MATCH("[[:punct:]]+", "ab!?c", "!?");
}

TEST(regex_quantifiers_greedy_and_lazy) {
    MATCH("a*", "aaab", "aaa");
    MATCH("a+?", "aaab", "a");
    MATCH("<.+>", "<a><b>", "<a><b>");
    MATCH("<.+?>", "<a><b>", "<a>");
    MATCH("a{2}", "aaaa", "aa");
    MATCH("a{2,}", "aaaa", "aaaa");
    MATCH("a{1,3}", "aaaa", "aaa");
    MATCH("a{1,3}?", "aaaa", "a");
    MATCH("x{2}", "x", "-");
    MATCH("ab?c", "ac", "ac");
    MATCH("a{,2}", "a{,2}", "a{,2}");  // not a quantifier: literal
}

TEST(regex_anchors_alternation_and_boundaries) {
    MATCH("^abc", "abcabc", "abc");
    MATCH("^abc", "xabc", "-");
    MATCH("abc$", "abcabc", "abc");
    MATCH("cat|dog", "hotdog", "dog");
    MATCH("(cat|dog)s?$", "cats", "cats");
    MATCH("\\bcat\\b", "concat cat", "cat");
    MATCH("\\Bcat", "concat", "cat");
    MATCH_F("^b", "a\nb", vdb::k_regex_multiline, "b");
    MATCH("^b", "a\nb", "-");
    MATCH_F("a$", "a\nb", vdb::k_regex_multiline, "a");
    MATCH("", "abc", "");
}

TEST(regex_groups_capture_leftmost_first) {
    GROUP("(\\d+)-(\\d+)", "tel 555-1234", 1, "555");
    GROUP("(\\d+)-(\\d+)", "tel 555-1234", 2, "1234");
    GROUP("(a|ab)(c|bcd)", "abcd", 0, "abcd");  // Perl order: a first, then bcd
    GROUP("(a|ab)(c|bcd)(d*)", "abcd", 3, "");
    GROUP("(a)|(b)", "b", 1, "unset");
    GROUP("(?:ab)+(c)", "ababc", 1, "c");
    GROUP("(a*)+", "aaa", 0, "aaa");
    GROUP("((a)(b))", "ab", 3, "b");
}

TEST(regex_case_insensitive_and_utf8) {
    MATCH_F("hello", "HeLLo world", vdb::k_regex_icase, "HeLLo");
    MATCH_F("[a-z]+", "ABC", vdb::k_regex_icase, "ABC");
    MATCH_F("ação", "AÇÃO", vdb::k_regex_icase, "AÇÃO");
    MATCH("a.o", "ação", "-");
    MATCH("ç.o", "ação", "ção");
    MATCH("[ãç]+", "ação", "çã");
    MATCH("\\w+", "São Paulo", "São");
    MATCH("\\u00e7", "ação", "ç");
    MATCH("\\x41", "zAz", "A");
}

TEST(regex_errors_are_messages) {
    MATCH("(abc", "x", "error: regex: missing ')'");
    MATCH("abc)", "x", "error: regex: unmatched ')'");
    MATCH("[abc", "x", "error: regex: missing ']'");
    MATCH("*a", "x", "error: regex: nothing to repeat");
    MATCH("a{3,1}", "x", "error: regex: bad repeat {m,n}");
    MATCH("(a)\\1", "aa", "error: regex: back-references are not supported");
    MATCH("(?=a)", "a", "error: regex: only (?: ) groups are supported");
    MATCH("\\q", "q", "error: regex: unknown escape");
    MATCH("[[:nope:]]", "q", "error: regex: unknown [:class:]");
    MATCH("a{2000}", "a", "error: regex: repeat count above 1000");
}

TEST(regex_pathological_patterns_stay_linear) {
    // (a+)+$ against "aaaa...b" is exponential for backtracking engines; the Pike VM is linear.
    static char text[20002];
    for (int i = 0; i < 20000; ++i) text[i] = 'a';
    text[20000] = 'b';
    text[20001] = 0;
    double start = cli::now_seconds();
    MATCH("(a+)+$", text, "-");
    MATCH("(a|aa)*c", text, "-");
    CHECK(cli::now_seconds() - start < 5.0);
}

TEST(regex_size_is_bounded) {
    MATCH("(((a{1000}){1000}){1000})", "a", "error: regex: pattern too large");
}

TEST(regex_flags_parse) {
    unsigned f;
    CHECK(vdb::regex_flags("im", 2, f) && (f & vdb::k_regex_icase) && (f & vdb::k_regex_multiline));
    CHECK(vdb::regex_flags("1g", 2, f) && (f & vdb::k_regex_first));
    CHECK(!vdb::regex_flags("x", 1, f));
}
