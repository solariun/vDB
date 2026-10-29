// A small test harness without the C++ standard library (spec 10).
#ifndef VDB_TEST_HARNESS_HPP
#define VDB_TEST_HARNESS_HPP

#include <stdio.h>
#include <string.h>

#include "vdb/vdb.hpp"

namespace test {

struct Case {
    const char* name;
    void (*function)();
    const char* file;
    int line;
    Case* next;
};

Case*& first_case();
int& failures();

struct Registrar {
    explicit Registrar(Case* c) {
        Case** link = &first_case();
        while (*link) link = &(*link)->next;
        *link = c;
    }
};

inline bool check(bool ok, const char* expression, const char* file, int line) {
    if (!ok) {
        printf("    %s:%d: CHECK(%s) failed\n", file, line, expression);
        ++failures();
    }
    return ok;
}

inline bool check_int(long long a, long long b, const char* ea, const char* eb, const char* file, int line) {
    if (a == b) return true;
    printf("    %s:%d: %s == %s failed: %lld != %lld\n", file, line, ea, eb, a, b);
    ++failures();
    return false;
}

inline bool check_str(const char* a, const char* b, const char* ea, const char* eb, const char* file, int line) {
    if (a && b && strcmp(a, b) == 0) return true;
    printf("    %s:%d: %s == %s failed:\n      got:      \"%s\"\n      expected: \"%s\"\n", file, line, ea, eb, a ? a : "(null)",
           b ? b : "(null)");
    ++failures();
    return false;
}

// Rows of a query in one line: fields joined by '|', rows by ';', NULL as NULL; errors as "ERROR: message".
class Rows {
public:
    Rows(vdb::Database& db, const char* sql) : text_(vdb::heap_allocator()) { fill(db.query(sql)); }
    Rows(vdb::Database& db, const char* sql, const vdb::Params& p) : text_(vdb::heap_allocator()) { fill(db.query(sql, p)); }
    explicit Rows(vdb::Result r) : text_(vdb::heap_allocator()) { fill(static_cast<vdb::Result&&>(r)); }
    const char* c_str() const { return text_.c_str(); }

private:
    void fill(vdb::Result r) {
        if (!r.ok()) {
            text_.append("ERROR: ");
            text_.append(r.error());
            return;
        }
        for (size_t i = 0; i < r.size(); ++i) {
            if (i) text_.append_char(';');
            for (size_t c = 0; c < r.columns(); ++c) {
                if (c) text_.append_char('|');
                vdb::Cell cell = r[i][c];
                text_.append(cell.is_null() ? "NULL" : cell.text());
            }
        }
    }
    vdb::Text text_;
};

// Column names of a query, joined by '|'.
class Names {
public:
    Names(vdb::Database& db, const char* sql) : text_(vdb::heap_allocator()) {
        vdb::Result r = db.query(sql);
        if (!r.ok()) {
            text_.append("ERROR: ");
            text_.append(r.error());
            return;
        }
        for (size_t c = 0; c < r.columns(); ++c) {
            if (c) text_.append_char('|');
            text_.append(r.column_name(c));
        }
    }
    const char* c_str() const { return text_.c_str(); }

private:
    vdb::Text text_;
};

}  // namespace test

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)
#define TEST(name)                                                                                   \
    static void test_##name();                                                                       \
    static test::Case TEST_CONCAT(case_, name) = {#name, test_##name, __FILE__, __LINE__, 0};         \
    static test::Registrar TEST_CONCAT(registrar_, name)(&TEST_CONCAT(case_, name));                 \
    static void test_##name()

#define CHECK(cond) test::check((cond), #cond, __FILE__, __LINE__)
#define CHECK_INT(a, b) test::check_int(static_cast<long long>(a), static_cast<long long>(b), #a, #b, __FILE__, __LINE__)
#define CHECK_STR(a, b) test::check_str((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_ROWS(db, sql, expected) CHECK_STR(test::Rows(db, sql).c_str(), expected)
#define CHECK_NAMES(db, sql, expected) CHECK_STR(test::Names(db, sql).c_str(), expected)

#endif
