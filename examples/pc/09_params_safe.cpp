// 09: user data in SQL text, safely: ? placeholders and Params (never paste user text into SQL).
#include <stdio.h>

#include <vdb.h>

int main() {
    vdb::Database db;
    db.exec("CREATE TABLE users (name, password_hint)");
    db.exec("INSERT INTO users VALUES ('ana', 'cat'), ('bruno', 'dog')");

    const char* typed = "nobody' OR '1'='1";  // a hostile input
    vdb::Result r = db.query("SELECT name FROM users WHERE name = ?", vdb::Params().add(typed));
    printf("rows for the hostile input: %zu\n", r.size());  // 0: it is only a value

    r = db.query("SELECT name FROM users WHERE name = ?1 OR name = ?2 ORDER BY name", vdb::Params().add("ana").add("bruno"));
    for (size_t i = 0; i < r.size(); ++i) printf("found %s\n", r[i][0].text());

    r = db.query("SELECT name FROM users WHERE name = ?");
    printf("missing value: %s\n", r.error());
    return 0;
}
