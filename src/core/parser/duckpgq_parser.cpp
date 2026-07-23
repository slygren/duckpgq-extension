
#include "duckpgq/core/parser/duckpgq_parser.hpp"

#include "duckdb/parser/parser_options.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include <duckdb/parser/parsed_data/create_table_info.hpp>
#include <duckdb/parser/parser.hpp>
#include <duckdb/parser/statement/copy_statement.hpp>
#include <duckdb/parser/statement/create_statement.hpp>
#include <duckdb/parser/statement/insert_statement.hpp>
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include <duckpgq/core/functions/table/create_property_graph.hpp>
#include <duckpgq_state.hpp>

#include "duckdb/parser/query_node/cte_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include <atomic>
#include <duckpgq/core/functions/table/describe_property_graph.hpp>
#include <duckpgq/core/functions/table/drop_property_graph.hpp>

#include <duckdb/parser/tableref/matchref.hpp>
#include <duckpgq/core/functions/table/summarize_property_graph.hpp>

#include "duckdb/main/extension_callback_manager.hpp"
#include "duckpgq/core/utils/duckpgq_utils.hpp"

namespace duckdb {

static void duckpgq_find_query_node(QueryNode *query_node, DuckPGQState &duckpgq_state);
static void duckpgq_find_query_node_parser_override(QueryNode *query_node);
static void duckpgq_find_expression(ParsedExpression *expr, DuckPGQState &duckpgq_state);
static void duckpgq_find_expression_parser_override(ParsedExpression *expr);
static void duckpgq_find_match_expression(MatchExpression *match_expression, DuckPGQState &duckpgq_state);
static void duckpgq_find_match_expression_parser_override(MatchExpression *match_expression);

static std::atomic<int32_t> parser_override_match_index {-1};
static std::mutex parser_override_match_lock;
static unordered_map<int32_t, unique_ptr<ParsedExpression>> parser_override_transform_expression;

ParsedExpression *DuckPGQGetParserOverrideExpression(int32_t match_index) {
	lock_guard<mutex> guard(parser_override_match_lock);
	auto entry = parser_override_transform_expression.find(match_index);
	if (entry == parser_override_transform_expression.end()) {
		return nullptr;
	}
	return entry->second.get();
}

// Returns true if the query contains PGQ-specific keywords. Used by parser_override to decide
// whether to intercept before DuckDB's standard parser.
static bool HasPGQKeywords(const string &query) {
	string upper = StringUtil::Upper(query);
	return upper.find("GRAPH_TABLE") != string::npos ||
	       upper.find("PROPERTY GRAPH") != string::npos;
}

static bool IsIdentifierStart(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool IsIdentifierPart(char c) {
	return IsIdentifierStart(c) || (c >= '0' && c <= '9');
}

static bool IsExpressionBoundary(const string &query, idx_t pos) {
	if (pos == 0) {
		return true;
	}
	char c = query[pos - 1];
	return !IsIdentifierPart(c) && c != '.';
}

static bool TryParseIdentifier(const string &query, idx_t &pos, string &identifier) {
	if (pos >= query.size() || !IsIdentifierStart(query[pos])) {
		return false;
	}
	const idx_t start = pos;
	pos++;
	while (pos < query.size() && IsIdentifierPart(query[pos])) {
		pos++;
	}
	identifier = query.substr(start, pos - start);
	return true;
}

static bool TryParseProjectionPrefix(const string &query, idx_t start, vector<string> &parts, idx_t &brace_start) {
	if (!IsExpressionBoundary(query, start)) {
		return false;
	}
	idx_t pos = start;
	string part;
	if (!TryParseIdentifier(query, pos, part)) {
		return false;
	}
	parts.push_back(part);
	while (pos < query.size() && query[pos] == '.') {
		pos++;
		if (pos < query.size() && query[pos] == '{') {
			if (parts.size() == 1 || parts.size() == 2) {
				brace_start = pos;
				return true;
			}
			return false;
		}
		if (!TryParseIdentifier(query, pos, part)) {
			return false;
		}
		parts.push_back(part);
		if (parts.size() > 2) {
			return false;
		}
	}
	return false;
}

static bool TryReadBraceItems(const string &query, idx_t brace_start, vector<string> &items, idx_t &brace_end) {
	idx_t pos = brace_start + 1;
	while (pos < query.size()) {
		while (pos < query.size() && StringUtil::CharacterIsSpace(query[pos])) {
			pos++;
		}
		string item;
		if (!TryParseIdentifier(query, pos, item)) {
			return false;
		}
		items.push_back(item);
		while (pos < query.size() && StringUtil::CharacterIsSpace(query[pos])) {
			pos++;
		}
		if (pos >= query.size()) {
			return false;
		}
		if (query[pos] == '}') {
			brace_end = pos;
			return !items.empty();
		}
		if (query[pos] != ',') {
			return false;
		}
		pos++;
	}
	return false;
}

static string RewriteObjectProjectionSyntax(const string &query) {
	string result;
	result.reserve(query.size());
	bool in_single_quote = false;
	bool in_double_quote = false;
	idx_t pos = 0;
	while (pos < query.size()) {
		char c = query[pos];
		if (in_single_quote) {
			result += c;
			if (c == '\'' && pos + 1 < query.size() && query[pos + 1] == '\'') {
				result += query[pos + 1];
				pos += 2;
				continue;
			}
			if (c == '\'') {
				in_single_quote = false;
			}
			pos++;
			continue;
		}
		if (in_double_quote) {
			result += c;
			if (c == '"' && pos + 1 < query.size() && query[pos + 1] == '"') {
				result += query[pos + 1];
				pos += 2;
				continue;
			}
			if (c == '"') {
				in_double_quote = false;
			}
			pos++;
			continue;
		}
		if (c == '\'') {
			in_single_quote = true;
			result += c;
			pos++;
			continue;
		}
		if (c == '"') {
			in_double_quote = true;
			result += c;
			pos++;
			continue;
		}
		if (IsIdentifierStart(c)) {
			vector<string> prefix_parts;
			idx_t brace_start = 0;
			idx_t brace_end = 0;
			vector<string> items;
			if (TryParseProjectionPrefix(query, pos, prefix_parts, brace_start) &&
			    TryReadBraceItems(query, brace_start, items, brace_end)) {
				result += "struct_pack(";
				for (idx_t item_idx = 0; item_idx < items.size(); item_idx++) {
					if (item_idx > 0) {
						result += ", ";
					}
					result += KeywordHelper::WriteOptionallyQuoted(items[item_idx]);
					result += " := ";
					result += KeywordHelper::WriteOptionallyQuoted(prefix_parts[0]);
					result += ".";
					if (prefix_parts.size() == 2) {
						result += KeywordHelper::WriteOptionallyQuoted(prefix_parts[1]);
						result += ".";
					}
					result += KeywordHelper::WriteOptionallyQuoted(items[item_idx]);
				}
				result += ")";
				pos = brace_end + 1;
				continue;
			}
		}
		result += c;
		pos++;
	}
	return result;
}

// parser_override: called by DuckDB BEFORE its standard parser.
// If the query contains PGQ keywords, we parse it here (stripping any leading '-' for backward
// compatibility) and return the parsed statements. DuckDB then binds them; if binding fails,
// duckpgq_bind handles it. If the query has no PGQ keywords, we return DISPLAY_ORIGINAL_ERROR
// so DuckDB proceeds with its normal parse path.
ParserOverrideResult duckpgq_parser_override(ParserExtensionInfo *, const string &query, ParserOptions &options) {
	if (!HasPGQKeywords(query)) {
		return ParserOverrideResult(); // DISPLAY_ORIGINAL_ERROR — not our query
	}
	// Strip leading '-' if present (backward compat with existing test queries).
	const string effective = RewriteObjectProjectionSyntax((!query.empty() && query[0] == '-') ? query.substr(1) : query);
	try {
		Parser parser(options);
		parser.ParseQuery(effective);
		if (parser.statements.empty()) {
			return ParserOverrideResult();
		}
		for (auto &statement : parser.statements) {
			if (statement->type == StatementType::SELECT_STATEMENT) {
				auto &select_statement = statement->Cast<SelectStatement>();
				duckpgq_find_query_node_parser_override(select_statement.node.get());
			}
		}
		return ParserOverrideResult(std::move(parser.statements));
	} catch (std::exception &e) {
		return ParserOverrideResult(e);
	}
}

ParserExtensionParseResult duckpgq_parse(ParserExtensionInfo *info, const std::string &query) {
	Parser parser;
	parser.ParseQuery(RewriteObjectProjectionSyntax((query[0] == '-') ? query.substr(1, query.length()) : query));
	if (parser.statements.size() != 1) {
		throw Exception(ExceptionType::PARSER, "More than one statement detected, please only give one.");
	}
	return ParserExtensionParseResult(
	    make_uniq_base<ParserExtensionParseData, DuckPGQParseData>(std::move(parser.statements[0])));
}

void duckpgq_find_match_function(TableRef *table_ref, DuckPGQState &duckpgq_state) {
	// TODO(dtenwolde) add support for other style of tableRef (e.g. PivotRef)
	if (auto table_function_ref = dynamic_cast<TableFunctionRef *>(table_ref)) {
		// Handle TableFunctionRef case
		auto function = dynamic_cast<FunctionExpression *>(table_function_ref->function.get());
		if (function->function_name != "duckpgq_match") {
			return;
		}
		auto match_expression = dynamic_cast<MatchExpression *>(function->children[0].get());
		if (!match_expression) {
			return;
		}
		duckpgq_find_match_expression(match_expression, duckpgq_state);
		table_function_ref->alias = match_expression->alias;
		int32_t match_index = duckpgq_state.match_index++;
		duckpgq_state.transform_expression[match_index] = std::move(function->children[0]);
		function->children.pop_back();
		auto function_identifier = make_uniq<ConstantExpression>(Value::CreateValue(match_index));
		function->children.push_back(std::move(function_identifier));
	} else if (auto join_ref = dynamic_cast<JoinRef *>(table_ref)) {
		// Handle JoinRef case
		duckpgq_find_match_function(join_ref->left.get(), duckpgq_state);
		duckpgq_find_match_function(join_ref->right.get(), duckpgq_state);
	} else if (auto subquery_ref = dynamic_cast<SubqueryRef *>(table_ref)) {
		duckpgq_find_query_node(subquery_ref->subquery->node.get(), duckpgq_state);
	}
}

static void duckpgq_find_match_function_parser_override(TableRef *table_ref) {
	if (!table_ref) {
		return;
	}
	if (auto table_function_ref = dynamic_cast<TableFunctionRef *>(table_ref)) {
		auto function = dynamic_cast<FunctionExpression *>(table_function_ref->function.get());
		if (!function || function->function_name != "duckpgq_match" || function->children.empty()) {
			return;
		}
		auto match_expression = dynamic_cast<MatchExpression *>(function->children[0].get());
		if (!match_expression) {
			return;
		}
		duckpgq_find_match_expression_parser_override(match_expression);
		table_function_ref->alias = match_expression->alias;
		int32_t match_index = parser_override_match_index--;
		{
			lock_guard<mutex> guard(parser_override_match_lock);
			parser_override_transform_expression[match_index] = function->children[0]->Copy();
		}
		auto function_identifier = make_uniq<ConstantExpression>(Value::CreateValue(match_index));
		function->children[0] = std::move(function_identifier);
	} else if (auto join_ref = dynamic_cast<JoinRef *>(table_ref)) {
		duckpgq_find_match_function_parser_override(join_ref->left.get());
		duckpgq_find_match_function_parser_override(join_ref->right.get());
	} else if (auto subquery_ref = dynamic_cast<SubqueryRef *>(table_ref)) {
		duckpgq_find_query_node_parser_override(subquery_ref->subquery->node.get());
	}
}

static void duckpgq_find_query_node_parser_override(QueryNode *query_node) {
	if (!query_node) {
		return;
	}

	for (auto const &kv_pair : query_node->cte_map.map) {
		auto const &cte = kv_pair.second;
		auto *cte_select_statement = dynamic_cast<SelectStatement *>(cte->query.get());
		if (!cte_select_statement) {
			continue;
		}
		duckpgq_find_query_node_parser_override(cte_select_statement->node.get());
	}

	if (auto *select_node = dynamic_cast<SelectNode *>(query_node)) {
		duckpgq_find_match_function_parser_override(select_node->from_table.get());
		for (auto &expr : select_node->select_list) {
			duckpgq_find_expression_parser_override(expr.get());
		}
		duckpgq_find_expression_parser_override(select_node->where_clause.get());
		return;
	}

	if (auto *cte_node = dynamic_cast<CTENode *>(query_node)) {
		duckpgq_find_query_node_parser_override(cte_node->child.get());
		return;
	}

	if (auto *set_operation_node = dynamic_cast<SetOperationNode *>(query_node)) {
		for (auto &child : set_operation_node->children) {
			duckpgq_find_query_node_parser_override(child.get());
		}
		return;
	}
}

static void duckpgq_find_match_expression_parser_override(MatchExpression *match_expression) {
	if (!match_expression) {
		return;
	}
	for (auto &expr : match_expression->column_list) {
		duckpgq_find_expression_parser_override(expr.get());
	}
	duckpgq_find_expression_parser_override(match_expression->where_clause.get());
	for (auto &union_match : match_expression->union_matches) {
		duckpgq_find_match_expression_parser_override(union_match.get());
	}
}

static void duckpgq_find_expression_parser_override(ParsedExpression *expr) {
	if (!expr) {
		return;
	}
	if (auto *subquery_expr = dynamic_cast<SubqueryExpression *>(expr)) {
		if (subquery_expr->subquery) {
			duckpgq_find_query_node_parser_override(subquery_expr->subquery->node.get());
		}
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		duckpgq_find_expression_parser_override(child.get());
	});
}

static void duckpgq_find_query_node(QueryNode *query_node, DuckPGQState &duckpgq_state) {
	if (!query_node) {
		return;
	}

	for (auto const &kv_pair : query_node->cte_map.map) {
		auto const &cte = kv_pair.second;
		auto *cte_select_statement = dynamic_cast<SelectStatement *>(cte->query.get());
		if (!cte_select_statement) {
			continue;
		}
		duckpgq_find_query_node(cte_select_statement->node.get(), duckpgq_state);
	}

	if (auto *select_node = dynamic_cast<SelectNode *>(query_node)) {
		duckpgq_find_match_function(select_node->from_table.get(), duckpgq_state);
		for (auto &expr : select_node->select_list) {
			duckpgq_find_expression(expr.get(), duckpgq_state);
		}
		duckpgq_find_expression(select_node->where_clause.get(), duckpgq_state);
		return;
	}

	if (auto *cte_node = dynamic_cast<CTENode *>(query_node)) {
		duckpgq_find_query_node(cte_node->child.get(), duckpgq_state);
		return;
	}

	if (auto *set_operation_node = dynamic_cast<SetOperationNode *>(query_node)) {
		for (auto &child : set_operation_node->children) {
			duckpgq_find_query_node(child.get(), duckpgq_state);
		}
		return;
	}
}

static void duckpgq_find_match_expression(MatchExpression *match_expression, DuckPGQState &duckpgq_state) {
	if (!match_expression) {
		return;
	}
	for (auto &expr : match_expression->column_list) {
		duckpgq_find_expression(expr.get(), duckpgq_state);
	}
	duckpgq_find_expression(match_expression->where_clause.get(), duckpgq_state);
	for (auto &union_match : match_expression->union_matches) {
		duckpgq_find_match_expression(union_match.get(), duckpgq_state);
	}
}

static void duckpgq_find_expression(ParsedExpression *expr, DuckPGQState &duckpgq_state) {
	if (!expr) {
		return;
	}
	if (auto *subquery_expr = dynamic_cast<SubqueryExpression *>(expr)) {
		if (subquery_expr->subquery) {
			duckpgq_find_query_node(subquery_expr->subquery->node.get(), duckpgq_state);
		}
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		duckpgq_find_expression(child.get(), duckpgq_state);
	});
}

ParserExtensionPlanResult duckpgq_find_select_statement(SQLStatement *statement, DuckPGQState &duckpgq_state) {
	const auto select_statement = dynamic_cast<SelectStatement *>(statement);
	if (!select_statement) {
		return {};
	}

	if (auto node = dynamic_cast<SelectNode *>(select_statement->node.get())) {
		const auto describe_node = dynamic_cast<ShowRef *>(node->from_table.get());
		if (describe_node) {
			ParserExtensionPlanResult result;
			result.requires_valid_transaction = true;
			result.return_type = StatementReturnType::QUERY_RESULT;
			if (describe_node->show_type == ShowType::SUMMARY) {
				result.function = SummarizePropertyGraphFunction();
				result.parameters.push_back(Value(describe_node->table_name));
				return result;
			}
			if (describe_node->show_type == ShowType::DESCRIBE) {
				result.function = DescribePropertyGraphFunction();
				return result;
			}
			throw BinderException("Unknown show type %s found.", describe_node->show_type);
		}
	}

	duckpgq_find_query_node(select_statement->node.get(), duckpgq_state);
	return {};
}

ParserExtensionPlanResult duckpgq_handle_statement(SQLStatement *statement, DuckPGQState &duckpgq_state) {
	if (statement->type == StatementType::SELECT_STATEMENT) {
		auto result = duckpgq_find_select_statement(statement, duckpgq_state);
		if (result.function.bind == nullptr) {
			throw Exception(ExceptionType::BINDER, "use duckpgq_bind instead");
		}
		return result;
	}
	if (statement->type == StatementType::CREATE_STATEMENT) {
		const auto &create_statement = statement->Cast<CreateStatement>();
		const auto create_property_graph = dynamic_cast<CreatePropertyGraphInfo *>(create_statement.info.get());
		if (create_property_graph) {
			ParserExtensionPlanResult result;
			result.function = CreatePropertyGraphFunction();
			result.requires_valid_transaction = true;
			result.return_type = StatementReturnType::QUERY_RESULT;
			return result;
		}
		const auto create_table = reinterpret_cast<CreateTableInfo *>(create_statement.info.get());
		duckpgq_handle_statement(create_table->query.get(), duckpgq_state);
	}
	if (statement->type == StatementType::DROP_STATEMENT) {
		ParserExtensionPlanResult result;
		result.function = DropPropertyGraphFunction();
		result.requires_valid_transaction = true;
		result.return_type = StatementReturnType::QUERY_RESULT;
		return result;
	}
	if (statement->type == StatementType::EXPLAIN_STATEMENT) {
		auto &explain_statement = statement->Cast<ExplainStatement>();
		duckpgq_handle_statement(explain_statement.stmt.get(), duckpgq_state);
	}
	if (statement->type == StatementType::COPY_STATEMENT) {
		const auto &copy_statement = statement->Cast<CopyStatement>();
		const auto select_node = dynamic_cast<SelectNode *>(copy_statement.info->select_statement.get());
		duckpgq_find_match_function(select_node->from_table.get(), duckpgq_state);
		throw Exception(ExceptionType::BINDER, "use duckpgq_bind instead");
	}
	if (statement->type == StatementType::INSERT_STATEMENT) {
		const auto &insert_statement = statement->Cast<InsertStatement>();
		if (insert_statement.node) {
			duckpgq_handle_statement(insert_statement.node->select_statement.get(), duckpgq_state);
		}
	}

	throw Exception(ExceptionType::NOT_IMPLEMENTED,
	                StatementTypeToString(statement->type) + "has not been implemented yet for DuckPGQ queries");
}

ParserExtensionPlanResult duckpgq_plan(ParserExtensionInfo *, ClientContext &context,
                                       unique_ptr<ParserExtensionParseData> parse_data) {
	auto duckpgq_state = GetDuckPGQState(context);
	duckpgq_state->parse_data = std::move(parse_data);
	auto duckpgq_parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state->parse_data.get());

	if (!duckpgq_parse_data) {
		throw Exception(ExceptionType::BINDER, "No DuckPGQ parse data found");
	}

	auto statement = duckpgq_parse_data->statement.get();
	return duckpgq_handle_statement(statement, *duckpgq_state);
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CorePGQParser::RegisterPGQParserExtension(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &manager = ExtensionCallbackManager::Get(db);
	manager.Register(DuckPGQParserExtension());
}

} // namespace duckdb
