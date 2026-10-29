#include "format.hpp"

#include <string.h>

namespace vdb {

void Output::write(const char*, size_t) {}
void Output::write(const char* text) { write(text, strlen(text)); }

bool parse_format(const char* name, Format& format) {
    if (names_equal(name, "csv")) format = k_format_csv;
    else if (names_equal(name, "tsv")) format = k_format_tsv;
    else if (names_equal(name, "table") || names_equal(name, "box")) format = k_format_table;
    else if (names_equal(name, "json")) format = k_format_json;
    else if (names_equal(name, "jsonl") || names_equal(name, "ndjson")) format = k_format_jsonl;
    else if (names_equal(name, "markdown") || names_equal(name, "md")) format = k_format_markdown;
    else return false;
    return true;
}

Formatter::Formatter(Output& out, const FormatOptions& options, Allocator& allocator)
    : out_(out), options_(options), allocator_(allocator), names_(allocator), name_pool_(allocator), buffer_(0), rows_(0), ok_(true) {
    if (options_.format == k_format_tsv) options_.delimiter = '\t';
    if (!options_.null_text) options_.null_text = "";
}

Formatter::~Formatter() {
    if (buffer_) destroy(allocator_, buffer_);
}

namespace {

bool needs_quotes(const char* s, size_t n, char delimiter, char quote) {
    if (!n) return false;
    if (s[0] == ' ' || s[n - 1] == ' ') return true;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == delimiter || s[i] == quote || s[i] == '\n' || s[i] == '\r') return true;
    }
    return false;
}

// JSON number grammar: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
bool json_number(const char* s, size_t n) {
    size_t i = 0;
    if (i < n && s[i] == '-') ++i;
    if (i >= n) return false;
    if (s[i] == '0') {
        ++i;
    } else if (s[i] >= '1' && s[i] <= '9') {
        while (i < n && is_digit(s[i])) ++i;
    } else {
        return false;
    }
    if (i < n && s[i] == '.') {
        ++i;
        size_t start = i;
        while (i < n && is_digit(s[i])) ++i;
        if (i == start) return false;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
        size_t start = i;
        while (i < n && is_digit(s[i])) ++i;
        if (i == start) return false;
    }
    return i == n;
}

}  // namespace

void Formatter::field_csv(const Cell& cell, char delimiter) {
    if (cell.is_null()) {
        out_.write(options_.null_text);
        return;
    }
    const char* s = cell.text();
    size_t n = cell.size();
    char q = options_.quote;
    if (n == 0 || needs_quotes(s, n, delimiter, q)) {
        out_.write(&q, 1);
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            if (s[i] == q) {
                out_.write(s + start, i - start + 1);
                out_.write(&q, 1);
                start = i + 1;
            }
        }
        out_.write(s + start, n - start);
        out_.write(&q, 1);
        return;
    }
    out_.write(s, n);
}

void Formatter::json_string(const char* s, size_t n) {
    out_.write("\"", 1);
    size_t start = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        const char* escape = 0;
        char hex[8];
        switch (c) {
            case '"': escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            case '\b': escape = "\\b"; break;
            case '\f': escape = "\\f"; break;
            default:
                if (c < 0x20) {
                    const char k_hex[] = "0123456789abcdef";
                    hex[0] = '\\';
                    hex[1] = 'u';
                    hex[2] = '0';
                    hex[3] = '0';
                    hex[4] = k_hex[c >> 4];
                    hex[5] = k_hex[c & 15];
                    hex[6] = 0;
                    escape = hex;
                }
        }
        if (escape) {
            out_.write(s + start, i - start);
            out_.write(escape);
            start = i + 1;
        }
    }
    out_.write(s + start, n - start);
    out_.write("\"", 1);
}

void Formatter::json_row(const Row& row) {
    out_.write("{", 1);
    for (size_t i = 0; i < names_.size(); ++i) {
        if (i) out_.write(",", 1);
        json_string(names_[i], strlen(names_[i]));
        out_.write(":", 1);
        Cell cell = row[i];
        if (cell.is_null()) out_.write("null", 4);
        else if (json_number(cell.text(), cell.size())) out_.write(cell.text(), cell.size());
        else json_string(cell.text(), cell.size());
    }
    out_.write("}", 1);
}

bool Formatter::begin(size_t columns, const char* const* names) {
    names_.clear();
    name_pool_.clear();
    rows_ = 0;
    for (size_t i = 0; i < columns; ++i) {
        char* copy = name_pool_.copy(names[i], strlen(names[i]));
        if (!copy || !names_.push(copy)) {
            ok_ = false;
            return false;
        }
    }
    switch (options_.format) {
        case k_format_csv:
        case k_format_tsv:
            if (options_.header && columns) {
                for (size_t i = 0; i < columns; ++i) {
                    if (i) out_.write(&options_.delimiter, 1);
                    Value v = Value::text(names_[i], strlen(names_[i]));
                    field_csv(Cell(v, '.'), options_.delimiter);
                }
                out_.write("\n", 1);
            }
            break;
        case k_format_json: out_.write("[", 1); break;
        case k_format_markdown:
            if (columns) {
                out_.write("|", 1);
                for (size_t i = 0; i < columns; ++i) {
                    out_.write(" ", 1);
                    out_.write(names_[i]);
                    out_.write(" |", 2);
                }
                out_.write("\n|", 2);
                for (size_t i = 0; i < columns; ++i) out_.write(" --- |", 6);
                out_.write("\n", 1);
            }
            break;
        case k_format_table:
            if (buffer_) destroy(allocator_, buffer_);
            buffer_ = make_table_buffer();
            if (!buffer_) ok_ = false;
            break;
        default: break;
    }
    return ok_;
}

Table* Formatter::make_table_buffer() {
    void* m = allocator_.allocate(sizeof(Table));
    if (!m) return 0;
    Table* t = new (m, PlaceTag()) Table(allocator_, "table", 5);
    if (!t->ok()) {
        destroy(allocator_, t);
        return 0;
    }
    return t;
}

bool Formatter::row(const Row& row) {
    switch (options_.format) {
        case k_format_csv:
        case k_format_tsv:
            for (size_t i = 0; i < names_.size(); ++i) {
                if (i) out_.write(&options_.delimiter, 1);
                field_csv(row[i], options_.delimiter);
            }
            out_.write("\n", 1);
            break;
        case k_format_json:
            out_.write(rows_ ? ",\n" : "\n", rows_ ? 2 : 1);
            json_row(row);
            break;
        case k_format_jsonl:
            json_row(row);
            out_.write("\n", 1);
            break;
        case k_format_markdown:
            out_.write("|", 1);
            for (size_t i = 0; i < names_.size(); ++i) {
                Cell cell = row[i];
                out_.write(" ", 1);
                const char* s = cell.is_null() ? options_.null_text : cell.text();
                size_t n = cell.is_null() ? strlen(s) : cell.size();
                size_t start = 0;
                for (size_t k = 0; k < n; ++k) {
                    if (s[k] == '|' || s[k] == '\n') {
                        out_.write(s + start, k - start);
                        out_.write(s[k] == '|' ? "\\|" : "<br>");
                        start = k + 1;
                    }
                }
                out_.write(s + start, n - start);
                out_.write(" |", 2);
            }
            out_.write("\n", 1);
            break;
        case k_format_table: {
            if (!buffer_) return false;
            Vec<Value> values(allocator_);
            if (!values.resize(names_.size() + 1)) {
                ok_ = false;
                return false;
            }
            for (size_t i = 0; i < names_.size(); ++i) values[i] = row[i].value();
            if (!buffer_->append_values(values.data(), names_.size())) {
                ok_ = false;
                return false;
            }
            break;
        }
    }
    ++rows_;
    return true;
}

void Formatter::print_table() {
    size_t columns = names_.size();
    Vec<size_t> widths(allocator_);
    if (!widths.resize(columns + 1)) {
        ok_ = false;
        return;
    }
    size_t null_width = utf8_count(options_.null_text, strlen(options_.null_text));
    for (size_t c = 0; c < columns; ++c) widths[c] = utf8_count(names_[c], strlen(names_[c]));
    for (size_t r = 0; r < buffer_->row_count(); ++r) {
        for (size_t c = 0; c < columns; ++c) {
            Value v;
            record_field(buffer_->record(r), c, v);
            size_t w = v.is_null() ? null_width : utf8_count(v.s, v.n);
            if (w > widths[c]) widths[c] = w;
        }
    }
    Output& o = out_;
    struct Line {
        static void rule(Output& out, const Vec<size_t>& w, size_t columns) {
            out.write("+", 1);
            for (size_t c = 0; c < columns; ++c) {
                for (size_t i = 0; i < w[c] + 2; ++i) out.write("-", 1);
                out.write("+", 1);
            }
            out.write("\n", 1);
        }
        static void cell(Output& out, const char* s, size_t n, size_t width, bool right) {
            size_t w = utf8_count(s, n);
            out.write(" ", 1);
            if (right) {
                for (size_t i = w; i < width; ++i) out.write(" ", 1);
            }
            out.write(s, n);
            if (!right) {
                for (size_t i = w; i < width; ++i) out.write(" ", 1);
            }
            out.write(" |", 2);
        }
    };
    Line::rule(o, widths, columns);
    if (options_.header) {
        o.write("|", 1);
        for (size_t c = 0; c < columns; ++c) Line::cell(o, names_[c], strlen(names_[c]), widths[c], false);
        o.write("\n", 1);
        Line::rule(o, widths, columns);
    }
    for (size_t r = 0; r < buffer_->row_count(); ++r) {
        o.write("|", 1);
        for (size_t c = 0; c < columns; ++c) {
            Value v;
            record_field(buffer_->record(r), c, v);
            Number n;
            if (v.is_null()) Line::cell(o, options_.null_text, strlen(options_.null_text), widths[c], false);
            else Line::cell(o, v.s, v.n, widths[c], parse_number(v.s, v.n, '.', n) || parse_number(v.s, v.n, ',', n));
        }
        o.write("\n", 1);
    }
    if (buffer_->row_count()) Line::rule(o, widths, columns);
    char count[k_number_text];
    size_t rows = buffer_->row_count();
    o.write("(", 1);
    o.write(count, format_int(static_cast<int64_t>(rows), count));
    o.write(rows == 1 ? " row)\n" : " rows)\n");
}

void Formatter::end() {
    switch (options_.format) {
        case k_format_json: out_.write(rows_ ? "\n]\n" : "]\n"); break;
        case k_format_table:
            if (buffer_) {
                print_table();
                destroy(allocator_, buffer_);
                buffer_ = 0;
            }
            break;
        default: break;
    }
}

bool Result::write(Output& out, const FormatOptions& options) const {
    if (!table_) return true;
    Formatter f(out, options, table_->allocator());
    Vec<const char*> names(table_->allocator());
    for (size_t i = 0; i < table_->column_count(); ++i) {
        if (!names.push(table_->column_name(i))) return false;
    }
    if (!f.begin(names.size(), names.data())) return false;
    for (size_t r = 0; r < table_->row_count(); ++r) {
        if (!f.row(Row(table_->record(r), table_, decimal_))) break;
    }
    f.end();
    return f.ok();
}

}  // namespace vdb
