// The word chain (spec D1, A1, 4.2): db.select("name").from("people").where.field("age").gt(30).run().
// Every chain writes SQL with names quoted and values escaped; the one parser compiles it. sql() shows it.
#ifndef VDB_BUILDER_HPP
#define VDB_BUILDER_HPP

#include "database.hpp"

namespace vdb {

enum Direction { ASC, DESC };

struct Column {
    const char* qualifier;
    const char* name;
};
inline Column column(const char* name) {
    Column c = {0, name};
    return c;
}
inline Column column(const char* table, const char* name) {
    Column c = {table, name};
    return c;
}

// Trusted SQL text, written as given. Never pass user input here: pass it as a value.
struct Raw {
    const char* sql;
};
inline Raw raw(const char* sql) {
    Raw r = {sql};
    return r;
}

// Values as SQL literals: text quoted with '' doubled, numbers as numbers, 0 pointers as NULL.
void write_literal(Text& out, const char* text);
void write_literal(Text& out, long long value);
void write_literal(Text& out, unsigned long long value);
void write_literal(Text& out, double value);
void write_literal(Text& out, bool value);
void write_literal(Text& out, const Column& value);
void write_literal(Text& out, const Raw& value);
inline void write_literal(Text& out, int value) { write_literal(out, static_cast<long long>(value)); }
inline void write_literal(Text& out, long value) { write_literal(out, static_cast<long long>(value)); }
inline void write_literal(Text& out, unsigned value) { write_literal(out, static_cast<unsigned long long>(value)); }
inline void write_literal(Text& out, unsigned long value) { write_literal(out, static_cast<unsigned long long>(value)); }
inline void write_literal(Text& out, float value) { write_literal(out, static_cast<double>(value)); }
void write_identifier(Text& out, const char* name);  // "name" with "" doubled

// State shared by every chain: the database and the first misuse.
class ChainBase {
public:
    explicit ChainBase(Database& db) : db_(&db) {}
    void fail(const char* flash_message);
    const Status& error() const { return error_; }

protected:
    Database* db_;
    Status error_;
};

template <class Owner>
class Condition {
public:
    Condition(Owner* owner, Condition& where_clause)
        : where(where_clause), owner_(owner), text_(owner->allocator()), state_(k_term), depth_(0), operand_(0) {}
    Condition(Owner* owner, Condition& where_clause, Condition&& other)
        : where(where_clause),
          owner_(owner),
          text_(static_cast<Text&&>(other.text_)),
          state_(other.state_),
          depth_(other.depth_),
          operand_(other.operand_) {}
    Condition(Owner* owner, Condition& where_clause, const Condition& other)
        : where(where_clause), owner_(owner), text_(owner->allocator()), state_(other.state_), depth_(other.depth_), operand_(other.operand_) {
        text_.append(other.text_.c_str(), other.text_.size());
    }
    Condition(const Condition&) = delete;
    Condition& operator=(const Condition&) = delete;

    // The owner's WHERE, so `.on.field(...).eq(...).where.field(...)` reads on.
    Condition& where;

    // Operands.
    Condition& field(const char* name) {
        operand();
        write_identifier(text_, name);
        return *this;
    }
    Condition& field(const char* table, const char* name) {
        operand();
        write_identifier(text_, table);
        text_.append_char('.');
        write_identifier(text_, name);
        return *this;
    }
    Condition& expr(const char* sql) {
        operand();
        text_.append(sql);
        return *this;
    }

    // Comparisons.
    template <class V>
    Condition& eq(const V& v) {
        return compare(" = ", v);
    }
    template <class V>
    Condition& ne(const V& v) {
        return compare(" <> ", v);
    }
    template <class V>
    Condition& lt(const V& v) {
        return compare(" < ", v);
    }
    template <class V>
    Condition& le(const V& v) {
        return compare(" <= ", v);
    }
    template <class V>
    Condition& gt(const V& v) {
        return compare(" > ", v);
    }
    template <class V>
    Condition& ge(const V& v) {
        return compare(" >= ", v);
    }
    template <class A, class B>
    Condition& between(const A& low, const B& high) {
        if (!need_operand()) return *this;
        text_.append(" BETWEEN ");
        write_literal(text_, low);
        text_.append(" AND ");
        write_literal(text_, high);
        state_ = k_after;
        return *this;
    }
    template <class... V>
    Condition& in(const V&... values) {
        return in_list(" IN (", values...);
    }
    template <class... V>
    Condition& not_in(const V&... values) {
        return in_list(" NOT IN (", values...);
    }
    Condition& in_query(const char* sql) { return suffix(" IN (", sql, ")"); }
    Condition& in_table(const char* table) {
        if (!need_operand()) return *this;
        text_.append(" IN ");
        write_identifier(text_, table);
        state_ = k_after;
        return *this;
    }
    Condition& is_null() { return suffix(" IS NULL", 0, 0); }
    Condition& is_not_null() { return suffix(" IS NOT NULL", 0, 0); }

    // Text matching.
    Condition& like(const char* pattern) { return compare(" LIKE ", pattern); }
    Condition& not_like(const char* pattern) { return compare(" NOT LIKE ", pattern); }
    Condition& ilike(const char* pattern) { return compare(" ILIKE ", pattern); }
    Condition& glob(const char* pattern) { return compare(" GLOB ", pattern); }
    Condition& regexp(const char* pattern) { return compare(" REGEXP ", pattern); }
    Condition& not_regexp(const char* pattern) { return compare(" NOT REGEXP ", pattern); }
    Condition& similar_to(const char* pattern) { return compare(" SIMILAR TO ", pattern); }
    Condition& contains(const char* text) { return wrap("CONTAINS(", text); }
    Condition& starts_with(const char* text) { return wrap("STARTS_WITH(", text); }
    Condition& ends_with(const char* text) { return wrap("ENDS_WITH(", text); }

    // Connectors. AND binds before OR; open()/close() group.
    Condition& and_() { return connect(" AND "); }
    Condition& or_() { return connect(" OR "); }
    Condition& not_() {
        if (state_ == k_after) text_.append(" AND ");
        else if (state_ == k_operand) return misuse(VDB_F("not_() after a field needs an operator first"));
        text_.append("NOT ");
        state_ = k_term;
        return *this;
    }
    Condition& open() {
        if (state_ == k_after) text_.append(" AND ");
        else if (state_ == k_operand) return misuse(VDB_F("open() after a field needs an operator first"));
        text_.append_char('(');
        ++depth_;
        state_ = k_term;
        return *this;
    }
    Condition& close() {
        if (state_ != k_after || depth_ == 0) return misuse(VDB_F("close() needs an open() and a complete condition"));
        text_.append_char(')');
        --depth_;
        return *this;
    }

    // Back to the statement: every method of the owner that may follow a condition.
    Owner& from(const char* table) { return owner_->from(table); }
    Owner& join(const char* table, const char* alias = 0) { return owner_->join(table, alias); }
    Owner& left_join(const char* table, const char* alias = 0) { return owner_->left_join(table, alias); }
    Owner& right_join(const char* table, const char* alias = 0) { return owner_->right_join(table, alias); }
    Owner& full_join(const char* table, const char* alias = 0) { return owner_->full_join(table, alias); }
    Owner& cross_join(const char* table, const char* alias = 0) { return owner_->cross_join(table, alias); }
    Owner& group_by(const char* columns) { return owner_->group_by(columns); }
    Owner& order_by(const char* column, Direction direction = ASC) { return owner_->order_by(column, direction); }
    Owner& limit(long long n) { return owner_->limit(n); }
    Owner& offset(long long n) { return owner_->offset(n); }
    Owner& then() { return owner_->then(); }
    Owner& set(const char* name, const char* v) { return owner_->set(name, v); }
    template <class V>
    Owner& set(const char* name, const V& v) {
        return owner_->set(name, v);
    }
    Result run() { return owner_->run(); }
    Status run(RowSink& sink) { return owner_->run(sink); }
    const char* sql() { return owner_->sql(); }

    // For the owner.
    const Text& text() const { return text_; }
    bool empty() const { return text_.empty(); }
    bool complete() const { return state_ == k_after && depth_ == 0; }
    void clear() {
        text_.clear();
        state_ = k_term;
        depth_ = 0;
    }

private:
    enum State { k_term, k_operand, k_after };

    Condition& misuse(const char* flash_message) {
        owner_->fail(flash_message);
        return *this;
    }
    void operand() {
        if (state_ == k_after) text_.append(" AND ");  // two conditions in a row: AND
        else if (state_ == k_operand) owner_->fail(VDB_F("field() twice: compare the first one first (eq, gt, like, ...)"));
        operand_ = text_.size();
        state_ = k_operand;
    }
    bool need_operand() {
        if (state_ == k_operand) return true;
        owner_->fail(VDB_F("a comparison needs field() or expr() first"));
        return false;
    }
    template <class V>
    Condition& compare(const char* op, const V& v) {
        if (!need_operand()) return *this;
        text_.append(op);
        write_literal(text_, v);
        state_ = k_after;
        return *this;
    }
    Condition& suffix(const char* before, const char* middle, const char* after) {
        if (!need_operand()) return *this;
        text_.append(before);
        if (middle) text_.append(middle);
        if (after) text_.append(after);
        state_ = k_after;
        return *this;
    }
    Condition& wrap(const char* function, const char* value) {
        if (!need_operand()) return *this;
        Text call(owner_->allocator());
        call.append(function);
        call.append(text_.c_str() + operand_, text_.size() - operand_);
        call.append(", ");
        write_literal(call, value);
        call.append_char(')');
        text_.truncate(operand_);
        text_.append(call.c_str(), call.size());
        if (!call.ok()) owner_->fail(VDB_F("out of memory"));
        state_ = k_after;
        return *this;
    }
    Condition& connect(const char* word) {
        if (state_ != k_after) return misuse(VDB_F("and_()/or_() need a complete condition before them"));
        text_.append(word);
        state_ = k_term;
        return *this;
    }
    void write_list(bool) {}
    template <class V, class... Rest>
    void write_list(bool first, const V& v, const Rest&... rest) {
        if (!first) text_.append(", ");
        write_literal(text_, v);
        write_list(false, rest...);
    }
    template <class... V>
    Condition& in_list(const char* op, const V&... values) {
        if (!need_operand()) return *this;
        text_.append(op);
        write_list(true, values...);
        text_.append_char(')');
        state_ = k_after;
        return *this;
    }

    Owner* owner_;
    Text text_;
    State state_;
    int depth_;
    size_t operand_;
};

class Select : public ChainBase {
public:
    Select(Database& db, const char* columns);
    Select(Select&& other);
    Select(const Select& other);  // vdb::Select q = db...; copies the chain
    Select& operator=(const Select&) = delete;

    Condition<Select> where;
    Condition<Select> having;
    Condition<Select> on;

    Select& select(const char* columns);  // the columns of a later stage (after then())
    Select& distinct();
    Select& from(const char* table, const char* alias = 0);
    Select& join(const char* table, const char* alias = 0) { return add_join(" JOIN ", table, alias); }
    Select& left_join(const char* table, const char* alias = 0) { return add_join(" LEFT JOIN ", table, alias); }
    Select& right_join(const char* table, const char* alias = 0) { return add_join(" RIGHT JOIN ", table, alias); }
    Select& full_join(const char* table, const char* alias = 0) { return add_join(" FULL JOIN ", table, alias); }
    Select& cross_join(const char* table, const char* alias = 0) { return add_join(" CROSS JOIN ", table, alias); }
    Select& group_by(const char* columns);
    Select& order_by(const char* column, Direction direction = ASC);
    Select& limit(long long n);
    Select& offset(long long n);
    Select& then();  // ends this stage: the next reads its rows (|>)
    const char* sql();
    Result run();
    Status run(RowSink& sink);
    Allocator& allocator() const { return db_->allocator(); }

private:
    Select& add_join(const char* kind, const char* table, const char* alias);
    void flush_on();
    void compose_stage(Text& out);

    Text done_;
    Text columns_;
    Text from_;
    Text joins_;
    Text group_;
    Text order_;
    Text sql_;
    long long limit_;
    long long offset_;
    bool distinct_;
    bool has_columns_;
    bool pending_on_;
    int stages_;
};

class Update : public ChainBase {
public:
    Update(Database& db, const char* table);
    Update(Update&& other);
    Update(const Update& other);  // vdb::Update q = db...; copies the chain
    Update& operator=(const Update&) = delete;

    Condition<Update> where;

    template <class V>
    Update& set(const char* name, const V& value) {
        if (!sets_.empty()) sets_.append(", ");
        write_identifier(sets_, name);
        sets_.append(" = ");
        write_literal(sets_, value);
        return *this;
    }
    Update& set(const char* name, const char* value) { return set<const char*>(name, value); }
    const char* sql();
    Result run();
    Status run(RowSink&) { return run().status(); }
    Allocator& allocator() const { return db_->allocator(); }

private:
    Text table_;
    Text sets_;
    Text sql_;
};

class Delete : public ChainBase {
public:
    Delete(Database& db, const char* table);
    Delete(Delete&& other);
    Delete(const Delete& other);  // vdb::Delete q = db...; copies the chain
    Delete& operator=(const Delete&) = delete;

    Condition<Delete> where;

    const char* sql();
    Result run();
    Status run(RowSink&) { return run().status(); }
    Allocator& allocator() const { return db_->allocator(); }

private:
    Text table_;
    Text sql_;
};

class Insert : public ChainBase {
public:
    Insert(Database& db, const char* table);
    Insert(Insert&& other);
    Insert(const Insert& other);  // vdb::Insert q = db...; copies the chain
    Insert& operator=(const Insert&) = delete;

    template <class V>
    Insert& value(const char* name, const V& v) {
        if (!names_.empty()) {
            names_.append(", ");
            values_.append(", ");
        }
        write_identifier(names_, name);
        write_literal(values_, v);
        return *this;
    }
    Insert& value(const char* name, const char* v) { return value<const char*>(name, v); }
    Insert& row();  // the next record
    const char* sql();
    Result run();
    Allocator& allocator() const { return db_->allocator(); }

private:
    Text table_;
    Text names_;
    Text values_;
    Text done_;
    Text sql_;
};

}  // namespace vdb

#endif
