// CSV reading (spec 5): a delimiter character or a regular expression, quotes, multi-line fields, header names.
#ifndef VDB_CSV_HPP
#define VDB_CSV_HPP

#include "database.hpp"
#include "regex.hpp"

namespace vdb {

// Where CSV bytes come from: a file, a serial port, a string. read() returns 0 at the end.
class ByteSource {
public:
    virtual size_t read(char* buffer, size_t size);

protected:
    ~ByteSource() {}
};

class TextSource final : public ByteSource {
public:
    TextSource(const char* text, size_t length) : text_(text), length_(length), at_(0) {}
    explicit TextSource(const char* text);
    size_t read(char* buffer, size_t size) override;

private:
    const char* text_;
    size_t length_;
    size_t at_;
};

struct CsvOptions {
    char delimiter;               // ';' by default (spec A18)
    const char* delimiter_regex;  // when set, fields are split by this regular expression
    bool auto_delimiter;          // sniff ; , TAB | : from the header line
    char quote;
    bool header;      // the first line names the fields
    bool trim;        // strip spaces around unquoted fields
    bool empty_text;  // unquoted empty fields are '' instead of NULL (spec A4)
    bool strict;      // irregular rows and text after a closing quote are errors
    CsvOptions()
        : delimiter(';'),
          delimiter_regex(0),
          auto_delimiter(false),
          quote('"'),
          header(true),
          trim(false),
          empty_text(false),
          strict(false) {}
};

class CsvReader final : public RowStream {
public:
    CsvReader(Allocator& allocator, ByteSource& source, const CsvOptions& options);
    CsvReader(const CsvReader&) = delete;
    CsvReader& operator=(const CsvReader&) = delete;

    // Reads the header (or looks at the first record without one). Call once before next().
    Status open();
    size_t column_count() const { return names_.size(); }
    const char* column_name(size_t index) const { return names_[index]; }
    int next(const Value*& values, size_t& count, Status& status) override;

    char delimiter() const { return delimiter_; }
    size_t line() const { return line_; }
    size_t records() const { return records_; }
    size_t short_rows() const { return short_rows_; }
    size_t long_rows() const { return long_rows_; }

private:
    struct Field {
        size_t offset;
        size_t length;
        bool null;
    };
    bool fill();
    int read_line(Text& line);  // 1 a line, 0 the end, -1 out of memory
    int read_record(Status& status);
    bool find_delimiter(size_t from, size_t& start, size_t& end, Status& status);
    bool make_names(Status& status);

    Allocator& allocator_;
    ByteSource& source_;
    CsvOptions options_;
    char delimiter_;
    Pool pool_;
    const Regex* regex_;
    RegexWork work_;
    Vec<char> buffer_;
    size_t buffer_at_;
    size_t buffer_end_;
    bool eof_;
    bool first_line_;
    Text record_;
    Text line_text_;
    Text out_;
    Vec<Field> fields_;
    Vec<Value> values_;
    Vec<const char*> names_;
    size_t line_;
    size_t record_line_;
    size_t records_;
    size_t short_rows_;
    size_t long_rows_;
    bool pending_;  // without a header, the first record waits for next()
};

}  // namespace vdb

#endif
