#define _POSIX_C_SOURCE 200809L  // fileno, isatty, clock_gettime, opendir under -std=c++11

#include "cli.hpp"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace cli {

char* copy_text(const char* text) {
    size_t n = strlen(text);
    char* copy = static_cast<char*>(malloc(n + 1));
    if (copy) memcpy(copy, text, n + 1);
    return copy;
}

bool stdin_is_terminal() { return isatty(fileno(stdin)) != 0; }
bool stdout_is_terminal() { return isatty(fileno(stdout)) != 0; }

double now_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

bool parse_delimiter(const char* text, char& out, bool& is_auto) {
    is_auto = false;
    struct Name {
        const char* name;
        char c;
    };
    static const Name k_names[] = {{"tab", '\t'},   {"\\t", '\t'},  {"comma", ','},  {"semicolon", ';'}, {"colon", ':'},
                                   {"pipe", '|'},   {"space", ' '}, {"bar", '|'}};
    for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); ++i) {
        if (vdb::names_equal(text, k_names[i].name)) {
            out = k_names[i].c;
            return true;
        }
    }
    if (vdb::names_equal(text, "auto")) {
        is_auto = true;
        return true;
    }
    if (strlen(text) == 1) {
        out = text[0];
        return true;
    }
    return false;
}

bool parse_size(const char* text, size_t& out) {
    char* end = 0;
    double v = strtod(text, &end);
    if (end == text || v < 0) return false;
    const double k_kilo = 1024.0;
    switch (*end) {
        case 0: break;
        case 'k': case 'K': v *= k_kilo; ++end; break;
        case 'm': case 'M': v *= k_kilo * k_kilo; ++end; break;
        case 'g': case 'G': v *= k_kilo * k_kilo * k_kilo; ++end; break;
        default: return false;
    }
    if (*end == 'B' || *end == 'b') ++end;
    if (*end) return false;
    out = static_cast<size_t>(v);
    return true;
}

namespace {
bool takes_value(int argc, int i, const char* program, const char* option) {
    if (i + 1 < argc) return true;
    fprintf(stderr, "%s: %s needs a value (see --help)\n", program, option);
    return false;
}

bool is(const char* arg, const char* short_name, const char* long_name) {
    return (short_name && strcmp(arg, short_name) == 0) || (long_name && strcmp(arg, long_name) == 0);
}
}  // namespace

int parse_common_option(const char* program, int argc, char** argv, int& i, Settings& s) {
    const char* a = argv[i];
    if (is(a, "-d", "--delimiter")) {
        if (!takes_value(argc, i, program, a)) return -1;
        bool is_auto;
        if (!parse_delimiter(argv[++i], s.csv.delimiter, is_auto)) {
            fprintf(stderr, "%s: unknown delimiter '%s' (one character, or tab, comma, semicolon, colon, pipe, space, auto)\n",
                    program, argv[i]);
            return -1;
        }
        s.csv.auto_delimiter = is_auto;
        s.delimiter_given = true;
        return 1;
    }
    if (is(a, "-r", "--regex-delimiter")) {
        if (!takes_value(argc, i, program, a)) return -1;
        s.csv.delimiter_regex = argv[++i];
        s.delimiter_given = true;
        return 1;
    }
    if (is(a, "-D", "--output-delimiter")) {
        if (!takes_value(argc, i, program, a)) return -1;
        bool is_auto;
        if (!parse_delimiter(argv[++i], s.format.delimiter, is_auto) || is_auto) {
            fprintf(stderr, "%s: unknown output delimiter '%s'\n", program, argv[i]);
            return -1;
        }
        s.output_delimiter_given = true;
        return 1;
    }
    if (is(a, "-o", "--output")) {
        if (!takes_value(argc, i, program, a)) return -1;
        if (!vdb::parse_format(argv[++i], s.format.format)) {
            fprintf(stderr, "%s: unknown output format '%s' (csv, tsv, table, json, jsonl, markdown)\n", program, argv[i]);
            return -1;
        }
        return 1;
    }
    if (is(a, "-H", "--no-header")) {
        s.csv.header = false;
        return 1;
    }
    if (is(a, 0, "--no-output-header")) {
        s.format.header = false;
        return 1;
    }
    if (is(a, 0, "--quote")) {
        if (!takes_value(argc, i, program, a)) return -1;
        const char* q = argv[++i];
        if (strlen(q) > 1) {
            fprintf(stderr, "%s: --quote takes one character (or \"\" for none)\n", program);
            return -1;
        }
        s.csv.quote = q[0];
        s.format.quote = q[0] ? q[0] : '"';
        return 1;
    }
    if (is(a, 0, "--trim")) {
        s.csv.trim = true;
        return 1;
    }
    if (is(a, 0, "--empty-text")) {
        s.csv.empty_text = true;
        return 1;
    }
    if (is(a, 0, "--decimal-comma")) {
        s.decimal_comma = true;
        return 1;
    }
    if (is(a, 0, "--null")) {
        if (!takes_value(argc, i, program, a)) return -1;
        s.format.null_text = argv[++i];
        return 1;
    }
    if (is(a, 0, "--strict")) {
        s.csv.strict = true;
        return 1;
    }
    if (is(a, 0, "--stats")) {
        s.stats = true;
        return 1;
    }
    if (is(a, 0, "--memory-limit")) {
        if (!takes_value(argc, i, program, a)) return -1;
        if (!parse_size(argv[++i], s.memory_limit)) {
            fprintf(stderr, "%s: bad size '%s' (examples: 512M, 2G, 100000)\n", program, argv[i]);
            return -1;
        }
        return 1;
    }
    return 0;
}

// ── Files ────────────────────────────────────────────────────────────────────

Files::Files(vdb::Database& db, Settings& settings, const char* program)
    : db_(db), settings_(settings), program_(program), entries_(vdb::heap_allocator()) {}

Files::~Files() {
    vdb::Allocator& a = vdb::heap_allocator();
    for (size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        vdb::destroy(a, e.reader);
        vdb::destroy(a, e.source);
        if (e.file && e.file != stdin) fclose(e.file);
        free(e.name);
        free(e.path);
    }
}

namespace {

vdb::Table* resolve(vdb::Database& db, const char* name, void* user, vdb::Status& status) {
    (void)db;
    Files* files = static_cast<Files*>(user);
    return files->open(name, name, status);
}

bool is_stdin_name(const char* name) {
    return strcmp(name, "-") == 0 || vdb::names_equal(name, "stdin") || strcmp(name, "_") == 0;
}

bool ends_with(const char* s, const char* tail) {
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && vdb::names_equal(s + n - m, m, tail);
}

}  // namespace

vdb::Table* Files::open(const char* name, const char* path, vdb::Status& status) {
    FILE* file = 0;
    bool from_stdin = is_stdin_name(path);
    if (from_stdin) {
        if (stdin_is_terminal()) {
            if (strcmp(path, "_") == 0) return 0;  // no input: SELECT without FROM gives one row
            status.fail(vdb::k_error_io, VDB_F("stdin is a terminal: pipe a CSV file in, or name a file in FROM"));
            return 0;
        }
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].file == stdin) {
                status.fail(vdb::k_error_io, VDB_F("stdin can be read only once (it is already the table %s)"), entries_[i].name);
                return 0;
            }
        }
        file = stdin;
    } else {
        file = fopen(path, "rb");
        if (!file && !strchr(path, '.')) {
            // FROM people finds people.csv.
            size_t n = strlen(path);
            char* with = static_cast<char*>(malloc(n + 5));
            if (with) {
                memcpy(with, path, n);
                memcpy(with + n, ".csv", 5);
                file = fopen(with, "rb");
                free(with);
            }
        }
        if (!file) {
            if (strchr(path, '/') || strchr(path, '.')) {
                status.fail(vdb::k_error_io, VDB_F("cannot open %s: %s"), path, strerror(errno));
            }
            return 0;
        }
    }
    vdb::Allocator& a = vdb::heap_allocator();
    Entry e;
    memset(&e, 0, sizeof(e));
    e.file = file;
    e.name = cli::copy_text(name);
    e.path = cli::copy_text(path);
    void* sm = a.allocate(sizeof(FileSource));
    e.source = sm ? new (sm, vdb::PlaceTag()) FileSource(file) : 0;
    vdb::CsvOptions options = settings_.csv;
    if (!settings_.delimiter_given && (ends_with(path, ".tsv") || ends_with(path, ".tab"))) options.delimiter = '\t';
    void* rm = a.allocate(sizeof(vdb::CsvReader));
    e.reader = (rm && e.source) ? new (rm, vdb::PlaceTag()) vdb::CsvReader(db_.allocator(), *e.source, options) : 0;
    if (!e.name || !e.path || !e.source || !e.reader || !entries_.push(e)) {
        status.fail_memory();
        return 0;
    }
    status = e.reader->open();
    if (!status.ok()) {
        vdb::Status wrapped;
        wrapped.fail(status.code(), VDB_F("%s: %s"), path, status.message());
        status = wrapped;
        return 0;
    }
    status = db_.attach_csv(name, *e.reader);
    if (!status.ok()) return 0;
    if (!settings_.streaming) {
        vdb::Table* t = db_.table(name);
        if (!t->load_stream(status)) return 0;
    }
    return db_.table(name);
}

bool Files::add_table(const char* spec) {
    const char* eq = strchr(spec, '=');
    vdb::Status status;
    vdb::Table* t;
    if (eq) {
        size_t n = static_cast<size_t>(eq - spec);
        char* name = static_cast<char*>(malloc(n + 1));
        if (!name) return false;
        memcpy(name, spec, n);
        name[n] = 0;
        t = open(name, eq + 1, status);
        free(name);
    } else {
        t = open(spec, spec, status);
    }
    if (!t) {
        if (status.ok()) status.fail(vdb::k_error_io, VDB_F("cannot open %s"), eq ? eq + 1 : spec);
        fprintf(stderr, "%s: %s\n", program_, status.message());
        return false;
    }
    return true;
}

void Files::install() {
    db_.options().table_resolver = resolve;
    db_.options().resolver_user = this;
}

size_t Files::rows_read() const {
    size_t n = 0;
    for (size_t i = 0; i < entries_.size(); ++i) n += entries_[i].reader->records();
    return n;
}

void Files::report_notes() {
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        vdb::CsvReader& r = *e.reader;
        const char* shown = e.file == stdin ? "stdin" : e.path;
        if (settings_.csv.strict) continue;  // strict mode already stopped with the line number
        if (r.short_rows()) {
            fprintf(stderr, "%s: note: %s: %zu rows with fewer fields than the header (the missing fields read NULL)\n",
                    program_, shown, r.short_rows());
        }
        if (r.long_rows()) {
            fprintf(stderr, "%s: note: %s: %zu rows with more fields than the header (extra fields ignored; --strict stops)\n",
                    program_, shown, r.long_rows());
        }
        if (!settings_.delimiter_given && r.column_count() == 1) {
            const char* only = r.column_name(0);
            const char* found = strchr(only, ',') ? "," : strchr(only, '\t') ? "tab" : strchr(only, '|') ? "|" : 0;
            if (found) {
                fprintf(stderr, "%s: note: %s has one column; its header holds '%s': try -d %s (or -d auto)\n", program_, shown,
                        strcmp(found, "tab") == 0 ? "\\t" : found, found);
            }
        }
    }
}

}  // namespace cli
