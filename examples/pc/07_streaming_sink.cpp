// 07: streaming. A RowSink gets rows as they are produced; returning false stops the query early.
#include <stdio.h>

#include <vdb.h>

struct PrintFirst : vdb::RowSink {
    int seen;
    PrintFirst() : seen(0) {}
    bool begin(size_t columns, const char* const* names) override {
        printf("columns:");
        for (size_t i = 0; i < columns; ++i) printf(" %s", names[i]);
        printf("\n");
        return true;
    }
    bool row(const vdb::Row& row) override {
        printf("  n=%s square=%s\n", row["n"].text(), row["square"].text());
        return ++seen < 3;  // enough after 3 rows
    }
};

// A CSV source that makes rows on the fly, as a serial port or a sensor would.
struct Counter : vdb::ByteSource {
    long next;
    long last;
    bool header;
    Counter(long count) : next(1), last(count), header(true) {}
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

int main() {
    vdb::Database db;
    Counter numbers(1000000);
    vdb::CsvReader reader(db.allocator(), numbers, vdb::CsvOptions());
    reader.open();
    db.attach_csv("numbers", reader);  // read lazily: nothing is loaded yet

    PrintFirst sink;
    db.run("SELECT n, n * n AS square FROM numbers WHERE n % 7 = 0", sink);
    printf("rows read from the source: %zu of 1000000 (the sink stopped the query)\n", reader.records());
    return 0;
}
