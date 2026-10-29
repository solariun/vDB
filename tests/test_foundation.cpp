// Memory, containers, text, numbers, UTF-8 (spec 2.2, 2.5, 4.3, A5, A12).
#include "test_harness.hpp"
#include "vdb/table.hpp"
#include "vdb/text.hpp"

using vdb::Number;

TEST(arena_serves_frees_and_merges) {
    static unsigned char buffer[1024];
    vdb::ArenaAllocator arena(buffer, sizeof(buffer));
    void* a = arena.allocate(100);
    void* b = arena.allocate(200);
    void* c = arena.allocate(300);
    CHECK(a && b && c);
    CHECK(arena.allocate(2000) == 0);  // never more than the buffer
    size_t used = arena.used();
    arena.release(b);
    CHECK(arena.used() < used);
    void* d = arena.allocate(150);  // fits in b's place
    CHECK(d == b);
    arena.release(a);
    arena.release(c);
    arena.release(d);
    CHECK_INT(arena.used(), 0);
    void* big = arena.allocate(900);  // only possible if the free blocks merged
    CHECK(big != 0);
    CHECK(arena.peak() >= 600);
}

TEST(arena_rejects_a_tiny_buffer) {
    unsigned char buffer[4];
    vdb::ArenaAllocator arena(buffer, sizeof(buffer));
    CHECK(arena.allocate(1) == 0);
}

TEST(budget_caps_and_counts) {
    vdb::BudgetAllocator budget(vdb::heap_allocator(), 1000);
    void* a = budget.allocate(400);
    void* b = budget.allocate(400);
    CHECK(a && b);
    CHECK(budget.allocate(400) == 0);
    budget.release(a);
    void* c = budget.allocate(400);
    CHECK(c != 0);
    budget.release(b);
    budget.release(c);
    CHECK_INT(budget.used(), 0);
    CHECK(budget.peak() >= 800);
}

TEST(pool_marks_release_in_order) {
    vdb::Pool pool(vdb::heap_allocator(), 64);
    char* keep = pool.copy("keep", 4);
    vdb::Pool::Mark m = pool.mark();
    for (int i = 0; i < 100; ++i) CHECK(pool.allocate(40) != 0);
    pool.release(m);
    CHECK_STR(keep, "keep");
    char* again = pool.copy("again", 5);
    CHECK_STR(again, "again");
    CHECK(pool.allocate(10000) != 0);  // larger than a chunk
}

TEST(vec_grows_removes_and_resizes) {
    vdb::Vec<int> v(vdb::heap_allocator());
    for (int i = 0; i < 1000; ++i) CHECK(v.push(i));
    CHECK_INT(v.size(), 1000);
    CHECK_INT(v[999], 999);
    v.remove(0);
    CHECK_INT(v[0], 1);
    CHECK(v.resize(2000));
    CHECK_INT(v[1999], 0);
}

TEST(text_appends_numbers) {
    vdb::Text t(vdb::heap_allocator());
    t.append("n=");
    t.append_int(-42);
    t.append_char(' ');
    t.append_real(3.25, '.', vdb::k_real_digits);
    CHECK_STR(t.c_str(), "n=-42 3.25");
    t.truncate(2);
    CHECK_STR(t.c_str(), "n=");
    CHECK(t.ok());
}

TEST(numbers_parse_like_spec_2_2) {
    Number n;
    CHECK(vdb::parse_number("42", 2, '.', n) && n.is_int && n.i == 42);
    CHECK(vdb::parse_number(" -017 ", 6, '.', n) && n.is_int && n.i == -17);
    CHECK(vdb::parse_number("3.5", 3, '.', n) && !n.is_int && n.r == 3.5);
    CHECK(vdb::parse_number(".5", 2, '.', n) && n.r == 0.5);
    CHECK(vdb::parse_number("5.", 2, '.', n) && n.r == 5.0);
    CHECK(vdb::parse_number("1e3", 3, '.', n) && !n.is_int && n.r == 1000.0);
    CHECK(vdb::parse_number("+2E-2", 5, '.', n) && n.r == 0.02);
    CHECK(vdb::parse_number("3,14", 4, ',', n) && n.r == 3.14);
    CHECK(!vdb::parse_number("3,14", 4, '.', n));
    CHECK(!vdb::parse_number("", 0, '.', n));
    CHECK(!vdb::parse_number("abc", 3, '.', n));
    CHECK(!vdb::parse_number("1e", 2, '.', n));
    CHECK(!vdb::parse_number(".", 1, '.', n));
    CHECK(!vdb::parse_number("12a", 3, '.', n));
    CHECK(!vdb::parse_number("inf", 3, '.', n));
    CHECK(!vdb::parse_number("0x10", 4, '.', n));
    CHECK(!vdb::parse_number("1 000", 5, '.', n));
    CHECK(vdb::parse_number("9223372036854775807", 19, '.', n) && n.is_int && n.i == INT64_MAX);
    CHECK(vdb::parse_number("-9223372036854775808", 20, '.', n) && n.is_int && n.i == INT64_MIN);
    CHECK(vdb::parse_number("9223372036854775808", 19, '.', n) && !n.is_int);
}

TEST(reals_format_like_spec_2_5) {
    char b[vdb::k_number_text];
    struct Case {
        double v;
        const char* text;
    };
    const Case cases[] = {{0, "0"},           {42, "42"},          {-42.5, "-42.5"},     {0.1 + 0.2, "0.3"},
                          {1.0 / 3, "0.333333333333333"},          {1e15, "1e+15"},      {123456789012345.0, "123456789012345"},
                          {1e-5, "0.00001"},  {1.5e-6, "1.5e-6"},  {2.5e20, "2.5e+20"},  {100, "100"},
                          {999999999999999.9, "1e+15"}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        vdb::format_real(cases[i].v, '.', vdb::k_real_digits, b);
        CHECK_STR(b, cases[i].text);
    }
    vdb::format_real(3.25, ',', vdb::k_real_digits, b);
    CHECK_STR(b, "3,25");
    vdb::format_int(INT64_MIN, b);
    CHECK_STR(b, "-9223372036854775808");
}

TEST(reals_round_trip_with_17_digits) {
    const double values[] = {0.1, 1.0 / 3, 2.0 / 3, 1e-300, 123456.789e10, -7.25};
    char b[vdb::k_number_text];
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        size_t n = vdb::format_real(values[i], '.', vdb::k_real_exact_digits, b);
        Number back;
        CHECK(vdb::parse_number(b, n, '.', back));
        CHECK(back.r == values[i]);
    }
}

TEST(utf8_counts_and_folds) {
    const char* s = "ação";  // 4 characters, 6 bytes
    size_t n = strlen(s);
    CHECK_INT(n, 6);
    CHECK_INT(vdb::utf8_count(s, n), 4);
    CHECK_INT(vdb::utf8_offset(s, n, 2), 3);
    uint32_t cp;
    CHECK_INT(vdb::utf8_decode(s + 1, n - 1, cp), 2);
    CHECK_INT(cp, 0xE7);  // ç
    CHECK_INT(vdb::fold_upper(0xE7), 0xC7);
    CHECK_INT(vdb::fold_lower(0xC3), 0xE3);  // Ã -> ã
    CHECK_INT(vdb::fold_lower(0xD7), 0xD7);  // × has no case
    CHECK_INT(vdb::fold_lower(0x141), 0x142);  // Ł -> ł (Latin Extended-A)
    CHECK_INT(vdb::fold_upper(0x15B), 0x15A);  // ś -> Ś
    char out[4];
    CHECK_INT(vdb::utf8_encode(0x20AC, out), 3);  // €
    CHECK_INT(vdb::utf8_decode("\xff", 1, cp), 1);  // invalid bytes pass alone
}

TEST(names_compare_without_case_and_suggest) {
    CHECK(vdb::names_equal("Name", "nAME"));
    CHECK(!vdb::names_equal("name", "names"));
    CHECK(vdb::names_equal("cityX", 4, "CITY"));
    CHECK_INT(vdb::edit_distance("nmae", "name", 3), 2);
    CHECK_INT(vdb::edit_distance("UPER", "upper", 3), 1);
    CHECK_INT(vdb::edit_distance("abc", "xyzw", 3), 3);
}

TEST(varints_and_records_round_trip) {
    uint8_t buffer[8];
    const uint32_t values[] = {0, 1, 127, 128, 16383, 16384, 0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        uint8_t* end = vdb::varint_write(buffer, values[i]);
        CHECK_INT(static_cast<size_t>(end - buffer), vdb::varint_size(values[i]));
        uint32_t back;
        vdb::varint_read(buffer, back);
        CHECK_INT(back, values[i]);
    }
    vdb::Value fields[3] = {vdb::Value::text("Ana", 3), vdb::Value::null(), vdb::Value::text("", 0)};
    uint8_t* record = vdb::encode_record(vdb::heap_allocator(), fields, 3);
    vdb::Value back[4];
    vdb::decode_record(record, back, 4);
    CHECK_STR(back[0].s, "Ana");
    CHECK(back[1].is_null());
    CHECK(!back[2].is_null() && back[2].n == 0);
    CHECK(back[3].is_null());  // a missing field reads NULL
    vdb::heap_allocator().release(record);
}

TEST(hash_index_finds_and_grows) {
    vdb::HashIndex index(vdb::heap_allocator());
    char key[16];
    for (int i = 0; i < 5000; ++i) {
        int n = snprintf(key, sizeof(key), "k%d", i);
        bool added;
        CHECK_INT(index.find_or_add(key, static_cast<size_t>(n), added), i);
        CHECK(added);
    }
    bool added;
    CHECK_INT(index.find_or_add("k42", 3, added), 42);
    CHECK(!added);
    CHECK_INT(index.find("nope", 4), -1);
    CHECK_INT(index.size(), 5000);
}

static int compare_mod(uint32_t a, uint32_t b, void*) {
    return static_cast<int>(a % 10) - static_cast<int>(b % 10);
}

TEST(merge_sort_is_stable) {
    uint32_t items[100];
    for (uint32_t i = 0; i < 100; ++i) items[i] = 99 - i;
    CHECK(vdb::merge_sort(items, 100, compare_mod, 0, vdb::heap_allocator()));
    for (size_t i = 1; i < 100; ++i) {
        CHECK(items[i - 1] % 10 <= items[i] % 10);
        if (items[i - 1] % 10 == items[i] % 10) CHECK(items[i - 1] > items[i]);  // input order kept
    }
}
