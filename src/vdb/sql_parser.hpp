// Lexer and parser: SQL text to the statement tree (spec 3.1, 3.2, A10).
#ifndef VDB_SQL_PARSER_HPP
#define VDB_SQL_PARSER_HPP

#include "sql_ast.hpp"

namespace vdb {

// Parses a whole script before anything runs (spec 9). Nodes and names go into `pool`.
// Returns the first statement (0 for an empty script); on error returns 0 with `status` set.
// `parameters` receives the number of ? parameters the script uses.
Statement* parse_script(Pool& pool, const char* sql, size_t length, Status& status, size_t& parameters);

// The default alias of a table name or path: the file name without its extension ("data/people.csv" -> "people").
const char* default_alias(Pool& pool, const char* name);

}  // namespace vdb

#endif
