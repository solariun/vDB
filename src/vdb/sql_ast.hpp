// The statement tree the parser builds and the executor runs (spec 3). Nodes live in a Pool.
#ifndef VDB_SQL_AST_HPP
#define VDB_SQL_AST_HPP

#include "sql_names.hpp"
#include "value.hpp"

namespace vdb {

enum ExprKind {
    e_literal,
    e_param,
    e_column,
    e_star,  // COUNT(*)
    e_unary_minus,
    e_binary,
    e_and,
    e_or,
    e_not,
    e_match,
    e_between,
    e_in_list,
    e_in_query,
    e_in_table,
    e_exists,
    e_subquery,
    e_case,
    e_when,
    e_cast,
    e_function,
    e_aggregate,
    e_is,
    e_output  // a select item, by index (ORDER BY alias or position)
};

enum BinaryOp {
    op_add,
    op_sub,
    op_mul,
    op_div,
    op_mod,
    op_concat,
    op_eq,
    op_ne,
    op_lt,
    op_le,
    op_gt,
    op_ge,
    op_distinct,     // IS DISTINCT FROM
    op_not_distinct  // IS NOT DISTINCT FROM
};

enum MatchOp { m_like, m_ilike, m_glob, m_regexp, m_iregexp, m_similar };
enum IsWhat { is_null_value, is_true_value, is_false_value };
enum CastType { cast_integer, cast_real, cast_numeric, cast_text };

struct Query;

struct Expr {
    uint8_t kind;
    uint8_t op;
    uint8_t negated;   // NOT LIKE, NOT IN, IS NOT ...
    uint8_t distinct;  // aggregate DISTINCT
    uint16_t function;
    Expr* a;
    Expr* b;
    Expr* c;
    Expr* args;  // function arguments, IN list, CASE whens (linked by next)
    Expr* next;
    Value literal;
    const char* name;
    const char* qualifier;
    Query* query;
    // Set by the executor's binder.
    int16_t depth;
    int16_t source;
    int16_t alt_source;
    int32_t column;
    int32_t alt_column;
    int32_t slot;
    void* cache;
    uint8_t cache_state;
    uint8_t correlated;
    uint32_t start;
    uint32_t end;
};

struct SelectItem {
    Expr* expr;  // 0 for * and t.*
    const char* alias;
    const char* star_qualifier;
    bool star;
    uint32_t start;
    uint32_t end;
    SelectItem* next;
};

enum JoinType { join_first, join_inner, join_left, join_right, join_full, join_cross };

struct NameList {
    const char* name;
    NameList* next;
};

struct FromItem {
    uint8_t join;
    const char* name;  // table name or path (spec A10)
    const char* alias;
    Query* query;  // FROM ( query ) alias
    Expr* on;
    NameList* using_columns;
    uint32_t position;
    FromItem* next;
};

struct OrderItem {
    Expr* expr;
    bool descending;
    int8_t nulls;  // 0 default, 1 first, 2 last
    OrderItem* next;
};

enum SetOp { set_none, set_union, set_union_all, set_intersect, set_except };

struct Core {
    bool distinct;
    SelectItem* items;  // 0 means *
    FromItem* from;
    bool from_omitted;
    Expr* where;
    Expr* group_by;  // linked by next
    Expr* having;
    uint8_t set_op;  // how this core combines with the ones before it
    Core* next;
};

struct Compound {
    Core* cores;
    OrderItem* order;
    Expr* limit;
    Expr* offset;
};

struct Cte {
    const char* name;
    Query* query;
    Cte* next;
};

struct Stage {
    Compound* body;
    Stage* next;
};

struct Query {
    Cte* with;
    Stage* stages;
};

enum StatementKind { st_query, st_insert, st_update, st_delete, st_create, st_drop };

struct ValuesRow {
    Expr* values;  // linked by next
    ValuesRow* next;
};

struct Assignment {
    const char* column;
    Expr* value;
    Assignment* next;
};

struct Statement {
    uint8_t kind;
    bool if_exists;  // DROP IF EXISTS / CREATE IF NOT EXISTS
    Query* query;    // SELECT, INSERT ... query, CREATE ... AS query
    const char* table;
    NameList* columns;
    ValuesRow* rows;
    Assignment* assignments;
    Expr* where;
    uint32_t start;
    uint32_t end;
    Statement* next;
};

}  // namespace vdb

#endif
