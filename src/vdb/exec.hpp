// The executor: runs statement trees push-based, from sources through joins, filters, groups, order and limits
// into sinks; pipeline stages are sinks of the stage before (spec A7, 3.8 – 3.11). Internal to the library.
#ifndef VDB_EXEC_HPP
#define VDB_EXEC_HPP

#include "database.hpp"
#include "regex.hpp"
#include "sql_ast.hpp"

namespace vdb {

enum Flow { k_error = -1, k_stop = 0, k_go = 1 };

// Receives rows inside the executor. Values stay valid during the call only.
class Sink {
public:
    virtual int begin(const char* const* names, size_t count);
    virtual int row(Value* values, size_t count);  // k_go, k_stop or k_error
    virtual int end();

protected:
    ~Sink() {}
};

struct Frame;
struct Scope;

class Executor {
public:
    Executor(Database& db, Pool& statement_pool, const char* sql, const Value* params, size_t param_count, Status& status);
    ~Executor();
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    bool run(Statement* statement, Sink* sink);  // `sink` gets a query's rows; 0 discards them
    size_t changes() const { return changes_; }

    // For the functions and the runs.
    bool eval(Expr* e, Frame* frame, Value& out);
    int truth(Expr* e, Frame* frame);  // k_true, k_false, k_unknown; -2 on error
    char decimal() const { return decimal_; }
    Pool& scratch() { return scratch_; }
    Pool& statement_pool() { return pool_; }
    RegexWork& regex_work() { return regex_work_; }
    Allocator& allocator() { return *allocator_; }
    Status& status() { return status_; }
    Database& database() { return db_; }
    const char* sql() const { return sql_; }
    char* text(size_t length);  // scratch text with room for a NUL
    bool memory();               // sets "out of memory", returns false
    bool own(Table* table);      // freed at the end of the statement
    bool own(HashIndex* index);
    Table* resolve_table(const char* name, Scope* scope, bool driving, bool& stream, Status& status);
    bool enter();
    void leave() { --depth_; }

    // Runs a query (WITH, stages) into a sink.
    int run_query(Query* q, Scope* scope, Frame* outer, Expr* owner, Sink* sink);
    // Runs a query into a new temporary table (owned by the caller unless own()ed).
    Table* run_to_table(Query* q, Scope* scope, Frame* outer, Expr* owner, const char* name);

private:
    bool eval_binary(Expr* e, Frame* frame, Value& out);
    bool eval_match(Expr* e, Frame* frame, Value& out);
    bool eval_in(Expr* e, Frame* frame, Value& out);
    bool eval_subquery(Expr* e, Frame* frame, Value& out);
    bool eval_case(Expr* e, Frame* frame, Value& out);
    bool eval_cast(Expr* e, Frame* frame, Value& out);
    bool eval_function(Expr* e, Frame* frame, Value& out);
    bool insert(Statement* s);
    bool update(Statement* s);
    bool remove(Statement* s);
    bool create(Statement* s);
    void count_names(Statement* s);
    void count_query(Query* q);
    void count_expr(Expr* e);
    void count_name(const char* name);
    size_t occurrences(const char* name) const;

    Database& db_;
    Allocator* allocator_;
    Pool& pool_;
    Pool scratch_;
    RegexWork regex_work_;
    const char* sql_;
    const Value* params_;
    size_t param_count_;
    Status& status_;
    size_t changes_;
    char decimal_;
    int depth_;
    Vec<Table*> owned_tables_;
    Vec<HashIndex*> owned_indexes_;
    Vec<const char*> names_;
};

// Scalar functions (functions.cpp). `args` are evaluated; e->args are the expressions.
bool call_function(Executor& ex, Expr* e, Frame* frame, Value* args, size_t count, Value& out);
bool like_match(const char* s, size_t n, const char* p, size_t m, uint32_t escape, bool icase);
bool glob_match(const char* s, size_t n, const char* p, size_t m);
// SIMILAR TO pattern to an anchored regular expression.
bool similar_to_regex(const char* p, size_t m, uint32_t escape, Text& out);
// The compiled regex for a pattern value, cached on `owner` when `constant`.
const Regex* pattern_regex(Executor& ex, Expr* owner, bool constant, const Value& pattern, unsigned flags);

}  // namespace vdb

#endif
