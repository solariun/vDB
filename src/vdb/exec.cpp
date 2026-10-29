#include "exec.hpp"

#include <math.h>
#include <string.h>

namespace vdb {

int Sink::begin(const char* const*, size_t) { return k_go; }
int Sink::row(Value*, size_t) { return k_go; }
int Sink::end() { return k_go; }

struct Scope {
    const char* name;
    Table* table;
    Scope* outer;
};

namespace {

const int k_max_join_keys = 8;
const size_t k_max_sources = 64;
const char* const k_previous = "_";

enum Clause { c_where, c_select, c_group, c_having, c_order, c_on, c_constant };

template <class T>
T* make_with(Allocator& a) {
    void* m = a.allocate(sizeof(T));
    return m ? new (m, PlaceTag()) T(a) : 0;
}

Table* new_table(Allocator& a, const char* name) {
    void* m = a.allocate(sizeof(Table));
    if (!m) return 0;
    Table* t = new (m, PlaceTag()) Table(a, name, strlen(name));
    if (!t->ok()) {
        destroy(a, t);
        return 0;
    }
    return t;
}

bool value_copy(Pool& pool, Value& v) {
    if (v.type != k_text && !(v.flags & k_has_text)) return true;
    char* s = pool.copy(v.s, v.n);
    if (!s) return false;
    v.s = s;
    return true;
}

}  // namespace

class CoreRun;
class CompoundRun;

struct Source {
    const char* alias;
    Table* table;
    bool owned;
    bool streaming;
    bool pushed;
    uint8_t join;
    Expr* on;
    NameList* using_columns;
    Value* row;
    size_t width;
    bool null_row;
    int keys;
    Expr* probe[k_max_join_keys];
    Expr* build[k_max_join_keys];
    HashIndex* index;
    uint32_t* heads;
    uint32_t* chain;
    uint8_t* matched;
    size_t rows;
};

struct Frame {
    Frame* outer;
    Source* sources;
    size_t count;
    Expr* owner;
    Value* aggregates;
    Value* outputs;
    CoreRun* run;
};

struct AggState {
    int64_t count;
    int64_t isum;
    double rsum;
    bool real;
    bool any;
    double mean;
    double m2;
    Value best;
    bool has_best;
    char* best_text;
    Text* concat;
    HashIndex* seen;
    Vec<double>* values;
};

struct Group {
    uint8_t** reps;
    AggState* aggs;
};

struct OutCol {
    Expr* expr;
    int16_t src;
    int32_t col;
    const char* name;
};

// ── CoreRun: one SELECT core ─────────────────────────────────────────────────

class CoreRun {
public:
    CoreRun(Executor& ex, Core* core, OrderItem* order, Frame* outer, Expr* owner, Scope* scope)
        : ex_(ex),
          a_(ex.allocator()),
          core_(core),
          order_(order),
          scope_(scope),
          target_(0),
          sources_(ex.allocator()),
          outs_(ex.allocator()),
          names_(ex.allocator()),
          aggregates_(ex.allocator()),
          order_exprs_(ex.allocator()),
          groups_(ex.allocator()),
          group_list_(ex.allocator()),
          distinct_(ex.allocator()),
          key_(ex.allocator()),
          group_by_(ex.allocator()),
          aggregate_(false),
          outputs_(0),
          keys_(0),
          agg_values_(0),
          derived_(ex.allocator()) {
        frame_.outer = outer;
        frame_.sources = 0;
        frame_.count = 0;
        frame_.owner = owner;
        frame_.aggregates = 0;
        frame_.outputs = 0;
        frame_.run = this;
    }

    ~CoreRun() {
        for (size_t i = 0; i < sources_.size(); ++i) {
            Source& s = sources_[i];
            if (s.row) a_.release(s.row);
            if (s.index) destroy(a_, s.index);
            if (s.heads) a_.release(s.heads);
            if (s.chain) a_.release(s.chain);
            if (s.matched) a_.release(s.matched);
            if (s.owned) destroy(a_, s.table);
        }
        for (size_t g = 0; g < group_list_.size(); ++g) free_group(group_list_[g]);
        if (outputs_) a_.release(outputs_);
        if (keys_) a_.release(keys_);
        if (agg_values_) a_.release(agg_values_);
    }

    Scope* scope() const { return scope_; }
    Frame* frame() { return &frame_; }
    bool bind_expr(Expr* e) { return bind(e, c_where); }
    size_t width() const { return outs_.size(); }
    const char* const* names() const { return names_.data(); }
    size_t key_count() const { return order_exprs_.size(); }
    void target(CompoundRun* c) { target_ = c; }
    Expr* output_expr(size_t i) const { return outs_[i].expr; }

    bool output_value(size_t i, Value& out) {
        const OutCol& o = outs_[i];
        if (o.expr) return ex_.eval(o.expr, &frame_, out);
        Source& s = sources_[static_cast<size_t>(o.src)];
        out = s.null_row ? Value::null() : s.row[o.col];
        return true;
    }

    bool prepare(Table* pushed_schema);
    int drive();
    int push(Value* values, size_t count);
    int finish();

private:
    bool add_source(const char* name, Query* query, const char* alias, uint8_t join, Expr* on, NameList* using_columns,
                    Table* pushed_schema);
    bool expand_items();
    bool bind(Expr* e, int clause);
    bool bind_column(Expr* e, int clause);
    bool bind_order();
    int find_output(const char* name) const;
    void column_hint(const char* name);
    bool plan_hash(Source& s, size_t k);
    bool build_index(Source& s);
    int level(size_t k);
    int post_pass(size_t k);
    int row_done();
    int project();
    int accumulate();
    bool update_agg(AggState& st, Expr* agg);
    bool final_agg(AggState& st, Expr* agg, Value& out);
    void free_group(Group* g);
    bool set_row(Source& s, size_t row);

    Executor& ex_;
    Allocator& a_;
    Core* core_;
    OrderItem* order_;
    Scope* scope_;
    CompoundRun* target_;
    Frame frame_;
    Vec<Source> sources_;
    Vec<OutCol> outs_;
    Vec<const char*> names_;
    Vec<Expr*> aggregates_;
    Vec<Expr*> order_exprs_;
    HashIndex groups_;
    Vec<Group*> group_list_;
    HashIndex distinct_;
    Text key_;
    Vec<Expr*> group_by_;
    bool aggregate_;
    Value* outputs_;
    Value* keys_;
    Value* agg_values_;
    Vec<Table*> derived_;
};

// ── CompoundRun: cores with set operations, ORDER BY and LIMIT ──────────────

class CompoundRun {
public:
    CompoundRun(Executor& ex, Compound* c, Frame* outer, Expr* owner, Scope* scope, Sink* out)
        : ex_(ex),
          a_(ex.allocator()),
          c_(c),
          outer_(outer),
          owner_(owner),
          scope_(scope),
          out_(out),
          core_(0),
          multi_(c->cores->next != 0),
          limit_(-1),
          offset_(0),
          emitted_(0),
          skipped_(0),
          buffer_(0),
          acc_(0),
          part_(0),
          width_(0),
          keys_(0),
          names_(ex.allocator()),
          stopped_(false),
          order_exprs_(ex.allocator()),
          trim_at_(k_first_trim) {}

    ~CompoundRun() {
        if (core_) destroy(a_, core_);
        if (buffer_) destroy(a_, buffer_);
        if (acc_) destroy(a_, acc_);
        if (part_) destroy(a_, part_);
    }

    int run();
    int begin_pushed(Table* schema);
    int push(Value* values, size_t count) { return core_->push(values, count); }
    int finish_pushed() {
        int rc = core_->finish();
        if (rc == k_error) return rc;
        return flush();
    }
    int accept(Value* outputs, size_t count, Value* keys, size_t key_count);

private:
    int start(CoreRun* core);
    bool limits();
    int emit(Value* values, size_t count);
    int flush();
    int sort_and_emit(Table* rows, size_t key_count, bool keys_stored);
    bool trim_buffer();
    bool combine(uint8_t op);
    bool bind_output_order();

    Executor& ex_;
    Allocator& a_;
    Compound* c_;
    Frame* outer_;
    Expr* owner_;
    Scope* scope_;
    Sink* out_;
    CoreRun* core_;
    bool multi_;
    int64_t limit_;
    int64_t offset_;
    int64_t emitted_;
    int64_t skipped_;
    Table* buffer_;
    Table* acc_;
    Table* part_;
    size_t width_;
    size_t keys_;
    Vec<const char*> names_;
    bool stopped_;
    Vec<Expr*> order_exprs_;
    size_t trim_at_;
    static const size_t k_first_trim = 1024;
};

// ── sinks ────────────────────────────────────────────────────────────────────

// Materializes rows into a table (temporary results, CREATE TABLE AS, results).
class TableSink final : public Sink {
public:
    TableSink(Executor& ex, Table* table) : ex_(ex), table_(table) {}
    int begin(const char* const* names, size_t count) override {
        for (size_t i = 0; i < count; ++i) {
            if (table_->add_column(names[i], strlen(names[i])) < 0) return ex_.memory() ? k_error : k_error;
        }
        return k_go;
    }
    int row(Value* values, size_t count) override {
        if (!table_->append_values(values, count)) return ex_.memory() ? k_error : k_error;
        return k_go;
    }

private:
    Executor& ex_;
    Table* table_;
};

// Feeds a pipeline stage with the rows of the stage before, read as text like a CSV (spec 3.11).
class StageSink final : public Sink {
public:
    StageSink(Executor& ex, Compound* body, Sink* next, Scope* scope, Frame* outer, Expr* owner)
        : ex_(ex), body_(body), next_(next), scope_(scope), outer_(outer), owner_(owner), run_(0), schema_(0), row_(ex.allocator()) {}
    ~StageSink() {
        if (run_) destroy(ex_.allocator(), run_);
        if (schema_) destroy(ex_.allocator(), schema_);
    }
    int begin(const char* const* names, size_t count) override {
        schema_ = new_table(ex_.allocator(), k_previous);
        if (!schema_ || !row_.resize(count)) return ex_.memory() ? k_error : k_error;
        for (size_t i = 0; i < count; ++i) {
            if (schema_->add_column(names[i], strlen(names[i])) < 0) return ex_.memory() ? k_error : k_error;
        }
        void* m = ex_.allocator().allocate(sizeof(CompoundRun));
        if (!m) return ex_.memory() ? k_error : k_error;
        run_ = new (m, PlaceTag()) CompoundRun(ex_, body_, outer_, owner_, scope_, next_);
        return run_->begin_pushed(schema_);
    }
    int row(Value* values, size_t count) override {
        for (size_t i = 0; i < count && i < row_.size(); ++i) {
            row_[i] = values[i].type == k_null ? Value::null() : Value::text(values[i].s, values[i].n);
        }
        return run_->push(row_.data(), row_.size());
    }
    int end() override { return run_ ? run_->finish_pushed() : k_go; }

private:
    Executor& ex_;
    Compound* body_;
    Sink* next_;
    Scope* scope_;
    Frame* outer_;
    Expr* owner_;
    CompoundRun* run_;
    Table* schema_;
    Vec<Value> row_;
};

// Collects a subquery's values: the first value (scalar), existence, or a key set (IN).
class SubquerySink final : public Sink {
public:
    enum Mode { scalar, exists, set };
    SubquerySink(Executor& ex, Mode mode, HashIndex* index)
        : ex_(ex), mode_(mode), index_(index), rows_(0), has_null_(false), value_(Value::null()), key_(ex.allocator()),
          text_(ex.allocator()) {}
    int row(Value* values, size_t count) override {
        ++rows_;
        if (mode_ == exists) return k_stop;
        if (mode_ == scalar) {
            if (rows_ > 1) {
                ex_.status().fail(k_error_runtime, VDB_F("a subquery used as a value returned more than one row"));
                return k_error;
            }
            value_ = count ? values[0] : Value::null();
            if (value_.type == k_text || (value_.flags & k_has_text)) {
                if (!text_.append(value_.s, value_.n)) return ex_.memory() ? k_error : k_error;
                value_.s = text_.c_str();
            }
            return k_go;
        }
        if (!count || values[0].type == k_null) {
            has_null_ = true;
            return k_go;
        }
        key_.clear();
        bool added;
        if (!key_append(key_, values[0], ex_.decimal()) || index_->find_or_add(key_.c_str(), key_.size(), added) < 0)
            return ex_.memory() ? k_error : k_error;
        return k_go;
    }
    size_t rows() const { return rows_; }
    bool has_null() const { return has_null_; }
    const Value& value() const { return value_; }

private:
    Executor& ex_;
    Mode mode_;
    HashIndex* index_;
    size_t rows_;
    bool has_null_;
    Value value_;
    Text key_;
    Text text_;
};

// ── Executor ─────────────────────────────────────────────────────────────────

Executor::Executor(Database& db, Pool& statement_pool, const char* sql, const Value* params, size_t param_count,
                   Status& status)
    : db_(db),
      allocator_(&db.allocator()),
      pool_(statement_pool),
      scratch_(db.allocator(), VDB_SCRATCH_CHUNK),
      regex_work_(db.allocator()),
      sql_(sql),
      params_(params),
      param_count_(param_count),
      status_(status),
      changes_(0),
      decimal_(db.options().decimal_separator),
      depth_(0),
      subquery_depth_(0),
      owned_tables_(db.allocator()),
      owned_indexes_(db.allocator()),
      names_(db.allocator()) {}

Executor::~Executor() {
    for (size_t i = 0; i < owned_tables_.size(); ++i) destroy(*allocator_, owned_tables_[i]);
    for (size_t i = 0; i < owned_indexes_.size(); ++i) destroy(*allocator_, owned_indexes_[i]);
}

bool Executor::memory() {
    if (status_.ok()) status_.fail_memory();
    return false;
}

char* Executor::text(size_t length) {
    char* p = static_cast<char*>(scratch_.allocate(length + 1));
    if (!p) {
        memory();
        return 0;
    }
    p[length] = 0;
    return p;
}

bool Executor::own(Table* table) {
    if (!owned_tables_.push(table)) {
        destroy(*allocator_, table);
        return memory();
    }
    return true;
}

bool Executor::own(HashIndex* index) {
    if (!owned_indexes_.push(index)) {
        destroy(*allocator_, index);
        return memory();
    }
    return true;
}

bool Executor::enter() {
    if (++depth_ > VDB_MAX_DEPTH) {
        if (status_.ok()) status_.fail(k_error_limit, VDB_F("queries nested too deeply (limit %d)"), VDB_MAX_DEPTH);
        return false;
    }
    return true;
}

void Executor::count_name(const char* name) {
    if (name && !names_.push(name)) memory();
}

void Executor::count_expr(Expr* e) {
    for (; e; e = e->next) {
        if (e->kind == e_in_table) count_name(e->name);
        if (e->query) count_query(e->query);
        count_expr(e->a);
        count_expr(e->b);
        count_expr(e->c);
        count_expr(e->args);
    }
}

void Executor::count_query(Query* q) {
    for (Cte* c = q->with; c; c = c->next) count_query(c->query);
    bool first = true;
    for (Stage* s = q->stages; s; s = s->next, first = false) {
        for (Core* core = s->body->cores; core; core = core->next) {
            if (core->from_omitted && first) count_name(k_previous);
            for (FromItem* f = core->from; f; f = f->next) {
                count_name(f->name);
                if (f->query) count_query(f->query);
                count_expr(f->on);
            }
            for (SelectItem* i = core->items; i; i = i->next) count_expr(i->expr);
            count_expr(core->where);
            count_expr(core->group_by);
            count_expr(core->having);
        }
        for (OrderItem* o = s->body->order; o; o = o->next) count_expr(o->expr);
        count_expr(s->body->limit);
        count_expr(s->body->offset);
    }
}

void Executor::count_names(Statement* s) {
    names_.clear();
    if (s->query) count_query(s->query);
    count_expr(s->where);
    for (ValuesRow* r = s->rows; r; r = r->next) count_expr(r->values);
    for (Assignment* a = s->assignments; a; a = a->next) count_expr(a->value);
    if (s->table) count_name(s->table);
}

size_t Executor::occurrences(const char* name) const {
    size_t n = 0;
    for (size_t i = 0; i < names_.size(); ++i) n += names_equal(names_[i], name);
    return n;
}

Table* Executor::resolve_table(const char* name, Scope* scope, bool driving, bool& stream, Status& status) {
    stream = false;
    for (Scope* s = scope; s; s = s->outer) {
        if (names_equal(s->name, name)) return s->table;
    }
    Table* t = db_.table(name);
    if (!t && db_.options().table_resolver) {
        t = db_.options().table_resolver(db_, name, db_.options().resolver_user, status);
        if (!status.ok()) return 0;
    }
    if (!t) return 0;
    if (t->streamed()) {
        status.fail(k_error_misuse,
                    VDB_F("table %s was streamed by an earlier statement and has no rows left (attach it again, or set "
                          "options().streaming = false to keep streamed tables)"),
                    t->name());
        return 0;
    }
    if (t->stream_pending()) {
        if (driving && subquery_depth_ == 0 && db_.options().streaming && occurrences(t->name()) <= 1) {
            stream = true;
        } else if (!t->load_stream(status)) {
            return 0;
        }
    }
    return t;
}

// ── evaluation ───────────────────────────────────────────────────────────────

int Executor::truth(Expr* e, Frame* frame) {
    Value v;
    if (!eval(e, frame, v)) return -2;
    return value_truth(v, decimal_);
}

namespace {

bool add_overflows(int64_t a, int64_t b) { return (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b); }
bool sub_overflows(int64_t a, int64_t b) { return (b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b); }
bool mul_overflows(int64_t a, int64_t b, int64_t& out) {
#if defined(__GNUC__)
    return __builtin_mul_overflow(a, b, &out);
#else
    if (a == 0 || b == 0) {
        out = 0;
        return false;
    }
    double d = static_cast<double>(a) * static_cast<double>(b);
    if (d >= 9.2e18 || d <= -9.2e18) return true;
    out = a * b;
    return false;
#endif
}

Value real_or_null(double r) {
    if (!finite(r)) return Value::null();
    return Value::real(r);
}

}  // namespace

bool Executor::eval_binary(Expr* e, Frame* frame, Value& out) {
    Value a, b;
    if (!eval(e->a, frame, a) || !eval(e->b, frame, b)) return false;
    uint8_t op = e->op;
    if (op == op_concat) {
        if (a.is_null() || b.is_null()) {
            out = Value::null();
            return true;
        }
        const char *sa, *sb;
        size_t na, nb;
        if (!value_text(a, decimal_, scratch_, sa, na) || !value_text(b, decimal_, scratch_, sb, nb)) return memory();
        char* s = text(na + nb);
        if (!s) return false;
        memcpy(s, sa, na);
        memcpy(s + na, sb, nb);
        out = Value::text(s, na + nb);
        return true;
    }
    if (op >= op_eq && op <= op_ge) {
        int c;
        if (!compare_sql(a, b, decimal_, c)) {
            out = Value::null();
            return true;
        }
        bool r = op == op_eq ? c == 0 : op == op_ne ? c != 0 : op == op_lt ? c < 0 : op == op_le ? c <= 0 : op == op_gt ? c > 0 : c >= 0;
        out = Value::boolean(r);
        return true;
    }
    if (op == op_distinct || op == op_not_distinct) {
        bool same;
        if (a.is_null() || b.is_null()) {
            same = a.is_null() && b.is_null();
        } else {
            int c;
            same = compare_sql(a, b, decimal_, c) && c == 0;
        }
        out = Value::boolean(op == op_distinct ? !same : same);
        return true;
    }
    Number x, y;
    if (!value_number(a, decimal_, x) || !value_number(b, decimal_, y)) {
        out = Value::null();
        return true;
    }
    if (x.is_int && y.is_int) {
        int64_t r;
        switch (op) {
            case op_add:
                if (!add_overflows(x.i, y.i)) {
                    out = Value::integer(x.i + y.i);
                    return true;
                }
                break;
            case op_sub:
                if (!sub_overflows(x.i, y.i)) {
                    out = Value::integer(x.i - y.i);
                    return true;
                }
                break;
            case op_mul:
                if (!mul_overflows(x.i, y.i, r)) {
                    out = Value::integer(r);
                    return true;
                }
                break;
            case op_div:
                if (y.i == 0) {
                    out = Value::null();
                    return true;
                }
                if (!(x.i == INT64_MIN && y.i == -1) && x.i % y.i == 0) {
                    out = Value::integer(x.i / y.i);
                    return true;
                }
                break;
            default:  // op_mod
                if (y.i == 0) {
                    out = Value::null();
                    return true;
                }
                out = Value::integer(y.i == -1 ? 0 : x.i % y.i);
                return true;
        }
    }
    double p = x.real(), q = y.real();
    switch (op) {
        case op_add: out = real_or_null(p + q); break;
        case op_sub: out = real_or_null(p - q); break;
        case op_mul: out = real_or_null(p * q); break;
        case op_div: out = q == 0 ? Value::null() : real_or_null(p / q); break;
        default: out = q == 0 ? Value::null() : real_or_null(fmod(p, q)); break;
    }
    return true;
}

const Regex* pattern_regex(Executor& ex, Expr* owner, bool constant, const Value& pattern, unsigned flags) {
    if (constant && owner->cache) return static_cast<const Regex*>(owner->cache);
    const char* s;
    size_t n;
    if (!value_text(pattern, ex.decimal(), ex.scratch(), s, n)) {
        ex.memory();
        return 0;
    }
    Pool& pool = constant ? ex.statement_pool() : ex.scratch();
    const Regex* re = Regex::compile(pool, s, n, flags, ex.status());
    if (re && constant) owner->cache = const_cast<Regex*>(re);
    return re;
}

namespace {
bool is_constant(Expr* e) { return e->kind == e_literal || e->kind == e_param; }
}  // namespace

bool Executor::eval_match(Expr* e, Frame* frame, Value& out) {
    Value a, p;
    if (!eval(e->a, frame, a) || !eval(e->b, frame, p)) return false;
    if (a.is_null() || p.is_null()) {
        out = Value::null();
        return true;
    }
    const char *s, *ps;
    size_t n, pn;
    if (!value_text(a, decimal_, scratch_, s, n) || !value_text(p, decimal_, scratch_, ps, pn)) return memory();
    uint32_t escape = 0xFFFFFFFFu;
    if (e->c) {
        Value esc;
        if (!eval(e->c, frame, esc)) return false;
        const char* es;
        size_t en;
        if (!value_text(esc, decimal_, scratch_, es, en)) return memory();
        if (en) utf8_decode(es, en, escape);
    }
    bool matched = false;
    switch (e->op) {
        case m_like: matched = like_match(s, n, ps, pn, escape, false); break;
        case m_ilike: matched = like_match(s, n, ps, pn, escape, true); break;
        case m_glob: matched = glob_match(s, n, ps, pn); break;
        case m_similar: {
            const Regex* re = static_cast<const Regex*>(e->cache);
            if (!re || !is_constant(e->b)) {
                Text rx(*allocator_);
                if (!similar_to_regex(ps, pn, e->c ? escape : '\\', rx)) return memory();
                Pool& pool = is_constant(e->b) && !e->c ? pool_ : scratch_;
                re = Regex::compile(pool, rx.c_str(), rx.size(), 0, status_);
                if (!re) return false;
                if (is_constant(e->b) && !e->c) e->cache = const_cast<Regex*>(re);
            }
            int r = re->search(s, n, 0, 0, 0, regex_work_);
            if (r < 0) return memory();
            matched = r == 1;
            break;
        }
        default: {
            const Regex* re = pattern_regex(*this, e, is_constant(e->b), p, e->op == m_iregexp ? k_regex_icase : 0);
            if (!re) return false;
            int r = re->search(s, n, 0, 0, 0, regex_work_);
            if (r < 0) return memory();
            matched = r == 1;
            break;
        }
    }
    out = Value::boolean(matched != (e->negated != 0));
    return true;
}

bool Executor::eval_in(Expr* e, Frame* frame, Value& out) {
    Value x;
    if (!eval(e->a, frame, x)) return false;
    if (e->kind == e_in_list) {
        if (x.is_null()) {
            out = e->args ? Value::null() : Value::boolean(e->negated != 0);
            return true;
        }
        bool unknown = false;
        for (Expr* item = e->args; item; item = item->next) {
            Value v;
            if (!eval(item, frame, v)) return false;
            int c;
            if (!compare_sql(x, v, decimal_, c)) {
                unknown = true;
            } else if (c == 0) {
                out = Value::boolean(!e->negated);
                return true;
            }
        }
        out = unknown ? Value::null() : Value::boolean(e->negated != 0);
        return true;
    }
    // e_in_query and e_in_table: a key set, cached unless correlated (spec 3.9).
    HashIndex* index = e->cache_state == 2 ? static_cast<HashIndex*>(e->cache) : 0;
    bool has_null = e->slot == 1;
    if (!index) {
        index = make_with<HashIndex>(*allocator_);
        if (!index || !own(index)) return memory();
        if (e->kind == e_in_table) {
            Table* t = static_cast<Table*>(e->cache);
            for (size_t r = 0; t && r < t->row_count(); ++r) {
                Value v;
                record_field(t->record(r), 0, v);
                if (v.is_null()) {
                    has_null = true;
                    continue;
                }
                Text key(*allocator_);
                bool added;
                if (!key_append(key, v, decimal_) || index->find_or_add(key.c_str(), key.size(), added) < 0) return memory();
            }
        } else {
            SubquerySink sink(*this, SubquerySink::set, index);
            ++subquery_depth_;
            int rc = run_query(e->query, frame->run ? frame->run->scope() : 0, frame, e, &sink);
            --subquery_depth_;
            if (rc == k_error) return false;
            has_null = sink.has_null();
        }
        if (e->kind == e_in_table || !e->correlated) {
            e->cache = index;
            e->cache_state = 2;
            e->slot = has_null ? 1 : 0;
        }
    }
    if (x.is_null()) {
        out = index->size() || has_null ? Value::null() : Value::boolean(e->negated != 0);
        return true;
    }
    Text key(*allocator_);
    if (!key_append(key, x, decimal_)) return memory();
    if (index->find(key.c_str(), key.size()) >= 0) {
        out = Value::boolean(!e->negated);
    } else {
        out = has_null ? Value::null() : Value::boolean(e->negated != 0);
    }
    return true;
}

bool Executor::eval_subquery(Expr* e, Frame* frame, Value& out) {
    if (e->cache_state == 2) {
        out = e->literal;
        return true;
    }
    Scope* scope = frame && frame->run ? frame->run->scope() : 0;
    SubquerySink sink(*this, e->kind == e_exists ? SubquerySink::exists : SubquerySink::scalar, 0);
    ++subquery_depth_;
    int rc = run_query(e->query, scope, frame, e, &sink);
    --subquery_depth_;
    if (rc == k_error) return false;
    if (e->kind == e_exists) out = Value::boolean(sink.rows() > 0);
    else out = sink.rows() ? sink.value() : Value::null();
    // The sink's text dies with it: keep a copy for the caller (or the cache).
    if (!value_copy(e->correlated ? scratch_ : pool_, out)) return memory();
    if (!e->correlated) {
        e->literal = out;
        e->cache_state = 2;
    }
    return true;
}

bool Executor::eval_case(Expr* e, Frame* frame, Value& out) {
    Value base;
    if (e->a && !eval(e->a, frame, base)) return false;
    for (Expr* w = e->args; w; w = w->next) {
        bool hit;
        if (e->a) {
            Value v;
            if (!eval(w->a, frame, v)) return false;
            int c;
            hit = compare_sql(base, v, decimal_, c) && c == 0;
        } else {
            int t = truth(w->a, frame);
            if (t == -2) return false;
            hit = t == k_true;
        }
        if (hit) return eval(w->b, frame, out);
    }
    if (e->c) return eval(e->c, frame, out);
    out = Value::null();
    return true;
}

bool Executor::eval_cast(Expr* e, Frame* frame, Value& out) {
    Value v;
    if (!eval(e->a, frame, v)) return false;
    if (v.is_null()) {
        out = v;
        return true;
    }
    if (e->op == cast_text) {
        const char* s;
        size_t n;
        if (!value_text(v, decimal_, scratch_, s, n)) return memory();
        out = Value::text(s, n, k_strict);
        return true;
    }
    Number num;
    if (!value_number(v, decimal_, num)) {
        out = Value::null();
        return true;
    }
    if (e->op == cast_integer) {
        int64_t whole;
        if (num.is_int) out = Value::integer(num.i);
        else if (real_is_integral(trunc(num.r), whole)) out = Value::integer(whole);
        else out = Value::null();
    } else if (e->op == cast_real) {
        out = Value::real(num.real());
    } else {
        out = number_value(num);
    }
    return true;
}

bool Executor::eval_function(Expr* e, Frame* frame, Value& out) {
    const size_t k_inline_args = 8;
    Value inline_args[k_inline_args];
    size_t count = 0;
    for (Expr* a = e->args; a; a = a->next) ++count;
    Value* args = inline_args;
    if (count > k_inline_args) {
        args = static_cast<Value*>(scratch_.allocate(count * sizeof(Value)));
        if (!args) return memory();
    }
    size_t i = 0;
    FunctionInfo info = function_info(static_cast<Function>(e->function));
    for (Expr* a = e->args; a; a = a->next, ++i) {
        if (!eval(a, frame, args[i])) return false;
        if (args[i].is_null() && !(info.flags & fn_takes_null)) {
            out = Value::null();
            return true;
        }
    }
    return call_function(*this, e, frame, args, count, out);
}

bool Executor::eval(Expr* e, Frame* frame, Value& out) {
    switch (e->kind) {
        case e_literal: out = e->literal; return true;
        case e_param:
            if (e->slot < 0 || static_cast<size_t>(e->slot) >= param_count_) {
                status_.fail(k_error_misuse, VDB_F("no value for parameter ?%d"), static_cast<int>(e->slot + 1));
                return false;
            }
            out = params_[e->slot];
            return true;
        case e_column: {
            Frame* f = frame;
            for (int d = 0; d < e->depth && f; ++d) f = f->outer;
            if (!f || e->source < 0) {
                status_.fail(k_error_runtime, VDB_F("column %s used outside its query"), e->name);
                return false;
            }
            Source& s = f->sources[e->source];
            out = s.null_row ? Value::null() : s.row[e->column];
            if (out.is_null() && e->alt_source >= 0) {
                Source& alt = f->sources[e->alt_source];
                out = alt.null_row ? Value::null() : alt.row[e->alt_column];
            }
            return true;
        }
        case e_output: {
            if (frame->outputs) {
                out = frame->outputs[e->slot];
                return true;
            }
            return frame->run->output_value(static_cast<size_t>(e->slot), out);
        }
        case e_aggregate:
            if (!frame->aggregates) {
                status_.fail(k_error_misuse, VDB_F("aggregate functions are not allowed here"));
                return false;
            }
            out = frame->aggregates[e->slot];
            return true;
        case e_unary_minus: {
            Value v;
            if (!eval(e->a, frame, v)) return false;
            Number n;
            if (!value_number(v, decimal_, n)) out = Value::null();
            else if (n.is_int && n.i != INT64_MIN) out = Value::integer(-n.i);
            else out = Value::real(-n.real());
            return true;
        }
        case e_binary: return eval_binary(e, frame, out);
        case e_and:
        case e_or: {
            int a = truth(e->a, frame);
            if (a == -2) return false;
            bool is_and = e->kind == e_and;
            if (is_and && a == k_false) {
                out = Value::boolean(false);
                return true;
            }
            if (!is_and && a == k_true) {
                out = Value::boolean(true);
                return true;
            }
            int b = truth(e->b, frame);
            if (b == -2) return false;
            if (is_and) {
                out = b == k_false ? Value::boolean(false) : (a == k_unknown || b == k_unknown) ? Value::null() : Value::boolean(true);
            } else {
                out = b == k_true ? Value::boolean(true) : (a == k_unknown || b == k_unknown) ? Value::null() : Value::boolean(false);
            }
            return true;
        }
        case e_not: {
            int a = truth(e->a, frame);
            if (a == -2) return false;
            out = a == k_unknown ? Value::null() : Value::boolean(a == k_false);
            return true;
        }
        case e_is: {
            Value v;
            if (!eval(e->a, frame, v)) return false;
            bool r;
            if (e->op == is_null_value) r = v.is_null();
            else if (e->op == is_true_value) r = value_truth(v, decimal_) == k_true;
            else r = value_truth(v, decimal_) == k_false;
            out = Value::boolean(r != (e->negated != 0));
            return true;
        }
        case e_between: {
            Value x, low, high;
            if (!eval(e->a, frame, x) || !eval(e->b, frame, low) || !eval(e->c, frame, high)) return false;
            int c1, c2;
            bool k1 = compare_sql(x, low, decimal_, c1), k2 = compare_sql(x, high, decimal_, c2);
            int t1 = k1 ? (c1 >= 0 ? k_true : k_false) : k_unknown;
            int t2 = k2 ? (c2 <= 0 ? k_true : k_false) : k_unknown;
            int t = (t1 == k_false || t2 == k_false) ? k_false : (t1 == k_unknown || t2 == k_unknown) ? k_unknown : k_true;
            if (t == k_unknown) out = Value::null();
            else out = Value::boolean((t == k_true) != (e->negated != 0));
            return true;
        }
        case e_match: return eval_match(e, frame, out);
        case e_in_list:
        case e_in_query:
        case e_in_table: return eval_in(e, frame, out);
        case e_exists:
        case e_subquery: return eval_subquery(e, frame, out);
        case e_case: return eval_case(e, frame, out);
        case e_cast: return eval_cast(e, frame, out);
        case e_function: return eval_function(e, frame, out);
        default:
            status_.fail(k_error_misuse, VDB_F("'*' is only allowed in SELECT * and COUNT(*)"));
            return false;
    }
}

// ── CoreRun ──────────────────────────────────────────────────────────────────

bool CoreRun::add_source(const char* name, Query* query, const char* alias, uint8_t join, Expr* on,
                         NameList* using_columns, Table* pushed_schema) {
    if (sources_.size() >= k_max_sources) {
        ex_.status().fail(k_error_limit, VDB_F("too many tables in FROM (limit %d)"), static_cast<int>(k_max_sources));
        return false;
    }
    Source s;
    memset(static_cast<void*>(&s), 0, sizeof(s));
    s.alias = alias ? alias : name;
    s.join = join;
    s.on = on;
    s.using_columns = using_columns;
    if (query) {
        s.table = ex_.run_to_table(query, scope_, frame_.outer, frame_.owner, alias);
        if (!s.table) return false;
        s.owned = true;
    } else if (pushed_schema && names_equal(name, k_previous) && sources_.empty()) {
        s.table = pushed_schema;
        s.pushed = true;
    } else {
        bool stream = false;
        s.table = ex_.resolve_table(name, scope_, sources_.empty(), stream, ex_.status());
        if (!ex_.status().ok()) return false;
        if (!s.table) {
            ex_.status().fail(k_error_name, VDB_F("no such table: %s"), name);
            Database& db = ex_.database();
            const size_t k_limit = 3;
            size_t best = k_limit;
            const char* hint = 0;
            for (size_t i = 0; i < db.table_count(); ++i) {
                size_t d = edit_distance(name, db.table_at(i)->name(), k_limit);
                if (d < best) {
                    best = d;
                    hint = db.table_at(i)->name();
                }
            }
            if (hint) ex_.status().add(VDB_F(" (did you mean %s?)"), hint);
            return false;
        }
        s.streaming = stream;
    }
    s.width = s.table->column_count();
    s.row = static_cast<Value*>(a_.allocate((s.width + 1) * sizeof(Value)));
    if (!s.row) return ex_.memory();
    for (size_t i = 0; i <= s.width; ++i) s.row[i] = Value::null();
    if (!sources_.push(s)) {
        a_.release(s.row);
        if (query) destroy(a_, s.table);
        return ex_.memory();
    }
    return true;
}

int CoreRun::find_output(const char* name) const {
    for (size_t i = 0; i < names_.size(); ++i) {
        if (names_equal(names_[i], name)) return static_cast<int>(i);
    }
    return -1;
}

void CoreRun::column_hint(const char* name) {
    const size_t k_limit = 3;
    size_t best = k_limit;
    const char* hint = 0;
    for (Frame* f = &frame_; f; f = f->outer) {
        for (size_t s = 0; s < f->count; ++s) {
            Table* t = f->sources[s].table;
            for (size_t c = 0; c < t->column_count(); ++c) {
                size_t d = edit_distance(name, t->column_name(c), k_limit);
                if (d < best) {
                    best = d;
                    hint = t->column_name(c);
                }
            }
        }
    }
    if (hint) ex_.status().add(VDB_F(" (did you mean %s?)"), hint);
}

namespace {
bool in_names(NameList* list, const char* name) {
    for (; list; list = list->next) {
        if (names_equal(list->name, name)) return true;
    }
    return false;
}
}  // namespace

bool CoreRun::bind_column(Expr* e, int clause) {
    if (clause == c_order && !e->qualifier) {
        int o = find_output(e->name);
        if (o >= 0) {
            e->kind = e_output;
            e->slot = o;
            return true;
        }
    }
    int depth = 0;
    for (Frame* f = &frame_; f; f = f->outer, ++depth) {
        int found = 0;
        int src[2] = {-1, -1}, col[2] = {-1, -1};
        for (size_t s = 0; s < f->count; ++s) {
            if (e->qualifier && !names_equal(f->sources[s].alias, e->qualifier)) continue;
            int c = f->sources[s].table->find_column(e->name);
            if (c < 0) continue;
            if (found < 2) {
                src[found] = static_cast<int>(s);
                col[found] = c;
            }
            ++found;
        }
        if (found > 1) {
            // A USING column reads the first non-NULL side (spec 3.2).
            bool using_column = found == 2 && in_names(f->sources[src[1]].using_columns, e->name);
            if (!using_column) {
                ex_.status().fail(k_error_name, VDB_F("ambiguous column: %s (write table.%s)"), e->name, e->name);
                return false;
            }
            e->alt_source = static_cast<int16_t>(src[1]);
            e->alt_column = col[1];
        }
        if (found) {
            e->depth = static_cast<int16_t>(depth);
            e->source = static_cast<int16_t>(src[0]);
            e->column = col[0];
            Frame* g = &frame_;
            for (int d = 0; d < depth; ++d, g = g->outer) {
                if (g->owner) g->owner->correlated = 1;
            }
            return true;
        }
        if (depth == 0 && (clause == c_group || clause == c_having) && !e->qualifier) {
            int o = find_output(e->name);
            if (o >= 0) {
                e->kind = e_output;
                e->slot = o;
                return true;
            }
        }
    }
    if (e->qualifier) ex_.status().fail(k_error_name, VDB_F("no such column: %s.%s"), e->qualifier, e->name);
    else ex_.status().fail(k_error_name, VDB_F("no such column: %s"), e->name);
    column_hint(e->name);
    return false;
}

bool CoreRun::bind(Expr* e, int clause) {
    for (; e; e = e->next) {
        switch (e->kind) {
            case e_column:
                if (clause == c_constant) {
                    ex_.status().fail(k_error_misuse, VDB_F("a column cannot be used here: %s"), e->name);
                    return false;
                }
                if (!bind_column(e, clause)) return false;
                break;
            case e_aggregate: {
                if (clause != c_select && clause != c_having && clause != c_order) {
                    ex_.status().fail(k_error_misuse,
                                      clause == c_where ? VDB_F("aggregate functions are not allowed in WHERE (use HAVING)")
                                                        : VDB_F("aggregate functions are not allowed here"));
                    return false;
                }
                e->slot = static_cast<int32_t>(aggregates_.size());
                if (!aggregates_.push(e)) return ex_.memory();
                if (e->args && e->args->kind != e_star && !bind(e->args, c_where)) {
                    if (ex_.status().code() == k_error_misuse) {
                        ex_.status().fail(k_error_misuse, VDB_F("aggregate functions cannot be nested"));
                    }
                    return false;
                }
                break;
            }
            case e_in_table: {
                if (!bind(e->a, clause)) return false;
                bool stream;
                Table* t = ex_.resolve_table(e->name, scope_, false, stream, ex_.status());
                if (!ex_.status().ok()) return false;
                if (!t) {
                    ex_.status().fail(k_error_name, VDB_F("no such table: %s"), e->name);
                    return false;
                }
                if (e->cache_state != 2) {
                    e->cache = t;
                    e->cache_state = 1;
                }
                break;
            }
            case e_subquery:
            case e_exists:
                break;  // bound when it runs, against this frame
            case e_in_query:
                if (!bind(e->a, clause)) return false;
                break;
            case e_when:
                if (!bind(e->a, clause) || !bind(e->b, clause)) return false;
                break;
            default:
                if ((e->a && !bind(e->a, clause)) || (e->b && !bind(e->b, clause)) || (e->c && !bind(e->c, clause)) ||
                    (e->args && !bind(e->args, clause)))
                    return false;
                break;
        }
    }
    return true;
}

bool CoreRun::expand_items() {
    char* text = 0;
    for (SelectItem* item = core_->items; item; item = item->next) {
        if (item->star) {
            bool any = false;
            for (size_t s = 0; s < sources_.size(); ++s) {
                if (item->star_qualifier && !names_equal(sources_[s].alias, item->star_qualifier)) continue;
                any = true;
                Table* t = sources_[s].table;
                for (size_t c = 0; c < t->column_count(); ++c) {
                    OutCol o = {0, static_cast<int16_t>(s), static_cast<int32_t>(c), t->column_name(c)};
                    if (!outs_.push(o)) return ex_.memory();
                }
            }
            if (!any) {
                if (item->star_qualifier) ex_.status().fail(k_error_name, VDB_F("no such table: %s"), item->star_qualifier);
                else ex_.status().fail(k_error_misuse, VDB_F("SELECT * needs a FROM"));
                return false;
            }
            continue;
        }
        OutCol o = {item->expr, -1, -1, item->alias};
        if (!o.name && item->expr->kind == e_column) o.name = item->expr->name;
        if (!o.name) {
            uint32_t start = item->start, end = item->end;
            const char* sql = ex_.sql();
            while (end > start && is_space(sql[end - 1])) --end;
            text = ex_.statement_pool().copy(sql + start, end - start);
            if (!text) return ex_.memory();
            o.name = text;
        }
        if (!outs_.push(o)) return ex_.memory();
    }
    // Unique names (spec A8).
    for (size_t i = 0; i < outs_.size(); ++i) {
        const char* name = outs_[i].name;
        int copy = 1;
        for (size_t j = 0; j < i; ++j) {
            if (names_equal(names_[j], name)) {
                ++copy;
                j = static_cast<size_t>(-1);  // restart with the new name
                Text t(a_);
                t.append(outs_[i].name);
                t.append_char('_');
                t.append_int(copy);
                char* unique = ex_.statement_pool().copy(t.c_str(), t.size());
                if (!t.ok() || !unique) return ex_.memory();
                name = unique;
            }
        }
        if (!names_.push(name)) return ex_.memory();
    }
    return true;
}

bool CoreRun::bind_order() {
    for (OrderItem* o = order_; o; o = o->next) {
        Expr* e = o->expr;
        if (e->kind == e_literal && e->literal.type == k_integer) {
            int64_t position = e->literal.i;
            if (position < 1 || static_cast<size_t>(position) > outs_.size()) {
                ex_.status().fail(k_error_misuse, VDB_F("ORDER BY position %d is not a column of the result"), static_cast<int>(position));
                return false;
            }
            e->kind = e_output;
            e->slot = static_cast<int32_t>(position - 1);
        } else if (!bind(e, c_order)) {
            return false;
        }
        if (!order_exprs_.push(e)) return ex_.memory();
    }
    return true;
}

bool CoreRun::prepare(Table* pushed_schema) {
    if (core_->from_omitted) {
        bool previous = pushed_schema != 0;
        for (Scope* s = scope_; s && !previous; s = s->outer) previous = names_equal(s->name, k_previous);
        if (!previous) previous = ex_.database().table(k_previous) != 0;
        if (!previous && ex_.database().options().table_resolver) {
            bool stream;
            previous = ex_.resolve_table(k_previous, scope_, true, stream, ex_.status()) != 0;
            if (!ex_.status().ok()) return false;
        }
        if (previous && !add_source(k_previous, 0, k_previous, join_first, 0, 0, pushed_schema)) return false;
    }
    for (FromItem* f = core_->from; f; f = f->next) {
        if (!add_source(f->name, f->query, f->alias, f->join, f->on, f->using_columns, pushed_schema)) return false;
    }
    frame_.sources = sources_.data();
    frame_.count = sources_.size();
    if (core_->items == 0) {
        SelectItem* star = make<SelectItem>(ex_.statement_pool());
        if (!star) return ex_.memory();
        star->star = true;
        core_->items = star;
    }
    if (!expand_items()) return false;
    for (size_t i = 0; i < outs_.size(); ++i) {
        if (outs_[i].expr && !bind(outs_[i].expr, c_select)) return false;
    }
    for (size_t k = 1; k < sources_.size(); ++k) {
        Source& s = sources_[k];
        // USING (a, b) becomes left.a = right.a AND ...
        for (NameList* u = s.using_columns; u; u = u->next) {
            int left = -1, left_col = -1;
            for (size_t j = 0; j < k && left < 0; ++j) {
                int c = sources_[j].table->find_column(u->name);
                if (c >= 0) {
                    left = static_cast<int>(j);
                    left_col = c;
                }
            }
            int right_col = s.table->find_column(u->name);
            if (left < 0 || right_col < 0) {
                ex_.status().fail(k_error_name, VDB_F("USING column %s is not in both tables"), u->name);
                return false;
            }
            Expr* l = make<Expr>(ex_.statement_pool());
            Expr* r = make<Expr>(ex_.statement_pool());
            Expr* eq = make<Expr>(ex_.statement_pool());
            if (!l || !r || !eq) return ex_.memory();
            l->kind = r->kind = e_column;
            l->name = r->name = u->name;
            l->source = static_cast<int16_t>(left);
            l->column = left_col;
            r->source = static_cast<int16_t>(k);
            r->column = right_col;
            l->alt_source = r->alt_source = -1;
            eq->kind = e_binary;
            eq->op = op_eq;
            eq->a = l;
            eq->b = r;
            eq->alt_source = -1;
            if (s.on) {
                Expr* both = make<Expr>(ex_.statement_pool());
                if (!both) return ex_.memory();
                both->kind = e_and;
                both->a = s.on;
                both->b = eq;
                s.on = both;
            } else {
                s.on = eq;
            }
        }
        if (s.on && !s.using_columns && !bind(s.on, c_on)) return false;
        if (!plan_hash(s, k)) return false;
    }
    if (core_->where && !bind(core_->where, c_where)) return false;
    for (Expr* g = core_->group_by; g; g = g->next) {
        if (g->kind == e_literal && g->literal.type == k_integer) {
            int64_t position = g->literal.i;
            if (position < 1 || static_cast<size_t>(position) > outs_.size()) {
                ex_.status().fail(k_error_misuse, VDB_F("GROUP BY position %d is not a column of the result"), static_cast<int>(position));
                return false;
            }
            g->kind = e_output;
            g->slot = static_cast<int32_t>(position - 1);
        } else {
            Expr* next = g->next;
            g->next = 0;
            bool ok = bind(g, c_group);
            g->next = next;
            if (!ok) return false;
        }
        if (!group_by_.push(g)) return ex_.memory();
    }
    if (core_->having && !bind(core_->having, c_having)) return false;
    if (!bind_order()) return false;
    aggregate_ = core_->group_by != 0 || !aggregates_.empty() || core_->having != 0;
    outputs_ = static_cast<Value*>(a_.allocate((outs_.size() + 1) * sizeof(Value)));
    keys_ = static_cast<Value*>(a_.allocate((order_exprs_.size() + 1) * sizeof(Value)));
    agg_values_ = static_cast<Value*>(a_.allocate((aggregates_.size() + 1) * sizeof(Value)));
    if (!outputs_ || !keys_ || !agg_values_) return ex_.memory();
    return true;
}

namespace {
// The sources an expression reads at depth 0, as a bit set; all bits when it reads something unknown.
uint64_t source_mask(Expr* e) {
    uint64_t mask = 0;
    for (; e; e = e->next) {
        if (e->kind == e_column && e->depth == 0) {
            mask |= static_cast<uint64_t>(1) << e->source;
            if (e->alt_source >= 0) mask |= static_cast<uint64_t>(1) << e->alt_source;
        } else if (e->kind == e_subquery || e->kind == e_exists || e->kind == e_in_query || e->kind == e_aggregate ||
                   e->kind == e_output) {
            mask = ~static_cast<uint64_t>(0);
        }
        mask |= source_mask(e->a) | source_mask(e->b) | source_mask(e->c) | source_mask(e->args);
    }
    return mask;
}

void collect_equalities(Expr* e, Expr** found, int& count, int limit) {
    if (!e || count >= limit) return;
    if (e->kind == e_and) {
        collect_equalities(e->a, found, count, limit);
        collect_equalities(e->b, found, count, limit);
    } else if (e->kind == e_binary && e->op == op_eq) {
        found[count++] = e;
    }
}
}  // namespace

bool CoreRun::plan_hash(Source& s, size_t k) {
    if (!s.on) return true;
    Expr* equalities[k_max_join_keys];
    int count = 0;
    collect_equalities(s.on, equalities, count, k_max_join_keys);
    uint64_t self = static_cast<uint64_t>(1) << k;
    uint64_t earlier = self - 1;
    for (int i = 0; i < count; ++i) {
        Expr* eq = equalities[i];
        Expr* l = eq->a;
        Expr* r = eq->b;
        Expr* lnext = l->next;
        Expr* rnext = r->next;
        l->next = r->next = 0;
        uint64_t ml = source_mask(l), mr = source_mask(r);
        l->next = lnext;
        r->next = rnext;
        if (mr == self && (ml & ~earlier) == 0) {
            s.probe[s.keys] = l;
            s.build[s.keys++] = r;
        } else if (ml == self && (mr & ~earlier) == 0) {
            s.probe[s.keys] = r;
            s.build[s.keys++] = l;
        }
    }
    return true;
}

bool CoreRun::set_row(Source& s, size_t row) {
    decode_record(s.table->record(row), s.row, s.width);
    s.null_row = false;
    return true;
}

bool CoreRun::build_index(Source& s) {
    s.index = make_with<HashIndex>(a_);
    size_t rows = s.table->row_count();
    s.rows = rows;
    s.chain = static_cast<uint32_t*>(a_.allocate((rows + 1) * sizeof(uint32_t)));
    if (!s.index || !s.chain) return ex_.memory();
    Vec<uint32_t> heads(a_);
    Text key(a_);
    for (size_t r = 0; r < rows; ++r) {
        Pool::Mark mark = ex_.scratch().mark();
        set_row(s, r);
        key.clear();
        bool null_key = false;
        for (int i = 0; i < s.keys && !null_key; ++i) {
            Value v;
            if (!ex_.eval(s.build[i], &frame_, v)) return false;
            if (v.is_null()) null_key = true;
            else if (!key_append(key, v, ex_.decimal())) return ex_.memory();
        }
        ex_.scratch().release(mark);
        s.chain[r] = 0;
        if (null_key) continue;
        bool added;
        long id = s.index->find_or_add(key.c_str(), key.size(), added);
        if (id < 0) return ex_.memory();
        if (added && !heads.push(0)) return ex_.memory();
        // Keep rows in table order inside a bucket: append at the tail.
        if (!heads[static_cast<size_t>(id)]) {
            heads[static_cast<size_t>(id)] = static_cast<uint32_t>(r + 1);
        } else {
            uint32_t at = heads[static_cast<size_t>(id)];
            while (s.chain[at - 1]) at = s.chain[at - 1];
            s.chain[at - 1] = static_cast<uint32_t>(r + 1);
        }
    }
    s.heads = static_cast<uint32_t*>(a_.allocate((heads.size() + 1) * sizeof(uint32_t)));
    if (!s.heads) return ex_.memory();
    if (heads.size()) memcpy(s.heads, heads.data(), heads.size() * sizeof(uint32_t));
    s.null_row = false;
    return true;
}

int CoreRun::level(size_t k) {
    if (k == sources_.size()) return row_done();
    Source& s = sources_[k];
    bool any = false;
    bool right_side = s.join == join_right || s.join == join_full;
    if (s.keys && !s.index && !build_index(s)) return k_error;
    if (right_side && !s.matched) {
        s.matched = static_cast<uint8_t*>(a_.allocate(s.table->row_count() + 1));
        if (!s.matched) return ex_.memory() ? k_error : k_error;
        memset(s.matched, 0, s.table->row_count() + 1);
        s.rows = s.table->row_count();
    }
    uint32_t candidate = 0;
    size_t rows = s.index ? s.rows : s.table->row_count();
    if (s.index) {
        Text key(a_);
        bool null_key = false;
        for (int i = 0; i < s.keys && !null_key; ++i) {
            Value v;
            if (!ex_.eval(s.probe[i], &frame_, v)) return k_error;
            if (v.is_null()) null_key = true;
            else if (!key_append(key, v, ex_.decimal())) return ex_.memory() ? k_error : k_error;
        }
        long id = null_key ? -1 : s.index->find(key.c_str(), key.size());
        candidate = id >= 0 ? s.heads[id] : 0;
    }
    for (size_t r = 0;; ++r) {
        size_t row;
        if (s.index) {
            if (!candidate) break;
            row = candidate - 1;
            candidate = s.chain[row];
        } else {
            if (r >= rows) break;
            row = r;
        }
        set_row(s, row);
        if (s.on) {
            int t = ex_.truth(s.on, &frame_);
            if (t == -2) return k_error;
            if (t != k_true) continue;
        }
        any = true;
        if (right_side) s.matched[row] = 1;
        int rc = level(k + 1);
        if (rc != k_go) return rc;
    }
    if (!any && (s.join == join_left || s.join == join_full)) {
        s.null_row = true;
        int rc = level(k + 1);
        s.null_row = false;
        if (rc != k_go) return rc;
    }
    return k_go;
}

int CoreRun::post_pass(size_t k) {
    Source& s = sources_[k];
    if (!(s.join == join_right || s.join == join_full)) return k_go;
    size_t rows = s.table->row_count();
    for (size_t row = 0; row < rows; ++row) {
        if (s.matched && s.matched[row]) continue;
        for (size_t j = 0; j < k; ++j) sources_[j].null_row = true;
        set_row(s, row);
        Pool::Mark mark = ex_.scratch().mark();
        int rc = level(k + 1);
        ex_.scratch().release(mark);
        for (size_t j = 0; j < k; ++j) sources_[j].null_row = false;
        if (rc != k_go) return rc;
    }
    return k_go;
}

int CoreRun::drive() {
    if (sources_.empty()) {
        Pool::Mark mark = ex_.scratch().mark();
        int rc = row_done();
        ex_.scratch().release(mark);
        return rc == k_error ? k_error : finish();
    }
    Source& first = sources_[0];
    int rc = k_go;
    if (first.streaming) {
        RowStream* stream = first.table->stream();
        for (;;) {
            const Value* values;
            size_t count;
            int got = stream->next(values, count, ex_.status());
            if (got < 0) return k_error;
            if (got == 0) {
                break;
            }
            for (size_t i = 0; i < first.width; ++i) first.row[i] = i < count ? values[i] : Value::null();
            first.null_row = false;
            Pool::Mark mark = ex_.scratch().mark();
            rc = level(1);
            ex_.scratch().release(mark);
            if (rc != k_go) break;
        }
    } else {
        size_t rows = first.table->row_count();
        for (size_t r = 0; r < rows; ++r) {
            set_row(first, r);
            Pool::Mark mark = ex_.scratch().mark();
            rc = level(1);
            ex_.scratch().release(mark);
            if (rc != k_go) break;
        }
    }
    if (first.streaming) first.table->set_streamed();  // read to the end, or stopped early: either way it is spent
    if (rc == k_error) return rc;
    if (rc == k_go) {
        for (size_t k = 1; k < sources_.size(); ++k) {
            rc = post_pass(k);
            if (rc != k_go) break;
        }
    }
    if (rc == k_error) return rc;
    return finish();
}

int CoreRun::push(Value* values, size_t count) {
    Source& first = sources_[0];
    for (size_t i = 0; i < first.width; ++i) first.row[i] = i < count ? values[i] : Value::null();
    first.null_row = false;
    Pool::Mark mark = ex_.scratch().mark();
    int rc = level(1);
    ex_.scratch().release(mark);
    return rc;
}

int CoreRun::row_done() {
    if (core_->where) {
        int t = ex_.truth(core_->where, &frame_);
        if (t == -2) return k_error;
        if (t != k_true) return k_go;
    }
    return aggregate_ ? accumulate() : project();
}

// Evaluates outputs and order keys for the current row (or group) and hands them to the compound.
int CompoundRun_accept(CompoundRun* c, Value* outputs, size_t count, Value* keys, size_t key_count);

int CoreRun::project() {
    frame_.outputs = 0;
    for (size_t i = 0; i < outs_.size(); ++i) {
        if (!output_value(i, outputs_[i])) return k_error;
        if (!textify(outputs_[i], ex_.decimal(), ex_.scratch())) return ex_.memory() ? k_error : k_error;
    }
    frame_.outputs = outputs_;
    if (aggregate_ && core_->having) {
        int t = ex_.truth(core_->having, &frame_);
        if (t == -2) {
            frame_.outputs = 0;
            return k_error;
        }
        if (t != k_true) {
            frame_.outputs = 0;
            return k_go;
        }
    }
    for (size_t k = 0; k < order_exprs_.size(); ++k) {
        if (!ex_.eval(order_exprs_[k], &frame_, keys_[k])) return k_error;
        if (!textify(keys_[k], ex_.decimal(), ex_.scratch())) return ex_.memory() ? k_error : k_error;
    }
    frame_.outputs = 0;
    if (core_->distinct) {
        key_.clear();
        for (size_t i = 0; i < outs_.size(); ++i) {
            if (!key_append(key_, outputs_[i], ex_.decimal())) return ex_.memory() ? k_error : k_error;
        }
        bool added;
        if (distinct_.find_or_add(key_.c_str(), key_.size(), added) < 0) return ex_.memory() ? k_error : k_error;
        if (!added) return k_go;
    }
    return CompoundRun_accept(target_, outputs_, outs_.size(), keys_, order_exprs_.size());
}

void CoreRun::free_group(Group* g) {
    if (!g) return;
    for (size_t s = 0; s < sources_.size(); ++s) {
        if (g->reps && g->reps[s]) a_.release(g->reps[s]);
    }
    if (g->reps) a_.release(g->reps);
    if (g->aggs) {
        for (size_t i = 0; i < aggregates_.size(); ++i) {
            AggState& st = g->aggs[i];
            if (st.best_text) a_.release(st.best_text);
            if (st.concat) destroy(a_, st.concat);
            if (st.seen) destroy(a_, st.seen);
            if (st.values) destroy(a_, st.values);
        }
        a_.release(g->aggs);
    }
    a_.release(g);
}

int CoreRun::accumulate() {
    key_.clear();
    for (size_t i = 0; i < group_by_.size(); ++i) {
        Value v;
        if (!ex_.eval(group_by_[i], &frame_, v)) return k_error;
        if (!key_append(key_, v, ex_.decimal())) return ex_.memory() ? k_error : k_error;
    }
    bool added;
    long id = groups_.find_or_add(key_.c_str(), key_.size(), added);
    if (id < 0) return ex_.memory() ? k_error : k_error;
    Group* g;
    if (added) {
        g = static_cast<Group*>(a_.allocate(sizeof(Group)));
        if (!g) return ex_.memory() ? k_error : k_error;
        g->reps = static_cast<uint8_t**>(a_.allocate((sources_.size() + 1) * sizeof(uint8_t*)));
        g->aggs = static_cast<AggState*>(a_.allocate((aggregates_.size() + 1) * sizeof(AggState)));
        if (!g->reps || !g->aggs || !group_list_.push(g)) {
            if (g->reps) a_.release(g->reps);
            if (g->aggs) a_.release(g->aggs);
            a_.release(g);
            return ex_.memory() ? k_error : k_error;
        }
        memset(static_cast<void*>(g->reps), 0, (sources_.size() + 1) * sizeof(uint8_t*));
        memset(static_cast<void*>(g->aggs), 0, (aggregates_.size() + 1) * sizeof(AggState));
        for (size_t s = 0; s < sources_.size(); ++s) {
            Source& src = sources_[s];
            if (src.null_row) continue;
            for (size_t c = 0; c < src.width; ++c) {
                if (!textify(src.row[c], ex_.decimal(), ex_.scratch())) return ex_.memory() ? k_error : k_error;
            }
            g->reps[s] = encode_record(a_, src.row, src.width);
            if (!g->reps[s]) return ex_.memory() ? k_error : k_error;
        }
    } else {
        g = group_list_[static_cast<size_t>(id)];
    }
    for (size_t i = 0; i < aggregates_.size(); ++i) {
        if (!update_agg(g->aggs[i], aggregates_[i])) return k_error;
    }
    return k_go;
}

bool CoreRun::update_agg(AggState& st, Expr* agg) {
    Function f = static_cast<Function>(agg->function);
    Expr* arg = agg->args;
    if (f == fn_COUNT && arg->kind == e_star) {
        ++st.count;
        return true;
    }
    Value v;
    if (!ex_.eval(arg, &frame_, v)) return false;
    if (v.is_null()) return true;
    if (agg->distinct) {
        if (!st.seen && !(st.seen = make_with<HashIndex>(a_))) return ex_.memory();
        Text key(a_);
        bool added;
        if (!key_append(key, v, ex_.decimal()) || st.seen->find_or_add(key.c_str(), key.size(), added) < 0)
            return ex_.memory();
        if (!added) return true;
    }
    switch (f) {
        case fn_COUNT: ++st.count; return true;
        case fn_MIN:
        case fn_MAX: {
            ++st.count;
            int c = st.has_best ? compare_order(v, st.best, ex_.decimal()) : 0;
            if (st.has_best && (f == fn_MIN ? c >= 0 : c <= 0)) return true;
            const char* s;
            size_t n;
            if (!value_text(v, ex_.decimal(), ex_.scratch(), s, n)) return ex_.memory();
            char* copy = static_cast<char*>(a_.allocate(n + 1));
            if (!copy) return ex_.memory();
            memcpy(copy, s, n);
            copy[n] = 0;
            if (st.best_text) a_.release(st.best_text);
            st.best_text = copy;
            st.best = Value::text(copy, n);
            st.has_best = true;
            return true;
        }
        case fn_GROUP_CONCAT:
        case fn_STRING_AGG: {
            const char* s;
            size_t n;
            if (!value_text(v, ex_.decimal(), ex_.scratch(), s, n)) return ex_.memory();
            if (!st.concat && !(st.concat = make_with<Text>(a_))) return ex_.memory();
            if (st.count) {
                const char* sep = ",";
                size_t sep_n = 1;
                if (arg->next) {
                    Value sv;
                    if (!ex_.eval(arg->next, &frame_, sv)) return false;
                    if (!value_text(sv, ex_.decimal(), ex_.scratch(), sep, sep_n)) return ex_.memory();
                }
                st.concat->append(sep, sep_n);
            }
            ++st.count;
            if (!st.concat->append(s, n)) return ex_.memory();
            return true;
        }
        default: {
            Number num;
            if (!value_number(v, ex_.decimal(), num)) return true;  // D3: not a number, skipped
            ++st.count;
            st.any = true;
            if (num.is_int && !st.real && !add_overflows(st.isum, num.i)) {
                st.isum += num.i;
            } else {
                if (!st.real) {
                    st.rsum = static_cast<double>(st.isum);
                    st.real = true;
                }
                st.rsum += num.real();
            }
            double x = num.real();
            double delta = x - st.mean;
            st.mean += delta / static_cast<double>(st.count);
            st.m2 += delta * (x - st.mean);
            if (f == fn_MEDIAN) {
                if (!st.values && !(st.values = make_with<Vec<double> >(a_))) return ex_.memory();
                if (!st.values->push(x)) return ex_.memory();
            }
            return true;
        }
    }
}

namespace {
int compare_doubles(uint32_t a, uint32_t b, void* context) {
    const double* v = static_cast<const double*>(context);
    return v[a] < v[b] ? -1 : v[a] > v[b] ? 1 : 0;
}
}  // namespace

bool CoreRun::final_agg(AggState& st, Expr* agg, Value& out) {
    Function f = static_cast<Function>(agg->function);
    double n = static_cast<double>(st.count);
    switch (f) {
        case fn_COUNT: out = Value::integer(st.count); return true;
        case fn_MIN:
        case fn_MAX: out = st.has_best ? st.best : Value::null(); return true;
        case fn_GROUP_CONCAT:
        case fn_STRING_AGG: out = st.concat ? Value::text(st.concat->c_str(), st.concat->size()) : Value::null(); return true;
        case fn_SUM: out = !st.any ? Value::null() : st.real ? real_or_null(st.rsum) : Value::integer(st.isum); return true;
        case fn_TOTAL: out = Value::real(st.real ? st.rsum : static_cast<double>(st.isum)); return true;
        case fn_AVG: out = st.any ? real_or_null((st.real ? st.rsum : static_cast<double>(st.isum)) / n) : Value::null(); return true;
        case fn_STDDEV:
        case fn_STDDEV_SAMP: out = st.count > 1 ? real_or_null(sqrt(st.m2 / (n - 1))) : Value::null(); return true;
        case fn_STDDEV_POP: out = st.count > 0 ? real_or_null(sqrt(st.m2 / n)) : Value::null(); return true;
        case fn_VARIANCE:
        case fn_VAR_SAMP: out = st.count > 1 ? real_or_null(st.m2 / (n - 1)) : Value::null(); return true;
        case fn_VAR_POP: out = st.count > 0 ? real_or_null(st.m2 / n) : Value::null(); return true;
        case fn_MEDIAN: {
            if (!st.values || st.values->empty()) {
                out = Value::null();
                return true;
            }
            size_t count = st.values->size();
            Vec<uint32_t> order(a_);
            if (!order.resize(count)) return ex_.memory();
            for (size_t i = 0; i < count; ++i) order[i] = static_cast<uint32_t>(i);
            if (!merge_sort(order.data(), count, compare_doubles, st.values->data(), a_)) return ex_.memory();
            double m = count % 2 ? (*st.values)[order[count / 2]]
                                 : ((*st.values)[order[count / 2 - 1]] + (*st.values)[order[count / 2]]) / 2;
            out = Value::real(m);
            return true;
        }
        default: out = Value::null(); return true;
    }
}

int CoreRun::finish() {
    if (!aggregate_) return k_go;
    if (group_list_.empty() && group_by_.empty()) {
        // An aggregate without GROUP BY gives one row, even over no rows.
        Group* g = static_cast<Group*>(a_.allocate(sizeof(Group)));
        if (!g) return ex_.memory() ? k_error : k_error;
        g->reps = static_cast<uint8_t**>(a_.allocate((sources_.size() + 1) * sizeof(uint8_t*)));
        g->aggs = static_cast<AggState*>(a_.allocate((aggregates_.size() + 1) * sizeof(AggState)));
        if (!g->reps || !g->aggs || !group_list_.push(g)) {
            if (g->reps) a_.release(g->reps);
            if (g->aggs) a_.release(g->aggs);
            a_.release(g);
            return ex_.memory() ? k_error : k_error;
        }
        memset(static_cast<void*>(g->reps), 0, (sources_.size() + 1) * sizeof(uint8_t*));
        memset(static_cast<void*>(g->aggs), 0, (aggregates_.size() + 1) * sizeof(AggState));
    }
    for (size_t gi = 0; gi < group_list_.size(); ++gi) {
        Group* g = group_list_[gi];
        Pool::Mark mark = ex_.scratch().mark();
        for (size_t s = 0; s < sources_.size(); ++s) {
            Source& src = sources_[s];
            if (g->reps[s]) {
                decode_record(g->reps[s], src.row, src.width);
                src.null_row = false;
            } else {
                src.null_row = true;
            }
        }
        for (size_t i = 0; i < aggregates_.size(); ++i) {
            if (!final_agg(g->aggs[i], aggregates_[i], agg_values_[i])) return k_error;
        }
        frame_.aggregates = agg_values_;
        int rc = project();
        frame_.aggregates = 0;
        ex_.scratch().release(mark);
        if (rc != k_go) return rc;
    }
    for (size_t s = 0; s < sources_.size(); ++s) sources_[s].null_row = false;
    return k_go;
}

// ── CompoundRun ──────────────────────────────────────────────────────────────

int CompoundRun_accept(CompoundRun* c, Value* outputs, size_t count, Value* keys, size_t key_count) {
    return c->accept(outputs, count, keys, key_count);
}

bool CompoundRun::limits() {
    Frame empty;
    memset(static_cast<void*>(&empty), 0, sizeof(empty));
    empty.outer = outer_;
    Value v;
    Number n;
    if (c_->limit) {
        if (!ex_.eval(c_->limit, &empty, v)) return false;
        if (v.is_null()) limit_ = -1;
        else if (value_number(v, ex_.decimal(), n)) limit_ = n.is_int ? n.i : static_cast<int64_t>(n.r);
        else {
            ex_.status().fail(k_error_misuse, VDB_F("LIMIT needs a number"));
            return false;
        }
    }
    if (c_->offset) {
        if (!ex_.eval(c_->offset, &empty, v)) return false;
        if (!v.is_null() && value_number(v, ex_.decimal(), n)) offset_ = n.is_int ? n.i : static_cast<int64_t>(n.r);
        if (offset_ < 0) offset_ = 0;
    }
    return true;
}

int CompoundRun::start(CoreRun* core) {
    width_ = core->width();
    keys_ = core->key_count();
    for (size_t i = 0; i < width_; ++i) {
        if (!names_.push(core->names()[i])) return ex_.memory() ? k_error : k_error;
    }
    if (!limits()) return k_error;
    if (c_->order) {
        buffer_ = new_table(a_, "order");
        if (!buffer_) return ex_.memory() ? k_error : k_error;
    }
    return out_->begin(names_.data(), width_);
}

int CompoundRun::begin_pushed(Table* schema) {
    void* m = a_.allocate(sizeof(CoreRun));
    if (!m) return ex_.memory() ? k_error : k_error;
    core_ = new (m, PlaceTag()) CoreRun(ex_, c_->cores, c_->order, outer_, owner_, scope_);
    core_->target(this);
    if (!core_->prepare(schema)) return k_error;
    return start(core_);
}

int CompoundRun::emit(Value* values, size_t count) {
    if (stopped_) return k_stop;
    if (skipped_ < offset_) {
        ++skipped_;
        return k_go;
    }
    if (limit_ >= 0 && emitted_ >= limit_) {
        stopped_ = true;
        return k_stop;
    }
    ++emitted_;
    int rc = out_->row(values, count);
    if (rc == k_stop) stopped_ = true;
    if (rc == k_go && limit_ >= 0 && emitted_ >= limit_) {
        stopped_ = true;
        return k_stop;
    }
    return rc;
}

int CompoundRun::accept(Value* outputs, size_t count, Value* keys, size_t key_count) {
    if (multi_) {
        if (!part_->append_values(outputs, count)) return ex_.memory() ? k_error : k_error;
        return k_go;
    }
    if (buffer_) {
        Vec<Value> both(a_);
        if (!both.resize(count + key_count)) return ex_.memory() ? k_error : k_error;
        for (size_t i = 0; i < count; ++i) both[i] = outputs[i];
        for (size_t i = 0; i < key_count; ++i) both[count + i] = keys[i];
        if (!buffer_->append_values(both.data(), both.size())) return ex_.memory() ? k_error : k_error;
        if (limit_ >= 0 && buffer_->row_count() >= trim_at_ && !trim_buffer()) return k_error;
        return k_go;
    }
    return emit(outputs, count);
}

namespace {
struct SortContext {
    Value* keys;
    size_t key_count;
    const bool* descending;
    const int8_t* nulls;
    char decimal;
};

int compare_rows(uint32_t a, uint32_t b, void* context) {
    SortContext* sc = static_cast<SortContext*>(context);
    for (size_t k = 0; k < sc->key_count; ++k) {
        const Value& x = sc->keys[a * sc->key_count + k];
        const Value& y = sc->keys[b * sc->key_count + k];
        if (sc->nulls[k] && (x.is_null() || y.is_null())) {
            if (x.is_null() && y.is_null()) continue;
            int first = sc->nulls[k] == 1 ? -1 : 1;
            return x.is_null() ? first : -first;
        }
        int c = compare_order(x, y, sc->decimal);
        if (c) return sc->descending[k] ? -c : c;
    }
    return 0;
}
}  // namespace

// ORDER BY with LIMIT keeps the first limit + offset rows only: sorted and trimmed whenever the buffer doubles
// (spec L5). The survivors keep their arrival order, so the final stable sort gives the same rows as a full sort.
bool CompoundRun::trim_buffer() {
    size_t keep = static_cast<size_t>(limit_ + offset_);
    size_t n = buffer_->row_count();
    size_t total = width_ + keys_;
    Vec<Value> keys(a_);
    Vec<uint32_t> order(a_);
    Vec<Value> row(a_);
    Vec<uint8_t> drop(a_);
    if (!keys.resize(n * keys_ + 1) || !order.resize(n + 1) || !row.resize(total + 1) || !drop.resize(n + 1)) return ex_.memory();
    const size_t k_max_keys = 64;
    bool descending[k_max_keys];
    int8_t nulls[k_max_keys];
    size_t k = 0;
    for (OrderItem* o = c_->order; o && k < k_max_keys; o = o->next, ++k) {
        descending[k] = o->descending;
        nulls[k] = o->nulls;
    }
    size_t key_count = keys_ < k_max_keys ? keys_ : k_max_keys;
    for (size_t r = 0; r < n; ++r) {
        order[r] = static_cast<uint32_t>(r);
        decode_record(buffer_->record(r), row.data(), total);
        for (size_t i = 0; i < key_count; ++i) keys[r * key_count + i] = row[width_ + i];
        drop[r] = 1;
    }
    SortContext sc = {keys.data(), key_count, descending, nulls, ex_.decimal()};
    if (!merge_sort(order.data(), n, compare_rows, &sc, a_)) return ex_.memory();
    for (size_t i = 0; i < keep && i < n; ++i) drop[order[i]] = 0;
    buffer_->remove_rows(drop.data());
    trim_at_ = 2 * keep > k_first_trim ? 2 * keep : k_first_trim;
    return true;
}

int CompoundRun::sort_and_emit(Table* rows, size_t key_count, bool keys_stored) {
    size_t n = rows->row_count();
    Vec<Value> keys(a_);
    Vec<uint32_t> order(a_);
    Vec<Value> row(a_);
    size_t total = width_ + (keys_stored ? key_count : 0);
    if (!keys.resize(n * key_count + 1) || !order.resize(n + 1) || !row.resize(total + 1)) return ex_.memory() ? k_error : k_error;
    bool descending[64];
    int8_t nulls[64];
    size_t k = 0;
    for (OrderItem* o = c_->order; o && k < 64; o = o->next, ++k) {
        descending[k] = o->descending;
        nulls[k] = o->nulls;
    }
    if (key_count > 64) key_count = 64;
    Frame frame;
    memset(static_cast<void*>(&frame), 0, sizeof(frame));
    frame.outer = outer_;
    for (size_t r = 0; r < n; ++r) {
        order[r] = static_cast<uint32_t>(r);
        decode_record(rows->record(r), row.data(), total);
        if (keys_stored) {
            for (size_t i = 0; i < key_count; ++i) keys[r * key_count + i] = row[width_ + i];
        } else {
            frame.outputs = row.data();
            for (size_t i = 0; i < key_count; ++i) {
                Value v;
                if (!ex_.eval(order_exprs_[i], &frame, v)) return k_error;
                if (v.type != k_null && v.type != k_text) {
                    if (!textify(v, ex_.decimal(), ex_.statement_pool())) return ex_.memory() ? k_error : k_error;
                } else if (v.type == k_text && !value_copy(ex_.statement_pool(), v)) {
                    return ex_.memory() ? k_error : k_error;
                }
                keys[r * key_count + i] = v;
            }
        }
    }
    SortContext sc = {keys.data(), key_count, descending, nulls, ex_.decimal()};
    if (!merge_sort(order.data(), n, compare_rows, &sc, a_)) return ex_.memory() ? k_error : k_error;
    for (size_t i = 0; i < n; ++i) {
        decode_record(rows->record(order[i]), row.data(), width_);
        int rc = emit(row.data(), width_);
        if (rc != k_go) return rc;
    }
    return k_go;
}

int CompoundRun::flush() {
    int rc = k_go;
    if (buffer_ && !stopped_) rc = sort_and_emit(buffer_, keys_, true);
    if (rc == k_error) return rc;
    return out_->end();
}

bool CompoundRun::combine(uint8_t op) {
    Table* merged = new_table(a_, "set");
    if (!merged) return ex_.memory();
    for (size_t i = 0; i < width_; ++i) {
        if (merged->add_column(names_[i], strlen(names_[i])) < 0) {
            destroy(a_, merged);
            return ex_.memory();
        }
    }
    HashIndex seen(a_), other(a_);
    Text key(a_);
    Vec<Value> row(a_);
    if (!row.resize(width_ + 1)) {
        destroy(a_, merged);
        return ex_.memory();
    }
    bool ok = true;
    if (op == set_intersect || op == set_except) {
        for (size_t r = 0; ok && r < part_->row_count(); ++r) {
            decode_record(part_->record(r), row.data(), width_);
            key.clear();
            for (size_t i = 0; i < width_; ++i) key_append(key, row[i], ex_.decimal());
            bool added;
            ok = key.ok() && other.find_or_add(key.c_str(), key.size(), added) >= 0;
        }
    }
    Table* inputs[2] = {acc_, op == set_union ? part_ : 0};
    for (int t = 0; ok && t < 2; ++t) {
        Table* in = inputs[t];
        for (size_t r = 0; ok && in && r < in->row_count(); ++r) {
            decode_record(in->record(r), row.data(), width_);
            key.clear();
            for (size_t i = 0; i < width_; ++i) key_append(key, row[i], ex_.decimal());
            if (!key.ok()) {
                ok = false;
                break;
            }
            if (op == set_intersect && other.find(key.c_str(), key.size()) < 0) continue;
            if (op == set_except && other.find(key.c_str(), key.size()) >= 0) continue;
            bool added;
            if (seen.find_or_add(key.c_str(), key.size(), added) < 0) {
                ok = false;
                break;
            }
            if (added) ok = merged->append_values(row.data(), width_);
        }
    }
    if (!ok) {
        destroy(a_, merged);
        return ex_.memory();
    }
    destroy(a_, acc_);
    acc_ = merged;
    return true;
}

bool CompoundRun::bind_output_order() {
    for (OrderItem* o = c_->order; o; o = o->next) {
        Expr* e = o->expr;
        if (e->kind == e_literal && e->literal.type == k_integer) {
            if (e->literal.i < 1 || static_cast<size_t>(e->literal.i) > width_) {
                ex_.status().fail(k_error_misuse, VDB_F("ORDER BY position %d is not a column of the result"), static_cast<int>(e->literal.i));
                return false;
            }
            e->kind = e_output;
            e->slot = static_cast<int32_t>(e->literal.i - 1);
        } else if (e->kind == e_column) {
            int found = -1;
            for (size_t i = 0; i < width_ && found < 0; ++i) {
                if (names_equal(names_[i], e->name)) found = static_cast<int>(i);
            }
            if (found < 0) {
                ex_.status().fail(k_error_name, VDB_F("ORDER BY after UNION/INTERSECT/EXCEPT must name a result column: %s"), e->name);
                return false;
            }
            e->kind = e_output;
            e->slot = found;
        } else {
            ex_.status().fail(k_error_misuse, VDB_F("ORDER BY after UNION/INTERSECT/EXCEPT takes column names or positions"));
            return false;
        }
        if (!order_exprs_.push(e)) return ex_.memory();
    }
    return true;
}

int CompoundRun::run() {
    if (!multi_) {
        void* m = a_.allocate(sizeof(CoreRun));
        if (!m) return ex_.memory() ? k_error : k_error;
        core_ = new (m, PlaceTag()) CoreRun(ex_, c_->cores, c_->order, outer_, owner_, scope_);
        core_->target(this);
        if (!core_->prepare(0)) return k_error;
        int rc = start(core_);
        if (rc == k_error) return rc;
        rc = core_->drive();
        if (rc == k_error) return rc;
        return flush();
    }
    // Set operations, left to right (spec 3.10).
    bool first = true;
    for (Core* core = c_->cores; core; core = core->next) {
        void* m = a_.allocate(sizeof(CoreRun));
        if (!m) return ex_.memory() ? k_error : k_error;
        CoreRun* run = new (m, PlaceTag()) CoreRun(ex_, core, 0, outer_, owner_, scope_);
        run->target(this);
        int rc = run->prepare(0) ? k_go : k_error;
        if (rc == k_go && first) {
            width_ = run->width();
            for (size_t i = 0; i < width_; ++i) {
                if (!names_.push(run->names()[i])) rc = ex_.memory() ? k_error : k_error;
            }
        } else if (rc == k_go && run->width() != width_) {
            ex_.status().fail(k_error_misuse, VDB_F("each SELECT of UNION/INTERSECT/EXCEPT needs %d columns"), static_cast<int>(width_));
            rc = k_error;
        }
        if (rc == k_go) {
            part_ = new_table(a_, "part");
            if (!part_) rc = ex_.memory() ? k_error : k_error;
        }
        if (rc == k_go) rc = run->drive();
        destroy(a_, run);
        if (rc == k_error) return rc;
        if (first) {
            acc_ = part_;
            part_ = 0;
            first = false;
            continue;
        }
        if (core->set_op == set_union_all) {
            const size_t width = width_;
            Vec<Value> row(a_);
            if (!row.resize(width + 1)) return ex_.memory() ? k_error : k_error;
            for (size_t r = 0; r < part_->row_count(); ++r) {
                decode_record(part_->record(r), row.data(), width);
                if (!acc_->append_values(row.data(), width)) return ex_.memory() ? k_error : k_error;
            }
        } else if (!combine(core->set_op)) {
            return k_error;
        }
        destroy(a_, part_);
        part_ = 0;
    }
    if (!limits()) return k_error;
    if (c_->order && !bind_output_order()) return k_error;
    int rc = out_->begin(names_.data(), width_);
    if (rc == k_error) return rc;
    if (c_->order) {
        rc = sort_and_emit(acc_, order_exprs_.size(), false);
    } else {
        Vec<Value> row(a_);
        if (!row.resize(width_ + 1)) return ex_.memory() ? k_error : k_error;
        for (size_t r = 0; r < acc_->row_count(); ++r) {
            decode_record(acc_->record(r), row.data(), width_);
            rc = emit(row.data(), width_);
            if (rc != k_go) break;
        }
    }
    if (rc == k_error) return rc;
    return out_->end();
}

// ── queries ──────────────────────────────────────────────────────────────────

namespace {

size_t count_previous(Expr* e);

size_t count_previous_query(Query* q) {
    size_t n = 0;
    for (Stage* s = q->stages; s; s = s->next) {
        for (Core* c = s->body->cores; c; c = c->next) {
            for (FromItem* f = c->from; f; f = f->next) {
                if (f->name && names_equal(f->name, k_previous)) ++n;
                if (f->query) n += count_previous_query(f->query);
                n += count_previous(f->on);
            }
            for (SelectItem* i = c->items; i; i = i->next) n += count_previous(i->expr);
            n += count_previous(c->where) + count_previous(c->group_by) + count_previous(c->having);
        }
    }
    return n;
}

size_t count_previous(Expr* e) {
    size_t n = 0;
    for (; e; e = e->next) {
        if (e->kind == e_in_table && names_equal(e->name, k_previous)) ++n;
        if (e->query) n += count_previous_query(e->query);
        n += count_previous(e->a) + count_previous(e->b) + count_previous(e->c) + count_previous(e->args);
    }
    return n;
}

// A stage fed row by row: its only source is _, used once (spec 3.11).
bool pushable(Stage* stage) {
    Core* core = stage->body->cores;
    if (core->next) return false;
    bool only_previous = core->from_omitted ||
                         (core->from && !core->from->next && !core->from->query && names_equal(core->from->name, k_previous));
    if (!only_previous) return false;
    size_t uses = core->from_omitted ? 1 : 0;
    for (FromItem* f = core->from; f; f = f->next) uses += names_equal(f->name, k_previous) ? 1 : 0;
    for (SelectItem* i = core->items; i; i = i->next) uses += count_previous(i->expr);
    uses += count_previous(core->where) + count_previous(core->group_by) + count_previous(core->having);
    for (OrderItem* o = stage->body->order; o; o = o->next) uses += count_previous(o->expr);
    return uses == 1;
}

}  // namespace

Table* Executor::run_to_table(Query* q, Scope* scope, Frame* outer, Expr* owner, const char* name) {
    Table* t = new_table(*allocator_, name ? name : "result");
    if (!t) {
        memory();
        return 0;
    }
    TableSink sink(*this, t);
    if (run_query(q, scope, outer, owner, &sink) == k_error) {
        destroy(*allocator_, t);
        return 0;
    }
    return t;
}

int Executor::run_query(Query* q, Scope* scope, Frame* outer, Expr* owner, Sink* sink) {
    if (!enter()) return k_error;
    Scope* inner = scope;
    Vec<Table*> ctes(*allocator_);
    int rc = k_go;
    for (Cte* c = q->with; c && rc != k_error; c = c->next) {
        Table* t = run_to_table(c->query, inner, outer, owner, c->name);
        Scope* s = make<Scope>(pool_);
        if (!t || !s || !ctes.push(t)) {
            if (t && !s) memory();
            if (t) destroy(*allocator_, t);
            rc = k_error;
            break;
        }
        s->name = c->name;
        s->table = t;
        s->outer = inner;
        inner = s;
    }
    Table* previous = 0;
    for (Stage* stage = q->stages; stage && rc != k_error;) {
        Stage* last = stage;
        while (last->next && pushable(last->next)) last = last->next;
        Table* temp = last->next ? new_table(*allocator_, k_previous) : 0;
        TableSink keep(*this, temp);
        Sink* target = sink;
        if (last->next) {
            if (!temp) {
                memory();
                rc = k_error;
                break;
            }
            target = &keep;
        }
        // Stages after `stage` up to `last` are fed row by row: chain their sinks from the end.
        Vec<StageSink*> chain(*allocator_);
        for (Stage* s = last; s != stage && rc != k_error;) {
            void* m = allocator_->allocate(sizeof(StageSink));
            if (!m || !chain.push(static_cast<StageSink*>(m))) {
                if (m) allocator_->release(m);
                memory();
                rc = k_error;
                break;
            }
            target = new (m, PlaceTag()) StageSink(*this, s->body, target, inner, outer, owner);
            Stage* before = stage;
            while (before->next != s) before = before->next;
            s = before;
        }
        Scope here;
        here.name = k_previous;
        here.table = previous;
        here.outer = inner;
        if (rc != k_error) {
            CompoundRun run(*this, stage->body, outer, owner, previous ? &here : inner, target);
            rc = run.run();
        }
        for (size_t i = 0; i < chain.size(); ++i) destroy(*allocator_, chain[i]);
        if (previous) destroy(*allocator_, previous);
        previous = temp;
        stage = last->next;
        if (rc == k_error) break;
    }
    if (previous) destroy(*allocator_, previous);
    for (size_t i = 0; i < ctes.size(); ++i) destroy(*allocator_, ctes[i]);
    leave();
    return rc;
}

// ── statements ───────────────────────────────────────────────────────────────

namespace {

class InsertSink final : public Sink {
public:
    InsertSink(Executor& ex, Table* table, NameList* columns)
        : ex_(ex), table_(table), columns_(columns), map_(ex.allocator()), row_(ex.allocator()), count_(0) {}
    int begin(const char* const* names, size_t count) override {
        if (!map_.resize(count + 1)) return ex_.memory() ? k_error : k_error;
        NameList* c = columns_;
        for (size_t i = 0; i < count; ++i) {
            const char* name = columns_ ? (c ? c->name : 0) : names[i];
            if (!name) {
                ex_.status().fail(k_error_misuse, VDB_F("the query has more columns than the column list"));
                return k_error;
            }
            int index = table_->ensure_column(name, strlen(name));
            if (index < 0) return ex_.memory() ? k_error : k_error;
            map_[i] = index;
            if (c) c = c->next;
        }
        if (c) {
            ex_.status().fail(k_error_misuse, VDB_F("the query has fewer columns than the column list"));
            return k_error;
        }
        return k_go;
    }
    int row(Value* values, size_t count) override {
        if (!row_.resize(table_->column_count())) return ex_.memory() ? k_error : k_error;
        for (size_t i = 0; i < row_.size(); ++i) row_[i] = Value::null();
        for (size_t i = 0; i < count; ++i) row_[static_cast<size_t>(map_[i])] = values[i];
        if (!table_->append_values(row_.data(), row_.size())) return ex_.memory() ? k_error : k_error;
        ++count_;
        return k_go;
    }
    size_t count() const { return count_; }

private:
    Executor& ex_;
    Table* table_;
    NameList* columns_;
    Vec<int> map_;
    Vec<Value> row_;
    size_t count_;
};

}  // namespace

bool Executor::insert(Statement* s) {
    Table* t = db_.table(s->table);
    if (!t) {
        t = db_.create_table(s->table, status_);  // records are free-form: INSERT creates the table (spec A21)
        if (!t) return false;
    }
    if (s->query) {
        InsertSink sink(*this, t, s->columns);
        if (run_query(s->query, 0, 0, 0, &sink) == k_error) return false;
        changes_ += sink.count();
        return true;
    }
    Vec<int> map(*allocator_);
    if (s->columns) {
        for (NameList* c = s->columns; c; c = c->next) {
            int index = t->ensure_column(c->name, strlen(c->name));
            if (index < 0 || !map.push(index)) return memory();
        }
    } else {
        for (size_t i = 0; i < t->column_count(); ++i) {
            if (!map.push(static_cast<int>(i))) return memory();
        }
    }
    Frame empty;
    memset(static_cast<void*>(&empty), 0, sizeof(empty));
    Vec<Value> row(*allocator_);
    for (ValuesRow* r = s->rows; r; r = r->next) {
        size_t count = 0;
        for (Expr* v = r->values; v; v = v->next) ++count;
        if (count != map.size()) {
            if (s->columns) {
                status_.fail(k_error_misuse, VDB_F("%d values for %d columns"), static_cast<int>(count), static_cast<int>(map.size()));
            } else {
                status_.fail(k_error_misuse, VDB_F("table %s has %d columns but %d values were given (name the columns: INSERT INTO t (a, b) VALUES ...)"),
                             t->name(), static_cast<int>(map.size()), static_cast<int>(count));
            }
            return false;
        }
        if (!row.resize(t->column_count() + 1)) return memory();
        for (size_t i = 0; i < row.size(); ++i) row[i] = Value::null();
        Pool::Mark mark = scratch_.mark();
        size_t i = 0;
        for (Expr* v = r->values; v; v = v->next, ++i) {
            Value value;
            if (v->kind == e_column) {
                status_.fail(k_error_misuse, VDB_F("VALUES cannot read a column (%s); quote text: '%s'"), v->name, v->name);
                return false;
            }
            if (!eval(v, &empty, value)) return false;
            if (!textify(value, decimal_, scratch_)) return memory();
            row[static_cast<size_t>(map[i])] = value;
        }
        bool ok = t->append_values(row.data(), t->column_count());
        scratch_.release(mark);
        if (!ok) return memory();
        ++changes_;
    }
    return true;
}

bool Executor::update(Statement* s) {
    Table* t = db_.table(s->table);
    if (!t) {
        status_.fail(k_error_name, VDB_F("no such table: %s"), s->table);
        return false;
    }
    if (t->stream_pending() && !t->load_stream(status_)) return false;
    for (Assignment* a = s->assignments; a; a = a->next) {
        if (t->ensure_column(a->column, strlen(a->column)) < 0) return memory();
    }
    Core core;
    memset(static_cast<void*>(&core), 0, sizeof(core));
    FromItem from;
    memset(static_cast<void*>(&from), 0, sizeof(from));
    from.name = t->name();
    from.alias = t->name();
    core.from = &from;
    core.where = s->where;
    SelectItem none;
    memset(static_cast<void*>(&none), 0, sizeof(none));
    none.star = true;
    core.items = &none;
    CoreRun run(*this, &core, 0, 0, 0, 0);
    if (!run.prepare(0)) return false;
    Frame* frame = run.frame();
    for (Assignment* a = s->assignments; a; a = a->next) {
        if (!run.bind_expr(a->value)) return false;
    }
    Source& src = frame->sources[0];
    size_t width = t->column_count();
    struct Change {
        size_t row;
        uint8_t* record;
    };
    Vec<Change> pending(*allocator_);
    Vec<Value> row(*allocator_);
    if (!row.resize(width + 1)) return memory();
    bool ok = true;
    for (size_t r = 0; ok && r < t->row_count(); ++r) {
        decode_record(t->record(r), src.row, src.width);
        src.null_row = false;
        Pool::Mark mark = scratch_.mark();
        int pass = k_true;
        if (s->where) pass = truth(s->where, frame);
        if (pass == -2) {
            ok = false;
        } else if (pass == k_true) {
            decode_record(t->record(r), row.data(), width);
            for (Assignment* a = s->assignments; ok && a; a = a->next) {
                Value v;
                ok = eval(a->value, frame, v) && textify(v, decimal_, scratch_);
                if (ok) row[static_cast<size_t>(t->find_column(a->column))] = v;
                else if (status_.ok()) memory();
            }
            if (ok) {
                Change c = {r, encode_record(*allocator_, row.data(), width)};
                if (!c.record || !pending.push(c)) {
                    if (c.record) allocator_->release(c.record);
                    ok = memory();
                }
            }
        }
        scratch_.release(mark);
    }
    if (!ok) {
        for (size_t i = 0; i < pending.size(); ++i) allocator_->release(pending[i].record);
        return false;
    }
    for (size_t i = 0; i < pending.size(); ++i) t->replace(pending[i].row, pending[i].record);
    changes_ += pending.size();
    return true;
}

bool Executor::remove(Statement* s) {
    Table* t = db_.table(s->table);
    if (!t) {
        status_.fail(k_error_name, VDB_F("no such table: %s"), s->table);
        return false;
    }
    if (t->stream_pending() && !t->load_stream(status_)) return false;
    if (!s->where) {
        // DELETE without WHERE allocates nothing: it frees a full database.
        changes_ += t->row_count();
        t->clear_rows();
        return true;
    }
    Core core;
    memset(static_cast<void*>(&core), 0, sizeof(core));
    FromItem from;
    memset(static_cast<void*>(&from), 0, sizeof(from));
    from.name = t->name();
    from.alias = t->name();
    core.from = &from;
    core.where = s->where;
    SelectItem none;
    memset(static_cast<void*>(&none), 0, sizeof(none));
    none.star = true;
    core.items = &none;
    CoreRun run(*this, &core, 0, 0, 0, 0);
    if (!run.prepare(0)) return false;
    Frame* frame = run.frame();
    Source& src = frame->sources[0];
    Vec<uint8_t> marks(*allocator_);
    if (!marks.resize(t->row_count() + 1)) return memory();
    size_t removed = 0;
    for (size_t r = 0; r < t->row_count(); ++r) {
        decode_record(t->record(r), src.row, src.width);
        src.null_row = false;
        Pool::Mark mark = scratch_.mark();
        int pass = s->where ? truth(s->where, frame) : k_true;
        scratch_.release(mark);
        if (pass == -2) return false;
        marks[r] = pass == k_true;
        removed += marks[r];
    }
    t->remove_rows(marks.data());
    changes_ += removed;
    return true;
}

bool Executor::create(Statement* s) {
    if (db_.table(s->table)) {
        if (s->if_exists) return true;
        status_.fail(k_error_misuse, VDB_F("table %s already exists"), s->table);
        return false;
    }
    Table* t = db_.create_table(s->table, status_);
    if (!t) return false;
    if (s->query) {
        TableSink sink(*this, t);
        if (run_query(s->query, 0, 0, 0, &sink) == k_error) {
            db_.drop_table(s->table);
            return false;
        }
        changes_ += t->row_count();
        return true;
    }
    for (NameList* c = s->columns; c; c = c->next) {
        if (t->find_column(c->name) >= 0) {
            status_.fail(k_error_misuse, VDB_F("column %s appears twice"), c->name);
            db_.drop_table(s->table);
            return false;
        }
        if (t->add_column(c->name, strlen(c->name)) < 0) {
            db_.drop_table(s->table);
            return memory();
        }
    }
    return true;
}

namespace {
class DiscardSink final : public Sink {};
}  // namespace

bool Executor::run(Statement* s, Sink* sink) {
    count_names(s);
    if (!status_.ok()) return false;
    DiscardSink discard;
    switch (s->kind) {
        case st_query: return run_query(s->query, 0, 0, 0, sink ? sink : &discard) != k_error;
        case st_insert: return insert(s);
        case st_update: return update(s);
        case st_delete: return remove(s);
        case st_create: return create(s);
        default:
            if (!db_.drop_table(s->table) && !s->if_exists) {
                status_.fail(k_error_name, VDB_F("no such table: %s"), s->table);
                return false;
            }
            return true;
    }
}

}  // namespace vdb
