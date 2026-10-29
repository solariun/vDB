// sel: a SELECT typed on the command line, over CSV files (spec 7.1 – 7.3, D5).
#define _POSIX_C_SOURCE 200809L  // fileno, isatty, clock_gettime, opendir under -std=c++11

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#include "cli.hpp"

namespace {

const char* const k_program = "sel";

enum Exit { k_exit_ok = 0, k_exit_sql = 1, k_exit_usage = 2, k_exit_file = 3 };

const char k_help[] =
    "sel - a SELECT typed on the command line, over CSV files (vDB " VDB_VERSION ")\n"
    "\n"
    "Usage:\n"
    "  sel [options] <columns> [from <file|->] [join <file> on ...] [where ...]\n"
    "      [group by ...] [having ...] [order by ...] [limit n] [|> stage ...]\n"
    "  sel [options] -q \"full SQL; more SQL\"      (INSERT, UPDATE, CREATE, ... too)\n"
    "  sel [options] -f script.sql\n"
    "\n"
    "Write the SELECT you would write, with sel in place of SELECT. Files in FROM and\n"
    "JOIN are read as tables named after the file (people.csv is people). Without\n"
    "FROM, the CSV comes from stdin. The first line of a CSV names its fields.\n"
    "\n"
    "Examples:\n"
    "  sel name, age from people.csv where \"age > 30\" order by age desc\n"
    "  sel \"*\" from people.csv where \"city like 'Rio%'\" -o table\n"
    "  sel city, \"count(*) as n\" from people.csv group by city order by n desc\n"
    "  sel \"o.id, c.name\" from orders.csv o join customers.csv c on \"o.customer_id = c.id\"\n"
    "  sel \"name, age from people.csv |> where age > 30 |> select name |> order by name\"\n"
    "  cat people.csv | sel \"count(*)\"\n"
    "  sel \"*\" from data.txt -r '\\s+' -o json          (fields split by any spaces)\n"
    "\n"
    "Quote what the shell would change: > < ( ) * ; | & and 'text'.\n"
    "  where age > 30        the shell writes the output to a file named 30!\n"
    "  where \"age > 30\"      right\n"
    "  where \"name = 'Ana'\"  right (the inner quotes reach sel)\n"
    "  sel \"name, age from people.csv where age > 30\"   quoting everything always works\n"
    "\n"
    "Input:\n"
    "  -d, --delimiter C         field delimiter: one character, or tab, comma, semicolon,\n"
    "                            colon, pipe, space, auto (default ;)\n"
    "  -r, --regex-delimiter RE  split fields with a regular expression\n"
    "  -H, --no-header           files have no header line (columns c1, c2, ...)\n"
    "      --quote C             quote character (default \")\n"
    "      --trim                strip spaces around unquoted fields\n"
    "      --empty-text          unquoted empty fields are '' instead of NULL\n"
    "      --decimal-comma       numbers use a decimal comma (3,14)\n"
    "      --strict              rows with too few or too many fields are errors\n"
    "  -t, --table NAME=FILE     load FILE as table NAME (repeatable)\n"
    "Output:\n"
    "  -o, --output FMT          csv (default), tsv, table, json, jsonl, markdown\n"
    "  -D, --output-delimiter C  output delimiter (default: the input one)\n"
    "      --no-output-header    no header line\n"
    "      --null TEXT           how NULL is written (default: empty)\n"
    "Query:\n"
    "  -q, --query SQL           run full SQL statements\n"
    "  -f, --file FILE           run the SQL in FILE\n"
    "      --show-sql            print the SQL built from the words, then run it\n"
    "      --memory-limit SIZE   stop with an error beyond SIZE (512K, 64M, 2G)\n"
    "      --stats               rows, memory and time on stderr\n"
    "  --                        everything after it is query text\n"
    "Help:\n"
    "  -h, --help                this text\n"
    "      --help-sql            the SQL vDB understands\n"
    "      --functions           every function\n"
    "  -V, --version             the version\n"
    "\n"
    "Exit status: 0 success, 1 SQL or data error, 2 usage error, 3 file error.\n";

const char k_help_sql[] =
    "SQL in vDB (full text: spec.md section 3, README)\n"
    "\n"
    "SELECT [DISTINCT] cols FROM t [AS a] [[INNER|LEFT|RIGHT|FULL|CROSS] JOIN u ON ...|USING (c)]\n"
    "       [WHERE ...] [GROUP BY ...] [HAVING ...] [ORDER BY x [ASC|DESC] [NULLS FIRST|LAST]]\n"
    "       [LIMIT n [OFFSET m]]   UNION [ALL] | INTERSECT | EXCEPT   WITH name AS (...) SELECT ...\n"
    "pipelines:  SELECT ... |> WHERE ... |> SELECT ... |> ORDER BY ... |> LIMIT n   (_ = the previous result)\n"
    "data:       INSERT INTO t [(a, b)] VALUES (...), (...) | SELECT ...;  UPDATE t SET a = x WHERE ...;\n"
    "            DELETE FROM t WHERE ...;  CREATE TABLE t (a, b) | AS SELECT ...;  DROP TABLE [IF EXISTS] t\n"
    "\n"
    "operators:  + - * / %  ||  = <> != < <= > >=  AND OR NOT  IS [NOT] NULL  BETWEEN a AND b\n"
    "            IN (list | SELECT ... | table)  EXISTS (SELECT ...)  CASE WHEN ... THEN ... ELSE ... END\n"
    "text match: LIKE 'A%' (_ one char)  ILIKE (any case)  GLOB '*.csv'  SIMILAR TO '(a|b)%'\n"
    "            REGEXP 'pat' / RLIKE / ~ 'pat' / ~* 'pat' (any case) / !~   CONTAINS(x, 'y')\n"
    "\n"
    "Numbers and text are flexible: a field is text that reads as a number when the operation\n"
    "needs one. age > 30 compares numbers (a non-number like 'N/A' does not match);\n"
    "zip = '01234' compares text (quoted); phone LIKE '%555%' looks for digits as text.\n"
    "Empty CSV fields are NULL; \"\" (quoted empty) is the empty text.\n";

struct Words {
    char* text;
    size_t size;
    size_t capacity;
};

bool append(Words& w, const char* s) {
    size_t n = strlen(s);
    if (w.size + n + 2 > w.capacity) {
        size_t capacity = (w.size + n + 2) * 2;
        char* grown = static_cast<char*>(realloc(w.text, capacity));
        if (!grown) return false;
        w.text = grown;
        w.capacity = capacity;
    }
    if (w.size) w.text[w.size++] = ' ';
    memcpy(w.text + w.size, s, n);
    w.size += n;
    w.text[w.size] = 0;
    return true;
}

void print_function(const char* name, const vdb::FunctionInfo& info, void* user) {
    int* column = static_cast<int*>(user);
    char args[24];
    if (info.max_args < 0) snprintf(args, sizeof(args), "(%d+)", info.min_args);
    else if (info.min_args == info.max_args) snprintf(args, sizeof(args), "(%d)", info.min_args);
    else snprintf(args, sizeof(args), "(%d-%d)", info.min_args, info.max_args);
    char item[48];
    snprintf(item, sizeof(item), "%s%s%s", name, args, (info.flags & vdb::fn_aggregate) ? "*" : "");
    printf("%-26s", item);
    const int k_columns = 3;
    if (++*column % k_columns == 0) printf("\n");
}

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

// The shell turned * into the names of the files here (spec 7.3): the words before FROM are exactly them.
bool shell_expanded_star(int count, char** words) {
    int from = -1;
    for (int i = 0; i < count && from < 0; ++i) {
        if (vdb::names_equal(words[i], "from")) from = i;
    }
    if (from <= 0) return false;
    DIR* dir = opendir(".");
    if (!dir) return false;
    int entries = 0;
    bool all_present = true;
    bool dotted = false;
    for (struct dirent* d; (d = readdir(dir)) != 0;) {
        if (d->d_name[0] == '.') continue;
        ++entries;
        bool present = false;
        for (int i = 0; i < from && !present; ++i) present = strcmp(words[i], d->d_name) == 0;
        if (!present) all_present = false;
        if (strchr(d->d_name, '.')) dotted = true;
    }
    closedir(dir);
    return entries > 0 && all_present && entries <= from && (entries >= 2 || dotted);
}

// A WHERE or HAVING followed by one bare word: a > or < may have become a redirection (spec 7.3).
void warn_bare_condition(int count, char** words) {
    for (int i = 0; i + 1 < count; ++i) {
        if (!vdb::names_equal(words[i], "where") && !vdb::names_equal(words[i], "having")) continue;
        const char* next = words[i + 1];
        bool bare = *next != 0;
        for (const char* p = next; *p && bare; ++p) bare = vdb::is_alpha(*p) || vdb::is_digit(*p) || *p == '_' || *p == '.';
        bool ends = i + 2 >= count || vdb::names_equal(words[i + 2], "group") || vdb::names_equal(words[i + 2], "order") ||
                    vdb::names_equal(words[i + 2], "limit") || vdb::names_equal(words[i + 2], "having") ||
                    strcmp(words[i + 2], "|>") == 0;
        if (bare && ends) {
            fprintf(stderr,
                    "%s: warning: \"%s %s\" has no comparison. If you typed > or <, the shell took it as a\n"
                    "     redirection (and made a file): quote the condition, e.g. %s \"%s > 30\"\n",
                    k_program, words[i], next, words[i], next);
        }
    }
}

// True when `name` appears in `sql` right after a comparison: then it was meant as a value.
bool after_comparison(const char* sql, const char* name) {
    size_t n = strlen(name);
    if (!n) return false;
    for (const char* hit = strstr(sql, name); hit; hit = strstr(hit + 1, name)) {
        bool starts = hit == sql || !(vdb::is_alpha(hit[-1]) || vdb::is_digit(hit[-1]) || hit[-1] == '_');
        bool ends = !(vdb::is_alpha(hit[n]) || vdb::is_digit(hit[n]) || hit[n] == '_');
        if (!starts || !ends) continue;
        const char* p = hit;
        while (p > sql && vdb::is_space(p[-1])) --p;
        if (p > sql && (p[-1] == '=' || p[-1] == '<' || p[-1] == '>')) return true;
        if (p > sql && p[-1] == '(') {
            // IN (Ana, ...): the word before the parenthesis is IN.
            const char* q = p - 1;
            while (q > sql && vdb::is_space(q[-1])) --q;
            if (q - sql >= 2 && vdb::names_equal(q - 2, 2, "in") && (q - 2 == sql || !vdb::is_alpha(q[-3]))) return true;
        }
        const char* word_end = p;
        while (p > sql && vdb::is_alpha(p[-1])) --p;
        size_t w = static_cast<size_t>(word_end - p);
        if (vdb::names_equal(p, w, "like") || vdb::names_equal(p, w, "ilike") || vdb::names_equal(p, w, "glob") ||
            vdb::names_equal(p, w, "regexp") || vdb::names_equal(p, w, "in"))
            return true;
    }
    return false;
}

void hint(const vdb::Status& status, const char* sql) {
    const char* m = status.message();
    const char k_column[] = "no such column: ";
    if (strncmp(m, k_column, sizeof(k_column) - 1) == 0) {
        char name[64];
        size_t n = 0;
        for (const char* p = m + sizeof(k_column) - 1; *p && *p != ' ' && n + 1 < sizeof(name); ++p) name[n++] = *p;
        name[n] = 0;
        if (after_comparison(sql, name)) {
            fprintf(stderr, "%s: hint: if %s is a text value, quote it so the shell keeps the quotes: \"... = '%s'\"\n",
                    k_program, name, name);
        } else if (strstr(sql, "|>")) {
            fprintf(stderr, "%s: hint: a stage after |> sees only the columns the stage before it selected\n", k_program);
        }
    } else if (strstr(m, "syntax error") && strstr(m, "near \"")) {
        fprintf(stderr, "%s: hint: quote the query to keep the shell away from it: sel \"...\" (see sel --help)\n", k_program);
    }
}

bool multiple_statements(const char* sql) {
    char quote = 0;
    for (const char* p = sql; *p; ++p) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == ';') {
            for (const char* q = p + 1; *q; ++q) {
                if (!vdb::is_space(*q) && *q != ';') return true;
            }
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    cli::Settings settings;
    Words words = {0, 0, 0};
    const char* query = 0;
    const char* file = 0;
    bool show_sql = false;
    char** query_words = static_cast<char**>(malloc(sizeof(char*) * static_cast<size_t>(argc + 1)));
    int word_count = 0;
    char** tables = static_cast<char**>(malloc(sizeof(char*) * static_cast<size_t>(argc + 1)));
    int table_count = 0;
    if (!query_words || !tables) return k_exit_sql;
    bool rest_is_query = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (rest_is_query || a[0] != '-' || a[1] == 0 || vdb::is_digit(a[1]) || (a[1] == '-' && a[2] == 0)) {
            if (!rest_is_query && strcmp(a, "--") == 0) {
                rest_is_query = true;
                continue;
            }
            query_words[word_count++] = argv[i];
            continue;
        }
        int used = cli::parse_common_option(k_program, argc, argv, i, settings);
        if (used < 0) return k_exit_usage;
        if (used) continue;
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            fputs(k_help, stdout);
            return k_exit_ok;
        }
        if (strcmp(a, "--help-sql") == 0) {
            fputs(k_help_sql, stdout);
            return k_exit_ok;
        }
        if (strcmp(a, "--functions") == 0) {
            int column = 0;
            printf("Functions (* = aggregate; (n) = arguments):\n");
            vdb::each_function(print_function, &column);
            printf("\n");
            return k_exit_ok;
        }
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("sel (vDB) %s\n", VDB_VERSION);
            return k_exit_ok;
        }
        if (strcmp(a, "--show-sql") == 0) {
            show_sql = true;
            continue;
        }
        if ((strcmp(a, "-q") == 0 || strcmp(a, "--query") == 0) && i + 1 < argc) {
            query = argv[++i];
            continue;
        }
        if ((strcmp(a, "-f") == 0 || strcmp(a, "--file") == 0) && i + 1 < argc) {
            file = argv[++i];
            continue;
        }
        if ((strcmp(a, "-t") == 0 || strcmp(a, "--table") == 0) && i + 1 < argc) {
            tables[table_count++] = argv[++i];
            continue;
        }
        fprintf(stderr, "%s: unknown option %s (see sel --help)\n", k_program, a);
        return k_exit_usage;
    }

    char* script = 0;
    if (file) {
        script = read_file(file);
        if (!script) {
            fprintf(stderr, "%s: cannot read %s\n", k_program, file);
            return k_exit_file;
        }
    } else if (query) {
        script = cli::copy_text(query);
    } else {
        if (word_count == 0) {
            fputs(k_help, stderr);
            return k_exit_usage;
        }
        if (shell_expanded_star(word_count, query_words)) {
            fprintf(stderr,
                    "%s: the shell replaced * with the names of the files in this folder.\n"
                    "     Quote it: sel \"*\" from ...   (or quote the whole query: sel \"* from ...\")\n",
                    k_program);
            return k_exit_usage;
        }
        warn_bare_condition(word_count, query_words);
        // The first word of the query text (it may be one quoted shell word holding the whole query).
        const char* first = query_words[0];
        while (vdb::is_space(*first)) ++first;
        size_t length = 0;
        while (vdb::is_alpha(first[length])) ++length;
        bool keyword = vdb::names_equal(first, length, "select") || vdb::names_equal(first, length, "with");
        if (!keyword && !append(words, "SELECT")) return k_exit_sql;
        for (int i = 0; i < word_count; ++i) {
            if (!append(words, query_words[i])) return k_exit_sql;
        }
        script = words.text;
        words.text = 0;
    }
    if (show_sql) fprintf(stderr, "%s\n", script);

    vdb::BudgetAllocator budget(vdb::heap_allocator(), settings.memory_limit ? settings.memory_limit : static_cast<size_t>(-1));
    vdb::Database db(budget);
    if (settings.decimal_comma) db.options().decimal_separator = ',';
    settings.streaming = !multiple_statements(script);
    db.options().streaming = settings.streaming;
    if (!settings.output_delimiter_given) {
        settings.format.delimiter = settings.csv.delimiter_regex || settings.csv.auto_delimiter ? ';' : settings.csv.delimiter;
    }
    cli::Files files(db, settings, k_program);
    for (int i = 0; i < table_count; ++i) {
        if (!files.add_table(tables[i])) return k_exit_file;
    }
    files.install();

    double start = cli::now_seconds();
    cli::FileOutput out(stdout);
    cli::Printer formatter(out, settings, files);
    vdb::Status status = db.run(script, formatter);
    fflush(stdout);
    files.report_notes();
    if (settings.stats) {
        fprintf(stderr, "%s: %zu rows read, %zu rows written, memory peak %zu bytes, %.3f s\n", k_program, files.rows_read(),
                formatter.rows(), budget.peak(), cli::now_seconds() - start);
    }
    free(query_words);
    free(tables);
    free(words.text);
    if (!status.ok()) {
        fprintf(stderr, "%s: error: %s\n", k_program, status.message());
        if (status.code() == vdb::k_error_memory && settings.memory_limit) {
            fprintf(stderr, "%s: hint: the --memory-limit was reached; a filter without ORDER BY, GROUP BY or DISTINCT\n"
                            "     streams the file and needs little memory\n", k_program);
        } else {
            hint(status, script);
        }
        free(script);
        return status.code() == vdb::k_error_io ? k_exit_file : k_exit_sql;
    }
    free(script);
    return k_exit_ok;
}
