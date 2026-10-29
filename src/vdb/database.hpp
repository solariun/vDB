// The public API: Database, Result, Row, Cell, Params, RowSink (spec 4.1).
#ifndef VDB_DATABASE_HPP
#define VDB_DATABASE_HPP

#include "table.hpp"

namespace vdb {

class Database;
class Select;
class Insert;
class Update;
class Delete;
class ByteSource;
class Output;
struct CsvOptions;
class CsvReader;
struct FormatOptions;

// Called with a table name the database does not know (spec A10). Return a table added with
// Database::add_table() (or 0 when the name is not yours; fill `status` on a real error).
typedef Table* (*TableResolver)(Database& db, const char* name, void* user, Status& status);

struct Options {
    char decimal_separator;  // '.' or ',' (spec A19)
    bool streaming;          // stream a CSV table read once (spec A7)
    TableResolver table_resolver;
    void* resolver_user;
    Options() : decimal_separator('.'), streaming(true), table_resolver(0), resolver_user(0) {}
};

// One field of a row. text() is never null ("" for NULL).
class Cell {
public:
    Cell(const Value& value, char decimal) : value_(value), decimal_(decimal) {}
    bool is_null() const { return value_.type == k_null; }
    bool is_number() const;
    const char* text() const { return value_.s ? value_.s : ""; }
    size_t size() const { return value_.n; }
    int64_t integer(int64_t fallback = 0) const;
    double number(double fallback = 0) const;
    const Value& value() const { return value_; }

private:
    Value value_;
    char decimal_;
};

// A row of a Result or of a streamed query. Fields by position or by column name (case-insensitive).
class Row {
public:
    Row(const Value* values, size_t count, const char* const* names, char decimal);
    Row(const uint8_t* record, const Table* table, char decimal);
    size_t size() const { return count_; }
    Cell operator[](size_t index) const;
    Cell operator[](int index) const { return (*this)[static_cast<size_t>(index)]; }
    Cell operator[](const char* name) const;
    const char* column_name(size_t index) const;

private:
    const Value* values_;
    const uint8_t* record_;
    const Table* table_;
    const char* const* names_;
    size_t count_;
    char decimal_;
};

// Receives the rows of each SELECT of a script as they are produced (spec A7).
class RowSink {
public:
    virtual bool begin(size_t columns, const char* const* names);  // false: stop the statement
    virtual bool row(const Row& row);                              // false: stop reading rows
    virtual void end();

protected:
    ~RowSink() {}
};

// Values for ? and ?N placeholders: the safe way to put user data into SQL text.
class Params {
public:
#ifndef VDB_NO_HEAP
    Params();
#endif
    explicit Params(Allocator& allocator);
    Params(const Params&) = delete;
    Params& operator=(const Params&) = delete;

    Params& add(const char* text);
    Params& add(const char* text, size_t length);
    Params& add(int value) { return add(static_cast<long long>(value)); }
    Params& add(long value) { return add(static_cast<long long>(value)); }
    Params& add(long long value);
    Params& add(unsigned value) { return add(static_cast<long long>(value)); }
    Params& add(unsigned long value) { return add(static_cast<long long>(value)); }
    Params& add(unsigned long long value) { return add(static_cast<long long>(value)); }
    Params& add(double value);
    Params& add(bool value);
    Params& add_null();
    size_t size() const { return values_.size(); }
    const Value* values() const { return values_.data(); }
    bool ok() const { return ok_; }

private:
    Vec<Value> values_;
    Pool text_;
    bool ok_;
};

class Result {
public:
    Result();
    explicit Result(const Status& status);  // a failed result
    ~Result();
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    Result(Result&& other);
    Result& operator=(Result&& other);

    bool ok() const { return status_.ok(); }
    explicit operator bool() const { return ok(); }
    const char* error() const { return status_.message(); }
    int error_code() const { return status_.code(); }
    const Status& status() const { return status_; }

    size_t size() const { return table_ ? table_->row_count() : 0; }
    size_t columns() const { return table_ ? table_->column_count() : 0; }
    const char* column_name(size_t index) const { return table_ && index < table_->column_count() ? table_->column_name(index) : ""; }
    int column_index(const char* name) const { return table_ ? table_->find_column(name) : -1; }
    // Out of range, or on a failed result: a row whose fields are all NULL (never a crash).
    Row operator[](size_t row) const {
        return table_ && row < table_->row_count() ? Row(table_->record(row), table_, decimal_) : Row(0, 0, 0, decimal_);
    }
    Row operator[](int row) const { return (*this)[static_cast<size_t>(row)]; }
    size_t changes() const { return changes_; }
    const Table* table() const { return table_; }
    bool write(Output& out, const FormatOptions& options) const;

private:
    friend class Database;
    void reset();
    Table* table_;
    Status status_;
    size_t changes_;
    char decimal_;
};

class Database {
public:
#ifndef VDB_NO_HEAP
    Database();
#endif
    explicit Database(Allocator& allocator);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Runs a script; the result of its last statement (rows, or changes()), or the first error.
    Result query(const char* sql);
    Result query(const char* sql, const Params& params);
    // Runs a script and discards the rows.
    Status exec(const char* sql);
    Status exec(const char* sql, const Params& params);
    // Streams the rows of every SELECT of the script into `sink`.
    Status run(const char* sql, RowSink& sink);
    Status run(const char* sql, const Params& params, RowSink& sink);

    // The word chain (spec 4.2).
    Select select(const char* columns = "*");
    Insert insert(const char* table);
    Update update(const char* table);
    Delete delete_from(const char* table);

    // CSV (spec 4.4, 5).
    Status load_csv(const char* table, ByteSource& source, const CsvOptions& options);
    Status load_csv_text(const char* table, const char* text, const CsvOptions& options);
    Status attach_csv(const char* table, CsvReader& reader);  // read lazily, streamed when read once

    Table* table(const char* name) const;
    size_t table_count() const { return tables_.size(); }
    Table* table_at(size_t index) const { return tables_[index]; }
    Table* create_table(const char* name, Status& status);
    Table* add_table(const char* name, size_t length, Status& status);  // for resolvers
    bool drop_table(const char* name);

    Options& options() { return options_; }
    const Options& options() const { return options_; }
    Allocator& allocator() const { return *allocator_; }

private:
    friend class Executor;
    Status script(const char* sql, const Params* params, RowSink* sink, Result* result);
    Allocator* allocator_;
    Vec<Table*> tables_;
    Options options_;
};

}  // namespace vdb

#endif
