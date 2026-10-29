// Tables of records, record encoding, the hash index and the stable sort (spec 2.1).
#ifndef VDB_TABLE_HPP
#define VDB_TABLE_HPP

#include "value.hpp"
#include "vec.hpp"

namespace vdb {

// A source of rows read once (a CSV file): the table's columns are known before the first row.
class RowStream {
public:
    // 1: a row in `values` (valid until the next call), 0: the end, -1: an error in `status`.
    virtual int next(const Value*& values, size_t& count, Status& status);

protected:
    ~RowStream() {}
};

// Record layout: varint(field count), then per field varint(0) for NULL or varint(length + 1), bytes, NUL.
uint8_t* encode_record(Allocator& allocator, const Value* values, size_t count);
size_t record_field_count(const uint8_t* record);
void decode_record(const uint8_t* record, Value* out, size_t columns);  // missing fields read NULL
bool record_field(const uint8_t* record, size_t index, Value& out);

class Table {
public:
    Table(Allocator& allocator, const char* name, size_t name_length);
    ~Table();
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    bool ok() const { return name_ != 0; }
    const char* name() const { return name_; }
    size_t column_count() const { return columns_.size(); }
    const char* column_name(size_t index) const { return columns_[index]; }
    int find_column(const char* name, size_t length) const;
    int find_column(const char* name) const;
    int add_column(const char* name, size_t length);     // appends; -1 when memory ran out
    int ensure_column(const char* name, size_t length);  // finds or appends

    size_t row_count() const { return records_.size(); }
    const uint8_t* record(size_t index) const { return records_[index]; }
    bool append(uint8_t* record);  // takes ownership (released on failure)
    bool append_values(const Value* values, size_t count);
    void replace(size_t index, uint8_t* record);
    void remove_rows(const uint8_t* marks);  // removes rows whose mark is not 0
    void clear_rows();

    // A stream that fills this table lazily (spec A7).
    void set_stream(RowStream* stream) { stream_ = stream; }
    RowStream* stream() const { return stream_; }
    bool stream_pending() const { return stream_ != 0 && !stream_done_; }
    void set_stream_done() { stream_done_ = true; }
    // Streamed by a statement (or stopped by a LIMIT): its rows are gone, a later read is an error (spec L6).
    void set_streamed() { streamed_ = stream_done_ = true; }
    bool streamed() const { return streamed_; }
    bool load_stream(Status& status);  // reads the rest of the stream into records

    Allocator& allocator() const { return *allocator_; }

private:
    Allocator* allocator_;
    char* name_;
    Vec<char*> columns_;
    Vec<uint8_t*> records_;
    RowStream* stream_;
    bool stream_done_;
    bool streamed_;
};

// Byte-string keys to dense ids 0..size-1 (grouping, DISTINCT, joins, sets).
class HashIndex {
public:
    explicit HashIndex(Allocator& allocator);
    long find_or_add(const char* key, size_t length, bool& added);  // -1 when memory ran out
    long find(const char* key, size_t length) const;
    size_t size() const { return offsets_.size() ? offsets_.size() - 1 : 0; }
    void clear();

private:
    bool grow();
    struct Slot {
        uint32_t hash;
        uint32_t id_plus_one;
    };
    Vec<Slot> slots_;
    Vec<size_t> offsets_;
    Text keys_;
};

typedef int (*CompareIndexes)(uint32_t a, uint32_t b, void* context);
bool merge_sort(uint32_t* items, size_t count, CompareIndexes compare, void* context, Allocator& allocator);

}  // namespace vdb

#endif
