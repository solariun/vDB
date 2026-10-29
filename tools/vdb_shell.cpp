// vdb: the interactive SQL shell (spec 7.4).
#define _POSIX_C_SOURCE 200809L  // fileno, isatty, clock_gettime, opendir under -std=c++11

#include <stdlib.h>
#include <string.h>

#include "cli.hpp"
#include "vdb/sql_parser.hpp"

namespace {

const char* const k_program = "vdb";

const char k_help[] =
    "vdb - the vDB interactive SQL shell (vDB " VDB_VERSION ")\n"
    "\n"
    "Usage:\n"
    "  vdb [options] [script.sql ...]   run scripts, then read SQL from stdin\n"
    "  vdb [options] -c \"SQL\"           run SQL and exit\n"
    "\n"
    "Options:\n"
    "  -c, --command SQL          run SQL and exit\n"
    "  -t, --table NAME=FILE      load a CSV file as table NAME (repeatable)\n"
    "  -o, --output FMT           csv, tsv, table, json, jsonl, markdown\n"
    "                             (default: table on a terminal, csv otherwise)\n"
    "  -d, -r, -H, -D, --quote, --trim, --empty-text, --decimal-comma, --strict,\n"
    "  --null, --no-output-header, --memory-limit: as in sel (see sel --help)\n"
    "  -h, --help                 this text\n"
    "\n"
    "In the shell, statements end with ';'. FROM may name CSV files: SELECT * FROM 'people.csv'\n"
    "or FROM people.csv. Type .help for the dot commands.\n";

const char k_dot_help[] =
    ".help                   this text\n"
    ".tables                 the tables and their sizes\n"
    ".schema [TABLE]         the columns of a table (or of all)\n"
    ".load FILE [NAME]       load a CSV file as a table (named after the file)\n"
    ".save TABLE FILE        write a table as CSV (with the output delimiter)\n"
    ".mode FMT               csv, tsv, table, json, jsonl, markdown\n"
    ".delimiter C            input delimiter for .load and FROM files (tab, comma, ...)\n"
    ".headers on|off         header line in csv, tsv, table and markdown\n"
    ".nullvalue TEXT         how NULL is shown\n"
    ".timer on|off           time each statement\n"
    ".read FILE              run the SQL in FILE\n"
    ".quit                   leave (also .exit or Ctrl-D)\n";

struct Shell {
    vdb::Database* db;
    cli::Settings* settings;
    cli::Files* files;
    bool timer;
    int failures;
};

char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t capacity = 4096, size = 0;
    char* text = static_cast<char*>(malloc(capacity));
    for (size_t got; text && (got = fread(text + size, 1, capacity - size - 1, f)) > 0;) {
        size += got;
        if (size + 1 >= capacity) {
            capacity *= 2;
            char* grown = static_cast<char*>(realloc(text, capacity));
            if (!grown) {
                free(text);
                text = 0;
            } else {
                text = grown;
            }
        }
    }
    fclose(f);
    if (text) text[size] = 0;
    return text;
}

void run_sql(Shell& sh, const char* sql) {
    cli::FileOutput out(stdout);
    cli::Printer formatter(out, *sh.settings, *sh.files);
    double start = cli::now_seconds();
    vdb::Status status = sh.db->run(sql, formatter);
    fflush(stdout);
    if (!status.ok()) {
        fprintf(stderr, "error: %s\n", status.message());
        ++sh.failures;
    }
    if (sh.timer) fprintf(stderr, "time: %.3f s\n", cli::now_seconds() - start);
}

void show_schema(Shell& sh, vdb::Table* t) {
    printf("%s (%zu rows):", t->name(), t->row_count());
    for (size_t c = 0; c < t->column_count(); ++c) printf("%s %s", c ? "," : "", t->column_name(c));
    printf("\n");
    (void)sh;
}

void save_table(Shell& sh, const char* table, const char* path) {
    vdb::Table* t = sh.db->table(table);
    if (!t) {
        fprintf(stderr, "error: no such table: %s\n", table);
        return;
    }
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "error: cannot write %s\n", path);
        return;
    }
    cli::FileOutput out(f);
    vdb::FormatOptions options = sh.settings->format;
    options.format = vdb::k_format_csv;
    vdb::Formatter formatter(out, options, vdb::heap_allocator());
    vdb::Text sql(vdb::heap_allocator());
    sql.append("SELECT * FROM \"");
    for (const char* p = table; *p; ++p) {
        if (*p == '"') sql.append_char('"');
        sql.append_char(*p);
    }
    sql.append_char('"');
    vdb::Status status = sh.db->run(sql.c_str(), formatter);
    fclose(f);
    if (!status.ok()) fprintf(stderr, "error: %s\n", status.message());
    else printf("saved %zu rows to %s\n", formatter.rows(), path);
}

// Splits "word word rest" in place.
char* next_word(char*& p) {
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) return 0;
    char* start = p;
    while (*p && *p != ' ' && *p != '\t') ++p;
    if (*p) *p++ = 0;
    return start;
}

// Returns false on .quit.
bool dot_command(Shell& sh, char* line) {
    char* p = line;
    char* command = next_word(p);
    char* a = next_word(p);
    char* b = next_word(p);
    if (!strcmp(command, ".quit") || !strcmp(command, ".exit")) return false;
    if (!strcmp(command, ".help")) {
        fputs(k_dot_help, stdout);
    } else if (!strcmp(command, ".tables")) {
        for (size_t i = 0; i < sh.db->table_count(); ++i) {
            vdb::Table* t = sh.db->table_at(i);
            printf("%-24s %zu rows, %zu columns%s\n", t->name(), t->row_count(), t->column_count(),
                   t->stream_pending() ? " (not read yet)" : "");
        }
    } else if (!strcmp(command, ".schema")) {
        for (size_t i = 0; i < sh.db->table_count(); ++i) {
            vdb::Table* t = sh.db->table_at(i);
            if (!a || vdb::names_equal(a, t->name())) show_schema(sh, t);
        }
    } else if (!strcmp(command, ".load") && a) {
        vdb::Status status;
        const char* name = b;
        char* alias = 0;
        if (!name) {
            vdb::Pool pool(vdb::heap_allocator());
            const char* base = vdb::default_alias(pool, a);
            alias = base ? cli::copy_text(base) : 0;
            name = alias;
        }
        vdb::Table* t = name ? sh.files->open(name, a, status) : 0;
        if (t && t->stream_pending() && !t->load_stream(status)) t = 0;
        if (!t) {
            fprintf(stderr, "error: %s\n", status.ok() ? "cannot open the file" : status.message());
        } else {
            printf("loaded %s: %zu rows, %zu columns\n", t->name(), t->row_count(), t->column_count());
        }
        free(alias);
    } else if (!strcmp(command, ".save") && a && b) {
        save_table(sh, a, b);
    } else if (!strcmp(command, ".mode") && a) {
        if (!vdb::parse_format(a, sh.settings->format.format)) fprintf(stderr, "error: unknown mode %s\n", a);
    } else if (!strcmp(command, ".delimiter") && a) {
        bool is_auto;
        if (!cli::parse_delimiter(a, sh.settings->csv.delimiter, is_auto)) fprintf(stderr, "error: unknown delimiter %s\n", a);
        sh.settings->csv.auto_delimiter = is_auto;
        sh.settings->delimiter_given = true;
        if (!sh.settings->output_delimiter_given && !is_auto) sh.settings->format.delimiter = sh.settings->csv.delimiter;
    } else if (!strcmp(command, ".headers") && a) {
        sh.settings->format.header = !strcmp(a, "on");
    } else if (!strcmp(command, ".nullvalue") && a) {
        static char null_text[64];
        strncpy(null_text, a, sizeof(null_text) - 1);
        sh.settings->format.null_text = null_text;
    } else if (!strcmp(command, ".timer") && a) {
        sh.timer = !strcmp(a, "on");
    } else if (!strcmp(command, ".read") && a) {
        char* text = read_file(a);
        if (!text) fprintf(stderr, "error: cannot read %s\n", a);
        else run_sql(sh, text);
        free(text);
    } else {
        fprintf(stderr, "error: unknown or incomplete command %s (type .help)\n", command);
    }
    return true;
}

// True when `sql` ends a statement: a ';' outside quotes and comments as the last non-space character.
bool complete(const char* sql) {
    char quote = 0;
    bool ends = false;
    for (const char* p = sql; *p; ++p) {
        if (quote) {
            if (*p == quote) quote = 0;
            continue;
        }
        if (*p == '-' && p[1] == '-') {
            while (*p && *p != '\n') ++p;
            if (!*p) break;
            continue;
        }
        if (*p == '\'' || *p == '"') {
            quote = *p;
            ends = false;
        } else if (*p == ';') {
            ends = true;
        } else if (!vdb::is_space(*p)) {
            ends = false;
        }
    }
    return ends && !quote;
}

void interactive(Shell& sh) {
    bool terminal = cli::stdin_is_terminal();
    if (terminal) printf("vDB %s - type SQL ending with ';', or .help\n", VDB_VERSION);
    vdb::Text buffer(vdb::heap_allocator());
    const size_t k_line = 65536;
    char* line = static_cast<char*>(malloc(k_line));
    if (!line) return;
    for (;;) {
        if (terminal) {
            fputs(buffer.empty() ? "vdb> " : "...> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, static_cast<int>(k_line), stdin)) break;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (buffer.empty() && line[0] == '.') {
            if (!dot_command(sh, line)) break;
            continue;
        }
        if (!buffer.empty()) buffer.append_char('\n');
        buffer.append(line, n);
        if (complete(buffer.c_str())) {
            run_sql(sh, buffer.c_str());
            buffer.clear();
        }
    }
    if (!buffer.empty()) {
        bool blank = true;
        for (const char* p = buffer.c_str(); *p && blank; ++p) blank = vdb::is_space(*p);
        if (!blank) run_sql(sh, buffer.c_str());
    }
    free(line);
    if (terminal) printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    cli::Settings settings;
    settings.streaming = false;
    settings.implicit_stdin = false;  // in vdb a SELECT without FROM is one row (spec A9)
    settings.format.format = cli::stdout_is_terminal() ? vdb::k_format_table : vdb::k_format_csv;
    const char* command = 0;
    const char** tables = static_cast<const char**>(malloc(sizeof(char*) * static_cast<size_t>(argc + 1)));
    const char** scripts = static_cast<const char**>(malloc(sizeof(char*) * static_cast<size_t>(argc + 1)));
    int table_count = 0, script_count = 0;
    if (!tables || !scripts) return 1;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        int used = cli::parse_common_option(k_program, argc, argv, i, settings);
        if (used < 0) return 2;
        if (used) continue;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(k_help, stdout);
            return 0;
        }
        if ((!strcmp(a, "-c") || !strcmp(a, "--command")) && i + 1 < argc) {
            command = argv[++i];
        } else if ((!strcmp(a, "-t") || !strcmp(a, "--table")) && i + 1 < argc) {
            tables[table_count++] = argv[++i];
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "%s: unknown option %s (see vdb --help)\n", k_program, a);
            return 2;
        } else {
            scripts[script_count++] = a;
        }
    }
    if (!settings.output_delimiter_given) settings.format.delimiter = settings.csv.delimiter;
    vdb::BudgetAllocator budget(vdb::heap_allocator(), settings.memory_limit ? settings.memory_limit : static_cast<size_t>(-1));
    vdb::Database db(budget);
    if (settings.decimal_comma) db.options().decimal_separator = ',';
    db.options().streaming = false;
    cli::Files files(db, settings, k_program);
    for (int i = 0; i < table_count; ++i) {
        if (!files.add_table(tables[i])) return 3;
    }
    files.install();
    Shell sh = {&db, &settings, &files, false, 0};
    for (int i = 0; i < script_count; ++i) {
        char* text = read_file(scripts[i]);
        if (!text) {
            fprintf(stderr, "%s: cannot read %s\n", k_program, scripts[i]);
            return 3;
        }
        run_sql(sh, text);
        free(text);
    }
    if (command) {
        run_sql(sh, command);
    } else if (!script_count || !cli::stdin_is_terminal()) {
        settings.stdin_is_sql = true;
        interactive(sh);
    }
    files.report_notes();
    free(tables);
    free(scripts);
    return sh.failures ? 1 : 0;
}
