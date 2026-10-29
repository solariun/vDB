#include "database.hpp"

#include <math.h>
#include <string.h>

#include "exec.hpp"
#include "sql_parser.hpp"

namespace vdb {

// ── Cell, Row, RowSink ───────────────────────────────────────────────────────

bool Cell::is_number() const {
    Number n;
    return value_number(value_, decimal_, n);
}

int64_t Cell::integer(int64_t fallback) const {
    Number n;
    if (!value_number(value_, decimal_, n)) return fallback;
    if (n.is_int) return n.i;
    int64_t whole;
    return real_is_integral(trunc(n.r), whole) ? whole : fallback;
}

double Cell::number(double fallback) const {
    Number n;
    return value_number(value_, decimal_, n) ? n.real() : fallback;
}

Row::Row(const Value* values, size_t count, const char* const* names, char decimal)
    : values_(values), record_(0), table_(0), names_(names), count_(count), decimal_(decimal) {}

Row::Row(const uint8_t* record, const Table* table, char decimal)
    : values_(0), record_(record), table_(table), names_(0), count_(table ? table->column_count() : 0), decimal_(decimal) {}

Cell Row::operator[](size_t index) const {
    if (index >= count_) return Cell(Value::null(), decimal_);
    if (values_) return Cell(values_[index], decimal_);
    Value v;
    record_field(record_, index, v);
    return Cell(v, decimal_);
}

Cell Row::operator[](const char* name) const {
    for (size_t i = 0; i < count_; ++i) {
        if (names_equal(column_name(i), name)) return (*this)[i];
    }
    return Cell(Value::null(), decimal_);
}

const char* Row::column_name(size_t index) const {
    if (index >= count_) return "";
    return table_ ? table_->column_name(index) : names_[index];
}

bool RowSink::begin(size_t, const char* const*) { return true; }
bool RowSink::row(const Row&) { return true; }
void RowSink::end() {}

// ── Params ───────────────────────────────────────────────────────────────────

#ifndef VDB_NO_HEAP
Params::Params() : values_(heap_allocator()), text_(heap_allocator(), VDB_SCRATCH_CHUNK), ok_(true) {}
#endif

Params::Params(Allocator& allocator) : values_(allocator), text_(allocator, VDB_SCRATCH_CHUNK), ok_(true) {}

Params& Params::add(const char* text) {
    if (!text) return add_null();
    return add(text, strlen(text));
}

Params& Params::add(const char* text, size_t length) {
    char* copy = text_.copy(text, length);
    if (!copy || !values_.push(Value::text(copy, length))) ok_ = false;
    return *this;
}

Params& Params::add(long long value) {
    if (!values_.push(Value::integer(value))) ok_ = false;
    return *this;
}

Params& Params::add(double value) {
    if (!values_.push(Value::real(value))) ok_ = false;
    return *this;
}

Params& Params::add(bool value) {
    if (!values_.push(Value::boolean(value))) ok_ = false;
    return *this;
}

Params& Params::add_null() {
    if (!values_.push(Value::null())) ok_ = false;
    return *this;
}

// ── Result ───────────────────────────────────────────────────────────────────

Result::Result() : table_(0), changes_(0), decimal_('.') {}

Result::Result(const Status& status) : table_(0), status_(status), changes_(0), decimal_('.') {}

Result::~Result() { reset(); }

void Result::reset() {
    if (table_) destroy(table_->allocator(), table_);
    table_ = 0;
}

Result::Result(Result&& other) : table_(other.table_), status_(other.status_), changes_(other.changes_), decimal_(other.decimal_) {
    other.table_ = 0;
}

Result& Result::operator=(Result&& other) {
    if (this != &other) {
        reset();
        table_ = other.table_;
        status_ = other.status_;
        changes_ = other.changes_;
        decimal_ = other.decimal_;
        other.table_ = 0;
    }
    return *this;
}

// ── Database ─────────────────────────────────────────────────────────────────

#ifndef VDB_NO_HEAP
Database::Database() : allocator_(&heap_allocator()), tables_(heap_allocator()) {}
#endif

Database::Database(Allocator& allocator) : allocator_(&allocator), tables_(allocator) {}

Database::~Database() {
    for (size_t i = 0; i < tables_.size(); ++i) destroy(*allocator_, tables_[i]);
}

Table* Database::table(const char* name) const {
    for (size_t i = 0; i < tables_.size(); ++i) {
        if (names_equal(tables_[i]->name(), name)) return tables_[i];
    }
    return 0;
}

Table* Database::add_table(const char* name, size_t length, Status& status) {
    void* m = allocator_->allocate(sizeof(Table));
    if (!m) {
        status.fail_memory();
        return 0;
    }
    Table* t = new (m, PlaceTag()) Table(*allocator_, name, length);
    if (!t->ok() || !tables_.push(t)) {
        destroy(*allocator_, t);
        status.fail_memory();
        return 0;
    }
    return t;
}

Table* Database::create_table(const char* name, Status& status) {
    if (table(name)) {
        status.fail(k_error_misuse, VDB_F("table %s already exists"), name);
        return 0;
    }
    return add_table(name, strlen(name), status);
}

bool Database::drop_table(const char* name) {
    for (size_t i = 0; i < tables_.size(); ++i) {
        if (names_equal(tables_[i]->name(), name)) {
            destroy(*allocator_, tables_[i]);
            tables_.remove(i);
            return true;
        }
    }
    return false;
}

namespace {

class UserSink final : public Sink {
public:
    UserSink(RowSink& user, char decimal) : user_(user), decimal_(decimal), names_(0), stopped_(false) {}
    int begin(const char* const* names, size_t count) override {
        names_ = names;
        stopped_ = !user_.begin(count, names);
        return k_go;
    }
    int row(Value* values, size_t count) override {
        if (stopped_) return k_stop;
        Row r(values, count, names_, decimal_);
        if (!user_.row(r)) {
            stopped_ = true;
            return k_stop;
        }
        return k_go;
    }
    int end() override {
        user_.end();
        return k_go;
    }

private:
    RowSink& user_;
    char decimal_;
    const char* const* names_;
    bool stopped_;
};

class ResultSink final : public Sink {
public:
    ResultSink(Table* table, Status& status) : table_(table), status_(status) {}
    int begin(const char* const* names, size_t count) override {
        for (size_t i = 0; i < count; ++i) {
            if (table_->add_column(names[i], strlen(names[i])) < 0) {
                status_.fail_memory();
                return k_error;
            }
        }
        return k_go;
    }
    int row(Value* values, size_t count) override {
        if (!table_->append_values(values, count)) {
            status_.fail_memory();
            return k_error;
        }
        return k_go;
    }

private:
    Table* table_;
    Status& status_;
};

}  // namespace

Status Database::script(const char* sql, const Params* params, RowSink* sink, Result* result) {
    Status status;
    if (!sql) return status.fail(k_error_misuse, VDB_F("no SQL given"));
    if (params && !params->ok()) return status.fail_memory();
    Pool pool(*allocator_, VDB_POOL_CHUNK);
    size_t needed = 0;
    size_t length = strlen(sql);
    Statement* first = parse_script(pool, sql, length, status, needed);
    if (!status.ok()) return status;
    size_t given = params ? params->size() : 0;
    if (needed > given) {
        return status.fail(k_error_misuse, VDB_F("the SQL uses %d parameters but %d values were given"),
                           static_cast<int>(needed), static_cast<int>(given));
    }
    Executor ex(*this, pool, sql, params ? params->values() : 0, given, status);
    for (Statement* s = first; s; s = s->next) {
        Sink* target = 0;
        Table* rows = 0;
        if (s->kind == st_query) {
            if (result && !s->next) {
                void* m = allocator_->allocate(sizeof(Table));
                if (!m) return status.fail_memory();
                rows = new (m, PlaceTag()) Table(*allocator_, "result", 6);
                if (!rows->ok()) {
                    destroy(*allocator_, rows);
                    return status.fail_memory();
                }
            }
        }
        ResultSink result_sink(rows, status);
        if (rows) target = &result_sink;
        bool ok;
        if (!rows && sink && s->kind == st_query) {
            UserSink forward(*sink, options_.decimal_separator);
            ok = ex.run(s, &forward);
        } else {
            ok = ex.run(s, target);
        }
        if (rows) {
            if (ok) {
                result->reset();
                result->table_ = rows;
            } else {
                destroy(*allocator_, rows);
            }
        }
        if (!ok) break;
    }
    if (result) {
        result->changes_ = ex.changes();
        result->decimal_ = options_.decimal_separator;
    }
    return status;
}

Result Database::query(const char* sql) {
    Result r;
    r.status_ = script(sql, 0, 0, &r);
    return r;
}

Result Database::query(const char* sql, const Params& params) {
    Result r;
    r.status_ = script(sql, &params, 0, &r);
    return r;
}

Status Database::exec(const char* sql) { return script(sql, 0, 0, 0); }
Status Database::exec(const char* sql, const Params& params) { return script(sql, &params, 0, 0); }
Status Database::run(const char* sql, RowSink& sink) { return script(sql, 0, &sink, 0); }
Status Database::run(const char* sql, const Params& params, RowSink& sink) { return script(sql, &params, &sink, 0); }

}  // namespace vdb
