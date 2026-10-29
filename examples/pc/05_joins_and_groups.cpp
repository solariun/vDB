// 05: joins, groups and set operations.
#include <stdio.h>

#include <vdb.h>

static void table(vdb::Database& db, const char* sql) {
    printf("%s\n", sql);
    vdb::Result r = db.query(sql);
    vdb::FormatOptions options;
    options.format = vdb::k_format_table;
    options.null_text = "NULL";
    struct Stdout : vdb::Output {
        void write(const char* text, size_t length) override { fwrite(text, 1, length, stdout); }
    } out;
    if (!r.ok()) printf("error: %s\n", r.error());
    else r.write(out, options);
    printf("\n");
}

int main() {
    vdb::Database db;
    db.exec("CREATE TABLE customers (id, name);"
            "INSERT INTO customers VALUES (1,'Ana'),(2,'Davi'),(3,'Eva'),(4,'Zoe');"
            "CREATE TABLE orders (id, customer_id, product, total);"
            "INSERT INTO orders VALUES (100,1,'antenna',120.5),(101,1,'cable',36),(102,2,'radio',899.9),"
            "(103,3,'antenna',241),(105,9,'tuner',210)");
    table(db, "SELECT c.name, o.product FROM customers c JOIN orders o ON o.customer_id = c.id ORDER BY c.name, o.product");
    table(db, "SELECT c.name, o.product FROM customers c LEFT JOIN orders o ON o.customer_id = c.id ORDER BY c.name");
    table(db, "SELECT c.name, o.id FROM customers c FULL JOIN orders o ON o.customer_id = c.id ORDER BY o.id");
    table(db, "SELECT c.name, COUNT(o.id) AS orders, SUM(o.total) AS spent FROM customers c "
              "LEFT JOIN orders o ON o.customer_id = c.id GROUP BY c.name HAVING COUNT(o.id) > 0 ORDER BY spent DESC");
    table(db, "SELECT product FROM orders WHERE total > 200 UNION SELECT product FROM orders WHERE product LIKE 'a%'");
    table(db, "WITH big AS (SELECT * FROM orders WHERE total > 100) "
              "SELECT product, COUNT(*) n, ROUND(AVG(total), 2) avg FROM big GROUP BY product ORDER BY n DESC, product");
    return 0;
}
