// 02: the word chain. The same questions without writing SQL text; sql() shows the SQL it writes.
#include <stdio.h>

#include <vdb.h>

static void print(vdb::Result r) {
    if (!r.ok()) {
        printf("  error: %s\n", r.error());
        return;
    }
    for (size_t i = 0; i < r.size(); ++i) {
        printf("  ");
        for (size_t c = 0; c < r.columns(); ++c) printf("%s%s", c ? " | " : "", r[i][c].text());
        printf("\n");
    }
}

int main() {
    vdb::Database db;
    db.insert("people").value("name", "Ana").value("age", 31).value("city", "Rio")
        .row().value("name", "Davi").value("age", 45).value("city", "Rio")
        .row().value("name", "Eva").value("age", 22).value("city", "Porto Alegre")
        .run();

    // where is a member: .where.field(...). AND binds before OR; open()/close() group.
    vdb::Select q = db.select("name, age")
                        .from("people")
                        .where.field("age").gt(30)
                        .and_().field("name").like("A%")
                        .order_by("age", vdb::DESC)
                        .limit(10);
    printf("SQL: %s\n", q.sql());
    print(q.run());

    // Values are always escaped: this name cannot break the query.
    const char* typed_by_a_user = "x' OR '1'='1";
    printf("SQL: %s\n", db.select().from("people").where.field("name").eq(typed_by_a_user).sql());
    vdb::Result none = db.select().from("people").where.field("name").eq(typed_by_a_user).run();
    printf("  %zu rows: the input stayed a value\n", none.size());

    // Grouping with open() and close(), IN, and an UPDATE and a DELETE.
    printf("(city = Rio OR age < 25) AND name IN (Ana, Eva):\n");
    print(db.select("name").from("people")
              .where.open().field("city").eq("Rio").or_().field("age").lt(25).close()
              .and_().field("name").in("Ana", "Eva")
              .run());
    db.update("people").set("age", 46).where.field("name").eq("Davi").run();
    db.delete_from("people").where.field("age").lt(18).run();
    printf("after the update and the delete:\n");
    print(db.select("name, age").from("people").order_by("name").run());
    return 0;
}
