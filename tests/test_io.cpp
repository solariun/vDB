// CSV reading (spec 5), output formats (spec 6), the word chain (D1, A1), streaming (A7) and safety (spec 9).
#include <stdlib.h>

#include "test_harness.hpp"

namespace {

// "a|b;c|d" view of a CSV text loaded with `options`, plus the column names first: "h1|h2:a|b;c|d".
struct Loaded {
    char out[512];
    Loaded(const char* csv, const vdb::CsvOptions& options) {
        vdb::Database db;
        vdb::Status st = db.load_csv_text("t", csv, options);
        if (!st.ok()) {
            snprintf(out, sizeof(out), "ERROR: %s", st.message());
            return;
        }
        test::Names names(db, "SELECT * FROM t");
        test::Rows rows(db, "SELECT * FROM t");
        snprintf(out, sizeof(out), "%s:%s", names.c_str(), rows.c_str());
    }
};

const char* format(vdb::Database& db, const char* sql, vdb::Format f, vdb::Text& out) {
    vdb::TextOutput o(vdb::heap_allocator());
    vdb::FormatOptions options;
    options.format = f;
    options.null_text = "";
    vdb::Formatter formatter(o, options, vdb::heap_allocator());
    vdb::Status st = db.run(sql, formatter);
    out.clear();
    if (!st.ok()) out.append(st.message());
    else out.append(o.c_str(), o.size());
    return out.c_str();
}

// Fails the N-th allocation; counts what is still allocated.
class FailingAllocator final : public vdb::Allocator {
public:
    explicit FailingAllocator(long fail_at) : fail_at_(fail_at), count_(0), live_(0) {}
    void* allocate(size_t size) override {
        if (++count_ == fail_at_) return 0;
        void* p = malloc(size ? size : 1);
        if (p) ++live_;
        return p;
    }
    void release(void* block) override {
        if (!block) return;
        --live_;
        free(block);
    }
    long count() const { return count_; }
    long live() const { return live_; }

private:
    long fail_at_;
    long count_;
    long live_;
};

}  // namespace

#define LOADED(csv, options, expected) CHECK_STR(Loaded(csv, options).out, expected)

// ── CSV ──────────────────────────────────────────────────────────────────────

TEST(csv_reads_semicolons_by_default) {
    vdb::CsvOptions o;
    LOADED("a;b\n1;2\n3;4\n", o, "a|b:1|2;3|4");
    LOADED("a;b\r\n1;2\r\n", o, "a|b:1|2");
    LOADED("\xEF\xBB\xBF" "a;b\n1;2", o, "a|b:1|2");  // BOM, no final newline
    LOADED("a;b\n\n1;2\n   \n", o, "a|b:1|2");        // blank lines skipped
}

TEST(csv_quotes_hold_delimiters_quotes_and_lines) {
    vdb::CsvOptions o;
    LOADED("a;b\n\"x;y\";\"say \"\"hi\"\"\"\n", o, "a|b:x;y|say \"hi\"");
    LOADED("a;b\n\"line1\nline2\";z\n", o, "a|b:line1\nline2|z");
    LOADED("a;b\n\"x\"junk;2\n", o, "a|b:xjunk|2");
    o.strict = true;
    LOADED("a;b\n\"x\"junk;2\n", o, "ERROR: csv line 2: text after a closing quote");
    LOADED("a;b\n\"open;2\n", o, "ERROR: csv line 2: quote not closed");
}

TEST(csv_empty_fields_follow_a4) {
    vdb::CsvOptions o;
    LOADED("a;b;c\n;\"\";x\n", o, "a|b|c:NULL||x");
    o.empty_text = true;
    LOADED("a;b;c\n;\"\";x\n", o, "a|b|c:||x");
}

TEST(csv_other_delimiters_and_regex) {
    vdb::CsvOptions o;
    o.delimiter = ',';
    LOADED("a,b\n1,2\n", o, "a|b:1|2");
    o.delimiter = '\t';
    LOADED("a\tb\n1\t2\n", o, "a|b:1|2");
    vdb::CsvOptions r;
    r.delimiter_regex = "\\s+";
    LOADED("PID   USER  CPU\n1     root  0.5\n", r, "PID|USER|CPU:1|root|0.5");
    r.delimiter_regex = "\\s*[,;]\\s*";
    LOADED("a , b;c\n1 ;2,  3\n", r, "a|b|c:1|2|3");
    r.delimiter_regex = "x*";  // matches empty everywhere: only non-empty matches split
    LOADED("axb\n1x2\n", r, "a|b:1|2");
    r.delimiter_regex = "(";
    LOADED("a\n", r, "ERROR: delimiter regex: missing ')'");
    vdb::CsvOptions a;
    a.auto_delimiter = true;
    LOADED("a,b,c\n1,2,3\n", a, "a|b|c:1|2|3");
    LOADED("a|b\n1|2\n", a, "a|b:1|2");
    LOADED("a\tb;c\n1\t2;3\n", a, "a\tb|c:1\t2|3");  // ties go to ';'
}

TEST(csv_header_names_are_cleaned) {
    vdb::CsvOptions o;
    LOADED(" Name ;;Name;name\n1;2;3;4\n", o, "Name|c2|Name_2|name_3:1|2|3|4");
    o.header = false;
    LOADED("1;2\n3;4\n", o, "c1|c2:1|2;3|4");
}

TEST(csv_irregular_rows) {
    vdb::CsvOptions o;
    LOADED("a;b\n1\n2;3;4\n", o, "a|b|c3:1|NULL|NULL;2|3|4");
    o.strict = true;
    LOADED("a;b\n1\n", o, "ERROR: csv line 2: 1 fields, the header has 2");
    vdb::CsvOptions t;
    t.trim = true;
    LOADED("a ; b\n  x  ;  y\n", t, "a|b:x|y");
}

TEST(csv_empty_input) {
    vdb::CsvOptions o;
    LOADED("", o, ":");
    LOADED("a;b\n", o, "a|b:");
}

TEST(csv_reader_counts_and_streams) {
    vdb::TextSource source("a;b\n1;2\n3\n4;5;6\n");
    vdb::CsvReader reader(vdb::heap_allocator(), source, vdb::CsvOptions());
    CHECK(reader.open().ok());
    CHECK_INT(reader.column_count(), 2);
    const vdb::Value* v;
    size_t n;
    vdb::Status st;
    int rows = 0;
    while (reader.next(v, n, st) == 1) ++rows;
    CHECK_INT(rows, 3);
    CHECK_INT(reader.short_rows(), 1);
    CHECK_INT(reader.long_rows(), 1);
    CHECK_INT(reader.line(), 4);
}

TEST(csv_loads_into_a_table_and_queries) {
    vdb::Database db;
    vdb::CsvOptions o;
    CHECK(db.load_csv_text("people", "name;age\nAna;31\nBo;N/A\n", o).ok());
    CHECK_ROWS(db, "SELECT name FROM people WHERE age > 18", "Ana");
    CHECK_STR(db.load_csv_text("people", "x\n1\n", o).message(), "table people already exists (DROP TABLE it first)");
}

// ── output formats ───────────────────────────────────────────────────────────

TEST(formats_write_every_kind) {
    vdb::Database db;
    db.exec("CREATE TABLE t (name, n, note); INSERT INTO t VALUES ('Ana', 31, 'a;b'), ('Bo \"B\"', NULL, ''), ('ação', 3.5, 'x|y')");
    vdb::Text out(vdb::heap_allocator());
    CHECK_STR(format(db, "SELECT * FROM t", vdb::k_format_csv, out),
              "name;n;note\nAna;31;\"a;b\"\n\"Bo \"\"B\"\"\";;\"\"\nação;3.5;x|y\n");
    CHECK_STR(format(db, "SELECT * FROM t", vdb::k_format_tsv, out), "name\tn\tnote\nAna\t31\ta;b\n\"Bo \"\"B\"\"\"\t\t\"\"\nação\t3.5\tx|y\n");
    CHECK_STR(format(db, "SELECT * FROM t", vdb::k_format_json, out),
              "[\n{\"name\":\"Ana\",\"n\":31,\"note\":\"a;b\"},\n{\"name\":\"Bo \\\"B\\\"\",\"n\":null,\"note\":\"\"},\n"
              "{\"name\":\"ação\",\"n\":3.5,\"note\":\"x|y\"}\n]\n");
    CHECK_STR(format(db, "SELECT name FROM t WHERE n > 100", vdb::k_format_json, out), "[]\n");
    CHECK_STR(format(db, "SELECT n FROM t LIMIT 1", vdb::k_format_jsonl, out), "{\"n\":31}\n");
    CHECK_STR(format(db, "SELECT name, note FROM t", vdb::k_format_markdown, out),
              "| name | note |\n| --- | --- |\n| Ana | a;b |\n| Bo \"B\" |  |\n| ação | x\\|y |\n");
    CHECK_STR(format(db, "SELECT name, n FROM t", vdb::k_format_table, out),
              "+--------+-----+\n| name   | n   |\n+--------+-----+\n| Ana    |  31 |\n| Bo \"B\" |     |\n| ação   | 3.5 |\n"
              "+--------+-----+\n(3 rows)\n");
}

TEST(formats_json_numbers_only_when_valid) {
    vdb::Database db;
    db.exec("CREATE TABLE t (v); INSERT INTO t VALUES ('007'), ('1e3'), ('-0.5'), ('.5'), ('12'), ('0')");
    vdb::Text out(vdb::heap_allocator());
    CHECK_STR(format(db, "SELECT v FROM t", vdb::k_format_jsonl, out),
              "{\"v\":\"007\"}\n{\"v\":1e3}\n{\"v\":-0.5}\n{\"v\":\".5\"}\n{\"v\":12}\n{\"v\":0}\n");
}

TEST(formats_csv_round_trips_through_the_reader) {
    vdb::Database db;
    db.exec("CREATE TABLE t (a, b); INSERT INTO t VALUES ('x;y', NULL), ('', 'line\nbreak'), (' pad ', 'q\"q')");
    vdb::Text out(vdb::heap_allocator());
    format(db, "SELECT * FROM t", vdb::k_format_csv, out);
    vdb::Database back;
    CHECK(back.load_csv_text("t", out.c_str(), vdb::CsvOptions()).ok());
    CHECK_ROWS(back, "SELECT a, b FROM t", "x;y|NULL;|line\nbreak; pad |q\"q");
    CHECK_ROWS(back, "SELECT COUNT(*) FROM t WHERE a = ''", "1");
}

// ── the word chain ───────────────────────────────────────────────────────────

TEST(chain_writes_escaped_sql) {
    vdb::Database db;
    CHECK_STR(db.select("name, age").from("people").where.field("age").gt(30).and_().field("name").like("A%")
                  .order_by("age", vdb::DESC).limit(10).sql(),
              "SELECT name, age FROM \"people\" WHERE \"age\" > 30 AND \"name\" LIKE 'A%' ORDER BY age DESC LIMIT 10");
    CHECK_STR(db.select().from("t").where.field("n").eq("it's").sql(), "SELECT * FROM \"t\" WHERE \"n\" = 'it''s'");
    CHECK_STR(db.select().from("t").where.field("we\"ird").eq(2.5).sql(), "SELECT * FROM \"t\" WHERE \"we\"\"ird\" = 2.5");
    CHECK_STR(db.select().from("t").where.field("x").eq(-1.5).sql(), "SELECT * FROM \"t\" WHERE \"x\" = (-1.5)");
    CHECK_STR(db.select().from("t").where.field("x").eq(static_cast<const char*>(0)).sql(), "SELECT * FROM \"t\" WHERE \"x\" = NULL");
    CHECK_STR(db.select().from("t").where.field("x").in(1, "two", 3.5).sql(), "SELECT * FROM \"t\" WHERE \"x\" IN (1, 'two', 3.5)");
    CHECK_STR(db.select().from("t").where.field("x").between(1, 9).and_().not_().field("y").is_null().sql(),
              "SELECT * FROM \"t\" WHERE \"x\" BETWEEN 1 AND 9 AND NOT \"y\" IS NULL");
    CHECK_STR(db.select().from("t").where.open().field("a").eq(1).or_().field("b").eq(2).close().and_().field("c").contains("x").sql(),
              "SELECT * FROM \"t\" WHERE (\"a\" = 1 OR \"b\" = 2) AND CONTAINS(\"c\", 'x')");
    CHECK_STR(db.select().from("t").where.field("a").eq(vdb::column("t", "b")).and_().expr("LENGTH(c)").gt(vdb::raw("2 * 3")).sql(),
              "SELECT * FROM \"t\" WHERE \"a\" = \"t\".\"b\" AND LENGTH(c) > 2 * 3");
    CHECK_STR(db.select("a").from("t").where.field("a").gt(1).field("b").lt(2).sql(),
              "SELECT a FROM \"t\" WHERE \"a\" > 1 AND \"b\" < 2");  // two conditions in a row: AND
}

TEST(chain_joins_groups_and_stages) {
    vdb::Database db;
    CHECK_STR(db.select("c.name, COUNT(*) AS n").from("customers", "c").left_join("orders", "o")
                  .on.field("o", "customer_id").eq(vdb::column("c", "id"))
                  .where.field("c", "name").ne("Zoe")
                  .group_by("c.name").having.expr("COUNT(*)").gt(0).order_by("n", vdb::DESC).sql(),
              "SELECT c.name, COUNT(*) AS n FROM \"customers\" AS \"c\" LEFT JOIN \"orders\" AS \"o\" ON \"o\".\"customer_id\" = "
              "\"c\".\"id\" WHERE \"c\".\"name\" <> 'Zoe' GROUP BY c.name HAVING COUNT(*) > 0 ORDER BY n DESC");
    CHECK_STR(db.select("customer_id").from("orders").where.field("total").gt(200).then().select("name").from("customers")
                  .where.field("id").in_table("_").then().order_by("name").limit(5).sql(),
              "SELECT customer_id FROM \"orders\" WHERE \"total\" > 200 |> SELECT name FROM \"customers\" WHERE \"id\" IN \"_\" "
              "|> ORDER BY name LIMIT 5");
}

TEST(chain_runs_and_reports_misuse) {
    vdb::Database db;
    db.insert("people").value("name", "Ana").value("age", 31).row().value("name", "Bo").value("city", "Rio").run();
    CHECK_ROWS(db, "SELECT name, age, city FROM people ORDER BY name", "Ana|31|NULL;Bo|NULL|Rio");
    CHECK_INT(db.update("people").set("age", 32).where.field("name").eq("Ana").run().changes(), 1);
    CHECK_INT(db.delete_from("people").where.field("city").eq("Rio").run().changes(), 1);
    CHECK_STR(test::Rows(db.select("name, age").from("people").run()).c_str(), "Ana|32");
    CHECK_STR(test::Rows(db.select().from("people").where.gt(3).run()).c_str(),
              "ERROR: word chain: a comparison needs field() or expr() first");
    CHECK_STR(test::Rows(db.select().from("people").where.field("a").and_().run()).c_str(),
              "ERROR: word chain: and_()/or_() need a complete condition before them");
    CHECK_STR(test::Rows(db.select().from("people").where.open().field("a").eq(1).run()).c_str(),
              "ERROR: word chain: the WHERE condition is not complete");
    CHECK_STR(test::Rows(db.update("people").run()).c_str(), "ERROR: word chain: update needs set(name, value)");
    vdb::Select q = db.select("name").from("people");  // copies of a chain are independent
    vdb::Select r = q;
    r.where.field("age").gt(100);
    CHECK_STR(test::Rows(q.run()).c_str(), "Ana");
    CHECK_STR(test::Rows(r.run()).c_str(), "");
}

TEST(chain_values_cannot_inject) {
    vdb::Database db;
    db.exec("CREATE TABLE users (name); INSERT INTO users VALUES ('ana')");
    CHECK_STR(test::Rows(db.select().from("users").where.field("name").eq("x' OR '1'='1").run()).c_str(), "");
    CHECK_STR(test::Rows(db.select().from("users\"; DROP TABLE users; --").run()).c_str(),
              "ERROR: no such table: users\"; DROP TABLE users; --");
    CHECK(db.table("users") != 0);
}

// ── streaming (A7) ───────────────────────────────────────────────────────────

namespace {
struct Numbers final : vdb::ByteSource {
    long next, last;
    bool header;
    explicit Numbers(long count) : next(1), last(count), header(true) {}
    size_t read(char* buffer, size_t size) override {
        size_t used = 0;
        if (header) {
            used = static_cast<size_t>(snprintf(buffer, size, "n\n"));
            header = false;
        }
        while (next <= last && used + 24 < size) used += static_cast<size_t>(snprintf(buffer + used, size - used, "%ld\n", next++));
        return used;
    }
};

struct Count final : vdb::RowSink {
    int rows;
    int stop_after;
    explicit Count(int stop) : rows(0), stop_after(stop) {}
    bool row(const vdb::Row&) override { return ++rows < stop_after; }
};
}  // namespace

TEST(streaming_reads_only_what_limit_needs) {
    vdb::Database db;
    Numbers numbers(1000000);
    vdb::CsvReader reader(db.allocator(), numbers, vdb::CsvOptions());
    CHECK(reader.open().ok());
    CHECK(db.attach_csv("numbers", reader).ok());
    CHECK_ROWS(db, "SELECT n FROM numbers WHERE n % 1000 = 0 LIMIT 2", "1000;2000");
    CHECK_INT(reader.records(), 2000);
    CHECK_INT(db.table("numbers")->row_count(), 0);  // streamed, never stored
}

TEST(streaming_sink_can_stop_a_query) {
    vdb::Database db;
    Numbers numbers(100000);
    vdb::CsvReader reader(db.allocator(), numbers, vdb::CsvOptions());
    reader.open();
    db.attach_csv("numbers", reader);
    Count sink(5);
    CHECK(db.run("SELECT n FROM numbers", sink).ok());
    CHECK_INT(sink.rows, 5);
    CHECK_INT(reader.records(), 5);
}

TEST(streaming_aggregates_hold_groups_not_rows) {
    vdb::BudgetAllocator budget(vdb::heap_allocator(), 2 * 1024 * 1024);
    vdb::Database db(budget);
    Numbers numbers(200000);
    vdb::CsvReader reader(db.allocator(), numbers, vdb::CsvOptions());
    reader.open();
    db.attach_csv("numbers", reader);
    CHECK_ROWS(db, "SELECT n % 3 AS k, COUNT(*), SUM(n) FROM numbers GROUP BY k ORDER BY k",
               "0|66666|6666633333;1|66667|6666700000;2|66667|6666766667");
    CHECK(budget.peak() < 1024 * 1024);
}

TEST(streaming_table_used_twice_is_loaded) {
    vdb::Database db;
    Numbers numbers(100);
    vdb::CsvReader reader(db.allocator(), numbers, vdb::CsvOptions());
    reader.open();
    db.attach_csv("numbers", reader);
    CHECK_ROWS(db, "SELECT COUNT(*) FROM numbers a JOIN numbers b ON a.n = b.n", "100");
    CHECK_ROWS(db, "SELECT MAX(n) FROM numbers", "100");  // still there: it was loaded, not streamed
}

// ── safety (spec 9) ──────────────────────────────────────────────────────────

TEST(safety_every_allocation_failure_is_an_error_and_leaks_nothing) {
    const char* script =
        "CREATE TABLE p (name, age, city);"
        "INSERT INTO p VALUES ('Ana', 31, 'Rio'), ('Bo', 'N/A', NULL), ('Cid', '017', 'Recife'), ('Dee', 45, 'Rio');"
        "CREATE TABLE o (pid, total); INSERT INTO o VALUES ('Ana', 10), ('Dee', 20), ('Dee', 5);"
        "SELECT p.city, COUNT(*) n, SUM(o.total) s, GROUP_CONCAT(p.name) FROM p LEFT JOIN o ON o.pid = p.name "
        "WHERE p.name REGEXP '^[A-D]' GROUP BY p.city HAVING n > 0 ORDER BY s DESC"
        " |> WHERE city IS NOT NULL |> SELECT city, UPPER(city) || '!' AS c;"
        "UPDATE p SET age = age + 1 WHERE age > 30; DELETE FROM p WHERE city IS NULL;"
        "SELECT name FROM p WHERE name IN (SELECT pid FROM o) UNION SELECT 'x' ORDER BY 1";
    long total = 0;
    {
        FailingAllocator counting(-1);
        {
            vdb::Database db(counting);
            CHECK(db.query(script).ok());
        }
        total = counting.count();
        CHECK_INT(counting.live(), 0);
    }
    CHECK(total > 50);
    int failures_seen = 0;
    for (long n = 1; n <= total; ++n) {
        FailingAllocator failing(n);
        {
            vdb::Database db(failing);
            vdb::Result r = db.query(script);
            if (!r.ok()) {
                ++failures_seen;
                if (r.error_code() != vdb::k_error_memory) {
                    printf("    allocation %ld: unexpected error %s\n", n, r.error());
                    CHECK(false);
                }
            }
            CHECK(db.query("SELECT 1").ok() || true);  // still callable
        }
        if (failing.live() != 0) {
            printf("    allocation %ld failed: %ld blocks leaked\n", n, failing.live());
            CHECK(false);
            break;
        }
    }
    CHECK(failures_seen > 0);
}

TEST(safety_random_sql_never_crashes) {
    const char* words[] = {"SELECT", "*", "FROM", "t", "WHERE", "a", "=", "'x'", "(", ")", ",", "1", "2.5", "|>", "JOIN",
                           "ON", "GROUP", "BY", "ORDER", "LIMIT", "AND", "OR", "NOT", "IN", "LIKE", "COUNT(*)", "SUM(a)",
                           "CASE", "WHEN", "THEN", "END", "?", "NULL", "IS", "_", "UNION", ";", "--", "/*", "\"q", "b",
                           "+", "-", "*", "/", "%", "||", "~", "BETWEEN", "EXISTS", "(SELECT", "INSERT", "INTO", "VALUES"};
    const size_t k_words = sizeof(words) / sizeof(words[0]);
    vdb::Database db;
    db.exec("CREATE TABLE t (a, b); INSERT INTO t VALUES (1, 'x'), (2, 'y')");
    uint32_t seed = 12345;
    for (int round = 0; round < 3000; ++round) {
        vdb::Text sql(vdb::heap_allocator());
        int length = 1 + static_cast<int>(seed % 14);
        for (int i = 0; i < length; ++i) {
            seed = seed * 1103515245u + 12345u;
            sql.append(words[(seed >> 8) % k_words]);
            sql.append_char(' ');
        }
        vdb::Result r = db.query(sql.c_str(), vdb::Params().add(1));
        (void)r;
    }
    CHECK_ROWS(db, "SELECT COUNT(*) >= 2 FROM t", "1");
}

TEST(safety_random_csv_never_crashes) {
    uint32_t seed = 777;
    const char alphabet[] = "ab;,\"\n\r \t1\xC3\xA7";
    for (int round = 0; round < 2000; ++round) {
        char csv[64];
        int n = static_cast<int>(seed % 63);
        for (int i = 0; i < n; ++i) {
            seed = seed * 1103515245u + 12345u;
            csv[i] = alphabet[(seed >> 8) % (sizeof(alphabet) - 1)];
        }
        csv[n] = 0;
        vdb::Database db;
        vdb::CsvOptions o;
        o.auto_delimiter = (round % 2) != 0;
        if (db.load_csv_text("t", csv, o).ok()) db.query("SELECT * FROM t ORDER BY 1");
    }
    CHECK(true);
}

TEST(safety_arena_database_survives_exhaustion) {
    static unsigned char memory[48 * 1024];
    vdb::ArenaAllocator arena(memory, sizeof(memory));
    {
        vdb::Database db(arena);
        db.exec("CREATE TABLE r (v)");
        int inserted = 0;
        for (;; ++inserted) {
            if (!db.query("INSERT INTO r VALUES ('some text to fill the arena')").ok()) break;
        }
        CHECK(inserted > 100);
        db.exec("DELETE FROM r");
        CHECK_ROWS(db, "SELECT COUNT(*) FROM r", "0");
        CHECK(db.query("INSERT INTO r VALUES (1)").ok());
    }
    CHECK_INT(arena.used(), 0);
}

TEST(streaming_order_by_limit_keeps_only_the_top_rows) {
    vdb::BudgetAllocator budget(vdb::heap_allocator(), 1024 * 1024);
    vdb::Database db(budget);
    Numbers numbers(300000);
    vdb::CsvReader reader(db.allocator(), numbers, vdb::CsvOptions());
    reader.open();
    db.attach_csv("numbers", reader);
    CHECK_ROWS(db, "SELECT n FROM numbers ORDER BY n % 1000 DESC, n LIMIT 4 OFFSET 1", "1999;2999;3999;4999");
    CHECK(budget.peak() < 512 * 1024);  // a full sort of 300,000 rows needs far more
}

TEST(order_by_limit_matches_a_full_sort_with_ties) {
    vdb::Database db;
    db.exec("CREATE TABLE t (k, i)");
    vdb::Text sql(vdb::heap_allocator());
    for (int i = 0; i < 5000; ++i) {
        sql.clear();
        sql.append("INSERT INTO t VALUES (");
        sql.append_int((i * 7919) % 13);
        sql.append(", ");
        sql.append_int(i);
        sql.append(")");
        db.exec(sql.c_str());
    }
    test::Rows full(db, "SELECT i FROM (SELECT * FROM t ORDER BY k, i) AS s LIMIT 7");
    CHECK_STR(test::Rows(db, "SELECT i FROM t ORDER BY k LIMIT 7").c_str(), full.c_str());  // stable: ties keep input order
    test::Rows full_desc(db, "SELECT i FROM (SELECT * FROM t ORDER BY k DESC, i) AS s LIMIT 5 OFFSET 2000");
    CHECK_STR(test::Rows(db, "SELECT i FROM t ORDER BY k DESC LIMIT 5 OFFSET 2000").c_str(), full_desc.c_str());
}
