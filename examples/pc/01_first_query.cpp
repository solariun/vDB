// 01: the first query. Create a table, put records in, ask with SQL, read the answer.
#include <stdio.h>

#include <vdb.h>

int main() {
    vdb::Database db;  // lives in memory, uses the heap

    db.exec("CREATE TABLE people (name, age, city)");
    db.exec("INSERT INTO people VALUES ('Ana', 31, 'Rio'), ('Bruno', 'N/A', 'Sao Paulo'), ('Carla', 17, 'Recife')");

    vdb::Result r = db.query("SELECT name, age FROM people WHERE age > 18 ORDER BY name");
    if (!r.ok()) {
        printf("error: %s\n", r.error());
        return 1;
    }
    printf("%zu adult(s):\n", r.size());
    for (size_t i = 0; i < r.size(); ++i) {
        printf("  %s is %lld\n", r[i]["name"].text(), static_cast<long long>(r[i]["age"].integer()));
    }
    // Bruno's age is 'N/A': a math comparison only matches numbers, so he is not listed (and not an error).
    return 0;
}
