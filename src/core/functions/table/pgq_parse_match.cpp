#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckpgq/common.hpp"
#include "duckpgq/core/functions/table.hpp"
#include "duckpgq/core/parser/duckpgq_grammar.hpp"

namespace duckdb {

namespace {

string DumpElement(const PathElement &element) {
	auto body = element.variable_binding + (element.label.empty() ? "" : ":" + element.label);
	switch (element.match_type) {
	case PGQMatchType::MATCH_VERTEX:
		return "(" + body + ")";
	case PGQMatchType::MATCH_EDGE_ANY:
		return "-[" + body + "]-";
	case PGQMatchType::MATCH_EDGE_LEFT:
		return "<-[" + body + "]-";
	case PGQMatchType::MATCH_EDGE_RIGHT:
		return "-[" + body + "]->";
	case PGQMatchType::MATCH_EDGE_LEFT_RIGHT:
		return "<-[" + body + "]->";
	}
	throw InternalException("Unknown graph element match type");
}

string DumpPathModeName(PGQPathMode mode) {
	switch (mode) {
	case PGQPathMode::NONE:
		return "";
	case PGQPathMode::WALK:
		return "WALK ";
	case PGQPathMode::SIMPLE:
		return "SIMPLE ";
	case PGQPathMode::TRAIL:
		return "TRAIL ";
	case PGQPathMode::ACYCLIC:
		return "ACYCLIC ";
	}
	throw InternalException("Unknown graph path mode");
}

string DumpReference(const PathReference &reference) {
	if (reference.path_reference_type == PGQPathReferenceType::PATH_ELEMENT) {
		return DumpElement(static_cast<const PathElement &>(reference));
	}
	auto &subpath = static_cast<const SubPath &>(reference);
	string result = subpath.path_variable.empty() ? "" : subpath.path_variable + " = ";
	result += DumpPathModeName(subpath.path_mode) + "{";
	for (idx_t i = 0; i < subpath.path_list.size(); i++) {
		result += (i > 0 ? " " : "") + DumpReference(*subpath.path_list[i]);
	}
	if (subpath.where_clause) {
		result += " WHERE " + subpath.where_clause->ToString();
	}
	if (subpath.cost_expr) {
		result += " COST " + subpath.cost_expr->ToString();
		result += " DEFAULT " + Value::DOUBLE(subpath.default_value).ToString();
	}
	result += "}";
	if (subpath.lower != 1 || subpath.upper != 1) {
		auto upper = subpath.upper == NumericLimits<int64_t>::Maximum() ? string() : to_string(subpath.upper);
		result += "{" + to_string(subpath.lower) + "," + upper + "}";
	}
	return result;
}

string DumpPattern(const PathPattern &pattern) {
	string result;
	if (pattern.cheapest) {
		result += pattern.all ? "ALL CHEAPEST " : "ANY CHEAPEST ";
	} else if (pattern.shortest) {
		result += pattern.topk > 1 ? "SHORTEST " + to_string(pattern.topk) + " "
		                           : (pattern.all ? "ALL SHORTEST " : "ANY SHORTEST ");
	}
	for (idx_t i = 0; i < pattern.path_elements.size(); i++) {
		result += (i > 0 ? " " : "") + DumpReference(*pattern.path_elements[i]);
	}
	return result;
}

string DumpPatternList(const vector<unique_ptr<PathPattern>> &patterns) {
	string result;
	for (idx_t i = 0; i < patterns.size(); i++) {
		result += (i > 0 ? ", " : "") + DumpPattern(*patterns[i]);
	}
	return result;
}

string DumpOneRowPer(const MatchExpression &match) {
	switch (match.one_row_per) {
	case OneRowPerMode::NONE:
		return "";
	case OneRowPerMode::VERTEX:
		return " ONE ROW PER VERTEX (" + StringUtil::Join(match.one_row_per_vars, ", ") + ")";
	case OneRowPerMode::EDGE:
		return " ONE ROW PER EDGE (" + StringUtil::Join(match.one_row_per_vars, ", ") + ")";
	case OneRowPerMode::STEP:
		return " ONE ROW PER STEP (" + StringUtil::Join(match.one_row_per_vars, ", ") + ")";
	}
	throw InternalException("Unknown ONE ROW PER mode");
}

string DumpMatchBlock(const MatchExpression &match);

//! EXISTS { ... } patterns print as an opaque subquery; list them after the expression that holds them.
void CollectExistsPatterns(ParsedExpression &expression, string &result) {
	auto exists_pattern = DuckPGQGetExistsPattern(expression);
	if (exists_pattern) {
		result += " [EXISTS {" + DumpMatchBlock(*exists_pattern) + "}]";
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(expression,
	                                            [&](ParsedExpression &child) { CollectExistsPatterns(child, result); });
}

string DumpMatchBlock(const MatchExpression &match) {
	string result = "MATCH " + DumpPatternList(match.path_patterns);
	for (auto &group : match.optional_match_patterns) {
		result += " OPTIONAL MATCH " + DumpPatternList(group);
	}
	result += DumpOneRowPer(match);
	if (match.where_clause) {
		result += " WHERE " + match.where_clause->ToString();
		CollectExistsPatterns(*match.where_clause, result);
	}
	return result;
}

string DumpColumns(const vector<unique_ptr<ParsedExpression>> &columns) {
	string result = " COLUMNS (";
	for (idx_t i = 0; i < columns.size(); i++) {
		auto &column = *columns[i];
		result += (i > 0 ? ", " : "") + column.ToString();
		if (column.HasAlias()) {
			result += " AS " + column.GetAlias().GetIdentifierName();
		}
	}
	return result + ")";
}

//! A canonical, deterministic rendering of everything the grammar put into a MatchExpression.
string DumpMatch(const MatchExpression &match) {
	// UNION branches carry their own COLUMNS only when every block has one.
	const bool branch_local = !match.union_matches.empty() && !match.union_matches[0]->column_list.empty();
	string result = DumpMatchBlock(match);
	if (branch_local) {
		result += DumpColumns(match.column_list);
	}
	for (idx_t i = 0; i < match.union_matches.size(); i++) {
		result += match.union_all_flags[i] ? " UNION ALL " : " UNION ";
		result += DumpMatchBlock(*match.union_matches[i]);
		if (branch_local) {
			result += DumpColumns(match.union_matches[i]->column_list);
		}
	}
	return branch_local ? result : result + DumpColumns(match.column_list);
}

// Called for every table ref; EnumerateQueryNodeChildren does the recursion itself.
void CollectMatch(TableRef &ref, vector<string> &matches) {
	if (ref.type != TableReferenceType::TABLE_FUNCTION) {
		return;
	}
	auto match_info = dynamic_cast<DuckPGQMatchInfo *>(ref.Cast<TableFunctionRef>().bind_info.get());
	if (match_info) {
		matches.push_back(DumpMatch(*match_info->match));
	}
}

struct PGQParseMatchData : TableFunctionData {
	vector<string> matches;
};

struct PGQParseMatchState : GlobalTableFunctionState {
	idx_t offset = 0;
};

unique_ptr<FunctionData> PGQParseMatchBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<PGQParseMatchData>();
	Parser parser(context);
	parser.ParseQuery(input.inputs[0].GetValue<string>());
	for (auto &statement : parser.statements) {
		if (statement->type != StatementType::SELECT_STATEMENT) {
			continue;
		}
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    *statement->Cast<SelectStatement>().node, [](unique_ptr<ParsedExpression> &) {},
		    [&](TableRef &ref) { CollectMatch(ref, result->matches); });
	}
	names.emplace_back("match_index");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("pattern");
	return_types.emplace_back(LogicalType::VARCHAR);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> PGQParseMatchInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PGQParseMatchState>();
}

void PGQParseMatchFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<PGQParseMatchData>();
	auto &state = input.global_state->Cast<PGQParseMatchState>();
	idx_t count = 0;
	while (state.offset < data.matches.size() && count < STANDARD_VECTOR_SIZE) {
		output.SetValue(0, count, Value::BIGINT(NumericCast<int64_t>(state.offset)));
		output.SetValue(1, count, Value(data.matches[state.offset]));
		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CoreTableFunctions::RegisterParseMatchTableFunction(ExtensionLoader &loader) {
	// Diagnostic: shows how the active grammar parsed each GRAPH_TABLE in a query, without binding it.
	loader.RegisterFunction(TableFunction("pgq_parse_match", {LogicalType::VARCHAR}, PGQParseMatchFunction,
	                                       PGQParseMatchBind, PGQParseMatchInit));
}

} // namespace duckdb
