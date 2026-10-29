#include "sql_parser.hpp"

#include <string.h>

namespace vdb {

namespace {

enum TokenKind { t_end, t_name, t_quoted_name, t_string, t_number, t_param, t_op, t_other };

enum OpToken {
    o_comma,
    o_dot,
    o_lparen,
    o_rparen,
    o_semicolon,
    o_star,
    o_plus,
    o_minus,
    o_slash,
    o_percent,
    o_concat,
    o_eq,
    o_ne,
    o_lt,
    o_le,
    o_gt,
    o_ge,
    o_tilde,
    o_tilde_star,
    o_not_tilde,
    o_not_tilde_star,
    o_pipe_gt
};

struct Token {
    uint8_t kind;
    uint8_t op;
    uint16_t keyword;
    uint32_t start;
    uint32_t end;
    long param;  // ?N, or 0 for ?
};

bool name_start(char c) { return is_alpha(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80; }
bool name_char(char c) { return name_start(c) || is_digit(c) || c == '$'; }

class Parser {
public:
    Parser(Pool& pool, const char* sql, size_t length, Status& status)
        : pool_(pool), sql_(sql), n_(length), status_(status), depth_(0), next_param_(0), max_param_(0), last_end_(0) {
        tok_.kind = t_end;
        tok_.start = tok_.end = 0;
        lex(0, tok_);
    }

    Statement* script(size_t& parameters) {
        Statement* first = 0;
        Statement** link = &first;
        for (;;) {
            while (accept_op(o_semicolon)) {
            }
            if (!ok()) return 0;
            if (tok_.kind == t_end) break;
            Statement* s = statement();
            if (!s) return 0;
            *link = s;
            link = &s->next;
            if (tok_.kind != t_end && !(tok_.kind == t_op && tok_.op == o_semicolon)) {
                return error(VDB_F("expected ';' or the end"));
            }
        }
        parameters = static_cast<size_t>(max_param_);
        return first;
    }

private:
    // ── lexer ────────────────────────────────────────────────────────────────

    void skip_space(size_t& at) {
        for (;;) {
            while (at < n_ && is_space(sql_[at])) ++at;
            if (at + 1 < n_ && sql_[at] == '-' && sql_[at + 1] == '-') {
                while (at < n_ && sql_[at] != '\n') ++at;
            } else if (at + 1 < n_ && sql_[at] == '/' && sql_[at + 1] == '*') {
                at += 2;
                while (at + 1 < n_ && !(sql_[at] == '*' && sql_[at + 1] == '/')) ++at;
                at = at + 2 <= n_ ? at + 2 : n_;
            } else {
                return;
            }
        }
    }

    void lex(size_t at, Token& t) {
        skip_space(at);
        t.start = static_cast<uint32_t>(at);
        t.keyword = kw_none;
        t.param = 0;
        t.op = 0;
        if (at >= n_) {
            t.kind = t_end;
            t.end = t.start;
            return;
        }
        char c = sql_[at];
        char d = at + 1 < n_ ? sql_[at + 1] : 0;
        if (name_start(c)) {
            size_t end = at;
            while (end < n_ && name_char(sql_[end])) ++end;
            t.kind = t_name;
            t.end = static_cast<uint32_t>(end);
            t.keyword = static_cast<uint16_t>(find_keyword(sql_ + at, end - at));
            return;
        }
        if (is_digit(c) || (c == '.' && is_digit(d))) {
            size_t end = at;
            while (end < n_ && is_digit(sql_[end])) ++end;
            if (end < n_ && sql_[end] == '.') {
                ++end;
                while (end < n_ && is_digit(sql_[end])) ++end;
            }
            if (end < n_ && (sql_[end] == 'e' || sql_[end] == 'E')) {
                size_t e = end + 1;
                if (e < n_ && (sql_[e] == '+' || sql_[e] == '-')) ++e;
                if (e < n_ && is_digit(sql_[e])) {
                    while (e < n_ && is_digit(sql_[e])) ++e;
                    end = e;
                }
            }
            t.kind = t_number;
            t.end = static_cast<uint32_t>(end);
            return;
        }
        if (c == '\'' || c == '"' || c == '`' || c == '[') {
            char close = c == '[' ? ']' : c;
            size_t end = at + 1;
            for (;;) {
                if (end >= n_) {
                    t.kind = t_end;
                    t.end = static_cast<uint32_t>(n_);
                    status_.fail(k_error_syntax, c == '\'' ? VDB_F("syntax error at line %d, column %d: text not closed (missing ')")
                                                        : VDB_F("syntax error at line %d, column %d: name not closed"),
                                 line_of(at), column_of(at));
                    return;
                }
                if (sql_[end] == close) {
                    if (close != ']' && end + 1 < n_ && sql_[end + 1] == close) {
                        end += 2;
                        continue;
                    }
                    ++end;
                    break;
                }
                ++end;
            }
            t.kind = c == '\'' ? t_string : t_quoted_name;
            t.end = static_cast<uint32_t>(end);
            return;
        }
        if (c == '?') {
            size_t end = at + 1;
            long index = 0;
            while (end < n_ && is_digit(sql_[end]) && index < 100000) index = index * 10 + (sql_[end++] - '0');
            t.kind = t_param;
            t.param = index;
            t.end = static_cast<uint32_t>(end);
            return;
        }
        t.kind = t_op;
        size_t width = 1;
        switch (c) {
            case ',': t.op = o_comma; break;
            case '.': t.op = o_dot; break;
            case '(': t.op = o_lparen; break;
            case ')': t.op = o_rparen; break;
            case ';': t.op = o_semicolon; break;
            case '*': t.op = o_star; break;
            case '+': t.op = o_plus; break;
            case '-': t.op = o_minus; break;
            case '/': t.op = o_slash; break;
            case '%': t.op = o_percent; break;
            case '=':
                t.op = o_eq;
                if (d == '=') width = 2;
                break;
            case '<':
                t.op = o_lt;
                if (d == '=') {
                    t.op = o_le;
                    width = 2;
                } else if (d == '>') {
                    t.op = o_ne;
                    width = 2;
                }
                break;
            case '>':
                t.op = o_gt;
                if (d == '=') {
                    t.op = o_ge;
                    width = 2;
                }
                break;
            case '!':
                if (d == '=') {
                    t.op = o_ne;
                    width = 2;
                } else if (d == '~') {
                    bool star = at + 2 < n_ && sql_[at + 2] == '*';
                    t.op = star ? o_not_tilde_star : o_not_tilde;
                    width = star ? 3 : 2;
                } else {
                    t.kind = t_other;
                    t.op = static_cast<uint8_t>(c);
                }
                break;
            case '~':
                t.op = d == '*' ? o_tilde_star : o_tilde;
                width = d == '*' ? 2 : 1;
                break;
            case '|':
                if (d == '|') {
                    t.op = o_concat;
                    width = 2;
                } else if (d == '>') {
                    t.op = o_pipe_gt;
                    width = 2;
                } else {
                    t.kind = t_other;
                    t.op = static_cast<uint8_t>(c);
                }
                break;
            default:
                t.kind = t_other;
                t.op = static_cast<uint8_t>(c);
                break;
        }
        t.end = static_cast<uint32_t>(at + width);
    }

    void advance() {
        last_end_ = tok_.end;
        lex(tok_.end, tok_);
    }

    Token peek() {
        Token t;
        lex(tok_.end, t);
        return t;
    }

    int line_of(size_t at) const {
        int line = 1;
        for (size_t i = 0; i < at && i < n_; ++i) line += sql_[i] == '\n';
        return line;
    }
    int column_of(size_t at) const {
        size_t start = at;
        while (start > 0 && sql_[start - 1] != '\n') --start;
        return static_cast<int>(utf8_count(sql_ + start, at - start) + 1);
    }

    bool ok() const { return status_.ok(); }

    decltype(nullptr) error(const char* flash_what) {
        if (!ok()) return nullptr;
        if (tok_.kind == t_end) {
            status_.fail(k_error_syntax, VDB_F("syntax error at the end: "));
        } else {
            const int k_shown = 24;
            int length = static_cast<int>(tok_.end - tok_.start);
            status_.fail(k_error_syntax, VDB_F("syntax error at line %d, column %d near \"%.*s\": "), line_of(tok_.start),
                         column_of(tok_.start), length < k_shown ? length : k_shown, sql_ + tok_.start);
        }
        status_.add(flash_what);
        return nullptr;
    }

    decltype(nullptr) unsupported(const char* flash_what) {
        if (ok()) status_.fail(k_error_unsupported, VDB_F("not supported: ")).add(flash_what);
        return nullptr;
    }

    decltype(nullptr) memory() {
        if (ok()) status_.fail_memory();
        return nullptr;
    }

    bool at_kw(Keyword k) const { return tok_.kind == t_name && tok_.keyword == k; }
    bool accept_kw(Keyword k) {
        if (!at_kw(k)) return false;
        advance();
        return true;
    }
    bool expect_kw(Keyword k, const char* flash_what) {
        if (accept_kw(k)) return true;
        error(flash_what);
        return false;
    }
    bool at_op(OpToken o) const { return tok_.kind == t_op && tok_.op == o; }
    bool accept_op(OpToken o) {
        if (!at_op(o)) return false;
        advance();
        return true;
    }
    bool expect_op(OpToken o, const char* flash_what) {
        if (accept_op(o)) return true;
        error(flash_what);
        return false;
    }

    // A name: an unquoted word that is not reserved, or a quoted name.
    bool at_name() const {
        return (tok_.kind == t_name && !keyword_reserved(static_cast<Keyword>(tok_.keyword))) || tok_.kind == t_quoted_name;
    }

    const char* take_name() {
        const char* name = token_text(tok_);
        if (!name) return memory();
        advance();
        return name;
    }

    // The token's text, unquoted and unescaped, copied into the pool.
    const char* token_text(const Token& t) {
        const char* s = sql_ + t.start;
        size_t n = t.end - t.start;
        if (t.kind != t_string && t.kind != t_quoted_name) return pool_.copy(s, n);
        char quote = s[0] == '[' ? ']' : s[0];
        char* out = static_cast<char*>(pool_.allocate(n));
        if (!out) return 0;
        size_t length = 0;
        for (size_t i = 1; i + 1 < n; ++i) {
            out[length++] = s[i];
            if (s[i] == quote && quote != ']' && i + 2 < n && s[i + 1] == quote) ++i;
        }
        out[length] = 0;
        return out;
    }

    bool enter() {
        if (++depth_ > VDB_MAX_DEPTH) {
            if (ok()) status_.fail(k_error_limit, VDB_F("query nested too deeply (limit %d)"), VDB_MAX_DEPTH);
            return false;
        }
        return true;
    }
    void leave() { --depth_; }

    Expr* node(uint8_t kind) {
        Expr* e = make<Expr>(pool_);
        if (!e) return memory();
        e->kind = kind;
        e->literal = Value::null();
        e->start = tok_.start;
        e->end = tok_.end;
        e->source = -1;
        e->alt_source = -1;
        e->column = -1;
        e->slot = -1;
        return e;
    }

    // ── statements ───────────────────────────────────────────────────────────

    Statement* statement() {
        Statement* s = make<Statement>(pool_);
        if (!s) return memory();
        s->start = tok_.start;
        if (at_kw(kw_SELECT) || at_kw(kw_WITH) || at_op(o_lparen)) {
            s->kind = st_query;
            s->query = query();
            if (!s->query) return 0;
        } else if (accept_kw(kw_INSERT)) {
            if (!insert(s)) return 0;
        } else if (accept_kw(kw_UPDATE)) {
            if (!update(s)) return 0;
        } else if (accept_kw(kw_DELETE)) {
            s->kind = st_delete;
            if (!expect_kw(kw_FROM, VDB_F("expected FROM after DELETE"))) return 0;
            if (!(s->table = table_name())) return 0;
            if (accept_kw(kw_WHERE) && !(s->where = expr())) return 0;
        } else if (accept_kw(kw_CREATE)) {
            if (!create(s)) return 0;
        } else if (accept_kw(kw_DROP)) {
            s->kind = st_drop;
            if (!expect_kw(kw_TABLE, VDB_F("expected TABLE after DROP"))) return 0;
            if (accept_kw(kw_IF)) {
                if (!expect_kw(kw_EXISTS, VDB_F("expected EXISTS after IF"))) return 0;
                s->if_exists = true;
            }
            if (!(s->table = table_name())) return 0;
        } else if (at_kw(kw_ALTER)) {
            return unsupported(VDB_F("ALTER"));
        } else {
            return error(VDB_F("expected a statement (SELECT, INSERT, UPDATE, DELETE, CREATE, DROP)"));
        }
        s->end = last_end_;
        return ok() ? s : 0;
    }

    NameList* name_list() {
        NameList* first = 0;
        NameList** link = &first;
        do {
            if (!at_name()) return error(VDB_F("expected a column name"));
            NameList* item = make<NameList>(pool_);
            if (!item) return memory();
            if (!(item->name = take_name())) return 0;
            *link = item;
            link = &item->next;
        } while (accept_op(o_comma));
        return first;
    }

    bool insert(Statement* s) {
        s->kind = st_insert;
        if (!expect_kw(kw_INTO, VDB_F("expected INTO after INSERT"))) return false;
        if (!(s->table = table_name())) return false;
        if (at_op(o_lparen)) {
            Token next = peek();
            if (!(next.kind == t_name && (next.keyword == kw_SELECT || next.keyword == kw_WITH))) {
                advance();
                if (!(s->columns = name_list())) return false;
                if (!expect_op(o_rparen, VDB_F("expected ')' after the column names"))) return false;
            }
        }
        if (accept_kw(kw_VALUES)) {
            ValuesRow** link = &s->rows;
            do {
                if (!expect_op(o_lparen, VDB_F("expected '(' before the values"))) return false;
                ValuesRow* row = make<ValuesRow>(pool_);
                if (!row) {
                memory();
                return false;
            }
                if (!(row->values = expr_list())) return false;
                if (!expect_op(o_rparen, VDB_F("expected ')' after the values"))) return false;
                *link = row;
                link = &row->next;
            } while (accept_op(o_comma));
            return true;
        }
        if (at_kw(kw_SELECT) || at_kw(kw_WITH) || at_op(o_lparen)) return (s->query = query()) != 0;
        error(VDB_F("expected VALUES or SELECT"));
        return false;
    }

    bool update(Statement* s) {
        s->kind = st_update;
        if (!(s->table = table_name())) return false;
        if (!expect_kw(kw_SET, VDB_F("expected SET"))) return false;
        Assignment** link = &s->assignments;
        do {
            if (!at_name()) {
                error(VDB_F("expected a column name"));
                return false;
            }
            Assignment* a = make<Assignment>(pool_);
            if (!a) {
                memory();
                return false;
            }
            if (!(a->column = take_name())) return false;
            if (!expect_op(o_eq, VDB_F("expected '='"))) return false;
            if (!(a->value = expr())) return false;
            *link = a;
            link = &a->next;
        } while (accept_op(o_comma));
        if (accept_kw(kw_WHERE) && !(s->where = expr())) return false;
        return true;
    }

    bool create(Statement* s) {
        s->kind = st_create;
        if (at_kw(kw_INDEX) || at_kw(kw_VIEW) || at_kw(kw_TRIGGER) || at_kw(kw_UNIQUE)) {
            unsupported(VDB_F("CREATE INDEX, VIEW and TRIGGER"));
            return false;
        }
        if (!expect_kw(kw_TABLE, VDB_F("expected TABLE after CREATE"))) return false;
        if (accept_kw(kw_IF)) {
            if (!expect_kw(kw_NOT, VDB_F("expected NOT after IF")) || !expect_kw(kw_EXISTS, VDB_F("expected EXISTS")))
                return false;
            s->if_exists = true;
        }
        if (!(s->table = table_name())) return false;
        if (accept_op(o_lparen)) {
            NameList** link = &s->columns;
            do {
                if (at_kw(kw_PRIMARY) || at_kw(kw_UNIQUE) || at_kw(kw_CHECK) || at_kw(kw_CONSTRAINT)) {
                    unsupported(VDB_F("constraints (records are free-form)"));
                    return false;
                }
                if (!at_name()) {
                    error(VDB_F("expected a column name"));
                    return false;
                }
                NameList* column = make<NameList>(pool_);
                if (!column) {
                memory();
                return false;
            }
                if (!(column->name = take_name())) return false;
                *link = column;
                link = &column->next;
                // The type is accepted and ignored: values are flexible (spec A2).
                const int k_max_type_words = 3;
                for (int words = 0; words < k_max_type_words && tok_.kind == t_name && !tok_.keyword; ++words) advance();
                if (accept_op(o_lparen)) {
                    while (tok_.kind == t_number || at_op(o_comma)) advance();
                    if (!expect_op(o_rparen, VDB_F("expected ')' after the type size"))) return false;
                }
                if (at_kw(kw_NOT) || at_kw(kw_NULL) || at_kw(kw_PRIMARY) || at_kw(kw_UNIQUE) || at_kw(kw_DEFAULT) ||
                    at_kw(kw_REFERENCES) || at_kw(kw_CHECK)) {
                    unsupported(VDB_F("column constraints (records are free-form)"));
                    return false;
                }
            } while (accept_op(o_comma));
            if (!expect_op(o_rparen, VDB_F("expected ')' after the columns"))) return false;
            return true;
        }
        if (!expect_kw(kw_AS, VDB_F("expected '(' or AS after the table name"))) return false;
        return (s->query = query()) != 0;
    }

    // ── queries ──────────────────────────────────────────────────────────────

    Query* query() {
        if (!enter()) return 0;
        Query* q = make<Query>(pool_);
        if (!q) return memory();
        if (accept_kw(kw_WITH)) {
            if (at_kw(kw_RECURSIVE)) return unsupported(VDB_F("WITH RECURSIVE"));
            Cte** link = &q->with;
            do {
                if (!at_name()) return error(VDB_F("expected a name after WITH"));
                Cte* cte = make<Cte>(pool_);
                if (!cte) return memory();
                if (!(cte->name = take_name())) return 0;
                if (!expect_kw(kw_AS, VDB_F("expected AS")) || !expect_op(o_lparen, VDB_F("expected '('"))) return 0;
                if (!(cte->query = query())) return 0;
                if (!expect_op(o_rparen, VDB_F("expected ')'"))) return 0;
                *link = cte;
                link = &cte->next;
            } while (accept_op(o_comma));
        }
        Stage* stage = make<Stage>(pool_);
        if (!stage) return memory();
        q->stages = stage;
        if (accept_op(o_lparen)) {
            // ( query ) as the first stage: its stages are taken over.
            Query* inner = query();
            if (!inner || !expect_op(o_rparen, VDB_F("expected ')'"))) return 0;
            if (inner->with) return unsupported(VDB_F("WITH inside parentheses here"));
            q->stages = inner->stages;
            stage = q->stages;
            while (stage->next) stage = stage->next;
            if (at_kw(kw_ORDER) || at_kw(kw_LIMIT) || at_kw(kw_OFFSET)) {
                Stage* extra = make<Stage>(pool_);
                if (!extra || !(extra->body = make<Compound>(pool_)) || !(extra->body->cores = make<Core>(pool_)))
                    return memory();
                extra->body->cores->from_omitted = true;
                if (!order_limit(extra->body)) return 0;
                stage->next = extra;
                stage = extra;
            }
        } else if (!(stage->body = compound())) {
            return 0;
        }
        while (accept_op(o_pipe_gt)) {
            Stage* next = make<Stage>(pool_);
            if (!next) return memory();
            if (!(next->body = stage_body())) return 0;
            stage->next = next;
            stage = next;
        }
        leave();
        return q;
    }

    Compound* compound() {
        Compound* c = make<Compound>(pool_);
        if (!c) return memory();
        if (!expect_kw(kw_SELECT, VDB_F("expected SELECT"))) return 0;
        Core* core = select_core();
        if (!core) return 0;
        c->cores = core;
        for (;;) {
            uint8_t op = set_none;
            if (accept_kw(kw_UNION)) {
                op = accept_kw(kw_ALL) ? set_union_all : set_union;
                accept_kw(kw_DISTINCT);
            } else if (accept_kw(kw_INTERSECT)) {
                op = set_intersect;
                accept_kw(kw_DISTINCT);
            } else if (accept_kw(kw_EXCEPT)) {
                op = set_except;
                accept_kw(kw_DISTINCT);
            } else {
                break;
            }
            if (!expect_kw(kw_SELECT, VDB_F("expected SELECT after the set operator"))) return 0;
            Core* next = select_core();
            if (!next) return 0;
            next->set_op = op;
            core->next = next;
            core = next;
        }
        if (!order_limit(c)) return 0;
        return c;
    }

    // After SELECT: [DISTINCT|ALL] items [FROM] [WHERE] [GROUP BY] [HAVING].
    Core* select_core() {
        Core* core = make<Core>(pool_);
        if (!core) return memory();
        if (accept_kw(kw_DISTINCT)) core->distinct = true;
        else accept_kw(kw_ALL);
        if (!(core->items = items())) return 0;
        return clauses(core) ? core : 0;
    }

    bool clauses(Core* core) {
        if (accept_kw(kw_FROM)) {
            if (!(core->from = from_list())) return false;
        } else {
            core->from_omitted = true;
        }
        if (accept_kw(kw_WHERE) && !(core->where = expr())) return false;
        if (accept_kw(kw_GROUP)) {
            if (!expect_kw(kw_BY, VDB_F("expected BY after GROUP"))) return false;
            if (!(core->group_by = expr_list())) return false;
        }
        if (accept_kw(kw_HAVING) && !(core->having = expr())) return false;
        if (at_kw(kw_WINDOW)) {
            unsupported(VDB_F("window functions"));
            return false;
        }
        return ok();
    }

    // A pipeline stage: any of SELECT, FROM, WHERE, GROUP BY, HAVING, ORDER BY, LIMIT (spec 3.11).
    Compound* stage_body() {
        Compound* c = make<Compound>(pool_);
        Core* core = make<Core>(pool_);
        if (!c || !core) return memory();
        c->cores = core;
        uint32_t start = tok_.start;
        if (accept_kw(kw_SELECT)) {
            if (accept_kw(kw_DISTINCT)) core->distinct = true;
            else accept_kw(kw_ALL);
            if (!(core->items = items())) return 0;
        }
        if (!clauses(core) || !order_limit(c)) return 0;
        if (tok_.start == start) return error(VDB_F("expected a clause after |> (SELECT, WHERE, ORDER BY, LIMIT, ...)"));
        return c;
    }

    bool order_limit(Compound* c) {
        if (accept_kw(kw_ORDER)) {
            if (!expect_kw(kw_BY, VDB_F("expected BY after ORDER"))) return false;
            OrderItem** link = &c->order;
            do {
                OrderItem* item = make<OrderItem>(pool_);
                if (!item) {
                memory();
                return false;
            }
                if (!(item->expr = expr())) return false;
                if (accept_kw(kw_DESC)) item->descending = true;
                else accept_kw(kw_ASC);
                if (accept_kw(kw_NULLS)) {
                    if (accept_kw(kw_FIRST)) item->nulls = 1;
                    else if (accept_kw(kw_LAST)) item->nulls = 2;
                    else {
                        error(VDB_F("expected FIRST or LAST after NULLS"));
                        return false;
                    }
                }
                *link = item;
                link = &item->next;
            } while (accept_op(o_comma));
        }
        if (accept_kw(kw_LIMIT)) {
            if (!(c->limit = expr())) return false;
            if (accept_op(o_comma)) {
                c->offset = c->limit;
                if (!(c->limit = expr())) return false;
            } else if (accept_kw(kw_OFFSET)) {
                if (!(c->offset = expr())) return false;
            }
        } else if (accept_kw(kw_OFFSET)) {
            if (!(c->offset = expr())) return false;
            if (!accept_kw(kw_ROWS)) accept_kw(kw_ROW);
        }
        if (accept_kw(kw_FETCH)) {
            if (!accept_kw(kw_FIRST) && !expect_kw(kw_NEXT, VDB_F("expected FIRST or NEXT"))) return false;
            if (at_kw(kw_ROW) || at_kw(kw_ROWS)) {
                if (!(c->limit = node(e_literal))) return false;
                c->limit->literal = Value::integer(1);
            } else if (!(c->limit = expr())) {
                return false;
            }
            if (!accept_kw(kw_ROWS) && !expect_kw(kw_ROW, VDB_F("expected ROWS"))) return false;
            if (!expect_kw(kw_ONLY, VDB_F("expected ONLY"))) return false;
        }
        return ok();
    }

    SelectItem* items() {
        SelectItem* first = 0;
        SelectItem** link = &first;
        do {
            SelectItem* item = make<SelectItem>(pool_);
            if (!item) return memory();
            item->start = tok_.start;
            if (accept_op(o_star)) {
                item->star = true;
            } else if ((tok_.kind == t_name || tok_.kind == t_quoted_name) && peek_is_dot_star()) {
                if (!(item->star_qualifier = token_text(tok_))) return memory();
                advance();
                advance();
                advance();
                item->star = true;
            } else {
                if (!(item->expr = expr())) return 0;
                if (accept_kw(kw_AS)) {
                    if (!at_name() && tok_.kind != t_string && tok_.kind != t_name) return error(VDB_F("expected a name after AS"));
                    if (!(item->alias = take_name())) return 0;
                } else if (at_name() || tok_.kind == t_string) {
                    if (!(item->alias = take_name())) return 0;
                }
            }
            item->end = last_end_;
            *link = item;
            link = &item->next;
        } while (accept_op(o_comma));
        return first;
    }

    bool peek_is_dot_star() {
        size_t at = tok_.end;
        Token dot;
        lex(at, dot);
        if (!(dot.kind == t_op && dot.op == o_dot)) return false;
        Token star;
        lex(dot.end, star);
        return star.kind == t_op && star.op == o_star;
    }

    FromItem* from_list() {
        FromItem* first = source(join_first);
        if (!first) return 0;
        FromItem* last = first;
        for (;;) {
            uint8_t join;
            bool comma = false;
            if (accept_op(o_comma)) {
                join = join_cross;
                comma = true;
            } else if (at_kw(kw_NATURAL)) {
                return unsupported(VDB_F("NATURAL JOIN (use ON or USING)"));
            } else if (accept_kw(kw_JOIN)) {
                join = join_inner;
            } else {
                if (accept_kw(kw_INNER)) {
                    join = join_inner;
                } else if (accept_kw(kw_LEFT)) {
                    join = join_left;
                    accept_kw(kw_OUTER);
                } else if (accept_kw(kw_RIGHT)) {
                    join = join_right;
                    accept_kw(kw_OUTER);
                } else if (accept_kw(kw_FULL)) {
                    join = join_full;
                    accept_kw(kw_OUTER);
                } else if (accept_kw(kw_CROSS)) {
                    join = join_cross;
                } else {
                    break;
                }
                if (!expect_kw(kw_JOIN, VDB_F("expected JOIN"))) return 0;
            }
            FromItem* item = source(join);
            if (!item) return 0;
            if (!comma && join != join_cross) {
                if (accept_kw(kw_ON)) {
                    if (!(item->on = expr())) return 0;
                } else if (accept_kw(kw_USING)) {
                    if (!expect_op(o_lparen, VDB_F("expected '(' after USING"))) return 0;
                    if (!(item->using_columns = name_list())) return 0;
                    if (!expect_op(o_rparen, VDB_F("expected ')'"))) return 0;
                } else {
                    return error(VDB_F("expected ON or USING after the joined table"));
                }
            }
            last->next = item;
            last = item;
        }
        return first;
    }

    FromItem* source(uint8_t join) {
        FromItem* item = make<FromItem>(pool_);
        if (!item) return memory();
        item->join = join;
        item->position = tok_.start;
        if (at_kw(kw_LATERAL)) return unsupported(VDB_F("LATERAL"));
        if (accept_op(o_lparen)) {
            if (!(item->query = query())) return 0;
            if (!expect_op(o_rparen, VDB_F("expected ')' after the subquery"))) return 0;
            accept_kw(kw_AS);
            if (!at_name()) return error(VDB_F("a subquery in FROM needs a name: (SELECT ...) AS name"));
            if (!(item->alias = take_name())) return 0;
            return item;
        }
        if (!(item->name = table_name())) return 0;
        if (accept_kw(kw_AS)) {
            if (!at_name()) return error(VDB_F("expected a name after AS"));
            if (!(item->alias = take_name())) return 0;
        } else if (at_name()) {
            if (!(item->alias = take_name())) return 0;
        } else {
            item->alias = default_alias(pool_, item->name);
            if (!item->alias) return memory();
        }
        return item;
    }

    static bool path_token(const Token& t) {
        if (t.kind == t_name || t.kind == t_number) return true;
        if (t.kind == t_other) return t.op == '@' || t.op == '+' || t.op == '#' || t.op == '&' || t.op == '!';
        return t.kind == t_op && (t.op == o_dot || t.op == o_slash || t.op == o_minus || t.op == o_tilde);
    }

    // A table name, or a path written without spaces (data/people.csv, ../x.csv, -), spec A10.
    const char* table_name() {
        if (tok_.kind == t_quoted_name || tok_.kind == t_string) return take_name();  // FROM 'people.csv' too
        if (!path_token(tok_) || tok_.kind == t_number) return error(VDB_F("expected a table name"));
        uint32_t start = tok_.start;
        Token first = tok_;
        advance();
        bool joined = false;
        while (path_token(tok_) && tok_.start == last_end_) {
            joined = true;
            advance();
        }
        if (!joined && first.kind == t_name && keyword_reserved(static_cast<Keyword>(first.keyword))) {
            tok_ = first;
            last_end_ = first.start;
            return error(VDB_F("expected a table name (quote names that are SQL words: \"order\")"));
        }
        if (!joined && first.kind == t_op && first.op != o_minus) {
            tok_ = first;
            return error(VDB_F("expected a table name"));
        }
        const char* name = pool_.copy(sql_ + start, last_end_ - start);
        return name ? name : memory();
    }

    // ── expressions ──────────────────────────────────────────────────────────

    Expr* expr_list() {
        Expr* first = 0;
        Expr** link = &first;
        do {
            Expr* e = expr();
            if (!e) return 0;
            *link = e;
            link = &e->next;
        } while (accept_op(o_comma));
        return first;
    }

    Expr* expr() {
        if (!enter()) return 0;
        Expr* e = or_expr();
        leave();
        return e;
    }

    Expr* binary(uint8_t kind, uint8_t op, Expr* a, Expr* b) {
        Expr* e = node(kind);
        if (!e) return 0;
        e->op = op;
        e->a = a;
        e->b = b;
        e->start = a->start;
        e->end = last_end_;
        return e;
    }

    Expr* or_expr() {
        Expr* left = and_expr();
        while (left && accept_kw(kw_OR)) {
            Expr* right = and_expr();
            if (!right) return 0;
            left = binary(e_or, 0, left, right);
        }
        return left;
    }

    Expr* and_expr() {
        Expr* left = not_expr();
        while (left && accept_kw(kw_AND)) {
            Expr* right = not_expr();
            if (!right) return 0;
            left = binary(e_and, 0, left, right);
        }
        return left;
    }

    Expr* not_expr() {
        if (at_kw(kw_NOT)) {
            Expr* e = node(e_not);
            if (!e) return 0;
            advance();
            if (!enter()) return 0;
            e->a = not_expr();
            leave();
            return e->a ? e : 0;
        }
        return comparison();
    }

    Expr* comparison() {
        Expr* left = concat();
        while (left) {
            if (tok_.kind == t_op && tok_.op >= o_eq && tok_.op <= o_ge) {
                uint8_t op = tok_.op == o_eq ? op_eq : tok_.op == o_ne ? op_ne : tok_.op == o_lt ? op_lt
                           : tok_.op == o_le ? op_le : tok_.op == o_gt ? op_gt : op_ge;
                advance();
                Expr* right = concat();
                if (!right) return 0;
                left = binary(e_binary, op, left, right);
                continue;
            }
            if (tok_.kind == t_op && tok_.op >= o_tilde && tok_.op <= o_not_tilde_star) {
                uint8_t o = tok_.op;
                advance();
                Expr* right = concat();
                if (!right) return 0;
                left = binary(e_match, (o == o_tilde || o == o_not_tilde) ? m_regexp : m_iregexp, left, right);
                if (left) left->negated = o == o_not_tilde || o == o_not_tilde_star;
                continue;
            }
            if (at_kw(kw_ISNULL) || at_kw(kw_NOTNULL)) {
                bool negated = at_kw(kw_NOTNULL);
                advance();
                Expr* e = binary(e_is, is_null_value, left, 0);
                if (e) e->negated = negated;
                left = e;
                continue;
            }
            if (accept_kw(kw_IS)) {
                bool negated = accept_kw(kw_NOT);
                if (accept_kw(kw_NULL)) {
                    left = binary(e_is, is_null_value, left, 0);
                } else if (accept_kw(kw_TRUE)) {
                    left = binary(e_is, is_true_value, left, 0);
                } else if (accept_kw(kw_FALSE)) {
                    left = binary(e_is, is_false_value, left, 0);
                } else if (accept_kw(kw_DISTINCT)) {
                    if (!expect_kw(kw_FROM, VDB_F("expected FROM after IS DISTINCT"))) return 0;
                    Expr* right = concat();
                    if (!right) return 0;
                    left = binary(e_binary, negated ? op_not_distinct : op_distinct, left, right);
                    continue;
                } else {
                    Expr* right = concat();  // IS as =, null-safe (SQLite)
                    if (!right) return 0;
                    left = binary(e_binary, negated ? op_distinct : op_not_distinct, left, right);
                    continue;
                }
                if (left) left->negated = negated;
                continue;
            }
            bool negated = false;
            if (at_kw(kw_NOT)) {
                Token next = peek();
                bool follows = next.kind == t_name &&
                               (next.keyword == kw_BETWEEN || next.keyword == kw_IN || next.keyword == kw_LIKE ||
                                next.keyword == kw_ILIKE || next.keyword == kw_GLOB || next.keyword == kw_REGEXP ||
                                next.keyword == kw_RLIKE || next.keyword == kw_SIMILAR);
                if (!follows) break;
                advance();
                negated = true;
            }
            if (accept_kw(kw_BETWEEN)) {
                Expr* low = concat();
                if (!low || !expect_kw(kw_AND, VDB_F("expected AND in BETWEEN"))) return 0;
                Expr* high = concat();
                if (!high) return 0;
                Expr* e = binary(e_between, 0, left, low);
                if (!e) return 0;
                e->c = high;
                e->negated = negated;
                left = e;
                continue;
            }
            if (accept_kw(kw_IN)) {
                left = in_expr(left, negated);
                continue;
            }
            uint8_t match = 0xFF;
            if (accept_kw(kw_LIKE)) match = m_like;
            else if (accept_kw(kw_ILIKE)) match = m_ilike;
            else if (accept_kw(kw_GLOB)) match = m_glob;
            else if (accept_kw(kw_REGEXP) || accept_kw(kw_RLIKE)) match = m_regexp;
            else if (accept_kw(kw_SIMILAR)) {
                if (!expect_kw(kw_TO, VDB_F("expected TO after SIMILAR"))) return 0;
                match = m_similar;
            }
            if (match == 0xFF) {
                if (negated) return error(VDB_F("expected BETWEEN, IN, LIKE, GLOB, REGEXP or SIMILAR after NOT"));
                break;
            }
            Expr* pattern = concat();
            if (!pattern) return 0;
            Expr* e = binary(e_match, match, left, pattern);
            if (!e) return 0;
            e->negated = negated;
            if (accept_kw(kw_ESCAPE)) {
                if (!(e->c = concat())) return 0;
            }
            left = e;
        }
        return left;
    }

    Expr* in_expr(Expr* left, bool negated) {
        Expr* e = node(e_in_list);
        if (!e) return 0;
        e->a = left;
        e->negated = negated;
        e->start = left->start;
        if (accept_op(o_lparen)) {
            if (at_kw(kw_SELECT) || at_kw(kw_WITH)) {
                e->kind = e_in_query;
                if (!(e->query = query())) return 0;
            } else if (!at_op(o_rparen)) {
                if (!(e->args = expr_list())) return 0;
            }
            if (!expect_op(o_rparen, VDB_F("expected ')' after the IN list"))) return 0;
        } else {
            e->kind = e_in_table;
            if (!(e->name = table_name())) return 0;
        }
        e->end = last_end_;
        return e;
    }

    Expr* concat() {
        Expr* left = additive();
        while (left && accept_op(o_concat)) {
            Expr* right = additive();
            if (!right) return 0;
            left = binary(e_binary, op_concat, left, right);
        }
        return left;
    }

    Expr* additive() {
        Expr* left = multiplicative();
        while (left && (at_op(o_plus) || at_op(o_minus))) {
            uint8_t op = tok_.op == o_plus ? op_add : op_sub;
            advance();
            Expr* right = multiplicative();
            if (!right) return 0;
            left = binary(e_binary, op, left, right);
        }
        return left;
    }

    Expr* multiplicative() {
        Expr* left = unary();
        while (left && (at_op(o_star) || at_op(o_slash) || at_op(o_percent))) {
            uint8_t op = tok_.op == o_star ? op_mul : tok_.op == o_slash ? op_div : op_mod;
            advance();
            Expr* right = unary();
            if (!right) return 0;
            left = binary(e_binary, op, left, right);
        }
        return left;
    }

    Expr* unary() {
        if (at_op(o_minus) || at_op(o_plus)) {
            bool minus = at_op(o_minus);
            uint32_t start = tok_.start;
            advance();
            if (!enter()) return 0;
            Expr* operand = unary();
            leave();
            if (!operand || !minus) return operand;
            if (operand->kind == e_literal && operand->literal.is_number()) {
                if (operand->literal.type == k_integer && operand->literal.i != INT64_MIN) operand->literal.i = -operand->literal.i;
                else if (operand->literal.type == k_real) operand->literal.r = -operand->literal.r;
                else operand->literal = Value::real(-static_cast<double>(operand->literal.i));
                operand->start = start;
                return operand;
            }
            Expr* e = node(e_unary_minus);
            if (!e) return 0;
            e->a = operand;
            e->start = start;
            e->end = last_end_;
            return e;
        }
        return primary();
    }

    Expr* literal(const Value& v) {
        Expr* e = node(e_literal);
        if (!e) return 0;
        e->literal = v;
        advance();
        e->end = last_end_;
        return e;
    }

    Expr* primary() {
        switch (tok_.kind) {
            case t_number: {
                Number n;
                if (!parse_number(sql_ + tok_.start, tok_.end - tok_.start, '.', n)) return error(VDB_F("bad number"));
                return literal(number_value(n));
            }
            case t_string: {
                const char* text = token_text(tok_);
                if (!text) return memory();
                return literal(Value::text(text, strlen(text), k_strict));
            }
            case t_param: {
                Expr* e = node(e_param);
                if (!e) return 0;
                long index = tok_.param ? tok_.param : ++next_param_;
                if (index > max_param_) max_param_ = index;
                e->slot = static_cast<int32_t>(index - 1);
                advance();
                return e;
            }
            case t_op:
                if (at_op(o_lparen)) return parenthesis();
                if (at_op(o_star)) return error(VDB_F("'*' is only allowed as SELECT * or COUNT(*)"));
                return error(VDB_F("expected a value, a column or a function"));
            case t_name:
                if (at_kw(kw_NULL)) return literal(Value::null());
                if (at_kw(kw_TRUE)) return literal(Value::boolean(true));
                if (at_kw(kw_FALSE)) return literal(Value::boolean(false));
                if (at_kw(kw_CASE)) return case_expr();
                if (at_kw(kw_CAST)) return cast_expr();
                if (at_kw(kw_EXISTS)) {
                    Expr* e = node(e_exists);
                    if (!e) return 0;
                    advance();
                    if (!expect_op(o_lparen, VDB_F("expected '(' after EXISTS"))) return 0;
                    if (!(e->query = query())) return 0;
                    if (!expect_op(o_rparen, VDB_F("expected ')'"))) return 0;
                    e->end = last_end_;
                    return e;
                }
                if (peek_is_lparen() && (!keyword_reserved(static_cast<Keyword>(tok_.keyword)) || at_kw(kw_LEFT) ||
                                         at_kw(kw_RIGHT))) {
                    return function_call();
                }
                if (keyword_reserved(static_cast<Keyword>(tok_.keyword))) {
                    return error(VDB_F("expected a value or a column (quote names that are SQL words: \"order\")"));
                }
                return column();
            case t_quoted_name: return column();
            case t_other: return error(VDB_F("unexpected character"));
            default: return error(VDB_F("expected a value, a column or a function"));
        }
    }

    bool peek_is_lparen() {
        Token t = peek();
        return t.kind == t_op && t.op == o_lparen;
    }

    Expr* parenthesis() {
        uint32_t start = tok_.start;
        advance();
        if (at_kw(kw_SELECT) || at_kw(kw_WITH)) {
            Expr* e = node(e_subquery);
            if (!e) return 0;
            e->start = start;
            if (!(e->query = query())) return 0;
            if (!expect_op(o_rparen, VDB_F("expected ')' after the subquery"))) return 0;
            e->end = last_end_;
            return e;
        }
        Expr* e = expr();
        if (!e) return 0;
        if (at_op(o_comma)) return unsupported(VDB_F("row values (a, b)"));
        if (!expect_op(o_rparen, VDB_F("expected ')'"))) return 0;
        e->start = start;
        e->end = last_end_;
        return e;
    }

    Expr* column() {
        Expr* e = node(e_column);
        if (!e) return 0;
        if (!(e->name = take_name())) return 0;
        if (at_op(o_dot)) {
            advance();
            if (!at_name() && tok_.kind != t_name) return error(VDB_F("expected a column name after '.'"));
            e->qualifier = e->name;
            if (!(e->name = take_name())) return 0;
        }
        e->end = last_end_;
        return e;
    }

    Expr* case_expr() {
        Expr* e = node(e_case);
        if (!e) return 0;
        advance();
        if (!at_kw(kw_WHEN) && !(e->a = expr())) return 0;
        Expr** link = &e->args;
        while (accept_kw(kw_WHEN)) {
            Expr* when = node(e_when);
            if (!when) return 0;
            if (!(when->a = expr())) return 0;
            if (!expect_kw(kw_THEN, VDB_F("expected THEN"))) return 0;
            if (!(when->b = expr())) return 0;
            *link = when;
            link = &when->next;
        }
        if (!e->args) return error(VDB_F("expected WHEN in CASE"));
        if (accept_kw(kw_ELSE) && !(e->c = expr())) return 0;
        if (!expect_kw(kw_END, VDB_F("expected END to close CASE"))) return 0;
        e->end = last_end_;
        return e;
    }

    Expr* cast_expr() {
        Expr* e = node(e_cast);
        if (!e) return 0;
        advance();
        if (!expect_op(o_lparen, VDB_F("expected '(' after CAST"))) return 0;
        if (!(e->a = expr())) return 0;
        if (!expect_kw(kw_AS, VDB_F("expected AS in CAST"))) return 0;
        if (tok_.kind != t_name) return error(VDB_F("expected a type name"));
        const char* s = sql_ + tok_.start;
        size_t n = tok_.end - tok_.start;
        if (names_equal(s, n, "INTEGER") || names_equal(s, n, "INT") || names_equal(s, n, "BIGINT") ||
            names_equal(s, n, "SMALLINT") || names_equal(s, n, "TINYINT")) {
            e->op = cast_integer;
        } else if (names_equal(s, n, "REAL") || names_equal(s, n, "FLOAT") || names_equal(s, n, "DOUBLE")) {
            e->op = cast_real;
        } else if (names_equal(s, n, "NUMERIC") || names_equal(s, n, "DECIMAL") || names_equal(s, n, "NUMBER")) {
            e->op = cast_numeric;
        } else if (names_equal(s, n, "TEXT") || names_equal(s, n, "VARCHAR") || names_equal(s, n, "CHAR") ||
                   names_equal(s, n, "CHARACTER") || names_equal(s, n, "STRING") || names_equal(s, n, "NVARCHAR")) {
            e->op = cast_text;
        } else {
            return error(VDB_F("unknown type (INTEGER, REAL, NUMERIC or TEXT)"));
        }
        advance();
        while (tok_.kind == t_name && !tok_.keyword) advance();  // DOUBLE PRECISION, CHARACTER VARYING
        if (accept_op(o_lparen)) {
            while (tok_.kind == t_number || at_op(o_comma)) advance();
            if (!expect_op(o_rparen, VDB_F("expected ')'"))) return 0;
        }
        if (!expect_op(o_rparen, VDB_F("expected ')' to close CAST"))) return 0;
        e->end = last_end_;
        return e;
    }

    struct Hint {
        const char* wanted;
        size_t best;
        char name[24];
    };

    static void hint_visit(const char* name, const FunctionInfo&, void* user) {
        Hint* h = static_cast<Hint*>(user);
        const size_t k_limit = 3;
        size_t d = edit_distance(h->wanted, name, k_limit);
        if (d < h->best) {
            h->best = d;
            strncpy(h->name, name, sizeof(h->name) - 1);
            h->name[sizeof(h->name) - 1] = 0;
        }
    }

    Expr* function_call() {
        Expr* e = node(e_function);
        if (!e) return 0;
        const char* s = sql_ + tok_.start;
        size_t n = tok_.end - tok_.start;
        Function f = find_function(s, n);
        if (f == fn_none && names_equal(s, n, "POSITION")) f = fn_INSTR;  // POSITION(x IN y), handled below
        if (f == fn_none && followed_by_over()) return unsupported(VDB_F("window functions (OVER)"));
        if (f == fn_none) {
            const int k_shown = 32;
            const char* name = pool_.copy(s, n);
            if (!name) return memory();
            status_.fail(k_error_name, VDB_F("no such function: %.*s"), static_cast<int>(n) < k_shown ? static_cast<int>(n) : k_shown, s);
            Hint h;
            h.wanted = name;
            h.best = 3;
            h.name[0] = 0;
            each_function(hint_visit, &h);
            if (h.name[0]) status_.add(VDB_F(" (did you mean %s?)"), h.name);
            return 0;
        }
        advance();
        advance();  // (
        e->function = static_cast<uint16_t>(f);
        FunctionInfo info = function_info(f);
        int count = 0;
        Expr** link = &e->args;
        if (f == fn_COUNT && at_op(o_star)) {
            Expr* star = node(e_star);
            if (!star) return 0;
            advance();
            e->args = star;
            count = 1;
        } else if (f == fn_SUBSTRING || f == fn_SUBSTR) {
            if (!(e->args = expr())) return 0;
            count = 1;
            link = &e->args->next;
            if (accept_kw(kw_FROM)) {
                if (!(*link = expr())) return 0;
                link = &(*link)->next;
                ++count;
                if (accept_kw(kw_FOR)) {
                    if (!(*link = expr())) return 0;
                    ++count;
                }
            } else {
                while (accept_op(o_comma)) {
                    if (!(*link = expr())) return 0;
                    link = &(*link)->next;
                    ++count;
                }
            }
        } else if (f == fn_TRIM && (at_kw(kw_BOTH) || at_kw(kw_LEADING) || at_kw(kw_TRAILING) || peek_from())) {
            Function which = fn_TRIM;
            if (accept_kw(kw_LEADING)) which = fn_LTRIM;
            else if (accept_kw(kw_TRAILING)) which = fn_RTRIM;
            else accept_kw(kw_BOTH);
            Expr* chars = 0;
            if (!at_kw(kw_FROM) && !(chars = concat())) return 0;
            if (accept_kw(kw_FROM)) {
                if (!(e->args = expr())) return 0;
                e->args->next = chars;
            } else {
                e->args = chars;
            }
            count = chars && e->args != chars ? 2 : 1;
            f = which;
            e->function = static_cast<uint16_t>(f);
        } else if (!at_op(o_rparen)) {
            if (accept_kw(kw_DISTINCT)) e->distinct = 1;
            else accept_kw(kw_ALL);
            Expr* first = 0;
            if (n == 8 && names_equal(s, n, "POSITION")) {
                if (!(first = concat()) || !expect_kw(kw_IN, VDB_F("expected IN in POSITION(x IN y)"))) return 0;
                Expr* haystack = concat();
                if (!haystack) return 0;
                e->args = haystack;
                haystack->next = first;
                count = 2;
                e->function = fn_INSTR;
                f = fn_INSTR;
            } else {
                do {
                    Expr* arg = expr();
                    if (!arg) return 0;
                    *link = arg;
                    link = &arg->next;
                    ++count;
                } while (accept_op(o_comma));
            }
        }
        if (!expect_op(o_rparen, VDB_F("expected ')' after the arguments"))) return 0;
        e->end = last_end_;
        if (at_kw(kw_OVER)) return unsupported(VDB_F("window functions (OVER)"));
        if (count < info.min_args || (info.max_args >= 0 && count > info.max_args)) {
            char name[24];
            status_.fail(k_error_syntax, VDB_F("wrong number of arguments to %s (%d given)"),
                         function_name(f, name, sizeof(name)), count);
            return 0;
        }
        if (info.flags & fn_aggregate) {
            if ((f == fn_MIN || f == fn_MAX) && count >= 2) {
                e->function = f == fn_MIN ? fn_LEAST : fn_GREATEST;
            } else {
                e->kind = e_aggregate;
                if (f == fn_COUNT && count == 0) return error(VDB_F("COUNT needs an argument: COUNT(*) or COUNT(x)"));
            }
        }
        return e;
    }

    // name ( ... ) OVER: skips the balanced arguments after the current name.
    bool followed_by_over() {
        Token t;
        lex(tok_.end, t);
        if (!(t.kind == t_op && t.op == o_lparen)) return false;
        int depth = 0;
        for (size_t at = t.start;;) {
            lex(at, t);
            if (t.kind == t_end) return false;
            if (t.kind == t_op && t.op == o_lparen) ++depth;
            if (t.kind == t_op && t.op == o_rparen && --depth == 0) {
                lex(t.end, t);
                return t.kind == t_name && t.keyword == kw_OVER;
            }
            at = t.end;
        }
    }

    bool peek_from() {
        // TRIM('x' FROM y) without BOTH/LEADING/TRAILING: look for FROM before the closing parenthesis.
        size_t at = tok_.start;
        int depth = 0;
        Token t;
        for (;;) {
            lex(at, t);
            if (t.kind == t_end) return false;
            if (t.kind == t_op && t.op == o_lparen) ++depth;
            if (t.kind == t_op && t.op == o_rparen) {
                if (depth == 0) return false;
                --depth;
            }
            if (depth == 0 && t.kind == t_name && t.keyword == kw_FROM) return true;
            at = t.end;
        }
    }

    Pool& pool_;
    const char* sql_;
    size_t n_;
    Status& status_;
    int depth_;
    long next_param_;
    long max_param_;
    uint32_t last_end_;
    Token tok_;
};

}  // namespace

Statement* parse_script(Pool& pool, const char* sql, size_t length, Status& status, size_t& parameters) {
    parameters = 0;
    Parser parser(pool, sql, length, status);
    if (!status.ok()) return 0;
    Statement* s = parser.script(parameters);
    return status.ok() ? s : 0;
}

const char* default_alias(Pool& pool, const char* name) {
    const char* start = name;
    for (const char* p = name; *p; ++p) {
        if (*p == '/' || *p == '\\') start = p + 1;
    }
    const char* end = start + strlen(start);
    for (const char* p = end; p > start; --p) {
        if (p[-1] == '.') {
            if (p - 1 > start) end = p - 1;
            break;
        }
    }
    return pool.copy(start, static_cast<size_t>(end - start));
}

}  // namespace vdb
