// SQL: values, operators, functions, aggregates, ordering, joins, subqueries, sets, pipelines, DML, errors
// (spec 2.3 – 3.12, D2, D3, D4, A8 – A16).
#include "test_harness.hpp"

namespace {

void people(vdb::Database& db) {
    db.exec("CREATE TABLE people (name, age, city);"
            "INSERT INTO people VALUES ('Ana', 31, 'Rio'), ('Bo', 'N/A', NULL), ('Cid', '017', 'Recife'),"
            "('Dee', 45, 'Rio'), ('Eve', 22, 'Recife')");
}

void shop(vdb::Database& db) {
    db.exec("CREATE TABLE customers (id, name);"
            "INSERT INTO customers VALUES (1, 'Ana'), (2, 'Davi'), (3, 'Eva'), (4, 'Zoe');"
            "CREATE TABLE orders (id, customer_id, total);"
            "INSERT INTO orders VALUES (100, 1, 120.5), (101, 1, 36), (102, 2, 899.9), (103, 3, 241), (105, 9, 210)");
}

}  // namespace

// ── values and comparisons (D3, A3) ──────────────────────────────────────────

TEST(sql_math_comparison_reads_numbers_and_skips_text) {
    vdb::Database db;
    people(db);
    CHECK_ROWS(db, "SELECT name FROM people WHERE age > 20 ORDER BY name", "Ana;Dee;Eve");
    CHECK_ROWS(db, "SELECT name FROM people WHERE NOT (age > 20) ORDER BY name", "Cid");
    CHECK_ROWS(db, "SELECT name FROM people WHERE age = 17", "Cid");
    CHECK_ROWS(db, "SELECT name FROM people WHERE age <> 17 ORDER BY name", "Ana;Dee;Eve");
    CHECK_ROWS(db, "SELECT name FROM people WHERE age BETWEEN 20 AND 40 ORDER BY name", "Ana;Eve");
    CHECK_ROWS(db, "SELECT name FROM people WHERE age NOT BETWEEN 20 AND 40 ORDER BY name", "Cid;Dee");
}

TEST(sql_quoted_literal_compares_as_text) {
    vdb::Database db;
    people(db);
    CHECK_ROWS(db, "SELECT name FROM people WHERE age = '017'", "Cid");
    CHECK_ROWS(db, "SELECT name FROM people WHERE age = '17'", "");
    CHECK_ROWS(db, "SELECT name FROM people WHERE age = 'N/A'", "Bo");
    CHECK_ROWS(db, "SELECT name FROM people WHERE age > '3' ORDER BY name", "Ana;Bo;Dee");  // text order: '31' > '3' 
    CHECK_ROWS(db, "SELECT name FROM people WHERE age LIKE '%1%' ORDER BY name", "Ana;Cid");
}

TEST(sql_columns_compare_as_numbers_when_both_read) {
    vdb::Database db;
    db.exec("CREATE TABLE t (a, b); INSERT INTO t VALUES ('10', '9'), ('010', '10'), ('x', 'y'), ('2', 'two')");
    CHECK_ROWS(db, "SELECT a FROM t WHERE a > b ORDER BY a", "10");
    CHECK_ROWS(db, "SELECT a FROM t WHERE a = b", "010");
    CHECK_ROWS(db, "SELECT a FROM t WHERE a < b ORDER BY a", "2;x");  // text: '2' < 'two', 'x' < 'y'
}

TEST(sql_null_logic_is_three_valued) {
    vdb::Database db;
    CHECK_ROWS(db, "SELECT NULL = NULL, NULL IS NULL, 1 IS NOT NULL, NULL AND FALSE, NULL OR TRUE, NOT NULL", "NULL|1|1|0|1|NULL");
    CHECK_ROWS(db, "SELECT NULL IS DISTINCT FROM NULL, 1 IS DISTINCT FROM NULL, 2 IS NOT DISTINCT FROM 2", "0|1|1");
    CHECK_ROWS(db, "SELECT 1 IN (1, NULL), 2 IN (1, NULL), 2 NOT IN (1, NULL), NULL IN (1)", "1|NULL|NULL|NULL");
    CHECK_ROWS(db, "SELECT 'yes' = TRUE, 'off' = FALSE, 'abc' IS TRUE, 0 IS FALSE", "1|1|0|1");
}

TEST(sql_math_operators) {
    vdb::Database db;
    CHECK_ROWS(db, "SELECT 1 + 2 * 3, (1 + 2) * 3, 7 / 2, 6 / 2, 7 % 3, -7 % 3, 2 - -3", "7|9|3.5|3|1|-1|5");
    CHECK_ROWS(db, "SELECT 1 / 0, 5 % 0, 'x' + 1, '3' * '4'", "NULL|NULL|NULL|12");
    CHECK_ROWS(db, "SELECT 9223372036854775807 + 1", "9.22337203685478e+18");
    CHECK_ROWS(db, "SELECT 0.1 + 0.2, 1e3, 2.50", "0.3|1000|2.5");
    CHECK_ROWS(db, "SELECT 'a' || 'b' || 1 || NULL, 'n=' || 2.5", "NULL|n=2.5");
}

TEST(sql_text_matching) {
    vdb::Database db;
    db.exec("CREATE TABLE f (name); INSERT INTO f VALUES ('report.csv'), ('Report.CSV'), ('data_1.txt'), ('ação'), ('50%off')");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name LIKE 'r%' ORDER BY name", "report.csv");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name ILIKE 'r%' ORDER BY name", "Report.CSV;report.csv");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name LIKE 'data_1%'", "data_1.txt");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name LIKE 'data\\_%' ESCAPE '\\'", "data_1.txt");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name LIKE '%!%%' ESCAPE '!'", "50%off");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name LIKE 'a__o'", "ação");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name ILIKE 'AÇÃO'", "ação");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name GLOB '*.csv'", "report.csv");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name GLOB '[A-Z]*'", "Report.CSV");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name GLOB 'data_?.*'", "data_1.txt");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name SIMILAR TO '(report|data)%' ORDER BY name", "data_1.txt;report.csv");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name REGEXP '^[rR]epo.*\\.(csv|CSV)$' ORDER BY name", "Report.CSV;report.csv");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name RLIKE '\\d'", "data_1.txt;50%off");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name ~ 'csv'", "report.csv");
    CHECK_ROWS(db, "SELECT name FROM f WHERE name ~* 'csv' ORDER BY name", "Report.CSV;report.csv");
    CHECK_ROWS(db, "SELECT COUNT(*) FROM f WHERE name !~ 'csv'", "4");
    CHECK_ROWS(db, "SELECT COUNT(*) FROM f WHERE name NOT LIKE '%.%'", "2");
    CHECK_ROWS(db, "SELECT COUNT(*) FROM f WHERE name NOT REGEXP 'a'", "3");
}

TEST(sql_text_functions) {
    vdb::Database db;
    CHECK_ROWS(db, "SELECT LENGTH('ação'), OCTET_LENGTH('ação'), UPPER('ação'), LOWER('ÁÉ'), INITCAP('hello wORLD')",
               "4|6|AÇÃO|áé|Hello World");
    CHECK_ROWS(db, "SELECT TRIM('  x  '), LTRIM('xxa', 'x'), RTRIM('a..', '.'), TRIM(BOTH '-' FROM '--a--'), TRIM(LEADING '0' FROM '007')",
               "x|a|a|a|7");
    CHECK_ROWS(db, "SELECT SUBSTR('hello', 2, 3), SUBSTR('hello', -3), SUBSTRING('hello' FROM 2 FOR 2), SUBSTR('ação', 2, 2), SUBSTR('abc', 0, 2)",
               "ell|llo|el|çã|a");
    CHECK_ROWS(db, "SELECT LEFT('hello', 2), RIGHT('hello', 2), LEFT('ab', 5), RIGHT('hello', -2)", "he|lo|ab|llo");
    CHECK_ROWS(db, "SELECT REPLACE('a-b-c', '-', '+'), INSTR('hello', 'l'), POSITION('lo' IN 'hello'), INSTR('a', 'z')", "a+b+c|3|4|0");
    CHECK_ROWS(db, "SELECT LPAD('7', 3, '0'), RPAD('ab', 5, 'xy'), LPAD('hello', 2), REVERSE('ação'), REPEAT('ab', 3)",
               "007|abxyx|he|oãça|ababab");
    CHECK_ROWS(db, "SELECT CONCAT('a', NULL, 1), CONCAT_WS('-', 'a', NULL, 'b'), SPLIT_PART('a,b,c', ',', 2), SPLIT_PART('a,b,c', ',', -1), SPLIT_PART('a,b', ',', 5)",
               "a1|a-b|b|c|");
    CHECK_ROWS(db, "SELECT ASCII('A'), CHAR(72, 105), CONTAINS('radio', 'dio'), STARTS_WITH('radio', 'ra'), ENDS_WITH('radio', 'x')",
               "65|Hi|1|1|0");
    CHECK_ROWS(db, "SELECT PRINTF('%05.1f|%-4s|%3d|%x|%%|%s', 3.14159, 'ab', 7, 255, NULL)", "003.1|ab  |  7|ff|%|");
}

TEST(sql_regex_functions) {
    vdb::Database db;
    CHECK_ROWS(db, "SELECT REGEXP_LIKE('Radio', '^r', 'i'), REGEXP_LIKE('Radio', '^r')", "1|0");
    CHECK_ROWS(db, "SELECT REGEXP_SUBSTR('tel 555-1234', '\\d+'), REGEXP_SUBSTR('tel 555-1234', '(\\d+)-(\\d+)', 2), REGEXP_SUBSTR('x', '\\d')",
               "555|1234|NULL");
    CHECK_ROWS(db, "SELECT REGEXP_REPLACE('a1b22c333', '\\d+', '#'), REGEXP_REPLACE('a1b22', '\\d', '#', '1')", "a#b#c#|a#b22");
    CHECK_ROWS(db, "SELECT REGEXP_REPLACE('2026-10-29', '(\\d+)-(\\d+)-(\\d+)', '$3/$2/$1'), REGEXP_REPLACE('abc', 'x*', '-')",
               "29/10/2026|-a-b-c-");
    CHECK_ROWS(db, "SELECT REGEXP_COUNT('banana', 'an'), REGEXP_INSTR('banana', 'n+'), REGEXP_EXTRACT('k=v', '(\\w)=(\\w)', 1)", "2|3|k");
    CHECK_ROWS(db, "SELECT REGEXP_LIKE('a', '(')", "ERROR: regex: missing ')'");
    CHECK_ROWS(db, "SELECT REGEXP_LIKE('a', 'a', 'z')", "ERROR: regex flags take only i, m, s, 1 (got 'z')");
}

TEST(sql_math_functions) {
    vdb::Database db;
    CHECK_ROWS(db, "SELECT ABS(-3), ABS(-2.5), SIGN(-9), ROUND(2.5), ROUND(-2.5), ROUND(3.14159, 2), ROUND(1234, -2), TRUNC(-3.7)",
               "3|2.5|-1|3|-3|3.14|1200|-3");
    CHECK_ROWS(db, "SELECT FLOOR(-1.5), CEIL(1.2), MOD(7, 3), POWER(2, 10), SQRT(16), SQRT(-1), LN(0), LOG10(1000), LOG(2, 8)",
               "-2|2|1|1024|4|NULL|NULL|3|3");
    CHECK_ROWS(db, "SELECT ROUND(PI(), 4), ROUND(DEGREES(PI()), 1), ROUND(SIN(0), 1), ROUND(EXP(1), 3), ABS('x')", "3.1416|180|0|2.718|NULL");
}

TEST(sql_null_and_type_functions) {
    vdb::Database db;
    people(db);
    CHECK_ROWS(db, "SELECT COALESCE(NULL, NULL, 3), IFNULL(NULL, 'x'), NULLIF(5, 5), NULLIF(5, 6), IIF(1 > 0, 'y', 'n')", "3|x|NULL|5|y");
    CHECK_ROWS(db, "SELECT GREATEST(3, 10, 7), LEAST('b', 'a'), GREATEST(1, NULL), MAX(1, 5, 2), MIN(4, 2)", "10|a|1|5|2");
    CHECK_ROWS(db, "SELECT TYPEOF(1), TYPEOF(1.5), TYPEOF('a'), TYPEOF(NULL), TYPEOF(age) FROM people WHERE name = 'Ana'",
               "integer|real|text|null|text");
    CHECK_ROWS(db, "SELECT name FROM people WHERE NOT IS_NUMBER(age)", "Bo");
    CHECK_ROWS(db, "SELECT CAST('017' AS INTEGER), CAST('3.9' AS INT), CAST(2 AS TEXT) || 'x', CAST('abc' AS REAL), CAST('1e2' AS NUMERIC)",
               "17|3|2x|NULL|100");
    CHECK_ROWS(db, "SELECT name FROM people WHERE CAST(age AS TEXT) = '017'", "Cid");
}

TEST(sql_case_expressions) {
    vdb::Database db;
    people(db);
    CHECK_ROWS(db, "SELECT name, CASE WHEN age >= 30 THEN 'senior' WHEN age >= 18 THEN 'adult' ELSE '?' END FROM people ORDER BY name",
               "Ana|senior;Bo|?;Cid|?;Dee|senior;Eve|adult");
    CHECK_ROWS(db, "SELECT CASE city WHEN 'Rio' THEN 'RJ' WHEN 'Recife' THEN 'PE' END FROM people ORDER BY name", "RJ;NULL;PE;RJ;PE");
}

// ── aggregates, grouping, ordering ───────────────────────────────────────────

TEST(sql_aggregates) {
    vdb::Database db;
    people(db);
    CHECK_ROWS(db, "SELECT COUNT(*), COUNT(age), COUNT(city), COUNT(DISTINCT city), SUM(age), TOTAL(age), AVG(age), MIN(age), MAX(age) FROM people",
               "5|5|4|2|115|115|28.75|017|N/A");
    CHECK_ROWS(db, "SELECT GROUP_CONCAT(name), STRING_AGG(name, '/'), GROUP_CONCAT(DISTINCT city) FROM people",
               "Ana,Bo,Cid,Dee,Eve|Ana/Bo/Cid/Dee/Eve|Rio,Recife");
    CHECK_ROWS(db, "SELECT MEDIAN(age), ROUND(STDDEV_POP(age), 3), ROUND(VARIANCE(age), 2) FROM people WHERE IS_NUMBER(age)",
               "26.5|10.639|150.92");
    CHECK_ROWS(db, "SELECT COUNT(*), SUM(age), AVG(age), TOTAL(age), MAX(name) FROM people WHERE name = 'nobody'", "0|NULL|NULL|0|NULL");
    CHECK_ROWS(db, "SELECT SUM(x) FROM (SELECT 9223372036854775807 AS x UNION ALL SELECT 1) AS t", "9.22337203685478e+18");
}

TEST(sql_group_by_having_and_bare_columns) {
    vdb::Database db;
    people(db);
    CHECK_ROWS(db, "SELECT city, COUNT(*) n FROM people GROUP BY city ORDER BY city", "NULL|1;Recife|2;Rio|2");
    CHECK_ROWS(db, "SELECT city, COUNT(*) n FROM people GROUP BY city HAVING COUNT(*) > 1 ORDER BY n DESC, city", "Recife|2;Rio|2");
    CHECK_ROWS(db, "SELECT city, COUNT(*) n FROM people GROUP BY city HAVING n > 1 ORDER BY 1", "Recife|2;Rio|2");
    CHECK_ROWS(db, "SELECT city AS c, MAX(age) FROM people GROUP BY c ORDER BY c", "NULL|N/A;Recife|22;Rio|45");
    CHECK_ROWS(db, "SELECT city, name, COUNT(*) FROM people WHERE city = 'Rio' GROUP BY 1", "Rio|Ana|2");  // first row of the group (A15)
    CHECK_ROWS(db, "SELECT COUNT(*) FROM people GROUP BY city HAVING city IS NULL", "1");
    CHECK_ROWS(db, "SELECT city FROM people GROUP BY city HAVING SUM(age) > 50", "Rio");
}

TEST(sql_group_keys_equal_like_numbers) {
    vdb::Database db;
    db.exec("CREATE TABLE t (k); INSERT INTO t VALUES ('10'), ('10.0'), (' 10'), (10), ('ten'), (NULL), (NULL)");
    CHECK_ROWS(db, "SELECT COUNT(*) FROM t GROUP BY k ORDER BY 1", "1;2;4");
    CHECK_ROWS(db, "SELECT COUNT(DISTINCT k) FROM t", "2");
    CHECK_ROWS(db, "SELECT DISTINCT k FROM t ORDER BY k", "NULL;10;ten");
}

TEST(sql_order_by_rules) {
    vdb::Database db;
    db.exec("CREATE TABLE t (v, w); INSERT INTO t VALUES ('b', 2), ('10', 1), ('9', 3), (NULL, 4), ('a', 5), ('-1', 6)");
    CHECK_ROWS(db, "SELECT v FROM t ORDER BY v", "NULL;-1;9;10;a;b");
    CHECK_ROWS(db, "SELECT v FROM t ORDER BY v DESC", "b;a;10;9;-1;NULL");
    CHECK_ROWS(db, "SELECT v FROM t ORDER BY v NULLS LAST", "-1;9;10;a;b;NULL");
    CHECK_ROWS(db, "SELECT v FROM t ORDER BY v DESC NULLS FIRST", "NULL;b;a;10;9;-1");
    CHECK_ROWS(db, "SELECT v AS x FROM t ORDER BY w DESC LIMIT 2", "-1;a");          // a column not selected
    CHECK_ROWS(db, "SELECT v, w * 2 AS d FROM t ORDER BY d DESC LIMIT 1", "-1|12");  // an alias
    CHECK_ROWS(db, "SELECT w FROM t ORDER BY 1 DESC LIMIT 2 OFFSET 1", "5;4");
    CHECK_ROWS(db, "SELECT w FROM t ORDER BY w LIMIT 1, 2", "2;3");
    CHECK_ROWS(db, "SELECT w FROM t ORDER BY w OFFSET 4 ROWS", "5;6");
    CHECK_ROWS(db, "SELECT w FROM t ORDER BY w FETCH FIRST 2 ROWS ONLY", "1;2");
    CHECK_ROWS(db, "SELECT w FROM t ORDER BY w LIMIT -1 OFFSET 5", "6");
}

TEST(sql_order_is_stable) {
    vdb::Database db;
    db.exec("CREATE TABLE t (k, i); INSERT INTO t VALUES (1, 'a'), (2, 'b'), (1, 'c'), (2, 'd'), (1, 'e')");
    CHECK_ROWS(db, "SELECT i FROM t ORDER BY k", "a;c;e;b;d");
}

TEST(sql_output_names) {
    vdb::Database db;
    people(db);
    CHECK_NAMES(db, "SELECT name, age + 1, COUNT(*) AS n, UPPER(city) c FROM people", "name|age + 1|n|c");
    CHECK_NAMES(db, "SELECT name, name, name AS name FROM people", "name|name_2|name_3");
    CHECK_NAMES(db, "SELECT * FROM people", "name|age|city");
}

// ── joins (D4) ───────────────────────────────────────────────────────────────

TEST(sql_joins_of_every_kind) {
    vdb::Database db;
    shop(db);
    CHECK_ROWS(db, "SELECT c.name, o.id FROM customers c JOIN orders o ON o.customer_id = c.id ORDER BY o.id",
               "Ana|100;Ana|101;Davi|102;Eva|103");
    CHECK_ROWS(db, "SELECT c.name, o.id FROM customers c LEFT JOIN orders o ON o.customer_id = c.id ORDER BY c.id, o.id",
               "Ana|100;Ana|101;Davi|102;Eva|103;Zoe|NULL");
    CHECK_ROWS(db, "SELECT c.name, o.id FROM customers c RIGHT JOIN orders o ON o.customer_id = c.id ORDER BY o.id",
               "Ana|100;Ana|101;Davi|102;Eva|103;NULL|105");
    CHECK_ROWS(db, "SELECT c.name, o.id FROM customers c FULL JOIN orders o ON o.customer_id = c.id ORDER BY c.name, o.id",
               "NULL|105;Ana|100;Ana|101;Davi|102;Eva|103;Zoe|NULL");
    CHECK_ROWS(db, "SELECT COUNT(*) FROM customers CROSS JOIN orders", "20");
    CHECK_ROWS(db, "SELECT COUNT(*) FROM customers, orders WHERE orders.customer_id = customers.id", "4");
}

TEST(sql_join_conditions_with_and_without_hash) {
    vdb::Database db;
    shop(db);
    // Equality joins use a hash index; other conditions are tested pair by pair. Results must agree.
    CHECK_ROWS(db, "SELECT c.name, o.id FROM customers c JOIN orders o ON o.customer_id = c.id AND o.total > 100 ORDER BY o.id",
               "Ana|100;Davi|102;Eva|103");
    CHECK_ROWS(db, "SELECT c.name, o.id FROM customers c JOIN orders o ON o.customer_id + 0 >= c.id AND o.customer_id <= c.id ORDER BY o.id",
               "Ana|100;Ana|101;Davi|102;Eva|103");
    CHECK_ROWS(db, "SELECT c.name, o.id FROM customers c JOIN orders o ON c.id < 2 AND o.total < 100", "Ana|101");
    CHECK_ROWS(db, "SELECT a.name, b.name FROM customers a JOIN customers b ON a.id + 1 = b.id ORDER BY a.id", "Ana|Davi;Davi|Eva;Eva|Zoe");
}

TEST(sql_join_keys_follow_number_equality) {
    vdb::Database db;
    db.exec("CREATE TABLE l (k); INSERT INTO l VALUES ('7'), ('07'), (NULL), ('x');"
            "CREATE TABLE r (k, v); INSERT INTO r VALUES (7.0, 'seven'), (NULL, 'null'), ('x', 'ex')");
    CHECK_ROWS(db, "SELECT l.k, r.v FROM l JOIN r ON l.k = r.k ORDER BY l.k", "7|seven;07|seven;x|ex");
    CHECK_ROWS(db, "SELECT COUNT(*) FROM l LEFT JOIN r ON l.k = r.k WHERE r.v IS NULL", "1");
}

TEST(sql_join_using_and_ambiguity) {
    vdb::Database db;
    db.exec("CREATE TABLE a (id, x); INSERT INTO a VALUES (1, 'a1'), (2, 'a2');"
            "CREATE TABLE b (id, y); INSERT INTO b VALUES (2, 'b2'), (3, 'b3')");
    CHECK_ROWS(db, "SELECT id, x, y FROM a JOIN b USING (id)", "2|a2|b2");
    CHECK_ROWS(db, "SELECT id, x, y FROM a FULL JOIN b USING (id) ORDER BY id", "1|a1|NULL;2|a2|b2;3|NULL|b3");
    CHECK_ROWS(db, "SELECT id FROM a JOIN b ON a.id = b.id", "ERROR: ambiguous column: id (write table.id)");
    CHECK_ROWS(db, "SELECT a.* FROM a JOIN b ON a.id = b.id", "2|a2");
}

TEST(sql_three_way_join_and_left_then_inner) {
    vdb::Database db;
    shop(db);
    db.exec("CREATE TABLE notes (order_id, text); INSERT INTO notes VALUES (100, 'gift'), (103, 'urgent')");
    CHECK_ROWS(db, "SELECT c.name, o.id, n.text FROM customers c JOIN orders o ON o.customer_id = c.id "
                   "LEFT JOIN notes n ON n.order_id = o.id ORDER BY o.id",
               "Ana|100|gift;Ana|101|NULL;Davi|102|NULL;Eva|103|urgent");
}

// ── subqueries, WITH, sets ───────────────────────────────────────────────────

TEST(sql_subqueries) {
    vdb::Database db;
    shop(db);
    CHECK_ROWS(db, "SELECT name FROM customers WHERE id IN (SELECT customer_id FROM orders WHERE total > 200) ORDER BY name", "Davi;Eva");
    CHECK_ROWS(db, "SELECT name FROM customers WHERE id NOT IN (SELECT customer_id FROM orders) ORDER BY name", "Zoe");
    CHECK_ROWS(db, "SELECT name, (SELECT COUNT(*) FROM orders o WHERE o.customer_id = c.id) n FROM customers c ORDER BY id",
               "Ana|2;Davi|1;Eva|1;Zoe|0");
    CHECK_ROWS(db, "SELECT name FROM customers c WHERE EXISTS (SELECT 1 FROM orders WHERE customer_id = c.id AND total > 200)", "Davi;Eva");
    CHECK_ROWS(db, "SELECT name FROM customers c WHERE NOT EXISTS (SELECT 1 FROM orders WHERE customer_id = c.id)", "Zoe");
    CHECK_ROWS(db, "SELECT (SELECT MAX(total) FROM orders), (SELECT name FROM customers WHERE id = 99)", "899.9|NULL");
    CHECK_ROWS(db, "SELECT (SELECT id FROM orders)", "ERROR: a subquery used as a value returned more than one row");
    CHECK_ROWS(db, "SELECT t.n FROM (SELECT COUNT(*) AS n FROM orders) AS t", "5");
    CHECK_ROWS(db, "SELECT name FROM customers WHERE id IN orders", "");  // first column of orders: the order ids
    CHECK_ROWS(db, "SELECT name FROM customers WHERE id = (SELECT customer_id FROM orders WHERE id = 102)", "Davi");
}

TEST(sql_with_queries) {
    vdb::Database db;
    shop(db);
    CHECK_ROWS(db, "WITH big AS (SELECT * FROM orders WHERE total > 200), who AS (SELECT customer_id FROM big) "
                   "SELECT name FROM customers WHERE id IN who ORDER BY name",
               "Davi;Eva");
    CHECK_ROWS(db, "WITH RECURSIVE x AS (SELECT 1) SELECT * FROM x", "ERROR: not supported: WITH RECURSIVE");
}

TEST(sql_set_operations) {
    vdb::Database db;
    db.exec("CREATE TABLE a (v); INSERT INTO a VALUES (1), (2), (2), (3);"
            "CREATE TABLE b (v); INSERT INTO b VALUES (2), (3), (4)");
    CHECK_ROWS(db, "SELECT v FROM a UNION SELECT v FROM b ORDER BY v", "1;2;3;4");
    CHECK_ROWS(db, "SELECT v FROM a UNION ALL SELECT v FROM b ORDER BY v", "1;2;2;2;3;3;4");
    CHECK_ROWS(db, "SELECT v FROM a INTERSECT SELECT v FROM b ORDER BY v", "2;3");
    CHECK_ROWS(db, "SELECT v FROM a EXCEPT SELECT v FROM b", "1");
    CHECK_ROWS(db, "SELECT v FROM a UNION SELECT v FROM b ORDER BY v DESC LIMIT 2", "4;3");
    CHECK_ROWS(db, "SELECT v, v FROM a UNION SELECT v FROM b", "ERROR: each SELECT of UNION/INTERSECT/EXCEPT needs 2 columns");
}

// ── pipelines (D2) ───────────────────────────────────────────────────────────

TEST(sql_pipelines_chain_results) {
    vdb::Database db;
    shop(db);
    CHECK_ROWS(db, "SELECT customer_id FROM orders WHERE total > 200 |> SELECT name FROM customers WHERE id IN _ |> ORDER BY name",
               "Davi;Eva");
    CHECK_ROWS(db, "SELECT * FROM orders |> WHERE total > 100 |> SELECT id |> ORDER BY id DESC |> LIMIT 2", "105;103");
    CHECK_ROWS(db, "SELECT customer_id, SUM(total) AS s FROM orders GROUP BY customer_id |> WHERE s > 200 |> ORDER BY s",
               "9|210;3|241;2|899.9");
    CHECK_ROWS(db, "SELECT customer_id AS cid FROM orders |> SELECT c.name FROM _ JOIN customers c ON c.id = _.cid |> SELECT DISTINCT name |> ORDER BY 1",
               "Ana;Davi;Eva");
    CHECK_ROWS(db, "SELECT id FROM orders |> SELECT COUNT(*) |> SELECT * WHERE 1 = 1", "5");
    CHECK_ROWS(db, "SELECT 1 AS a |> SELECT a + 1 AS b |> SELECT b * 10", "20");
}

TEST(sql_pipeline_stages_read_text_like_a_csv) {
    vdb::Database db;
    CHECK_ROWS(db, "SELECT '007' AS c |> WHERE c = 7", "007");
    CHECK_ROWS(db, "SELECT 1.50 AS c |> WHERE c = '1.5'", "1.5");
    CHECK_ROWS(db, "SELECT 'x' AS c |> SELECT c, TYPEOF(c)", "x|text");
}

TEST(sql_pipeline_stage_sees_only_previous_columns) {
    vdb::Database db;
    shop(db);
    CHECK_ROWS(db, "SELECT id FROM orders |> WHERE total > 1", "ERROR: no such column: total");
}

TEST(sql_pipeline_with_and_in_queries) {
    vdb::Database db;
    shop(db);
    CHECK_ROWS(db, "WITH c AS (SELECT * FROM customers) SELECT customer_id FROM orders |> SELECT name FROM c WHERE id IN _ |> ORDER BY name",
               "Ana;Davi;Eva");
    CHECK_ROWS(db, "SELECT name FROM customers WHERE id IN (SELECT customer_id, total FROM orders |> WHERE total > 500)", "Davi");
    db.exec("CREATE TABLE vip AS SELECT customer_id, total FROM orders |> WHERE total > 200 |> SELECT customer_id");
    CHECK_ROWS(db, "SELECT COUNT(*) FROM vip", "3");
    db.exec("INSERT INTO vip SELECT id AS customer_id FROM customers |> WHERE customer_id = 4");
    CHECK_ROWS(db, "SELECT customer_id FROM vip ORDER BY customer_id", "2;3;4;9");
}

TEST(sql_select_without_from) {
    vdb::Database db;
    CHECK_ROWS(db, "SELECT 1 + 1 AS two, 'x'", "2|x");
    CHECK_ROWS(db, "SELECT COUNT(*)", "1");
    CHECK_ROWS(db, "SELECT *", "ERROR: SELECT * needs a FROM");
    db.exec("CREATE TABLE _ (v); INSERT INTO _ VALUES (5), (6)");  // a default input (spec A9)
    CHECK_ROWS(db, "SELECT SUM(v)", "11");
}

// ── changing data ────────────────────────────────────────────────────────────

TEST(sql_insert_forms) {
    vdb::Database db;
    db.exec("CREATE TABLE t (a, b)");
    CHECK_INT(db.query("INSERT INTO t VALUES (1, 'x'), (2, 'y')").changes(), 2);
    CHECK_INT(db.query("INSERT INTO t (b) VALUES ('only b')").changes(), 1);
    CHECK_INT(db.query("INSERT INTO t (c, a) VALUES ('new column', 9)").changes(), 1);  // free-form records
    CHECK_ROWS(db, "SELECT a, b, c FROM t ORDER BY a NULLS LAST", "1|x|NULL;2|y|NULL;9|NULL|new column;NULL|only b|NULL");
    CHECK_ROWS(db, "INSERT INTO t VALUES (1)", "ERROR: table t has 3 columns but 1 values were given (name the columns: INSERT INTO t (a, b) VALUES ...)");
    CHECK_ROWS(db, "INSERT INTO t (a) VALUES (1, 2)", "ERROR: 2 values for 1 columns");
    CHECK_ROWS(db, "INSERT INTO t (a) VALUES (oops)", "ERROR: VALUES cannot read a column (oops); quote text: 'oops'");
    db.exec("INSERT INTO fresh (k) VALUES ('made')");  // INSERT creates a missing table (A21)
    CHECK_ROWS(db, "SELECT k FROM fresh", "made");
}

TEST(sql_insert_select_maps_by_name_or_position) {
    vdb::Database db;
    db.exec("CREATE TABLE src (x, y); INSERT INTO src VALUES (1, 'a'), (2, 'b');"
            "CREATE TABLE dst (y, x)");
    db.exec("INSERT INTO dst SELECT x, y FROM src");  // by name (A11)
    CHECK_ROWS(db, "SELECT y, x FROM dst ORDER BY x", "a|1;b|2");
    db.exec("INSERT INTO dst (x, y) SELECT y, x FROM src");  // by position into the list
    CHECK_ROWS(db, "SELECT COUNT(*) FROM dst WHERE x IN ('a', 'b')", "2");
    db.exec("INSERT INTO src SELECT * FROM src");  // reads the rows that were there when it started
    CHECK_ROWS(db, "SELECT COUNT(*) FROM src", "4");
}

TEST(sql_update_and_delete) {
    vdb::Database db;
    people(db);
    CHECK_INT(db.query("UPDATE people SET age = age + 1, city = UPPER(city) WHERE city = 'Rio'").changes(), 2);
    CHECK_ROWS(db, "SELECT name, age, city FROM people WHERE city = 'RIO' ORDER BY name", "Ana|32|RIO;Dee|46|RIO");
    CHECK_INT(db.query("UPDATE people SET country = 'BR'").changes(), 5);
    CHECK_ROWS(db, "SELECT COUNT(*) FROM people WHERE country = 'BR'", "5");
    CHECK_INT(db.query("UPDATE people SET age = 0 WHERE age = (SELECT MAX(age) FROM people)").changes(), 1);
    CHECK_INT(db.query("DELETE FROM people WHERE NOT IS_NUMBER(age) OR age < 18").changes(), 2);
    CHECK_ROWS(db, "SELECT name FROM people ORDER BY name", "Ana;Dee;Eve");
    CHECK_INT(db.query("DELETE FROM people").changes(), 3);
    CHECK_ROWS(db, "SELECT COUNT(*) FROM people", "0");
    CHECK_ROWS(db, "UPDATE nowhere SET a = 1", "ERROR: no such table: nowhere");
}

TEST(sql_create_and_drop) {
    vdb::Database db;
    CHECK(db.exec("CREATE TABLE t (a INTEGER, b VARCHAR(20), c DOUBLE PRECISION)").ok());
    CHECK_NAMES(db, "SELECT * FROM t", "a|b|c");
    CHECK_ROWS(db, "CREATE TABLE t (x)", "ERROR: table t already exists");
    CHECK(db.exec("CREATE TABLE IF NOT EXISTS t (x)").ok());
    CHECK_ROWS(db, "CREATE TABLE u (a, a)", "ERROR: column a appears twice");
    CHECK_ROWS(db, "CREATE TABLE u (a PRIMARY KEY)", "ERROR: not supported: column constraints (records are free-form)");
    CHECK(db.exec("CREATE TABLE copy AS SELECT 1 AS one, 'two' AS two").ok());
    CHECK_ROWS(db, "SELECT * FROM copy", "1|two");
    CHECK(db.exec("DROP TABLE copy").ok());
    CHECK_ROWS(db, "DROP TABLE copy", "ERROR: no such table: copy");
    CHECK(db.exec("DROP TABLE IF EXISTS copy").ok());
    CHECK_ROWS(db, "CREATE TABLE bad AS SELECT * FROM missing", "ERROR: no such table: missing");
    CHECK(db.table("bad") == 0);  // a failed CREATE AS leaves nothing behind
}

// ── errors and hints (A20, spec 9) ───────────────────────────────────────────

TEST(sql_errors_explain_themselves) {
    vdb::Database db;
    people(db);
    CHECK_ROWS(db, "SELECT nmae FROM people", "ERROR: no such column: nmae (did you mean name?)");
    CHECK_ROWS(db, "SELECT * FROM peple", "ERROR: no such table: peple (did you mean people?)");
    CHECK_ROWS(db, "SELECT UPER(name) FROM people", "ERROR: no such function: UPER (did you mean UPPER?)");
    CHECK_ROWS(db, "SELECT name FORM people", "ERROR: syntax error at line 1, column 18 near \"people\": expected ';' or the end");
    CHECK_ROWS(db, "SELECT name\nFROM people\nWHERE age >", "ERROR: syntax error at the end: expected a value, a column or a function");
    CHECK_ROWS(db, "SELECT 'open", "ERROR: syntax error at line 1, column 8: text not closed (missing ')");
    CHECK_ROWS(db, "SELECT name FROM people WHERE COUNT(*) > 1", "ERROR: aggregate functions are not allowed in WHERE (use HAVING)");
    CHECK_ROWS(db, "SELECT SUM(COUNT(*)) FROM people", "ERROR: aggregate functions cannot be nested");
    CHECK_ROWS(db, "SELECT SUBSTR('a')", "ERROR: wrong number of arguments to SUBSTR (1 given)");
    CHECK_ROWS(db, "SELECT order FROM people", "ERROR: syntax error at line 1, column 8 near \"order\": expected a value or a column (quote names that are SQL words: \"order\")");
    CHECK_ROWS(db, "SELECT ROW_NUMBER() OVER (ORDER BY name) FROM people", "ERROR: not supported: window functions (OVER)");
    CHECK_ROWS(db, "SELECT COUNT(*) OVER () FROM people", "ERROR: not supported: window functions (OVER)");
    CHECK_ROWS(db, "SELECT name FROM people ORDER BY 9", "ERROR: ORDER BY position 9 is not a column of the result");
    CHECK_ROWS(db, "SELECT name FROM people LIMIT 'x'", "ERROR: LIMIT needs a number");
    CHECK_ROWS(db, "ALTER TABLE people ADD x", "ERROR: not supported: ALTER");
}

TEST(sql_a_syntax_error_changes_nothing) {
    vdb::Database db;
    people(db);
    CHECK(!db.exec("DELETE FROM people; SELECT FROM WHERE").ok());
    CHECK_ROWS(db, "SELECT COUNT(*) FROM people", "5");  // the whole script is parsed first (spec 9)
}

TEST(sql_nesting_is_bounded) {
    vdb::Database db;
    vdb::Text deep(vdb::heap_allocator());
    deep.append("SELECT ");
    for (int i = 0; i < 5000; ++i) deep.append_char('(');
    deep.append_char('1');
    for (int i = 0; i < 5000; ++i) deep.append_char(')');
    vdb::Result r = db.query(deep.c_str());
    CHECK(!r.ok());
    CHECK_INT(r.error_code(), vdb::k_error_limit);
}

TEST(sql_parameters) {
    vdb::Database db;
    people(db);
    CHECK_STR(test::Rows(db, "SELECT name FROM people WHERE name = ?", vdb::Params().add("x' OR '1'='1")).c_str(), "");
    CHECK_STR(test::Rows(db, "SELECT name FROM people WHERE age > ? AND city = ? ORDER BY name", vdb::Params().add(30).add("Rio")).c_str(),
              "Ana;Dee");
    CHECK_STR(test::Rows(db, "SELECT ?2, ?1, ?", vdb::Params().add(1).add(2.5).add(true)).c_str(), "2.5|1|1");
    CHECK_STR(test::Rows(db, "SELECT ? IS NULL", vdb::Params().add_null()).c_str(), "1");
    CHECK_ROWS(db, "SELECT ?", "ERROR: the SQL uses 1 parameters but 0 values were given");
}

TEST(sql_decimal_comma_option) {
    vdb::Database db;
    db.options().decimal_separator = ',';
    db.exec("CREATE TABLE p (price); INSERT INTO p VALUES ('3,50'), ('10,25')");
    CHECK_ROWS(db, "SELECT SUM(price), MAX(price) * 2 FROM p", "13,75|20,5");
}

TEST(sql_comments_and_quoted_names) {
    vdb::Database db;
    db.exec("CREATE TABLE \"my table\" (\"first name\", [order], `select`)");
    db.exec("INSERT INTO \"my table\" VALUES ('Ana', 1, 'x') -- a comment\n");
    CHECK_ROWS(db, "/* block */ SELECT \"first name\", \"order\", [select] FROM \"my table\" -- end", "Ana|1|x");
}

TEST(sql_result_access_is_safe) {
    vdb::Database db;
    vdb::Result bad = db.query("SELECT * FROM missing");
    CHECK(!bad.ok());
    CHECK(bad[0][0].is_null());       // a failed result: NULL cells, never a crash
    CHECK_STR(bad[5]["x"].text(), "");
    vdb::Result r = db.query("SELECT 42 AS n, 'x' AS t, NULL AS z");
    CHECK_INT(r[0]["n"].integer(), 42);
    CHECK(r[0]["n"].number() == 42.0);
    CHECK_INT(r[0]["t"].integer(-1), -1);  // not a number: the fallback
    CHECK(r[0]["z"].is_null());
    CHECK(r[0]["nope"].is_null());
    CHECK(r[9][0].is_null());
    CHECK_INT(r.column_index("T"), 1);
    vdb::Result moved = static_cast<vdb::Result&&>(r);
    CHECK_INT(moved.size(), 1);
    CHECK_INT(r.size(), 0);
}

TEST(sql_right_join_inside_a_chain) {
    vdb::Database db;
    db.exec("CREATE TABLE a (id); INSERT INTO a VALUES (1), (2);"
            "CREATE TABLE b (id, a_id); INSERT INTO b VALUES (10, 1), (20, 9);"
            "CREATE TABLE c (b_id, tag); INSERT INTO c VALUES (10, 'x'), (20, 'y'), (30, 'z')");
    // (a RIGHT JOIN b) JOIN c: b's row 20 has no a, and still meets c.
    CHECK_ROWS(db, "SELECT a.id, b.id, c.tag FROM a RIGHT JOIN b ON b.a_id = a.id JOIN c ON c.b_id = b.id ORDER BY b.id",
               "1|10|x;NULL|20|y");
    CHECK_ROWS(db, "SELECT b.id, c.tag FROM b RIGHT JOIN c ON c.b_id = b.id ORDER BY c.tag", "10|x;20|y;NULL|z");
    CHECK_ROWS(db, "SELECT COUNT(*) FROM a FULL JOIN b ON b.a_id = a.id", "3");
}

TEST(sql_correlated_subqueries_in_groups) {
    vdb::Database db;
    db.exec("CREATE TABLE s (team, player, points);"
            "INSERT INTO s VALUES ('red', 'ana', 10), ('red', 'bo', 30), ('blue', 'cid', 20), ('blue', 'dee', 5)");
    CHECK_ROWS(db, "SELECT team, SUM(points), (SELECT player FROM s AS t WHERE t.team = s.team ORDER BY points DESC LIMIT 1) AS best "
                   "FROM s GROUP BY team ORDER BY team",
               "blue|25|cid;red|40|bo");
    CHECK_ROWS(db, "SELECT player FROM s AS o WHERE points > (SELECT AVG(points) FROM s AS i WHERE i.team = o.team) ORDER BY player",
               "bo;cid");
    CHECK_ROWS(db, "SELECT team FROM s GROUP BY team HAVING MAX(points) > (SELECT AVG(points) FROM s) ORDER BY team", "blue;red");  // average 16.25
}

TEST(sql_pipeline_feeds_groups_row_by_row) {
    vdb::Database db;
    db.exec("CREATE TABLE t (k, v); INSERT INTO t VALUES ('a', 1), ('b', 2), ('a', 3), ('c', NULL)");
    CHECK_ROWS(db, "SELECT * FROM t |> SELECT k, SUM(v) s GROUP BY k |> WHERE s IS NOT NULL |> ORDER BY s DESC", "a|4;b|2");
    CHECK_ROWS(db, "SELECT * FROM t WHERE v > 0 |> SELECT COUNT(*) AS n, MAX(v) AS m", "3|3");
    CHECK_ROWS(db, "SELECT * FROM t |> SELECT DISTINCT k |> ORDER BY k DESC |> LIMIT 2", "c;b");
    CHECK_ROWS(db, "SELECT * FROM t |> LIMIT 0", "");
}

TEST(sql_unsupported_features_say_so) {
    vdb::Database db;
    db.exec("CREATE TABLE a (x); CREATE TABLE b (x)");
    CHECK_ROWS(db, "SELECT * FROM a NATURAL JOIN b", "ERROR: not supported: NATURAL JOIN (use ON or USING)");
    CHECK_ROWS(db, "SELECT ROW_NUMBER() OVER (ORDER BY x) FROM a", "ERROR: not supported: window functions (OVER)");
    CHECK_ROWS(db, "SELECT RANK() OVER (PARTITION BY x ORDER BY (x)) FROM a", "ERROR: not supported: window functions (OVER)");
    CHECK_ROWS(db, "SELECT NOPE(x) FROM a", "ERROR: no such function: NOPE");
}
