#!/bin/sh
# Program tests (spec 10): sel and vdb against golden outputs, from examples/data.
# Usage: sh tests/tools/run.sh <bin directory>
BIN=$(cd "${1:-bin}" && pwd)
cd "$(dirname "$0")/../../examples/data" || exit 1
SEL="$BIN/sel"
VDB="$BIN/vdb"
passed=0
failed=0

# check NAME EXPECTED_EXIT COMMAND...   (expected stdout+stderr on stdin)
check() {
    name=$1
    want_exit=$2
    shift 2
    expected=$(cat)
    actual=$("$@" 2>&1)
    code=$?
    if [ "$actual" = "$expected" ] && [ "$code" = "$want_exit" ]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        echo "FAIL $name (exit $code, wanted $want_exit)"
        echo "--- expected"
        echo "$expected"
        echo "--- got"
        echo "$actual"
    fi
}

check select_where_order 0 "$SEL" name, age from people.csv where "age > 30" order by age desc <<'EOF'
name;age
Davi;45
Fábio;38
Ana;31
EOF

check table_output 0 "$SEL" name, city from people.csv where "city like 'Rio%'" -o table <<'EOF'
+------+----------------+
| name | city           |
+------+----------------+
| Ana  | Rio de Janeiro |
| Davi | Rio de Janeiro |
+------+----------------+
(2 rows)
EOF

check json_output 0 "$SEL" "name, age from people.csv where age < 30 order by name" -o json <<'EOF'
[
{"name":"Carla","age":"017"},
{"name":"Eva","age":22},
{"name":"Gil \"Junior\"","age":29}
]
EOF

check join_two_files 0 "$SEL" "c.name, SUM(o.quantity * o.price) AS spent" from orders.csv o join customers.csv c on "o.customer_id = c.id" group by c.name order by spent desc <<'EOF'
name;spent
Davi;899.9
Eva;391
Ana;156.5
EOF

check left_join_and_nulls 0 "$SEL" "c.name, o.id from customers.csv c left join orders.csv o on o.customer_id = c.id where o.id is null" --null '(none)' <<'EOF'
name;id
Zoe;(none)
EOF

check stdin_count 0 sh -c "cat people.csv | '$SEL' 'count(*) as n'" <<'EOF'
n
7
EOF

check shell_pipe_between_sels 0 sh -c "'$SEL' '* from people.csv where age > 25' | '$SEL' 'name order by name desc limit 2'" <<'EOF'
name
"Gil ""Junior"""
Fábio
EOF

check pipeline_stages 0 "$SEL" "name, age from people.csv |> where age > 30 |> select upper(name) as n |> order by n" <<'EOF'
n
ANA
DAVI
FÁBIO
EOF

check regex_delimiter 0 "$SEL" "USER, CPU from processes.txt where CPU > 1 order by CPU desc" -r '\s+' -D , <<'EOF'
USER,CPU
gustavo,12.0
gustavo,3.2
EOF

check comma_file_with_option 0 "$SEL" call, band from qso_log.csv where "mode = 'FT8' and band = '20m'" -d , <<'EOF'
call,band
LU1XYZ,20m
VK2GHI,20m
EOF

check auto_delimiter 0 "$SEL" "country, count(*) n from qso_log.csv group by country having n > 1" -d auto -o tsv <<'EOF'
country	n
Brazil	2
EOF

check decimal_comma 0 "$SEL" "produto, preço * estoque as total from prices_br.csv order by total desc" --decimal-comma <<'EOF'
produto;total
Rádio;1799,8
Antena;482
Cabo coaxial;360
EOF

check full_sql_with_table_option 0 "$SEL" -t p=people.csv -q "UPDATE p SET age = age + 1 WHERE name = 'Ana'; SELECT name, age FROM p WHERE name = 'Ana'" <<'EOF'
name;age
Ana;32
EOF

check no_header_input 0 sh -c "printf '1;x\n2;y\n' | '$SEL' 'c2 where c1 = 2' -H" <<'EOF'
c2
y
EOF

check empty_fields 0 sh -c "printf 'a;b\n;\"\"\n' | '$SEL' 'a is null, b is null, length(b)' -o jsonl" <<'EOF'
{"a is null":1,"b is null":0,"length(b)":0}
EOF

check markdown_output 0 "$SEL" "name from customers.csv where country = 'BR'" -o markdown <<'EOF'
| name |
| --- |
| Ana |
| Davi |
EOF

check show_sql 0 "$SEL" name from customers.csv limit 1 --show-sql <<'EOF'
SELECT name from customers.csv limit 1
name
Ana
EOF

check strict_rows 1 sh -c "printf 'a;b\n1\n' | '$SEL' '*' --strict" <<'EOF'
a;b
sel: error: csv line 2: 1 fields, the header has 2
EOF

check missing_file 3 "$SEL" "*" from nothing.csv <<'EOF'
sel: error: cannot open nothing.csv: No such file or directory
EOF

check sql_error 1 "$SEL" nmae from people.csv <<'EOF'
sel: error: no such column: nmae (did you mean name?)
EOF

check value_hint 1 "$SEL" name from people.csv where "name = Ana" <<'EOF'
sel: error: no such column: Ana (did you mean age?)
sel: hint: if Ana is a text value, quote it so the shell keeps the quotes: "... = 'Ana'"
EOF

check unknown_option 2 "$SEL" name from people.csv --bogus <<'EOF'
sel: unknown option --bogus (see sel --help)
EOF

check expanded_star 2 "$SEL" customers.csv orders.csv people.csv prices_br.csv processes.txt qso_log.csv from people.csv <<'EOF'
sel: the shell replaced * with the names of the files in this folder.
     Quote it: sel "*" from ...   (or quote the whole query: sel "* from ...")
EOF

check bare_where_warning 0 "$SEL" name from customers.csv where id <<'EOF'
sel: warning: "where id" has no comparison. If you typed > or <, the shell took it as a
     redirection (and made a file): quote the condition, e.g. where "id > 30"
name
Ana
Davi
Eva
Zoe
EOF

# 20,000 rows of x;1 (awk, not yes | head: GNU yes reports the broken pipe on stderr).
ROWS_20K="awk 'BEGIN { print \"a;b\"; for (i = 0; i < 20000; i++) print \"x;1\" }'"

check memory_limit_sort 1 sh -c "$ROWS_20K | '$SEL' '* order by a' --memory-limit 256K" <<'EOF'
a;b
sel: error: out of memory
sel: hint: the --memory-limit was reached; a filter without ORDER BY, GROUP BY or DISTINCT
     streams the file and needs little memory
EOF

check memory_limit_stream 0 sh -c "$ROWS_20K | '$SEL' \"count(*), sum(b) where a = 'x'\" --memory-limit 256K" <<'EOF'
count(*);sum(b)
20000;20000
EOF

check version 0 "$SEL" --version <<'EOF'
sel (vDB) 1.0.0
EOF

check shell_command 0 "$VDB" -t c=customers.csv -o csv -c "SELECT country, COUNT(*) AS n FROM c GROUP BY country ORDER BY n DESC, country" <<'EOF'
country;n
BR;2
PT;1
US;1
EOF

check shell_script_and_dot_commands 0 sh -c "printf '.load customers.csv\n.mode csv\nSELECT name\n  FROM customers\n  WHERE id > 2;\n.tables\n' | '$VDB'" <<'EOF'
loaded customers: 4 rows, 3 columns
name
Eva
Zoe
customers                4 rows, 3 columns
EOF

check shell_from_file_name 0 "$VDB" -o csv -c "SELECT COUNT(*) FROM 'orders.csv'; SELECT MAX(price) FROM orders.csv" <<'EOF'
COUNT(*)
6
MAX(price)
899.90
EOF

check shell_error_exit 1 "$VDB" -c "SELECT * FROM nowhere" <<'EOF'
error: no such table: nowhere
EOF

echo "tools: $passed passed, $failed failed"
[ "$failed" = 0 ]
