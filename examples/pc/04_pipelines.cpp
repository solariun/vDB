// 04: pipelines. The result of one SELECT flows into the next with |>; _ is the previous result.
#include <stdio.h>

#include <vdb.h>

static void print(const char* title, vdb::Result r) {
    printf("%s\n", title);
    if (!r.ok()) printf("  error: %s\n", r.error());
    for (size_t i = 0; i < r.size(); ++i) {
        printf("  ");
        for (size_t c = 0; c < r.columns(); ++c) printf("%s%s=%s", c ? ", " : "", r.column_name(c), r[i][c].text());
        printf("\n");
    }
}

int main() {
    vdb::Database db;
    db.exec("CREATE TABLE customers (id, name, country);"
            "INSERT INTO customers VALUES (1,'Ana','BR'),(2,'Davi','BR'),(3,'Eva','PT'),(4,'Zoe','US');"
            "CREATE TABLE orders (id, customer_id, total);"
            "INSERT INTO orders VALUES (100,1,120.5),(101,1,36),(102,2,899.9),(103,3,241),(104,3,150)");

    // A relation without JOIN: the ids of big orders, then the customers with those ids.
    print("customers with an order above 200:",
          db.query("SELECT customer_id FROM orders WHERE total > 200"
                   " |> SELECT name, country FROM customers WHERE id IN _"
                   " |> ORDER BY name"));

    // Stages may be just a clause.
    print("top 2 orders:", db.query("SELECT * FROM orders |> ORDER BY total DESC |> LIMIT 2"));

    // Group, then filter the groups, then shape the output.
    print("spend per customer, above 100:",
          db.query("SELECT customer_id, SUM(total) AS spent FROM orders GROUP BY customer_id"
                   " |> WHERE spent > 100"
                   " |> SELECT c.name, p.spent FROM _ AS p JOIN customers c ON c.id = p.customer_id"
                   " |> ORDER BY spent DESC"));

    // The same pipeline with the word chain: then() ends a stage.
    print("word chain:", db.select("customer_id").from("orders").where.field("total").gt(200)
                             .then().select("name").from("customers").where.field("id").in_table("_")
                             .run());

    // Keep a step under a name for later statements.
    db.exec("CREATE TABLE vip AS SELECT customer_id FROM orders WHERE total > 200");
    print("vip names:", db.query("SELECT name FROM customers WHERE id IN vip"));
    return 0;
}
