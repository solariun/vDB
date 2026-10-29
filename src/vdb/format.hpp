// Writing rows as CSV, TSV, a table, JSON, JSON lines or Markdown (spec 6). No stdio: the caller gives an Output.
#ifndef VDB_FORMAT_HPP
#define VDB_FORMAT_HPP

#include "database.hpp"

namespace vdb {

// Where text goes: stdout, a file, a serial port, a buffer.
class Output {
public:
    virtual void write(const char* text, size_t length);
    void write(const char* text);

protected:
    ~Output() {}
};

// Collects everything written (tests, small MCU buffers).
class TextOutput final : public Output {
public:
    explicit TextOutput(Allocator& allocator) : text_(allocator) {}
    void write(const char* text, size_t length) override { text_.append(text, length); }
    const char* c_str() const { return text_.c_str(); }
    size_t size() const { return text_.size(); }
    void clear() { text_.clear(); }

private:
    Text text_;
};

enum Format { k_format_csv, k_format_tsv, k_format_table, k_format_json, k_format_jsonl, k_format_markdown };

bool parse_format(const char* name, Format& format);

struct FormatOptions {
    Format format;
    char delimiter;         // csv
    char quote;             // csv
    bool header;            // csv, tsv, table, markdown
    const char* null_text;  // csv, tsv, table, markdown
    FormatOptions() : format(k_format_csv), delimiter(';'), quote('"'), header(true), null_text("") {}
};

// A RowSink that writes: give it to Database::run() to stream any query out.
class Formatter final : public RowSink {
public:
    Formatter(Output& out, const FormatOptions& options, Allocator& allocator);
    ~Formatter();
    Formatter(const Formatter&) = delete;
    Formatter& operator=(const Formatter&) = delete;

    bool begin(size_t columns, const char* const* names) override;
    bool row(const Row& row) override;
    void end() override;
    size_t rows() const { return rows_; }
    bool ok() const { return ok_; }

private:
    void field_csv(const Cell& cell, char delimiter);
    void json_string(const char* s, size_t n);
    void json_row(const Row& row);
    void print_table();
    Table* make_table_buffer();

    Output& out_;
    FormatOptions options_;
    Allocator& allocator_;
    Vec<const char*> names_;
    Pool name_pool_;
    Table* buffer_;
    size_t rows_;
    bool ok_;
};

}  // namespace vdb

#endif
