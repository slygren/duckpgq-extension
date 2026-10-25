#include "duckpgq/core/parser/duckpgq_grammar.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/peg/ast/table_alias.hpp"
#include "duckdb/parser/peg/matcher.hpp"
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

//! Names for anonymous elements: "<prefix><n>", numbered in pattern order within one scope.
struct AnonymousNames {
	explicit AnonymousNames(string prefix_p) : prefix(std::move(prefix_p)) {
	}
	string Next() {
		return prefix + to_string(next++);
	}
	string prefix;
	idx_t next = 0;
};

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

// '(' Identifier? DuckPGQElementLabel? WhereClause? DuckPGQCost? ')', or between '[' and ']' with
// DuckPGQCypherQuantifier? after the label. An edge body passes cypher_quantifier to receive that quantifier.
// An anonymous element binds a generated variable; an unlabeled one keeps an empty label (any label).
unique_ptr<PathReference> TransformElement(PEGTransformer &transformer, ParseResult &parse_result,
                                           PGQMatchType match_type, AnonymousNames &anon_names,
                                           optional_ptr<optional_ptr<ParseResult>> cypher_quantifier = nullptr) {
	auto &list = parse_result.Cast<ListParseResult>();
	idx_t offset = 0;
	if (cypher_quantifier) {
		auto &quantifier = list.Child<OptionalParseResult>(3);
		*cypher_quantifier = quantifier.HasResult() ? &quantifier.GetResult() : nullptr;
		offset = 1;
	}
	auto element = make_uniq<PathElement>(PGQPathReferenceType::PATH_ELEMENT);
	element->match_type = match_type;
	auto &variable = list.Child<OptionalParseResult>(1);
	if (variable.HasResult()) {
		element->variable_binding = transformer.Transform<Identifier>(variable.GetResult()).GetIdentifierName();
	} else {
		element->variable_binding = anon_names.Next();
	}
	auto &label = list.Child<OptionalParseResult>(2);
	if (label.HasResult()) {
		auto &label_list = label.GetResult().Cast<ListParseResult>();
		element->label = TransformLabelExpression(transformer, label_list.GetChild(1)).text;
	}
	auto &where_clause = list.Child<OptionalParseResult>(3 + offset);
	auto &cost = list.Child<OptionalParseResult>(4 + offset);
	if (!where_clause.HasResult() && !cost.HasResult()) {
		return std::move(element);
	}
	// An element-local WHERE or COST is represented as a single-element subpath carrying it.
	auto subpath = make_uniq<SubPath>(PGQPathReferenceType::SUBPATH);
	if (where_clause.HasResult()) {
		subpath->where_clause = transformer.Transform<unique_ptr<ParsedExpression>>(where_clause.GetResult());
	}
	if (cost.HasResult()) {
		// 'COST' Expression DuckPGQCostDefault?, with DuckPGQCostDefault <- 'DEFAULT' NumberLiteral
		auto &cost_list = cost.GetResult().Cast<ListParseResult>();
		subpath->cost_expr = transformer.Transform<unique_ptr<ParsedExpression>>(cost_list.GetChild(1));
		auto &cost_default = cost_list.Child<OptionalParseResult>(2);
		if (cost_default.HasResult()) {
			auto default_value = transformer.Transform<unique_ptr<ParsedExpression>>(
			    cost_default.GetResult().Cast<ListParseResult>().GetChild(1));
			subpath->default_value =
			    default_value->Cast<ConstantExpression>().GetLiteral().ToValue().GetValue<double>();
			subpath->has_default = true;
		}
	}
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

int64_t ParseCypherBound(const string &text, const string &quantifier) {
	if (text.size() > 18) {
		throw ParserException("Quantifier bound in *%s is too large", quantifier);
	}
	return std::stoll(text);
}

// '*' DuckPGQCypherRange*: Cypher's variable-length quantifier, read with Cypher's meaning. '*' alone is one or more
// hops; '*n' exactly n; '*a..b' from a to b; a missing lower bound is 1 and a missing upper bound unbounded.
void ApplyCypherQuantifier(ParseResult &parse_result, SubPath &subpath) {
	string text;
	auto &ranges = parse_result.Cast<ListParseResult>().Child<OptionalParseResult>(1);
	if (ranges.HasResult()) {
		for (auto &range : ranges.GetResult().Cast<RepeatParseResult>().GetChildren()) {
			auto &number = range.get().Cast<NumberParseResult>().number;
			// "1 2" would read as 12: two bounds must be separated by "..".
			if (!text.empty() && !number.empty() && StringUtil::CharacterIsDigit(text.back()) &&
			    StringUtil::CharacterIsDigit(number.front())) {
				throw ParserException("Invalid quantifier *%s %s: write *n, *a..b, *..b or *a..", text, number);
			}
			text += number;
		}
	}
	auto unbounded = NumericLimits<int64_t>::Maximum();
	auto dots = text.find("..");
	if (dots == string::npos) {
		subpath.lower = text.empty() ? 1 : ParseCypherBound(text, text);
		subpath.upper = text.empty() ? unbounded : subpath.lower;
		return;
	}
	auto lower = text.substr(0, dots);
	auto upper = text.substr(dots + 2);
	subpath.lower = lower.empty() ? 1 : ParseCypherBound(lower, text);
	subpath.upper = upper.empty() ? unbounded : ParseCypherBound(upper, text);
	if (subpath.lower > subpath.upper) {
		throw ConstraintException("Lower bound greater than upper bound");
	}
}

// DuckPGQEdgeLeft DuckPGQEdgeBody DuckPGQEdgeTail
unique_ptr<PathReference> TransformQuantifiedEdge(PEGTransformer &transformer, ParseResult &parse_result,
                                                  AnonymousNames &anon_names) {
	auto &list = parse_result.Cast<ListParseResult>();
	optional_ptr<ParseResult> cypher_quantifier;
	auto edge =
	    TransformElement(transformer, list.GetChild(1), PGQMatchType::MATCH_EDGE_ANY, anon_names, &cypher_quantifier);
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
	if (cypher_quantifier && has_quantifier) {
		throw ParserException("An edge takes one quantifier: write -[:R*1..5]-> or -[:R]->{1,5}, not both");
	}
	if (!has_quantifier && !cypher_quantifier) {
		return edge;
	}
	auto result = make_uniq<SubPath>(PGQPathReferenceType::SUBPATH);
	if (edge->path_reference_type == PGQPathReferenceType::SUBPATH) {
		// Fold an element-local WHERE or COST into the quantified subpath instead of nesting two subpaths.
		auto &nested = static_cast<SubPath &>(*edge);
		result->where_clause = std::move(nested.where_clause);
		result->cost_expr = std::move(nested.cost_expr);
		result->default_value = nested.default_value;
		result->has_default = nested.has_default;
		result->path_list.push_back(std::move(nested.path_list[0]));
	} else {
		result->path_list.push_back(std::move(edge));
	}
	if (cypher_quantifier) {
		ApplyCypherQuantifier(*cypher_quantifier, *result);
	} else {
		ApplyQuantifier(transformer, quantifier, fused_quantifier, *result);
	}
	return std::move(result);
}

// DuckPGQAbbreviatedFused / (DuckPGQAbbreviatedArrow DuckPGQQuantifier?)
unique_ptr<PathReference> TransformAbbreviatedEdge(PEGTransformer &transformer, ParseResult &parse_result,
                                                   AnonymousNames &anon_names) {
	auto element = make_uniq<PathElement>(PGQPathReferenceType::PATH_ELEMENT);
	element->variable_binding = anon_names.Next();
	auto &edge = DuckPGQGetChoice(parse_result);
	string arrow;
	string fused_quantifier;
	optional_ptr<ParseResult> quantifier;
	if (edge.Name() == "DuckPGQAbbreviatedFused") {
		auto fused = DuckPGQLiteralText(edge);
		arrow = fused.substr(0, fused.size() - 1);
		fused_quantifier = "*";
	} else {
		auto &edge_list = edge.Cast<ListParseResult>();
		arrow = DuckPGQLiteralText(edge_list.GetChild(0));
		auto &optional_quantifier = edge_list.Child<OptionalParseResult>(1);
		if (optional_quantifier.HasResult()) {
			quantifier = &optional_quantifier.GetResult();
		}
	}
	element->match_type = EdgeDirection(arrow.substr(0, 2) == "<-" ? "<-" : "-", arrow.back() == '>' ? "->" : "-");
	if (!quantifier && fused_quantifier.empty()) {
		return std::move(element);
	}
	auto result = make_uniq<SubPath>(PGQPathReferenceType::SUBPATH);
	result->path_list.push_back(std::move(element));
	ApplyQuantifier(transformer, quantifier, fused_quantifier, *result);
	return std::move(result);
}

// DuckPGQVertex DuckPGQEdgeVertex*
unique_ptr<PathPattern> TransformPathSequence(PEGTransformer &transformer, ParseResult &parse_result,
                                              AnonymousNames &anon_names) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto pattern = make_uniq<PathPattern>();
	pattern->path_elements.push_back(
	    TransformElement(transformer, list.GetChild(0), PGQMatchType::MATCH_VERTEX, anon_names));
	auto &repeat = list.Child<OptionalParseResult>(1);
	if (!repeat.HasResult()) {
		return pattern;
	}
	for (auto &edge_vertex : repeat.GetResult().Cast<RepeatParseResult>().GetChildren()) {
		auto &pair = edge_vertex.get().Cast<ListParseResult>();
		auto &edge = DuckPGQGetChoice(pair.GetChild(0));
		if (edge.Name() == "DuckPGQAbbreviatedEdge") {
			pattern->path_elements.push_back(TransformAbbreviatedEdge(transformer, edge, anon_names));
		} else {
			pattern->path_elements.push_back(TransformQuantifiedEdge(transformer, edge, anon_names));
		}
		pattern->path_elements.push_back(
		    TransformElement(transformer, pair.GetChild(1), PGQMatchType::MATCH_VERTEX, anon_names));
	}
	return pattern;
}

// DuckPGQPathVariable? DuckPGQSearchPrefix? DuckPGQPathMode? DuckPGQPathSequence
unique_ptr<PathPattern> TransformPathPattern(PEGTransformer &transformer, ParseResult &parse_result,
                                             AnonymousNames &anon_names) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto pattern = TransformPathSequence(transformer, list.GetChild(3), anon_names);
	auto &prefix = list.Child<OptionalParseResult>(1);
	if (prefix.HasResult()) {
		auto &choice = DuckPGQGetChoice(prefix.GetResult());
		// CHEAPEST is a shortest path by cost rather than by hop count.
		pattern->shortest = true;
		pattern->cheapest = choice.Name() == "DuckPGQAllCheapest" || choice.Name() == "DuckPGQAnyCheapest";
		pattern->all = choice.Name() == "DuckPGQAllShortest" || choice.Name() == "DuckPGQAllCheapest";
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

vector<unique_ptr<PathPattern>> TransformPatternList(PEGTransformer &transformer, ParseResult &parse_result,
                                                     AnonymousNames &anon_names) {
	vector<unique_ptr<PathPattern>> patterns;
	for (auto &item : PEGTransformerFactory::ExtractParseResultsFromList(parse_result)) {
		patterns.push_back(TransformPathPattern(transformer, item.get(), anon_names));
	}
	return patterns;
}

// 'ONE' 'ROW' 'PER' (DuckPGQOneRowPerVertex / DuckPGQOneRowPerEdge / DuckPGQOneRowPerStep)
void TransformOneRowPer(PEGTransformer &transformer, ParseResult &parse_result, MatchExpression &match) {
	auto &target = DuckPGQGetChoice(parse_result.Cast<ListParseResult>().GetChild(3));
	auto &variables = PEGTransformerFactory::ExtractResultFromParens(target.Cast<ListParseResult>().GetChild(1));
	if (target.Name() == "DuckPGQOneRowPerStep") {
		match.one_row_per = OneRowPerMode::STEP;
		for (auto &variable : PEGTransformerFactory::ExtractParseResultsFromList(variables)) {
			match.one_row_per_vars.push_back(transformer.Transform<Identifier>(variable.get()).GetIdentifierName());
		}
		if (match.one_row_per_vars.size() != 3) {
			throw ParserException("ONE ROW PER STEP takes three variables: (vertex, edge, vertex)");
		}
		return;
	}
	match.one_row_per = target.Name() == "DuckPGQOneRowPerVertex" ? OneRowPerMode::VERTEX : OneRowPerMode::EDGE;
	match.one_row_per_vars.push_back(transformer.Transform<Identifier>(variables).GetIdentifierName());
}

// 'MATCH' List(DuckPGQPathPattern) DuckPGQOptionalMatch* DuckPGQOneRowPer? WhereClause?
void TransformMatchBlock(PEGTransformer &transformer, ParseResult &parse_result, MatchExpression &match) {
	auto &list = parse_result.Cast<ListParseResult>();
	// Generated variables are numbered across the mandatory and optional patterns of one MATCH block.
	AnonymousNames anon_names("_anon_");
	match.path_patterns = TransformPatternList(transformer, list.GetChild(1), anon_names);
	auto &optional_matches = list.Child<OptionalParseResult>(2);
	if (optional_matches.HasResult()) {
		for (auto &optional_match : optional_matches.GetResult().Cast<RepeatParseResult>().GetChildren()) {
			auto &optional_list = optional_match.get().Cast<ListParseResult>();
			match.optional_match_patterns.push_back(
			    TransformPatternList(transformer, optional_list.GetChild(2), anon_names));
		}
	}
	auto &one_row_per = list.Child<OptionalParseResult>(3);
	if (one_row_per.HasResult()) {
		TransformOneRowPer(transformer, one_row_per.GetResult(), match);
	}
	auto &where_clause = list.Child<OptionalParseResult>(4);
	if (where_clause.HasResult()) {
		match.where_clause = transformer.Transform<unique_ptr<ParsedExpression>>(where_clause.GetResult());
	}
}

// DuckPGQColumns <- 'COLUMNS' Parens(TargetList)
vector<unique_ptr<ParsedExpression>> TransformColumns(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &target_list = PEGTransformerFactory::ExtractResultFromParens(parse_result.Cast<ListParseResult>().GetChild(1));
	return transformer.Transform<vector<unique_ptr<ParsedExpression>>>(target_list);
}

// DuckPGQGraphTableKeyword '(' QualifiedName DuckPGQMatchBlock DuckPGQColumns? DuckPGQUnionBranch* DuckPGQColumns? ')'
// TableAlias?
arena_ptr<TransformResultValue> TransformGraphTable(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto match = make_uniq<MatchExpression>();
	auto graph_name = transformer.Transform<QualifiedName>(list.GetChild(2));
	match->pg_name = graph_name.Name().GetIdentifierName();
	match->pg_catalog = graph_name.Catalog().GetIdentifierName();
	match->pg_schema = graph_name.Schema().GetIdentifierName();
	TransformMatchBlock(transformer, list.GetChild(3), *match);
	// The COLUMNS written after each MATCH block, in block order (null where a block has none), and after the last.
	vector<optional_ptr<ParseResult>> block_columns;
	auto &head_columns = list.Child<OptionalParseResult>(4);
	block_columns.push_back(head_columns.HasResult() ? &head_columns.GetResult() : nullptr);
	auto &union_branches = list.Child<OptionalParseResult>(5);
	if (union_branches.HasResult()) {
		// 'UNION' 'ALL'? DuckPGQMatchBlock DuckPGQColumns?
		for (auto &branch : union_branches.GetResult().Cast<RepeatParseResult>().GetChildren()) {
			auto &branch_list = branch.get().Cast<ListParseResult>();
			auto branch_match = make_uniq<MatchExpression>();
			branch_match->pg_name = match->pg_name;
			branch_match->pg_catalog = match->pg_catalog;
			branch_match->pg_schema = match->pg_schema;
			TransformMatchBlock(transformer, branch_list.GetChild(2), *branch_match);
			match->union_all_flags.push_back(branch_list.Child<OptionalParseResult>(1).HasResult());
			match->union_matches.push_back(std::move(branch_match));
			auto &branch_columns = branch_list.Child<OptionalParseResult>(3);
			block_columns.push_back(branch_columns.HasResult() ? &branch_columns.GetResult() : nullptr);
		}
	}
	auto &final_columns = list.Child<OptionalParseResult>(6);
	if (final_columns.HasResult()) {
		if (block_columns.back()) {
			throw ParserException("GRAPH_TABLE takes one COLUMNS clause after its last MATCH block, not two");
		}
		block_columns.back() = &final_columns.GetResult();
	}
	// Branch-local COLUMNS: a COLUMNS clause before the last block means every block has its own. Otherwise the
	// COLUMNS after the last block (if any) is shared by all of them.
	bool branch_local = false;
	for (idx_t i = 0; i + 1 < block_columns.size(); i++) {
		branch_local |= block_columns[i] != nullptr;
	}
	if (branch_local) {
		for (idx_t i = 0; i < block_columns.size(); i++) {
			if (!block_columns[i]) {
				throw ParserException("MATCH block %llu of the UNION has no COLUMNS: when one block of a UNION has its "
				                      "own COLUMNS, every block needs one (or write a single COLUMNS after the last "
				                      "block, shared by all)",
				                      static_cast<unsigned long long>(i + 1));
			}
		}
		match->column_list = TransformColumns(transformer, *block_columns[0]);
		for (idx_t i = 0; i < match->union_matches.size(); i++) {
			match->union_matches[i]->column_list = TransformColumns(transformer, *block_columns[i + 1]);
		}
	} else if (block_columns.back()) {
		match->column_list = TransformColumns(transformer, *block_columns.back());
	} else {
		// Without COLUMNS, expose every column of every vertex binding in the pattern.
		for (auto &path_pattern : match->path_patterns) {
			for (auto &path_reference : path_pattern->path_elements) {
				CollectVertexStars(*path_reference, match->column_list);
			}
		}
	}

	auto result = make_uniq<TableFunctionRef>();
	auto &alias = list.Child<OptionalParseResult>(8);
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

// ColId 'IS' 'NOT'? 'LABELED' ColId
arena_ptr<TransformResultValue> TransformIsLabeled(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ColumnRefExpression>(transformer.Transform<Identifier>(list.GetChild(0))));
	children.push_back(
	    make_uniq<ConstantExpression>(
	    Literal::String(transformer.Transform<Identifier>(list.GetChild(4)).GetIdentifierName())));
	auto name = list.Child<OptionalParseResult>(2).HasResult() ? "not_labeled" : "is_labeled";
	unique_ptr<ParsedExpression> result = make_uniq<FunctionExpression>(Identifier(name), std::move(children));
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(result));
}

// 'EXISTS' '{' List(DuckPGQPathPattern) WhereClause? '}'
// Becomes EXISTS (SELECT 1 FROM duckpgq_exists_pattern()) carrying the pattern in bind_info. Inside a GRAPH_TABLE
// the MATCH expansion rewrites it into a correlated subquery; anywhere else the table function rejects it.
arena_ptr<TransformResultValue> TransformExistsPattern(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto match = make_uniq<MatchExpression>();
	// A separate prefix, so an anonymous inner element is never taken for an outer variable of the same name.
	AnonymousNames anon_names("_anon_exists_");
	match->path_patterns = TransformPatternList(transformer, list.GetChild(2), anon_names);
	auto &where_clause = list.Child<OptionalParseResult>(3);
	if (where_clause.HasResult()) {
		match->where_clause = transformer.Transform<unique_ptr<ParsedExpression>>(where_clause.GetResult());
	}
	auto pattern_ref = make_uniq<TableFunctionRef>();
	pattern_ref->function = make_uniq<FunctionExpression>(Identifier(DUCKPGQ_EXISTS_PATTERN_FUNCTION),
	                                                      vector<unique_ptr<ParsedExpression>>());
	pattern_ref->bind_info = make_shared_ptr<DuckPGQMatchInfo>(std::move(match));
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<ConstantExpression>(Literal::Integer(1)));
	select_node->from_table = std::move(pattern_ref);
	auto select = make_uniq<SelectStatement>();
	select->node = std::move(select_node);
	auto exists = make_uniq<SubqueryExpression>();
	exists->GetSubqueryTypeMutable() = SubqueryType::EXISTS;
	exists->SubqueryMutable() = std::move(select);
	unique_ptr<ParsedExpression> result = std::move(exists);
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(result));
}

// ColId DuckPGQProjectionLink? '.' '{' List(ColId) '}', with DuckPGQProjectionLink <- '.' ColId
arena_ptr<TransformResultValue> TransformPropertyProjection(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	vector<Identifier> prefix;
	prefix.push_back(transformer.Transform<Identifier>(list.GetChild(0)));
	auto &link = list.Child<OptionalParseResult>(1);
	if (link.HasResult()) {
		prefix.push_back(transformer.Transform<Identifier>(link.GetResult().Cast<ListParseResult>().GetChild(1)));
	}
	vector<FunctionArgument> fields;
	for (auto &item : PEGTransformerFactory::ExtractParseResultsFromList(list.GetChild(4))) {
		auto property = transformer.Transform<Identifier>(item.get());
		auto column_names = prefix;
		column_names.push_back(property);
		fields.emplace_back(property, make_uniq<ColumnRefExpression>(std::move(column_names)));
	}
	unique_ptr<ParsedExpression> result = make_uniq<FunctionExpression>(Identifier("struct_pack"), std::move(fields));
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(result));
}

template <arena_ptr<TransformResultValue> (*TRANSFORM)(PEGTransformer &, ParseResult &)>
arena_ptr<TransformProcess> StartTransform(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, TRANSFORM);
}

//! The bound of a Cypher quantifier: one token of digits with at most one "..", e.g. "2", "1..5", "..3", "1.." or "..".
//! The tokenizer reads such a run as one number token, but NumberLiteral accepts at most one '.'.
class DuckPGQCypherRangeMatcher : public AtomicMatcher {
public:
	DuckPGQCypherRangeMatcher() : AtomicMatcher(MatcherType::CUSTOM) {
		name = "DuckPGQCypherRange";
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		auto token = state.token_iterator.Current();
		if (!token || token->type != TokenType::NUMBER_LITERAL || !IsRange(token->text)) {
			return MatcherResult::Failure();
		}
		auto text = token->text;
		auto offset = optional_idx(token->offset);
		auto length = optional_idx(token->length);
		state.token_iterator.Advance();
		state.UpdateMaxTokenIndex();
		return state.AllocateParseResult<NumberParseResult>(std::move(text), offset, length);
	}

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		return "DuckPGQCypherRange";
	}

private:
	static bool IsRange(const string &text) {
		if (text.empty()) {
			return false;
		}
		idx_t dots = 0;
		for (idx_t i = 0; i < text.size(); i++) {
			if (text[i] == '.') {
				if (dots > 0 && text[i - 1] != '.') {
					return false;
				}
				dots++;
			} else if (!StringUtil::CharacterIsDigit(text[i])) {
				return false;
			}
		}
		return dots == 0 || dots == 2;
	}
};

} // namespace

optional_ptr<const MatchExpression> DuckPGQGetExistsPattern(const ParsedExpression &expression) {
	if (expression.GetExpressionClass() != ExpressionClass::SUBQUERY) {
		return nullptr;
	}
	auto &subquery = expression.Cast<SubqueryExpression>();
	if (subquery.GetSubqueryType() != SubqueryType::EXISTS || !subquery.Subquery() ||
	    subquery.Subquery()->node->type != QueryNodeType::SELECT_NODE) {
		return nullptr;
	}
	auto &from_table = subquery.Subquery()->node->Cast<SelectNode>().from_table;
	if (!from_table || from_table->type != TableReferenceType::TABLE_FUNCTION) {
		return nullptr;
	}
	auto &function_ref = from_table->Cast<TableFunctionRef>();
	auto match_info = dynamic_cast<const DuckPGQMatchInfo *>(function_ref.bind_info.get());
	auto &function_name = function_ref.function->Cast<FunctionExpression>().FunctionName();
	if (!match_info || function_name != Identifier(DUCKPGQ_EXISTS_PATTERN_FUNCTION)) {
		return nullptr;
	}
	return match_info->match.get();
}

optional_ptr<MatchExpression> DuckPGQGetExistsPattern(ParsedExpression &expression) {
	if (expression.GetExpressionClass() != ExpressionClass::SUBQUERY) {
		return nullptr;
	}
	auto &subquery = expression.Cast<SubqueryExpression>();
	if (subquery.GetSubqueryType() != SubqueryType::EXISTS || !subquery.Subquery() ||
	    subquery.Subquery()->node->type != QueryNodeType::SELECT_NODE) {
		return nullptr;
	}
	auto &from_table = subquery.Subquery()->node->Cast<SelectNode>().from_table;
	if (!from_table || from_table->type != TableReferenceType::TABLE_FUNCTION) {
		return nullptr;
	}
	auto &function_ref = from_table->Cast<TableFunctionRef>();
	auto match_info = dynamic_cast<DuckPGQMatchInfo *>(function_ref.bind_info.get());
	auto &function_name = function_ref.function->Cast<FunctionExpression>().FunctionName();
	if (!match_info || function_name != Identifier(DUCKPGQ_EXISTS_PATTERN_FUNCTION)) {
		return nullptr;
	}
	return match_info->match.get();
}

void DuckPGQAddGraphTableGrammar(vector<GrammarChange> &changes) {
	for (auto rule : DUCKPGQ_GRAPH_TABLE_RULES) {
		changes.push_back(GrammarChange::AddRule(rule));
	}
	changes.push_back(GrammarChange::AddTerminalRuleOverride(
	    "DuckPGQCypherRange", [](const PEGKeywordHelper &) -> unique_ptr<Matcher> {
		    return make_uniq<DuckPGQCypherRangeMatcher>();
	    }));
	changes.push_back(GrammarChange::SetTransformProcess("DuckPGQGraphTable", StartGraphTableTransform));
	// Tried before the native table references, so GRAPH_TABLE(...) is not read as a table-function call.
	changes.push_back(GrammarChange::PrependChoice("InnerTableRef", "DuckPGQGraphTable"));
	// Tried before the native expressions: each starts like a column reference or an EXISTS subquery.
	changes.push_back(GrammarChange::SetTransformProcess("DuckPGQIsLabeled", StartTransform<TransformIsLabeled>));
	changes.push_back(
	    GrammarChange::SetTransformProcess("DuckPGQExistsPattern", StartTransform<TransformExistsPattern>));
	changes.push_back(
	    GrammarChange::SetTransformProcess("DuckPGQPropertyProjection", StartTransform<TransformPropertyProjection>));
	changes.push_back(GrammarChange::PrependChoice("SingleExpression", "DuckPGQIsLabeled"));
	changes.push_back(GrammarChange::PrependChoice("SingleExpression", "DuckPGQExistsPattern"));
	changes.push_back(GrammarChange::PrependChoice("SingleExpression", "DuckPGQPropertyProjection"));
}

} // namespace duckdb
