#include "builder.hpp"

namespace vdb {

void write_literal(Text& out, const char* text) {
    if (!text) {
        out.append("NULL");
        return;
    }
    out.append_char('\'');
    for (const char* p = text; *p; ++p) {
        if (*p == '\'') out.append_char('\'');
        out.append_char(*p);
    }
    out.append_char('\'');
}

void write_literal(Text& out, long long value) { out.append_int(static_cast<int64_t>(value)); }

void write_literal(Text& out, unsigned long long value) {
    if (value > static_cast<unsigned long long>(INT64_MAX)) {
        out.append_real(static_cast<double>(value), '.', k_real_exact_digits);
        return;
    }
    out.append_int(static_cast<int64_t>(value));
}

void write_literal(Text& out, double value) {
    if (!finite(value)) {
        out.append("NULL");
        return;
    }
    if (value < 0) out.append_char('(');
    out.append_real(value, '.', k_real_exact_digits);
    if (value < 0) out.append_char(')');
}

void write_literal(Text& out, bool value) { out.append(value ? "TRUE" : "FALSE"); }

void write_literal(Text& out, const Column& value) {
    if (value.qualifier) {
        write_identifier(out, value.qualifier);
        out.append_char('.');
    }
    write_identifier(out, value.name);
}

void write_literal(Text& out, const Raw& value) { out.append(value.sql ? value.sql : "NULL"); }

void write_identifier(Text& out, const char* name) {
    out.append_char('"');
    for (const char* p = name ? name : ""; *p; ++p) {
        if (*p == '"') out.append_char('"');
        out.append_char(*p);
    }
    out.append_char('"');
}

void ChainBase::fail(const char* flash_message) {
    if (error_.ok()) error_.fail(k_error_misuse, VDB_F("word chain: %s"), "").add(flash_message);
}

namespace {
template <class Owner>
bool condition_ok(ChainBase& chain, const Condition<Owner>& c, const char* flash_name) {
    if (c.empty() || c.complete()) return true;
    chain.fail(flash_name);
    return false;
}
}  // namespace

// ── Select ───────────────────────────────────────────────────────────────────

Select::Select(Database& db, const char* columns)
    : ChainBase(db),
      where(this, where),
      having(this, where),
      on(this, where),
      done_(db.allocator()),
      columns_(db.allocator()),
      from_(db.allocator()),
      joins_(db.allocator()),
      group_(db.allocator()),
      order_(db.allocator()),
      sql_(db.allocator()),
      limit_(-1),
      offset_(-1),
      distinct_(false),
      has_columns_(true),
      pending_on_(false),
      stages_(0) {
    columns_.append(columns && *columns ? columns : "*");
}

Select::Select(Select&& other)
    : ChainBase(other),
      where(this, where, static_cast<Condition<Select>&&>(other.where)),
      having(this, where, static_cast<Condition<Select>&&>(other.having)),
      on(this, where, static_cast<Condition<Select>&&>(other.on)),
      done_(static_cast<Text&&>(other.done_)),
      columns_(static_cast<Text&&>(other.columns_)),
      from_(static_cast<Text&&>(other.from_)),
      joins_(static_cast<Text&&>(other.joins_)),
      group_(static_cast<Text&&>(other.group_)),
      order_(static_cast<Text&&>(other.order_)),
      sql_(static_cast<Text&&>(other.sql_)),
      limit_(other.limit_),
      offset_(other.offset_),
      distinct_(other.distinct_),
      has_columns_(other.has_columns_),
      pending_on_(other.pending_on_),
      stages_(other.stages_) {}

namespace {
void copy_text(Text& to, const Text& from) { to.append(from.c_str(), from.size()); }
}  // namespace

Select::Select(const Select& other)
    : ChainBase(other),
      where(this, where, other.where),
      having(this, where, other.having),
      on(this, where, other.on),
      done_(other.allocator()),
      columns_(other.allocator()),
      from_(other.allocator()),
      joins_(other.allocator()),
      group_(other.allocator()),
      order_(other.allocator()),
      sql_(other.allocator()),
      limit_(other.limit_),
      offset_(other.offset_),
      distinct_(other.distinct_),
      has_columns_(other.has_columns_),
      pending_on_(other.pending_on_),
      stages_(other.stages_) {
    copy_text(done_, other.done_);
    copy_text(columns_, other.columns_);
    copy_text(from_, other.from_);
    copy_text(joins_, other.joins_);
    copy_text(group_, other.group_);
    copy_text(order_, other.order_);
}

Select& Select::select(const char* columns) {
    columns_.clear();
    columns_.append(columns && *columns ? columns : "*");
    has_columns_ = true;
    return *this;
}

Select& Select::distinct() {
    distinct_ = true;
    return *this;
}

Select& Select::from(const char* table, const char* alias) {
    from_.clear();
    write_identifier(from_, table);
    if (alias) {
        from_.append(" AS ");
        write_identifier(from_, alias);
    }
    return *this;
}

void Select::flush_on() {
    if (!pending_on_) return;
    if (!on.empty()) {
        if (!condition_ok(*this, on, VDB_F("the ON condition is not complete"))) return;
        joins_.append(" ON ");
        joins_.append(on.text().c_str(), on.text().size());
    }
    on.clear();
    pending_on_ = false;
}

Select& Select::add_join(const char* kind, const char* table, const char* alias) {
    flush_on();
    joins_.append(kind);
    write_identifier(joins_, table);
    if (alias) {
        joins_.append(" AS ");
        write_identifier(joins_, alias);
    }
    pending_on_ = true;
    return *this;
}

Select& Select::group_by(const char* columns) {
    if (!group_.empty()) group_.append(", ");
    group_.append(columns);
    return *this;
}

Select& Select::order_by(const char* column, Direction direction) {
    if (!order_.empty()) order_.append(", ");
    order_.append(column);
    if (direction == DESC) order_.append(" DESC");
    return *this;
}

Select& Select::limit(long long n) {
    limit_ = n;
    return *this;
}

Select& Select::offset(long long n) {
    offset_ = n;
    return *this;
}

void Select::compose_stage(Text& out) {
    flush_on();
    bool first_clause = true;
    if (has_columns_) {
        out.append("SELECT ");
        if (distinct_) out.append("DISTINCT ");
        out.append(columns_.c_str(), columns_.size());
        first_clause = false;
    }
    if (!from_.empty()) {
        out.append(first_clause ? "FROM " : " FROM ");
        out.append(from_.c_str(), from_.size());
        out.append(joins_.c_str(), joins_.size());
        first_clause = false;
    }
    if (!where.empty() && condition_ok(*this, where, VDB_F("the WHERE condition is not complete"))) {
        out.append(first_clause ? "WHERE " : " WHERE ");
        out.append(where.text().c_str(), where.text().size());
        first_clause = false;
    }
    if (!group_.empty()) {
        out.append(first_clause ? "GROUP BY " : " GROUP BY ");
        out.append(group_.c_str(), group_.size());
        first_clause = false;
    }
    if (!having.empty() && condition_ok(*this, having, VDB_F("the HAVING condition is not complete"))) {
        out.append(first_clause ? "HAVING " : " HAVING ");
        out.append(having.text().c_str(), having.text().size());
        first_clause = false;
    }
    if (!order_.empty()) {
        out.append(first_clause ? "ORDER BY " : " ORDER BY ");
        out.append(order_.c_str(), order_.size());
        first_clause = false;
    }
    if (limit_ >= 0) {
        out.append(first_clause ? "LIMIT " : " LIMIT ");
        out.append_int(limit_);
        first_clause = false;
    }
    if (offset_ >= 0) {
        out.append(first_clause ? "OFFSET " : " OFFSET ");
        out.append_int(offset_);
    }
}

Select& Select::then() {
    if (stages_ > 0 || !done_.empty()) done_.append(" |> ");
    compose_stage(done_);
    ++stages_;
    columns_.clear();
    from_.clear();
    joins_.clear();
    group_.clear();
    order_.clear();
    where.clear();
    having.clear();
    on.clear();
    limit_ = offset_ = -1;
    distinct_ = false;
    has_columns_ = false;
    pending_on_ = false;
    return *this;
}

const char* Select::sql() {
    sql_.clear();
    sql_.append(done_.c_str(), done_.size());
    if (stages_ > 0) sql_.append(" |> ");
    compose_stage(sql_);
    if (!sql_.ok()) fail(VDB_F("out of memory"));
    return sql_.c_str();
}

Result Select::run() {
    const char* text = sql();
    if (!error_.ok()) return Result(error_);
    return db_->query(text);
}

Status Select::run(RowSink& sink) {
    const char* text = sql();
    if (!error_.ok()) return error_;
    return db_->run(text, sink);
}

// ── Update ───────────────────────────────────────────────────────────────────

Update::Update(Database& db, const char* table)
    : ChainBase(db), where(this, where), table_(db.allocator()), sets_(db.allocator()), sql_(db.allocator()) {
    write_identifier(table_, table);
}

Update::Update(Update&& other)
    : ChainBase(other),
      where(this, where, static_cast<Condition<Update>&&>(other.where)),
      table_(static_cast<Text&&>(other.table_)),
      sets_(static_cast<Text&&>(other.sets_)),
      sql_(static_cast<Text&&>(other.sql_)) {}

Update::Update(const Update& other)
    : ChainBase(other), where(this, where, other.where), table_(other.allocator()), sets_(other.allocator()), sql_(other.allocator()) {
    copy_text(table_, other.table_);
    copy_text(sets_, other.sets_);
}

const char* Update::sql() {
    sql_.clear();
    sql_.append("UPDATE ");
    sql_.append(table_.c_str(), table_.size());
    sql_.append(" SET ");
    sql_.append(sets_.c_str(), sets_.size());
    if (sets_.empty()) fail(VDB_F("update needs set(name, value)"));
    if (!where.empty() && condition_ok(*this, where, VDB_F("the WHERE condition is not complete"))) {
        sql_.append(" WHERE ");
        sql_.append(where.text().c_str(), where.text().size());
    }
    return sql_.c_str();
}

Result Update::run() {
    const char* text = sql();
    if (!error_.ok()) return Result(error_);
    return db_->query(text);
}

// ── Delete ───────────────────────────────────────────────────────────────────

Delete::Delete(Database& db, const char* table) : ChainBase(db), where(this, where), table_(db.allocator()), sql_(db.allocator()) {
    write_identifier(table_, table);
}

Delete::Delete(Delete&& other)
    : ChainBase(other),
      where(this, where, static_cast<Condition<Delete>&&>(other.where)),
      table_(static_cast<Text&&>(other.table_)),
      sql_(static_cast<Text&&>(other.sql_)) {}

Delete::Delete(const Delete& other)
    : ChainBase(other), where(this, where, other.where), table_(other.allocator()), sql_(other.allocator()) {
    copy_text(table_, other.table_);
}

const char* Delete::sql() {
    sql_.clear();
    sql_.append("DELETE FROM ");
    sql_.append(table_.c_str(), table_.size());
    if (!where.empty() && condition_ok(*this, where, VDB_F("the WHERE condition is not complete"))) {
        sql_.append(" WHERE ");
        sql_.append(where.text().c_str(), where.text().size());
    }
    return sql_.c_str();
}

Result Delete::run() {
    const char* text = sql();
    if (!error_.ok()) return Result(error_);
    return db_->query(text);
}

// ── Insert ───────────────────────────────────────────────────────────────────

Insert::Insert(Database& db, const char* table)
    : ChainBase(db), table_(db.allocator()), names_(db.allocator()), values_(db.allocator()), done_(db.allocator()), sql_(db.allocator()) {
    write_identifier(table_, table);
}

Insert::Insert(Insert&& other)
    : ChainBase(other),
      table_(static_cast<Text&&>(other.table_)),
      names_(static_cast<Text&&>(other.names_)),
      values_(static_cast<Text&&>(other.values_)),
      done_(static_cast<Text&&>(other.done_)),
      sql_(static_cast<Text&&>(other.sql_)) {}

Insert::Insert(const Insert& other)
    : ChainBase(other),
      table_(other.allocator()),
      names_(other.allocator()),
      values_(other.allocator()),
      done_(other.allocator()),
      sql_(other.allocator()) {
    copy_text(table_, other.table_);
    copy_text(names_, other.names_);
    copy_text(values_, other.values_);
    copy_text(done_, other.done_);
}

Insert& Insert::row() {
    if (names_.empty()) return *this;
    done_.append("INSERT INTO ");
    done_.append(table_.c_str(), table_.size());
    done_.append(" (");
    done_.append(names_.c_str(), names_.size());
    done_.append(") VALUES (");
    done_.append(values_.c_str(), values_.size());
    done_.append(");\n");
    names_.clear();
    values_.clear();
    return *this;
}

const char* Insert::sql() {
    row();
    sql_.clear();
    sql_.append(done_.c_str(), done_.size());
    if (sql_.empty()) fail(VDB_F("insert needs value(name, v)"));
    if (!sql_.ok()) fail(VDB_F("out of memory"));
    return sql_.c_str();
}

Result Insert::run() {
    const char* text = sql();
    if (!error_.ok()) return Result(error_);
    return db_->query(text);
}

// ── Database entry points ────────────────────────────────────────────────────

Select Database::select(const char* columns) { return Select(*this, columns); }
Insert Database::insert(const char* table) { return Insert(*this, table); }
Update Database::update(const char* table) { return Update(*this, table); }
Delete Database::delete_from(const char* table) { return Delete(*this, table); }

}  // namespace vdb
