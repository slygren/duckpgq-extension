#include "duckpgq/core/parser/duckpgq_grammar.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/peg/ast/table_alias.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckpgq/parser/path_element.hpp"
#include "duckpgq/parser/path_pattern.hpp"
#include "duckpgq/parser/subpath_element.hpp"
#include "duckpgq_graph_table_grammar.hpp"

namespace duckdb {

// A rule whose body is a single choice parses to a list holding that choice; return the chosen alternative.
ParseResult &DuckPGQGetChoice(ParseResult &parse_result) {
	return parse_result.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
}

// Concatenate the keyword/operator tokens below a parse result, e.g. "<-" for the sequence '<' '-'.
static void CollectLiteralText(ParseResult &parse_result, string &text) {
	switch (parse_result.type) {
	case ParseResultType::KEYWORD:
		text += parse_result.Cast<KeywordParseResult>().keyword;
		break;
	case ParseResultType::LIST:
		for (auto &child : parse_result.Cast<ListParseResult>().GetChildren()) {
			CollectLiteralText(child.get(), text);
		}
		break;
	case ParseResultType::CHOICE:
		CollectLiteralText(parse_result.Cast<ChoiceParseResult>().GetResult(), text);
		break;
	case ParseResultType::OPTIONAL: {
		auto &optional = parse_result.Cast<OptionalParseResult>();
		if (optional.HasResult()) {
			CollectLiteralText(optional.GetResult(), text);
		}
		break;
	}
	default:
		break;
	}
}

string DuckPGQLiteralText(ParseResult &parse_result) {
	string text;
	CollectLiteralText(parse_result, text);
	return StringUtil::Upper(text);
}

namespace {

int64_t TransformNumber(PEGTransformer &transformer, ParseResult &parse_result) {
	auto expression = transformer.Transform<unique_ptr<ParsedExpression>>(parse_result);
	return expression->Cast<ConstantExpression>().GetLiteral().ToValue().GetValue<int64_t>();
}

PGQPathMode PathModeFromText(const string &text) {
	if (text == "WALK") {
		return PGQPathMode::WALK;
	}
	if (text == "TRAIL") {
		return PGQPathMode::TRAIL;
	}
	if (text == "SIMPLE") {
		return PGQPathMode::SIMPLE;
	}
	if (text == "ACYCLIC") {
		return PGQPathMode::ACYCLIC;
	}
	throw ParserException("Unsupported graph path mode %s", text);
}

PathElement &GetSinglePathElement(PathReference &path_reference) {
	if (path_reference.path_reference_type == PGQPathReferenceType::PATH_ELEMENT) {
		return static_cast<PathElement &>(path_reference);
	}
	auto &subpath = static_cast<SubPath &>(path_reference);
	if (subpath.path_list.size() != 1 ||
	    subpath.path_list[0]->path_reference_type != PGQPathReferenceType::PATH_ELEMENT) {
		throw ParserException("Expected a single graph path element");
	}
	return static_cast<PathElement &>(*subpath.path_list[0]);
}

struct LabelText {
	string text;
	//! The top-level operator of the text, if any: '|', '&' or '!'.
	char op = '\0';
};

LabelText TransformLabelExpression(PEGTransformer &transformer, ParseResult &parse_result);

LabelText NegateLabel(const LabelText &operand) {
	LabelText result;
	result.text = "!" + (operand.op == '|' || operand.op == '&' ? "(" + operand.text + ")" : operand.text);
	result.op = '!';
	return result;
}

// DuckPGQLabelNegation / DuckPGQLabelParens / ColId
LabelText TransformLabelFactor(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &choice = DuckPGQGetChoice(parse_result);
	if (choice.Name() == "DuckPGQLabelNegation") {
		return NegateLabel(TransformLabelFactor(transformer, choice.Cast<ListParseResult>().GetChild(1)));
	}
	if (choice.Name() == "DuckPGQLabelParens") {
		return TransformLabelExpression(transformer, choice.Cast<ListParseResult>().GetChild(1));
	}
	LabelText result;
	result.text = transformer.Transform<Identifier>(choice).GetIdentifierName();
	return result;
}

// Joins operands with op. Negations are always parenthesized so the label-string reader, which checks for a leading
// '!' before splitting, cannot read "!a|b" as "!(a|b)"; an '|' operand of '&' needs parentheses for precedence.
LabelText JoinLabels(const vector<LabelText> &operands, char op) {
	if (operands.size() == 1) {
		return operands[0];
	}
	LabelText result;
	for (auto &operand : operands) {
		auto parenthesize = operand.op == '!' || (op == '&' && operand.op == '|');
		result.text += (result.text.empty() ? "" : string(1, op)) +
		               (parenthesize ? "(" + operand.text + ")" : operand.text);
	}
	result.op = op;
	return result;
}

// DuckPGQLabelFactor DuckPGQLabelAndTail*. negate_first applies a fused "|!" to the first factor.
LabelText TransformLabelTerm(PEGTransformer &transformer, ParseResult &parse_result, bool negate_first) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto first = TransformLabelFactor(transformer, list.GetChild(0));
	vector<LabelText> operands {negate_first ? NegateLabel(first) : first};
	auto &tail = list.Child<OptionalParseResult>(1);
	if (tail.HasResult()) {
		for (auto &entry : tail.GetResult().Cast<RepeatParseResult>().GetChildren()) {
			auto &choice = DuckPGQGetChoice(entry.get());
			auto operand = TransformLabelFactor(transformer, choice.Cast<ListParseResult>().GetChild(1));
			operands.push_back(choice.Name() == "DuckPGQLabelAndNegated" ? NegateLabel(operand) : operand);
		}
	}
	return JoinLabels(operands, '&');
}

// DuckPGQLabelTerm DuckPGQLabelOrTail*
LabelText TransformLabelExpression(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	vector<LabelText> operands;
	operands.push_back(TransformLabelTerm(transformer, list.GetChild(0), false));
	auto &tail = list.Child<OptionalParseResult>(1);
	if (tail.HasResult()) {
		for (auto &entry : tail.GetResult().Cast<RepeatParseResult>().GetChildren()) {
			auto &choice = DuckPGQGetChoice(entry.get());
			auto &term = choice.Cast<ListParseResult>().GetChild(1);
			operands.push_back(TransformLabelTerm(transformer, term, choice.Name() == "DuckPGQLabelOrNegated"));
		}
	}
	return JoinLabels(operands, '|');
}

// '(' Identifier? DuckPGQElementLabel? WhereClause? ')' or '[' Identifier? DuckPGQElementLabel? WhereClause? ']'
// An anonymous element binds a generated "_anon_<n>" variable; an unlabeled one keeps an empty label (any label).
unique_ptr<PathReference> TransformElement(PEGTransformer &transformer, ParseResult &parse_result,
                                           PGQMatchType match_type, idx_t &anon_counter) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto element = make_uniq<PathElement>(PGQPathReferenceType::PATH_ELEMENT);
	element->match_type = match_type;
	auto &variable = list.Child<OptionalParseResult>(1);
	if (variable.HasResult()) {
		element->variable_binding = transformer.Transform<Identifier>(variable.GetResult()).GetIdentifierName();
	} else {
		element->variable_binding = "_anon_" + to_string(anon_counter++);
	}
	auto &label = list.Child<OptionalParseResult>(2);
	if (label.HasResult()) {
		auto &label_list = label.GetResult().Cast<ListParseResult>();
		element->label = TransformLabelExpression(transformer, label_list.GetChild(1)).text;
	}
	auto &where_clause = list.Child<OptionalParseResult>(3);
	if (!where_clause.HasResult()) {
		return std::move(element);
	}
	// An element-local WHERE is represented as a single-element subpath carrying the condition.
	auto subpath = make_uniq<SubPath>(PGQPathReferenceType::SUBPATH);
	subpath->where_clause = transformer.Transform<unique_ptr<ParsedExpression>>(where_clause.GetResult());
	subpath->path_list.push_back(std::move(element));
	return std::move(subpath);
}

PGQMatchType EdgeDirection(const string &left, const string &right) {
	auto left_arrow = left == "<-";
	auto right_arrow = right == "->";
	if (left_arrow && right_arrow) {
		return PGQMatchType::MATCH_EDGE_LEFT_RIGHT;
	}
	if (left_arrow) {
		return PGQMatchType::MATCH_EDGE_LEFT;
	}
	if (right_arrow) {
		return PGQMatchType::MATCH_EDGE_RIGHT;
	}
	return PGQMatchType::MATCH_EDGE_ANY;
}

void ApplyQuantifier(PEGTransformer &transformer, optional_ptr<ParseResult> quantifier, const string &fused_quantifier,
                     SubPath &subpath) {
	auto unbounded = NumericLimits<int64_t>::Maximum();
	string text = fused_quantifier;
	if (quantifier) {
		auto &choice = DuckPGQGetChoice(*quantifier);
		if (choice.Name() == "DuckPGQFixedQuantifier") {
			auto fixed = TransformNumber(transformer, choice.Cast<ListParseResult>().GetChild(1));
			subpath.lower = fixed;
			subpath.upper = fixed;
			return;
		}
		if (choice.Name() == "DuckPGQRangeQuantifier") {
			auto &range = choice.Cast<ListParseResult>();
			auto &lower = range.Child<OptionalParseResult>(1);
			auto &upper = range.Child<OptionalParseResult>(3);
			subpath.lower = lower.HasResult() ? TransformNumber(transformer, lower.GetResult()) : 0;
			subpath.upper = upper.HasResult() ? TransformNumber(transformer, upper.GetResult()) : unbounded;
			if (subpath.lower > subpath.upper) {
				throw ConstraintException("Lower bound greater than upper bound");
			}
			return;
		}
		text = DuckPGQLiteralText(choice);
	}
	subpath.lower = text == "+" ? 1 : 0;
	subpath.upper = text == "?" ? 1 : unbounded;
}

// DuckPGQEdgeLeft DuckPGQEdgeBody DuckPGQEdgeTail
unique_ptr<PathReference> TransformQuantifiedEdge(PEGTransformer &transformer, ParseResult &parse_result,
                                                  idx_t &anon_counter) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto edge = TransformElement(transformer, list.GetChild(1), PGQMatchType::MATCH_EDGE_ANY, anon_counter);
	auto left = DuckPGQLiteralText(list.GetChild(0));

	// DuckPGQFusedTail / (DuckPGQEdgeRight DuckPGQQuantifier?)
	auto &tail = DuckPGQGetChoice(list.GetChild(2));
	string right;
	string fused_quantifier;
	optional_ptr<ParseResult> quantifier;
	bool has_quantifier;
	if (tail.Name() == "DuckPGQFusedTail") {
		auto fused = DuckPGQLiteralText(tail);
		right = fused.substr(0, fused.size() - 1);
		fused_quantifier = fused.substr(fused.size() - 1);
		has_quantifier = true;
	} else {
		auto &tail_list = tail.Cast<ListParseResult>();
		right = DuckPGQLiteralText(tail_list.GetChild(0));
		auto &optional_quantifier = tail_list.Child<OptionalParseResult>(1);
		has_quantifier = optional_quantifier.HasResult();
		if (has_quantifier) {
			quantifier = &optional_quantifier.GetResult();
		}
	}
	GetSinglePathElement(*edge).match_type = EdgeDirection(left, right);
	if (!has_quantifier) {
		return edge;
	}
	auto result = make_uniq<SubPath>(PGQPathReferenceType::SUBPATH);
	if (edge->path_reference_type == PGQPathReferenceType::SUBPATH) {
		// Fold an element-local WHERE into the quantified subpath instead of nesting two subpaths.
		auto &nested = static_cast<SubPath &>(*edge);
		result->where_clause = std::move(nested.where_clause);
		result->path_list.push_back(std::move(nested.path_list[0]));
	} else {
		result->path_list.push_back(std::move(edge));
	}
	ApplyQuantifier(transformer, quantifier, fused_quantifier, *result);
	return std::move(result);
}

// DuckPGQVertex DuckPGQEdgeVertex*
unique_ptr<PathPattern> TransformPathSequence(PEGTransformer &transformer, ParseResult &parse_result,
                                              idx_t &anon_counter) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto pattern = make_uniq<PathPattern>();
	pattern->path_elements.push_back(
	    TransformElement(transformer, list.GetChild(0), PGQMatchType::MATCH_VERTEX, anon_counter));
	auto &repeat = list.Child<OptionalParseResult>(1);
	if (!repeat.HasResult()) {
		return pattern;
	}
	for (auto &edge_vertex : repeat.GetResult().Cast<RepeatParseResult>().GetChildren()) {
		auto &pair = edge_vertex.get().Cast<ListParseResult>();
		pattern->path_elements.push_back(TransformQuantifiedEdge(transformer, pair.GetChild(0), anon_counter));
		pattern->path_elements.push_back(
		    TransformElement(transformer, pair.GetChild(1), PGQMatchType::MATCH_VERTEX, anon_counter));
	}
	return pattern;
}

// DuckPGQPathVariable? DuckPGQSearchPrefix? DuckPGQPathMode? DuckPGQPathSequence
unique_ptr<PathPattern> TransformPathPattern(PEGTransformer &transformer, ParseResult &parse_result,
                                             idx_t &anon_counter) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto pattern = TransformPathSequence(transformer, list.GetChild(3), anon_counter);
	auto &prefix = list.Child<OptionalParseResult>(1);
	if (prefix.HasResult()) {
		auto &choice = DuckPGQGetChoice(prefix.GetResult());
		pattern->shortest = true;
		pattern->all = choice.Name() == "DuckPGQAllShortest";
		if (choice.Name() == "DuckPGQTopKShortest") {
			auto k = TransformNumber(transformer, choice.Cast<ListParseResult>().GetChild(1));
			if (k <= 0) {
				throw ParserException("SHORTEST k requires a positive k");
			}
			pattern->topk = NumericCast<int32_t>(k);
		}
	}
	auto &variable = list.Child<OptionalParseResult>(0);
	auto &mode = list.Child<OptionalParseResult>(2);
	if (!variable.HasResult() && !mode.HasResult()) {
		return pattern;
	}
	// A named or mode-restricted path wraps the whole sequence in one subpath.
	auto subpath = make_uniq<SubPath>(PGQPathReferenceType::SUBPATH);
	if (variable.HasResult()) {
		auto &variable_list = variable.GetResult().Cast<ListParseResult>();
		subpath->path_variable = transformer.Transform<Identifier>(variable_list.GetChild(0)).GetIdentifierName();
	}
	if (mode.HasResult()) {
		subpath->path_mode = PathModeFromText(DuckPGQLiteralText(mode.GetResult()));
	}
	subpath->path_list = std::move(pattern->path_elements);
	pattern->path_elements.clear();
	pattern->path_elements.push_back(std::move(subpath));
	return pattern;
}

void CollectVertexStars(PathReference &path_reference, vector<unique_ptr<ParsedExpression>> &columns) {
	if (path_reference.path_reference_type == PGQPathReferenceType::SUBPATH) {
		for (auto &child : static_cast<SubPath &>(path_reference).path_list) {
			CollectVertexStars(*child, columns);
		}
		return;
	}
	auto &element = static_cast<PathElement &>(path_reference);
	if (element.match_type == PGQMatchType::MATCH_VERTEX) {
		columns.push_back(make_uniq<StarExpression>(Identifier(element.variable_binding)));
	}
}

// DuckPGQGraphTableKeyword '(' QualifiedName 'MATCH' List(DuckPGQPathPattern) DuckPGQOptionalMatch* WhereClause?
// DuckPGQColumns? ')' TableAlias?
arena_ptr<TransformResultValue> TransformGraphTable(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto match = make_uniq<MatchExpression>();
	auto graph_name = transformer.Transform<QualifiedName>(list.GetChild(2));
	match->pg_name = graph_name.Name().GetIdentifierName();
	match->pg_catalog = graph_name.Catalog().GetIdentifierName();
	match->pg_schema = graph_name.Schema().GetIdentifierName();
	// Generated variables are numbered across the mandatory and optional patterns of one MATCH.
	idx_t anon_counter = 0;
	for (auto &item : PEGTransformerFactory::ExtractParseResultsFromList(list.GetChild(4))) {
		match->path_patterns.push_back(TransformPathPattern(transformer, item.get(), anon_counter));
	}
	auto &optional_matches = list.Child<OptionalParseResult>(5);
	if (optional_matches.HasResult()) {
		for (auto &optional_match : optional_matches.GetResult().Cast<RepeatParseResult>().GetChildren()) {
			auto &optional_list = optional_match.get().Cast<ListParseResult>();
			vector<unique_ptr<PathPattern>> group;
			for (auto &item : PEGTransformerFactory::ExtractParseResultsFromList(optional_list.GetChild(2))) {
				group.push_back(TransformPathPattern(transformer, item.get(), anon_counter));
			}
			match->optional_match_patterns.push_back(std::move(group));
		}
	}
	auto &where_clause = list.Child<OptionalParseResult>(6);
	if (where_clause.HasResult()) {
		match->where_clause = transformer.Transform<unique_ptr<ParsedExpression>>(where_clause.GetResult());
	}
	auto &columns = list.Child<OptionalParseResult>(7);
	if (columns.HasResult()) {
		auto &target_list =
		    PEGTransformerFactory::ExtractResultFromParens(columns.GetResult().Cast<ListParseResult>().GetChild(1));
		match->column_list = transformer.Transform<vector<unique_ptr<ParsedExpression>>>(target_list);
	} else {
		// Without COLUMNS, expose every column of every vertex binding in the pattern.
		for (auto &path_pattern : match->path_patterns) {
			for (auto &path_reference : path_pattern->path_elements) {
				CollectVertexStars(*path_reference, match->column_list);
			}
		}
	}

	auto result = make_uniq<TableFunctionRef>();
	auto &alias = list.Child<OptionalParseResult>(9);
	if (alias.HasResult()) {
		auto table_alias = transformer.Transform<TableAlias>(alias.GetResult());
		match->alias = table_alias.name.GetIdentifierName();
		result->alias = table_alias.name;
		result->column_name_alias = std::move(table_alias.column_name_alias);
	}
	// The pattern travels in bind_info rather than as an argument, so DuckDB never tries to bind it as a scalar.
	result->function =
	    make_uniq<FunctionExpression>(Identifier("duckpgq_match"), vector<unique_ptr<ParsedExpression>>());
	result->bind_info = make_shared_ptr<DuckPGQMatchInfo>(std::move(match));
	unique_ptr<TableRef> table_ref = std::move(result);
	return transformer.MakeResult<unique_ptr<TableRef>>(std::move(table_ref));
}

arena_ptr<TransformProcess> StartGraphTableTransform(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, TransformGraphTable);
}

} // namespace

void DuckPGQAddGraphTableGrammar(vector<GrammarChange> &changes) {
	for (auto rule : DUCKPGQ_GRAPH_TABLE_RULES) {
		changes.push_back(GrammarChange::AddRule(rule));
	}
	changes.push_back(GrammarChange::SetTransformProcess("DuckPGQGraphTable", StartGraphTableTransform));
	// Tried before the native table references, so GRAPH_TABLE(...) is not read as a table-function call.
	changes.push_back(GrammarChange::PrependChoice("InnerTableRef", "DuckPGQGraphTable"));
}

} // namespace duckdb
