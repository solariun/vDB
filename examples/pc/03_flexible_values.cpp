// 03: flexible values. Everything is kept as text; the operation decides how to read it (spec 2.3).
#include <stdio.h>

#include <vdb.h>

static void ask(vdb::Database& db, const char* what, const char* sql) {
    vdb::Result r = db.query(sql);
    printf("%-44s ->", what);
    for (size_t i = 0; i < r.size(); ++i) printf(" %s", r[i][0].text());
    if (!r.ok()) printf(" error: %s", r.error());
    printf("\n");
}

int main() {
    vdb::Database db;
    db.exec("CREATE TABLE t (name, code); "
            "INSERT INTO t VALUES ('a', '017'), ('b', '17'), ('c', 'N/A'), ('d', '170'), ('e', NULL)");
    printf("codes: a='017' b='17' c='N/A' d='170' e=NULL\n\n");
    ask(db, "code = 17          (a number: math)", "SELECT name FROM t WHERE code = 17 ORDER BY name");
    ask(db, "code = '017'       (quoted: text)", "SELECT name FROM t WHERE code = '017'");
    ask(db, "code > 20          (N/A never matches math)", "SELECT name FROM t WHERE code > 20");
    ask(db, "NOT (code > 20)    (nor its NOT)", "SELECT name FROM t WHERE NOT (code > 20) ORDER BY name");
    ask(db, "code LIKE '%17%'   (digits as text)", "SELECT name FROM t WHERE code LIKE '%17%' ORDER BY name");
    ask(db, "code IS NULL", "SELECT name FROM t WHERE code IS NULL");
    ask(db, "IS_NUMBER(code) = 0", "SELECT name FROM t WHERE IS_NUMBER(code) = 0 ORDER BY name");
    ask(db, "ORDER BY code      (NULL, numbers, text)", "SELECT name FROM t ORDER BY code, name");
    ask(db, "SUM(code)          (N/A is skipped)", "SELECT SUM(code) FROM t");
    ask(db, "code * 2", "SELECT code * 2 FROM t ORDER BY name");
    return 0;
}
