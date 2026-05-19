#include <duckpgq_extension.hpp>
#include "duckpgq/core/functions/table/match.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/parser/tableref/matchref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"

#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"

#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/common/enums/subquery_type.hpp"
#include "duckdb/common/enums/joinref_type.hpp"
#include "duckpgq/core/utils/compressed_sparse_row.hpp"

#include "duckdb/parser/property_graph_table.hpp"
#include "duckdb/parser/subpath_element.hpp"
#include <duckdb/common/enums/set_operation_type.hpp>
#include <duckpgq/core/functions/table.hpp>
#include <duckpgq/core/utils/duckpgq_utils.hpp>

namespace duckdb {

namespace {

// Get fully-qualified column names for the property graph [tbl], and insert
// into set [col_names].
void PopulateFullyQualifiedColName(const vector<shared_ptr<PropertyGraphTable>> &tbls,
                                   const case_insensitive_map_t<vector<string>> &tbl_name_to_aliases,
                                   case_insensitive_set_t &col_names) {
	for (const auto &cur_tbl : tbls) {
		for (const auto &cur_col : cur_tbl->column_names) {
			// It's legal to query by `<col>` instead of `<table>.<col>`.
			col_names.insert(cur_col);

			const string &tbl_name = cur_tbl->table_name;
			auto iter = tbl_name_to_aliases.find(tbl_name);
			// Prefer to use table alias specified in the statement, otherwise use
			// table name.
			if (iter == tbl_name_to_aliases.end()) {
				col_names.insert(StringUtil::Format("%s.%s", tbl_name, cur_col));
			} else {
				const auto &all_aliases = iter->second;
				for (const auto &cur_alias : all_aliases) {
					col_names.insert(StringUtil::Format("%s.%s", cur_alias, cur_col));
				}
			}
		}
	}
}

// Get fully-qualified column names from property graph.
case_insensitive_set_t
GetFullyQualifiedColFromPg(const CreatePropertyGraphInfo &pg,
                           const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map) {
	case_insensitive_map_t<vector<string>> relation_name_to_aliases;
	for (const auto &entry : alias_map) {
		relation_name_to_aliases[entry.second->table_name].emplace_back(entry.first);
	}

	case_insensitive_set_t col_names;
	PopulateFullyQualifiedColName(pg.vertex_tables, relation_name_to_aliases, col_names);
	PopulateFullyQualifiedColName(pg.edge_tables, relation_name_to_aliases, col_names);
	return col_names;
}

// Get all fully-qualified column names from the given property graph [pg] for
// the given relation [alias], only vertex table is selected.
//
// Return column reference expressions which represent columns to select.
vector<unique_ptr<ColumnRefExpression>>
GetColRefExprFromPg(const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map, const std::string &alias) {
	vector<unique_ptr<ColumnRefExpression>> registered_col_names;
	auto iter = alias_map.find(alias);
	D_ASSERT(iter != alias_map.end());
	const auto &tbl = iter->second;
	registered_col_names.reserve(tbl->column_names.size());
	for (const auto &cur_col : tbl->column_names) {
		auto new_col_names = vector<string> {"", ""};
		new_col_names[0] = alias;
		new_col_names[1] = cur_col;
		registered_col_names.emplace_back(make_uniq<ColumnRefExpression>(std::move(new_col_names)));
	}
	return registered_col_names;
}

// Get all fully-qualified column names from the given property graph [pg] for
// all vertex relations.
//
// Return column reference expressions which represent columns to select.
vector<unique_ptr<ColumnRefExpression>>
GetColRefExprFromPg(const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map) {
	vector<unique_ptr<ColumnRefExpression>> registered_col_names;
	for (const auto &alias_and_table : alias_map) {
		const auto &alias = alias_and_table.first;
		const auto &tbl = alias_and_table.second;
		// Skip edge table.
		registered_col_names.reserve(registered_col_names.size() + tbl->column_names.size());
		for (const auto &cur_col : tbl->column_names) {
			auto new_col_names = vector<string> {"", ""};
			new_col_names[0] = alias;
			new_col_names[1] = cur_col;
			registered_col_names.emplace_back(make_uniq<ColumnRefExpression>(std::move(new_col_names)));
		}
	}
	return registered_col_names;
}

} // namespace

shared_ptr<PropertyGraphTable> PGQMatchFunction::FindGraphTable(const string &label,
                                                                CreatePropertyGraphInfo &pg_table) {
	const auto graph_table_entry = pg_table.label_map.find(label);
	if (graph_table_entry == pg_table.label_map.end()) {
		throw Exception(ExceptionType::BINDER,
		                "The label " + label + " is not registered in property graph " + pg_table.property_graph_name);
	}

	return graph_table_entry->second;
}

// Forward declaration
static shared_ptr<PropertyGraphTable> ResolveLabelExprTable(const string &raw_expr, PGQMatchType match_type,
                                                            CreatePropertyGraphInfo &pg_table);

// Resolve the table for a label expression string (simple or compound).
// Delegates to ResolveLabelExprTable which validates that compound expressions
// don't span multiple tables.
static shared_ptr<PropertyGraphTable> ResolveTable(const string &label, PGQMatchType match_type,
                                                   CreatePropertyGraphInfo &pg_table) {
	return ResolveLabelExprTable(label, match_type, pg_table);
}

// Returns true if the label expression string contains any compound operator.
static bool IsCompoundLabel(const string &label) {
	return !label.empty() && label.find_first_of("|&!") != string::npos;
}

// Split a top-level label expression by the given single-char operator (|  or &),
// respecting parentheses. Returns the parts; if the operator isn't found at top
// level, returns a single-element vector containing the original string.
static vector<string> SplitTopLevel(const string &expr, char op) {
	vector<string> parts;
	int depth = 0;
	string current;
	for (char c : expr) {
		if (c == '(') {
			depth++;
			current += c;
		} else if (c == ')') {
			depth--;
			current += c;
		} else if (c == op && depth == 0) {
			parts.push_back(current);
			current.clear();
		} else {
			current += c;
		}
	}
	parts.push_back(current);
	return parts;
}

// Remove outer parentheses if the entire string is wrapped in them.
static string StripParens(const string &s) {
	if (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
		return s.substr(1, s.size() - 2);
	}
	return s;
}

// Collect all edge PropertyGraphTables for a '|'-separated compound label.
// Only simple OR ("|") of leaf labels is supported; "&" and "!" are rejected.
static vector<shared_ptr<PropertyGraphTable>> CollectEdgeTablesFromLabel(const string &label,
                                                                          PGQMatchType /*match_type*/,
                                                                          CreatePropertyGraphInfo &pg_table) {
	auto or_parts = SplitTopLevel(label, '|');
	vector<shared_ptr<PropertyGraphTable>> result;
	for (auto &part : or_parts) {
		string leaf = StripParens(part);
		if (leaf.find_first_of("|&!") != string::npos) {
			throw NotImplementedException(
			    "Traversal with nested compound label expressions ('%s') is not supported.", label);
		}
		auto it = pg_table.label_map.find(leaf);
		if (it == pg_table.label_map.end()) {
			throw Exception(ExceptionType::BINDER,
			                "The label " + leaf + " is not registered in property graph " +
			                    pg_table.property_graph_name);
		}
		result.push_back(it->second);
	}
	return result;
}

// Build (SELECT src_fk AS src_pk, dst_fk AS dst_pk FROM tbl UNION ALL ...) __edges
// For undirected, also adds reversed-direction rows for each table.
static unique_ptr<SubqueryRef> BuildEdgeUnionSubquery(const vector<shared_ptr<PropertyGraphTable>> &edge_tables,
                                                       bool undirected) {
	D_ASSERT(!edge_tables.empty());

	auto make_sel = [](const shared_ptr<PropertyGraphTable> &etbl, bool reversed) -> unique_ptr<QueryNode> {
		auto node = make_uniq<SelectNode>();
		node->from_table = etbl->CreateBaseTableRef();
		auto src_col = make_uniq<ColumnRefExpression>(reversed ? etbl->destination_fk[0] : etbl->source_fk[0]);
		src_col->alias = "src_pk";
		auto dst_col = make_uniq<ColumnRefExpression>(reversed ? etbl->source_fk[0] : etbl->destination_fk[0]);
		dst_col->alias = "dst_pk";
		node->select_list.push_back(std::move(src_col));
		node->select_list.push_back(std::move(dst_col));
		return node;
	};

	vector<unique_ptr<QueryNode>> selects;
	for (const auto &etbl : edge_tables) {
		selects.push_back(make_sel(etbl, false));
		if (undirected) {
			selects.push_back(make_sel(etbl, true));
		}
	}

	// Chain into a left-associative UNION ALL tree
	unique_ptr<QueryNode> root = std::move(selects[0]);
	for (size_t i = 1; i < selects.size(); i++) {
		auto unode = make_uniq<SetOperationNode>();
		unode->setop_type = SetOperationType::UNION;
		unode->setop_all = true;
		unode->children.push_back(std::move(root));
		unode->children.push_back(std::move(selects[i]));
		root = std::move(unode);
	}

	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(root);
	return make_uniq<SubqueryRef>(std::move(stmt), "__edges");
}

// Build the discriminator bit-mask condition for a single leaf label on a known table.
// Returns nullptr if the label is the main_label (no condition needed — it's always true).
static unique_ptr<ParsedExpression> MakeSingleLabelCond(const string &leaf_label,
                                                         const shared_ptr<PropertyGraphTable> &tbl,
                                                         const string &var_alias) {
	if (tbl->main_label == leaf_label) {
		return nullptr; // No filtering needed; all rows in this table have this label.
	}
	const auto itr = std::find(tbl->sub_labels.begin(), tbl->sub_labels.end(), leaf_label);
	if (itr == tbl->sub_labels.end()) {
		throw BinderException("Label '%s' is not the main label or a sub-label of table '%s'", leaf_label,
		                      tbl->table_name);
	}
	const auto idx = static_cast<int32_t>(std::distance(tbl->sub_labels.begin(), itr));
	const auto bit = static_cast<int32_t>(std::pow(2, idx));

	vector<unique_ptr<ParsedExpression>> and_args;
	and_args.push_back(make_uniq<ColumnRefExpression>(tbl->discriminator, var_alias));
	and_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(bit)));
	auto bit_and = make_uniq<FunctionExpression>("&", std::move(and_args));
	return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(bit_and),
	                                       make_uniq<ConstantExpression>(Value::INTEGER(bit)));
}

// Forward declaration
static unique_ptr<ParsedExpression> BuildLabelExprCond(const string &expr,
                                                        const shared_ptr<PropertyGraphTable> &tbl,
                                                        const string &var_alias,
                                                        const string &pg_name);

// Build a condition for a compound label expression string, recursing as needed.
static unique_ptr<ParsedExpression> BuildLabelExprCond(const string &raw_expr,
                                                        const shared_ptr<PropertyGraphTable> &tbl,
                                                        const string &var_alias,
                                                        const string &pg_name) {
	string expr = StripParens(raw_expr);

	// NOT: starts with '!' (after stripping outer parens)
	if (!expr.empty() && expr[0] == '!') {
		auto inner = BuildLabelExprCond(StripParens(expr.substr(1)), tbl, var_alias, pg_name);
		if (!inner) {
			// "NOT always-true" → false constant (shouldn't happen in practice)
			return make_uniq<ConstantExpression>(Value::BOOLEAN(false));
		}
		return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(inner));
	}

	// OR: split by '|' at top level
	auto or_parts = SplitTopLevel(expr, '|');
	if (or_parts.size() > 1) {
		unique_ptr<ParsedExpression> result;
		for (auto &part : or_parts) {
			auto cond = BuildLabelExprCond(part, tbl, var_alias, pg_name);
			if (!cond) {
				// One branch is always true → the whole OR is always true
				return nullptr;
			}
			if (!result) {
				result = std::move(cond);
			} else {
				result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(result),
				                                          std::move(cond));
			}
		}
		return result;
	}

	// AND: split by '&' at top level
	auto and_parts = SplitTopLevel(expr, '&');
	if (and_parts.size() > 1) {
		unique_ptr<ParsedExpression> result;
		for (auto &part : and_parts) {
			auto cond = BuildLabelExprCond(part, tbl, var_alias, pg_name);
			if (!cond) {
				continue; // always-true branch contributes nothing to AND
			}
			if (!result) {
				result = std::move(cond);
			} else {
				result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(result),
				                                          std::move(cond));
			}
		}
		return result; // nullptr means all parts are always true
	}

	// Leaf label
	return MakeSingleLabelCond(expr, tbl, var_alias);
}

// Resolve the underlying PropertyGraphTable for a label expression.
// For compound OR/AND expressions, all leaf labels must map to the same table.
// Throws a clear error if labels span multiple tables.
static shared_ptr<PropertyGraphTable> ResolveLabelExprTable(const string &raw_expr, PGQMatchType match_type,
                                                             CreatePropertyGraphInfo &pg_table) {
	// Anonymous: fall back to sole-table resolution
	if (raw_expr.empty()) {
		bool is_vertex = (match_type == PGQMatchType::MATCH_VERTEX);
		const auto &tables = is_vertex ? pg_table.vertex_tables : pg_table.edge_tables;
		if (tables.size() == 1) {
			return tables[0];
		}
		throw Exception(ExceptionType::BINDER,
		                string("Unlabeled ") + (is_vertex ? "vertex" : "edge") +
		                    " pattern is ambiguous: property graph '" + pg_table.property_graph_name + "' has " +
		                    to_string(tables.size()) + (is_vertex ? " vertex" : " edge") +
		                    " tables. Specify a label.");
	}

	// Collect all leaf labels (strip leading '!' and whitespace/parens for lookup)
	// Strategy: collect unique table pointers from all leaves; they must all agree.
	std::function<shared_ptr<PropertyGraphTable>(const string &)> resolve_single =
	    [&](const string &expr) -> shared_ptr<PropertyGraphTable> {
		string e = StripParens(expr);
		if (!e.empty() && e[0] == '!') {
			return resolve_single(e.substr(1));
		}
		auto or_parts = SplitTopLevel(e, '|');
		if (or_parts.size() > 1) {
			shared_ptr<PropertyGraphTable> result;
			for (auto &part : or_parts) {
				auto tbl = resolve_single(part);
				if (!result) {
					result = tbl;
				} else if (result.get() != tbl.get()) {
					throw Exception(ExceptionType::BINDER,
					                "Cross-table label disjunctions are not yet supported. Labels '" + e +
					                    "' span multiple tables in property graph '" +
					                    pg_table.property_graph_name + "'.");
				}
			}
			return result;
		}
		auto and_parts = SplitTopLevel(e, '&');
		if (and_parts.size() > 1) {
			shared_ptr<PropertyGraphTable> result;
			for (auto &part : and_parts) {
				auto tbl = resolve_single(part);
				if (!result) {
					result = tbl;
				} else if (result.get() != tbl.get()) {
					throw Exception(ExceptionType::BINDER,
					                "Cross-table label conjunctions are not supported. Labels '" + e +
					                    "' span multiple tables in property graph '" +
					                    pg_table.property_graph_name + "'.");
				}
			}
			return result;
		}
		// Leaf
		const auto it = pg_table.label_map.find(e);
		if (it == pg_table.label_map.end()) {
			throw Exception(ExceptionType::BINDER,
			                "The label " + e + " is not registered in property graph " +
			                    pg_table.property_graph_name);
		}
		return it->second;
	};

	return resolve_single(raw_expr);
}

void PGQMatchFunction::CheckInheritance(const shared_ptr<PropertyGraphTable> &tableref, PathElement *element,
                                        vector<unique_ptr<ParsedExpression>> &conditions) {
	if (element->label.empty()) {
		return; // anonymous element — no label constraint
	}
	// Compound label expression: build via the generic label condition builder.
	if (IsCompoundLabel(element->label)) {
		auto cond = BuildLabelExprCond(element->label, tableref, element->variable_binding,
		                               tableref->table_name);
		if (cond) {
			conditions.push_back(std::move(cond));
		}
		return;
	}
	// Simple label: existing bit-mask logic.
	if (tableref->main_label == element->label) {
		return;
	}
	auto constant_expression_two = make_uniq<ConstantExpression>(Value::INTEGER(2));
	const auto itr = std::find(tableref->sub_labels.begin(), tableref->sub_labels.end(), element->label);

	const auto idx_of_label = std::distance(tableref->sub_labels.begin(), itr);
	auto constant_expression_idx_label =
	    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(idx_of_label)));

	vector<unique_ptr<ParsedExpression>> power_of_children;
	power_of_children.push_back(std::move(constant_expression_two));
	power_of_children.push_back(std::move(constant_expression_idx_label));
	auto power_of_term = make_uniq<FunctionExpression>("power", std::move(power_of_children));
	auto bigint_cast = make_uniq<CastExpression>(LogicalType::INTEGER, std::move(power_of_term));
	auto subcategory_colref = make_uniq<ColumnRefExpression>(tableref->discriminator, element->variable_binding);
	vector<unique_ptr<ParsedExpression>> and_children;
	and_children.push_back(std::move(subcategory_colref));
	and_children.push_back(std::move(bigint_cast));

	auto and_expression = make_uniq<FunctionExpression>("&", std::move(and_children));

	auto constant_expression_idx_label_comparison =
	    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(std::pow(2, idx_of_label))));

	auto subset_compare = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(and_expression),
	                                                      std::move(constant_expression_idx_label_comparison));
	conditions.push_back(std::move(subset_compare));
}

void PGQMatchFunction::CheckEdgeTableConstraints(const string &src_reference, const string &dst_reference,
                                                 const shared_ptr<PropertyGraphTable> &edge_table) {
	if (src_reference != edge_table->source_reference) {
		throw BinderException("Label %s is not registered as a source reference "
		                      "for edge pattern of table %s",
		                      src_reference, edge_table->table_name);
	}
	if (dst_reference != edge_table->destination_reference) {
		throw BinderException("Label %s is not registered as a destination "
		                      "reference for edge pattern of table %s",
		                      src_reference, edge_table->table_name);
	}
}

// Expand ALL_DIFFERENT(e1, e2, ..., en) into pairwise rowid inequality conjunctions.
// Recurses into AND/OR/NOT to handle ALL_DIFFERENT nested inside boolean expressions.
static unique_ptr<ParsedExpression> RewriteAllDifferent(unique_ptr<ParsedExpression> expr) {
	if (!expr) {
		return expr;
	}

	auto *func = dynamic_cast<FunctionExpression *>(expr.get());
	if (func && StringUtil::Lower(func->function_name) == "all_different") {
		vector<unique_ptr<ParsedExpression>> ne_exprs;
		for (idx_t i = 0; i < func->children.size(); i++) {
			for (idx_t j = i + 1; j < func->children.size(); j++) {
				auto *col_i = dynamic_cast<ColumnRefExpression *>(func->children[i].get());
				auto *col_j = dynamic_cast<ColumnRefExpression *>(func->children[j].get());
				if (!col_i || !col_j) {
					throw BinderException("ALL_DIFFERENT arguments must be element variable references");
				}
				string var_i = col_i->column_names.back();
				string var_j = col_j->column_names.back();
				ne_exprs.push_back(make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_NOTEQUAL, make_uniq<ColumnRefExpression>("rowid", var_i),
				    make_uniq<ColumnRefExpression>("rowid", var_j)));
			}
		}
		if (ne_exprs.empty()) {
			return make_uniq<ConstantExpression>(Value::BOOLEAN(true));
		}
		auto result = std::move(ne_exprs[0]);
		for (idx_t k = 1; k < ne_exprs.size(); k++) {
			result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(result),
			                                          std::move(ne_exprs[k]));
		}
		return result;
	}

	// Recurse into conjunction/disjunction children
	auto *conj = dynamic_cast<ConjunctionExpression *>(expr.get());
	if (conj) {
		for (auto &child : conj->children) {
			child = RewriteAllDifferent(std::move(child));
		}
		return expr;
	}

	return expr;
}

// Rewrite element_id(x) → x.rowid and label(x) → constant string in WHERE clause expressions.
static unique_ptr<ParsedExpression>
RewriteGraphFunctions(unique_ptr<ParsedExpression> expr,
                      const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map) {
	if (!expr) {
		return expr;
	}

	auto *func = dynamic_cast<FunctionExpression *>(expr.get());
	if (func) {
		// element_id(x) → x.rowid
		if (func->function_name == "element_id" && func->children.size() == 1) {
			auto *col = dynamic_cast<ColumnRefExpression *>(func->children[0].get());
			if (col != nullptr && !col->column_names.empty()) {
				auto rowid_ref = make_uniq<ColumnRefExpression>("rowid", col->column_names.back());
				rowid_ref->alias = func->alias;
				return rowid_ref;
			}
		}
		// label(x) → string constant of x's label
		if (func->function_name == "label" && func->children.size() == 1) {
			auto *col = dynamic_cast<ColumnRefExpression *>(func->children[0].get());
			if (col != nullptr && !col->column_names.empty()) {
				auto tbl_iter = alias_map.find(col->column_names.back());
				if (tbl_iter != alias_map.end()) {
					auto label_const = make_uniq<ConstantExpression>(Value(tbl_iter->second->main_label));
					label_const->alias = func->alias;
					return label_const;
				}
			}
		}
		// x IS LABELED 'lbl' → label matches (case-insensitive)
		if (func->function_name == "is_labeled" && func->children.size() == 2) {
			auto *col = dynamic_cast<ColumnRefExpression *>(func->children[0].get());
			auto *lbl = dynamic_cast<ConstantExpression *>(func->children[1].get());
			if (col != nullptr && lbl != nullptr && !col->column_names.empty()) {
				string var = col->column_names.back();
				auto tbl_iter = alias_map.find(var);
				if (tbl_iter != alias_map.end()) {
					string expected = StringUtil::Lower(lbl->value.ToString());
					bool matches = tbl_iter->second->main_label == expected;
					if (!matches) {
						for (const auto &sub : tbl_iter->second->sub_labels) {
							if (StringUtil::Lower(sub) == expected) {
								matches = true;
								break;
							}
						}
					}
					return make_uniq<ConstantExpression>(Value::BOOLEAN(matches));
				}
			}
		}
		// x IS NOT LABELED 'lbl' → negation of is_labeled
		if (func->function_name == "not_labeled" && func->children.size() == 2) {
			auto *col = dynamic_cast<ColumnRefExpression *>(func->children[0].get());
			auto *lbl = dynamic_cast<ConstantExpression *>(func->children[1].get());
			if (col != nullptr && lbl != nullptr && !col->column_names.empty()) {
				string var = col->column_names.back();
				auto tbl_iter = alias_map.find(var);
				if (tbl_iter != alias_map.end()) {
					string expected = StringUtil::Lower(lbl->value.ToString());
					bool matches = tbl_iter->second->main_label == expected;
					if (!matches) {
						for (const auto &sub : tbl_iter->second->sub_labels) {
							if (StringUtil::Lower(sub) == expected) {
								matches = true;
								break;
							}
						}
					}
					return make_uniq<ConstantExpression>(Value::BOOLEAN(!matches));
				}
			}
		}
	}

	// Recurse into children of conjunction/disjunction
	auto *conj = dynamic_cast<ConjunctionExpression *>(expr.get());
	if (conj) {
		for (auto &child : conj->children) {
			child = RewriteGraphFunctions(std::move(child), alias_map);
		}
		return expr;
	}

	// Recurse into children of comparison (e.g. label(x) = 'Person')
	auto *comp = dynamic_cast<ComparisonExpression *>(expr.get());
	if (comp) {
		comp->left = RewriteGraphFunctions(std::move(comp->left), alias_map);
		comp->right = RewriteGraphFunctions(std::move(comp->right), alias_map);
		return expr;
	}

	// Recurse into other function expressions
	if (func) {
		for (auto &child : func->children) {
			child = RewriteGraphFunctions(std::move(child), alias_map);
		}
		return expr;
	}

	return expr;
}

unique_ptr<ParsedExpression> PGQMatchFunction::CreateMatchJoinExpression(vector<string> vertex_keys,
                                                                         vector<string> edge_keys,
                                                                         const string &vertex_alias,
                                                                         const string &edge_alias) {
	vector<unique_ptr<ParsedExpression>> conditions;

	if (vertex_keys.size() != edge_keys.size()) {
		throw BinderException("Vertex columns and edge columns size mismatch");
	}
	for (idx_t i = 0; i < vertex_keys.size(); i++) {
		auto vertex_colref = make_uniq<ColumnRefExpression>(vertex_keys[i], vertex_alias);
		auto edge_colref = make_uniq<ColumnRefExpression>(edge_keys[i], edge_alias);
		conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(vertex_colref),
		                                                     std::move(edge_colref)));
	}
	unique_ptr<ParsedExpression> where_clause;

	for (auto &condition : conditions) {
		if (where_clause) {
			where_clause = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(where_clause),
			                                                std::move(condition));
		} else {
			where_clause = std::move(condition);
		}
	}

	return where_clause;
}

PathElement *PGQMatchFunction::GetPathElement(const unique_ptr<PathReference> &path_reference) {
	if (path_reference->path_reference_type == PGQPathReferenceType::PATH_ELEMENT) {
		return reinterpret_cast<PathElement *>(path_reference.get());
	}
	if (path_reference->path_reference_type == PGQPathReferenceType::SUBPATH) {
		return nullptr;
	}
	throw InternalException("Unknown path reference type detected");
}

SubPath *PGQMatchFunction::GetSubPath(const unique_ptr<PathReference> &path_reference) {
	if (path_reference->path_reference_type == PGQPathReferenceType::PATH_ELEMENT) {
		return nullptr;
	}
	if (path_reference->path_reference_type == PGQPathReferenceType::SUBPATH) {
		return reinterpret_cast<SubPath *>(path_reference.get());
	}
	throw InternalException("Unknown path reference type detected");
}

unique_ptr<SubqueryRef> PGQMatchFunction::CreateCountCTESubquery() {
	//! BEGIN OF (SELECT count(cte1.temp) as temp * 0 from cte1) __x

	auto temp_cte_select_node = make_uniq<SelectNode>();

	auto cte_table_ref = make_uniq<BaseTableRef>();

	cte_table_ref->table_name = "cte1";
	temp_cte_select_node->from_table = std::move(cte_table_ref);
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ColumnRefExpression>("temp", "cte1"));

	auto count_function = make_uniq<FunctionExpression>("count", std::move(children));

	auto zero = make_uniq<ConstantExpression>(Value::INTEGER(0));

	vector<unique_ptr<ParsedExpression>> multiply_children;

	multiply_children.push_back(std::move(zero));
	multiply_children.push_back(std::move(count_function));
	auto multiply_function = make_uniq<FunctionExpression>("multiply", std::move(multiply_children));
	multiply_function->alias = "temp";
	temp_cte_select_node->select_list.push_back(std::move(multiply_function));
	auto temp_cte_select_statement = make_uniq<SelectStatement>();
	temp_cte_select_statement->node = std::move(temp_cte_select_node);

	auto temp_cte_select_subquery = make_uniq<SubqueryRef>(std::move(temp_cte_select_statement), "__x");
	//! END OF (SELECT count(cte1.temp) * 0 as temp from cte1) __x
	return temp_cte_select_subquery;
}

void PGQMatchFunction::EdgeTypeAny(const shared_ptr<PropertyGraphTable> &edge_table, const string &edge_binding,
                                   const string &prev_binding, const string &next_binding,
                                   vector<unique_ptr<ParsedExpression>> &conditions,
                                   unique_ptr<TableRef> &from_clause) {
	// START SELECT src, dst, * from edge_table
	auto src_dst_select_node = make_uniq<SelectNode>();

	auto edge_left_ref = edge_table->CreateBaseTableRef(edge_binding);
	src_dst_select_node->from_table = std::move(edge_left_ref);
	auto src_dst_children = vector<unique_ptr<ParsedExpression>>();
	src_dst_children.push_back(make_uniq<ColumnRefExpression>(edge_table->source_fk[0], edge_binding));
	src_dst_children.push_back(make_uniq<ColumnRefExpression>(edge_table->destination_fk[0], edge_binding));
	src_dst_children.push_back(make_uniq<StarExpression>());

	src_dst_select_node->select_list = std::move(src_dst_children);
	// END SELECT src, dst, * from edge_table

	// START SELECT dst, src, * from edge_table
	auto dst_src_select_node = make_uniq<SelectNode>();

	auto edge_right_ref = edge_table->CreateBaseTableRef(edge_binding);
	auto dst_src_children = vector<unique_ptr<ParsedExpression>>();
	dst_src_select_node->from_table = std::move(edge_right_ref);

	dst_src_children.push_back(make_uniq<ColumnRefExpression>(edge_table->destination_fk[0], edge_binding));
	dst_src_children.push_back(make_uniq<ColumnRefExpression>(edge_table->source_fk[0], edge_binding));
	dst_src_children.push_back(make_uniq<StarExpression>());

	dst_src_select_node->select_list = std::move(dst_src_children);
	// END SELECT dst, src, * from edge_table

	auto union_node = make_uniq<SetOperationNode>();
	union_node->setop_type = SetOperationType::UNION;
	union_node->setop_all = true;
	union_node->children.push_back(std::move(src_dst_select_node));
	union_node->children.push_back(std::move(dst_src_select_node));
	auto union_select = make_uniq<SelectStatement>();
	union_select->node = std::move(union_node);
	// (SELECT src, dst, * from edge_table UNION ALL SELECT dst, src, * from
	// edge_table)
	auto union_subquery = make_uniq<SubqueryRef>(std::move(union_select));
	union_subquery->alias = edge_binding;
	if (from_clause) {
		auto from_join = make_uniq<JoinRef>(JoinRefType::CROSS);
		from_join->left = std::move(from_clause);
		from_join->right = std::move(union_subquery);
		from_clause = std::move(from_join);
	} else {
		from_clause = std::move(union_subquery);
	}
	// (a) src.key = edge.src
	auto src_left_expr =
	    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, prev_binding, edge_binding);
	// (b) dst.key = edge.dst
	auto dst_left_expr =
	    CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk, next_binding, edge_binding);
	// (a) AND (b)
	auto combined_left_expr = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
	                                                           std::move(src_left_expr), std::move(dst_left_expr));

	conditions.push_back(std::move(combined_left_expr));
}

void PGQMatchFunction::EdgeTypeLeft(const shared_ptr<PropertyGraphTable> &edge_table, const string &next_table_name,
                                    const string &prev_table_name, const string &edge_binding,
                                    const string &prev_binding, const string &next_binding,
                                    vector<unique_ptr<ParsedExpression>> &conditions) {
	CheckEdgeTableConstraints(next_table_name, prev_table_name, edge_table);
	conditions.push_back(
	    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, next_binding, edge_binding));
	conditions.push_back(
	    CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk, prev_binding, edge_binding));
}

void PGQMatchFunction::EdgeTypeRight(const shared_ptr<PropertyGraphTable> &edge_table, const string &next_table_name,
                                     const string &prev_table_name, const string &edge_binding,
                                     const string &prev_binding, const string &next_binding,
                                     vector<unique_ptr<ParsedExpression>> &conditions) {
	CheckEdgeTableConstraints(prev_table_name, next_table_name, edge_table);
	conditions.push_back(
	    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, prev_binding, edge_binding));
	conditions.push_back(
	    CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk, next_binding, edge_binding));
}

void PGQMatchFunction::EdgeTypeLeftRight(const shared_ptr<PropertyGraphTable> &edge_table, const string &edge_binding,
                                         const string &prev_binding, const string &next_binding,
                                         vector<unique_ptr<ParsedExpression>> &conditions,
                                         case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                         int32_t &extra_alias_counter) {
	auto src_left_expr =
	    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, next_binding, edge_binding);
	auto dst_left_expr =
	    CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk, prev_binding, edge_binding);

	auto combined_left_expr = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
	                                                           std::move(src_left_expr), std::move(dst_left_expr));

	const auto additional_edge_alias = edge_binding + std::to_string(extra_alias_counter);
	extra_alias_counter++;

	alias_map[additional_edge_alias] = edge_table;

	auto src_right_expr =
	    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, prev_binding, additional_edge_alias);
	auto dst_right_expr = CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk,
	                                                next_binding, additional_edge_alias);
	auto combined_right_expr = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
	                                                            std::move(src_right_expr), std::move(dst_right_expr));

	auto combined_expr = make_uniq<ConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(combined_left_expr), std::move(combined_right_expr));
	conditions.push_back(std::move(combined_expr));
}

PathElement *PGQMatchFunction::HandleNestedSubPath(unique_ptr<PathReference> &path_reference,
                                                   vector<unique_ptr<ParsedExpression>> &conditions,
                                                   idx_t element_idx) {
	auto subpath = reinterpret_cast<SubPath *>(path_reference.get());
	return GetPathElement(subpath->path_list[element_idx]);
}

unique_ptr<ParsedExpression> PGQMatchFunction::CreateWhereClause(vector<unique_ptr<ParsedExpression>> &conditions) {
	unique_ptr<ParsedExpression> where_clause;
	for (auto &condition : conditions) {
		if (where_clause) {
			where_clause = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(where_clause),
			                                                std::move(condition));
		} else {
			where_clause = std::move(condition);
		}
	}
	return where_clause;
}

// Build list_slice(col_from_tbl, begin, end, step) expression.
static unique_ptr<ParsedExpression> MakeListSlice(const string &col, const string &tbl, int begin, int end, int step) {
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(make_uniq<ColumnRefExpression>(col, tbl));
	args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(begin)));
	args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(end)));
	args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(step)));
	return make_uniq<FunctionExpression>("list_slice", std::move(args));
}

static unique_ptr<ParsedExpression> MakeLen(unique_ptr<ParsedExpression> list_expr) {
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(std::move(list_expr));
	return make_uniq<FunctionExpression>("len", std::move(args));
}

static unique_ptr<ParsedExpression> MakeListDistinct(unique_ptr<ParsedExpression> list_expr) {
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(std::move(list_expr));
	return make_uniq<FunctionExpression>("list_distinct", std::move(args));
}

// Generate a WHERE condition enforcing the requested path mode on the path
// returned by the shortest_path CTE.
//
// The path list alternates: [v0, e1, v1, e2, v2, ...]  (1-indexed).
//   vertices = list_slice(path, 1, -1, 2)
//   edges    = list_slice(path, 2, -1, 2)
//
// TRAIL:   no repeated edges  -> len(list_distinct(edges))  = len(edges)
// ACYCLIC: no repeated verts  -> len(list_distinct(verts))  = len(verts)
// SIMPLE:  no repeated verts, except first may equal last
//          -> len(list_distinct(verts)) >= len(verts) - 1
static unique_ptr<ParsedExpression> CreatePathModeCondition(PGQPathMode mode, const string &path_cte) {
	switch (mode) {
	case PGQPathMode::TRAIL: {
		auto edges_a = MakeListSlice("path", path_cte, 2, -1, 2);
		auto edges_b = MakeListSlice("path", path_cte, 2, -1, 2);
		return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
		                                       MakeLen(MakeListDistinct(std::move(edges_a))),
		                                       MakeLen(std::move(edges_b)));
	}
	case PGQPathMode::ACYCLIC: {
		auto verts_a = MakeListSlice("path", path_cte, 1, -1, 2);
		auto verts_b = MakeListSlice("path", path_cte, 1, -1, 2);
		return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
		                                       MakeLen(MakeListDistinct(std::move(verts_a))),
		                                       MakeLen(std::move(verts_b)));
	}
	case PGQPathMode::SIMPLE: {
		auto verts_a = MakeListSlice("path", path_cte, 1, -1, 2);
		auto verts_b = MakeListSlice("path", path_cte, 1, -1, 2);
		auto one = make_uniq<ConstantExpression>(Value::INTEGER(1));
		vector<unique_ptr<ParsedExpression>> sub_args;
		sub_args.push_back(MakeLen(std::move(verts_b)));
		sub_args.push_back(std::move(one));
		auto len_minus_one = make_uniq<FunctionExpression>("subtract", std::move(sub_args));
		return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		                                       MakeLen(MakeListDistinct(std::move(verts_a))),
		                                       std::move(len_minus_one));
	}
	default:
		return nullptr;
	}
}

unique_ptr<CommonTableExpressionInfo>
PGQMatchFunction::GenerateShortestPathCTE(CreatePropertyGraphInfo &pg_table, SubPath *edge_subpath,
                                          PathElement *previous_vertex_element, PathElement *next_vertex_element,
                                          vector<unique_ptr<ParsedExpression>> &path_finding_conditions,
                                          bool all_shortest) {
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	auto select_statement = make_uniq<SelectStatement>();
	auto select_node = make_uniq<SelectNode>();

	auto edge_element = GetPathElement(edge_subpath->path_list[0]);
	auto edge_table = ResolveTable(edge_element->label, edge_element->match_type, pg_table);

	auto src_row_id = make_uniq<ColumnRefExpression>("rowid", previous_vertex_element->variable_binding);
	auto dst_row_id = make_uniq<ColumnRefExpression>("rowid", next_vertex_element->variable_binding);
	auto csr_id = make_uniq<ConstantExpression>(Value::INTEGER(0));

	vector<unique_ptr<ParsedExpression>> pathfinding_children;
	pathfinding_children.push_back(std::move(csr_id));
	pathfinding_children.push_back(std::move(GetCountTable(
	    edge_table->source_pg_table, previous_vertex_element->variable_binding, edge_table->source_pk[0])));
	pathfinding_children.push_back(std::move(src_row_id));
	pathfinding_children.push_back(std::move(dst_row_id));

	if (all_shortest) {
		auto all_sp_function = make_uniq<FunctionExpression>("allshortestpaths", std::move(pathfinding_children));
		vector<unique_ptr<ParsedExpression>> unnest_children;
		unnest_children.push_back(std::move(all_sp_function));
		auto unnest_function = make_uniq<FunctionExpression>("unnest", std::move(unnest_children));
		unnest_function->alias = "path";
		select_node->select_list.push_back(std::move(unnest_function));
	} else {
		auto shortest_path_function = make_uniq<FunctionExpression>("shortestpath", std::move(pathfinding_children));
		shortest_path_function->alias = "path";
		select_node->select_list.push_back(std::move(shortest_path_function));
	}
	auto src_rowid_outer_select = make_uniq<ColumnRefExpression>("rowid", previous_vertex_element->variable_binding);
	src_rowid_outer_select->alias = "src_rowid";
	select_node->select_list.push_back(std::move(src_rowid_outer_select));
	auto dst_rowid_outer_select = make_uniq<ColumnRefExpression>("rowid", next_vertex_element->variable_binding);
	dst_rowid_outer_select->alias = "dst_rowid";
	select_node->select_list.push_back(std::move(dst_rowid_outer_select));

	auto src_tableref = edge_table->source_pg_table->CreateBaseTableRef();
	src_tableref->alias = previous_vertex_element->variable_binding;
	auto dst_tableref = edge_table->destination_pg_table->CreateBaseTableRef();
	dst_tableref->alias = next_vertex_element->variable_binding;
	auto first_cross_join_ref = make_uniq<JoinRef>(JoinRefType::CROSS);
	first_cross_join_ref->left = std::move(src_tableref);
	first_cross_join_ref->right = std::move(dst_tableref);
	auto temp_cte_select_subquery = CreateCountCTESubquery();

	auto second_cross_join_ref = make_uniq<JoinRef>(JoinRefType::CROSS);
	second_cross_join_ref->left = std::move(first_cross_join_ref);
	second_cross_join_ref->right = std::move(temp_cte_select_subquery);

	select_node->from_table = std::move(second_cross_join_ref);
	vector<unique_ptr<ParsedExpression>> count_children;
	count_children.push_back(make_uniq<ColumnRefExpression>("temp", "cte1"));
	auto count_function = make_uniq<FunctionExpression>("count", std::move(count_children));

	path_finding_conditions.push_back(AddPathQuantifierCondition(
	    previous_vertex_element->variable_binding, next_vertex_element->variable_binding, edge_table, edge_subpath));

	select_node->where_clause = CreateWhereClause(path_finding_conditions);

	select_statement->node = std::move(select_node);
	cte_info->query = std::move(select_statement);
	return cte_info;
}

unique_ptr<ParsedExpression> PGQMatchFunction::CreatePathFindingFunction(
    vector<unique_ptr<PathReference>> &path_list, CreatePropertyGraphInfo &pg_table, const string &path_variable,
    unique_ptr<SelectNode> &final_select_node, vector<unique_ptr<ParsedExpression>> &conditions,
    bool all_shortest) {
	// This method will return a SubqueryRef of a list of rowids
	// For every vertex and edge element, we add the rowid to the list using
	// list_append, or list_prepend The difficulty is that there may be a
	// (un)bounded path pattern at some point in the query This is computed using
	// the shortestpath() UDF and returns a list. This list will be part of the
	// full list of element rowids, using list_concat. For now we will only
	// support returning rowids

	unique_ptr<ParsedExpression> final_list;
	vector<unique_ptr<ParsedExpression>> path_finding_conditions;
	auto previous_vertex_element = GetPathElement(path_list[0]);
	SubPath *previous_vertex_subpath = nullptr; // NOLINT
	if (!previous_vertex_element) {
		// We hit a vertex element with a WHERE, but we only care about the rowid
		// here
		// In the future this might be a recursive path pattern
		previous_vertex_subpath = reinterpret_cast<SubPath *>(path_list[0].get());
		previous_vertex_element = GetPathElement(previous_vertex_subpath->path_list[0]);
	}

	for (idx_t idx_i = 1; idx_i < path_list.size(); idx_i = idx_i + 2) {
		auto next_vertex_element = GetPathElement(path_list[idx_i + 1]);
		SubPath *next_vertex_subpath = nullptr; // NOLINT
		if (!next_vertex_element) {
			next_vertex_subpath = reinterpret_cast<SubPath *>(path_list[idx_i + 1].get());
			next_vertex_element = GetPathElement(next_vertex_subpath->path_list[0]);
		}

		auto edge_element = GetPathElement(path_list[idx_i]);
		if (!edge_element) {
			auto edge_subpath = reinterpret_cast<SubPath *>(path_list[idx_i].get());
			if (edge_subpath->upper > 1) {
				// (un)bounded shortest path
				// Add the shortest path UDF as a CTE
				if (previous_vertex_subpath) {
					path_finding_conditions.push_back(std::move(previous_vertex_subpath->where_clause));
				}
				if (next_vertex_subpath) {
					path_finding_conditions.push_back(std::move(next_vertex_subpath->where_clause));
				}
				if (final_select_node->cte_map.map.find("cte1") == final_select_node->cte_map.map.end()) {
					edge_element = reinterpret_cast<PathElement *>(edge_subpath->path_list[0].get());
					if (edge_element->match_type == PGQMatchType::MATCH_EDGE_RIGHT) {
						final_select_node->cte_map.map["cte1"] = CreateDirectedCSRCTE(
						    ResolveTable(edge_element->label, edge_element->match_type, pg_table),
						    previous_vertex_element->variable_binding, edge_element->variable_binding,
						    next_vertex_element->variable_binding);
					} else if (edge_element->match_type == PGQMatchType::MATCH_EDGE_ANY) {
						final_select_node->cte_map.map["cte1"] =
						    CreateUndirectedCSRCTE(ResolveTable(edge_element->label, edge_element->match_type, pg_table),
						                          final_select_node);
					} else {
						throw NotImplementedException("Cannot do shortest path for edge type %s",
						                              edge_element->match_type == PGQMatchType::MATCH_EDGE_LEFT
						                                  ? "MATCH_EDGE_LEFT"
						                                  : "MATCH_EDGE_LEFT_RIGHT");
					}
				}
				string shortest_path_cte_name = "shortest_path_cte";
				if (final_select_node->cte_map.map.find(shortest_path_cte_name) ==
				    final_select_node->cte_map.map.end()) {
					final_select_node->cte_map.map[shortest_path_cte_name] = GenerateShortestPathCTE(
					    pg_table, edge_subpath, previous_vertex_element, next_vertex_element, path_finding_conditions,
					    all_shortest);
					auto cte_shortest_path_ref = make_uniq<BaseTableRef>();
					cte_shortest_path_ref->table_name = shortest_path_cte_name;
					if (!final_select_node->from_table) {
						final_select_node->from_table = std::move(cte_shortest_path_ref);
					} else {
						auto join_ref = make_uniq<JoinRef>(JoinRefType::CROSS);
						join_ref->left = std::move(final_select_node->from_table);
						join_ref->right = std::move(cte_shortest_path_ref);
						final_select_node->from_table = std::move(join_ref);
					}

					conditions.push_back(make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL,
					    make_uniq<ColumnRefExpression>("src_rowid", shortest_path_cte_name),
					    make_uniq<ColumnRefExpression>("rowid", previous_vertex_element->variable_binding)));
					conditions.push_back(make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL,
					    make_uniq<ColumnRefExpression>("dst_rowid", shortest_path_cte_name),
					    make_uniq<ColumnRefExpression>("rowid", next_vertex_element->variable_binding)));
				}
				auto shortest_path_ref = make_uniq<ColumnRefExpression>("path", shortest_path_cte_name);
				if (!final_list) {
					final_list = std::move(shortest_path_ref);
				} else {
					auto pop_front_shortest_path_children = vector<unique_ptr<ParsedExpression>>();
					pop_front_shortest_path_children.push_back(std::move(shortest_path_ref));
					auto pop_front =
					    make_uniq<FunctionExpression>("array_pop_front", std::move(pop_front_shortest_path_children));

					auto final_list_children = vector<unique_ptr<ParsedExpression>>();
					final_list_children.push_back(std::move(final_list));
					final_list_children.push_back(std::move(pop_front));
					final_list = make_uniq<FunctionExpression>("list_concat", std::move(final_list_children));
				}
				// Set next vertex to be previous
				previous_vertex_element = next_vertex_element;
				continue;
			}
			if (previous_vertex_subpath) {
				conditions.push_back(std::move(previous_vertex_subpath->where_clause));
			}
			if (next_vertex_subpath) {
				conditions.push_back(std::move(next_vertex_subpath->where_clause));
			}
			edge_element = GetPathElement(edge_subpath->path_list[0]);
		}
		auto previous_rowid = make_uniq<ColumnRefExpression>("rowid", previous_vertex_element->variable_binding);
		auto edge_rowid = make_uniq<ColumnRefExpression>("rowid", edge_element->variable_binding);
		auto next_rowid = make_uniq<ColumnRefExpression>("rowid", next_vertex_element->variable_binding);
		auto starting_list_children = vector<unique_ptr<ParsedExpression>>();

		if (!final_list) {
			starting_list_children.push_back(std::move(previous_rowid));
			starting_list_children.push_back(std::move(edge_rowid));
			starting_list_children.push_back(std::move(next_rowid));
			final_list = make_uniq<FunctionExpression>("list_value", std::move(starting_list_children));
		} else {
			starting_list_children.push_back(std::move(edge_rowid));
			starting_list_children.push_back(std::move(next_rowid));
			auto next_elements_list = make_uniq<FunctionExpression>("list_value", std::move(starting_list_children));
			auto final_list_children = vector<unique_ptr<ParsedExpression>>();
			final_list_children.push_back(std::move(final_list));
			final_list_children.push_back(std::move(next_elements_list));
			final_list = make_uniq<FunctionExpression>("list_concat", std::move(final_list_children));
		}
		previous_vertex_element = next_vertex_element;
		previous_vertex_subpath = next_vertex_subpath;
	}

	return final_list;
}

void PGQMatchFunction::AddEdgeJoins(const shared_ptr<PropertyGraphTable> &edge_table,
                                    const shared_ptr<PropertyGraphTable> &previous_vertex_table,
                                    const shared_ptr<PropertyGraphTable> &next_vertex_table, PGQMatchType edge_type,
                                    const string &edge_binding, const string &prev_binding, const string &next_binding,
                                    vector<unique_ptr<ParsedExpression>> &conditions,
                                    case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                    int32_t &extra_alias_counter, unique_ptr<TableRef> &from_clause) {
	if (edge_type != PGQMatchType::MATCH_EDGE_ANY) {
		alias_map[edge_binding] = edge_table;
	}
	switch (edge_type) {
	case PGQMatchType::MATCH_EDGE_ANY: {
		EdgeTypeAny(edge_table, edge_binding, prev_binding, next_binding, conditions, from_clause);
		break;
	}
	case PGQMatchType::MATCH_EDGE_LEFT:
		EdgeTypeLeft(edge_table, next_vertex_table->table_name, previous_vertex_table->table_name, edge_binding,
		             prev_binding, next_binding, conditions);
		break;
	case PGQMatchType::MATCH_EDGE_RIGHT:
		EdgeTypeRight(edge_table, next_vertex_table->table_name, previous_vertex_table->table_name, edge_binding,
		              prev_binding, next_binding, conditions);
		break;
	case PGQMatchType::MATCH_EDGE_LEFT_RIGHT: {
		EdgeTypeLeftRight(edge_table, edge_binding, prev_binding, next_binding, conditions, alias_map,
		                  extra_alias_counter);
		break;
	}
	default:
		throw InternalException("Unknown match type found");
	}
}

unique_ptr<ParsedExpression>
PGQMatchFunction::AddPathQuantifierCondition(const string &prev_binding, const string &next_binding,
                                             const shared_ptr<PropertyGraphTable> &edge_table, const SubPath *subpath) {
	auto src_row_id = make_uniq<ColumnRefExpression>("rowid", prev_binding);
	auto dst_row_id = make_uniq<ColumnRefExpression>("rowid", next_binding);
	auto csr_id = make_uniq<ConstantExpression>(Value::INTEGER(0));

	vector<unique_ptr<ParsedExpression>> pathfinding_children;
	pathfinding_children.push_back(std::move(csr_id));
	pathfinding_children.push_back(
	    std::move(GetCountTable(edge_table->source_pg_table, prev_binding, edge_table->source_pk[0])));
	pathfinding_children.push_back(std::move(src_row_id));
	pathfinding_children.push_back(std::move(dst_row_id));

	auto reachability_function = make_uniq<FunctionExpression>("iterativelength", std::move(pathfinding_children));

	auto cte_col_ref = make_uniq<ColumnRefExpression>("temp", "__x");

	vector<unique_ptr<ParsedExpression>> addition_children;
	addition_children.push_back(std::move(cte_col_ref));
	addition_children.push_back(std::move(reachability_function));

	auto addition_function = make_uniq<FunctionExpression>("add", std::move(addition_children));
	auto lower_limit = make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(subpath->lower)));
	auto upper_limit = make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(subpath->upper)));
	auto between_expression =
	    make_uniq<BetweenExpression>(std::move(addition_function), std::move(lower_limit), std::move(upper_limit));
	return std::move(between_expression);
}

void PGQMatchFunction::AddPathFinding(unique_ptr<SelectNode> &select_node,
                                      vector<unique_ptr<ParsedExpression>> &conditions, const string &prev_binding,
                                      const string &edge_binding, const string &next_binding,
                                      const shared_ptr<PropertyGraphTable> &edge_table,
                                      CreatePropertyGraphInfo &pg_table, SubPath *subpath, PGQMatchType edge_type) {
	//! START
	//! FROM (SELECT count(cte1.temp) * 0 as temp from cte1) __x
	if (select_node->cte_map.map.find("cte1") == select_node->cte_map.map.end()) {
		if (edge_type == PGQMatchType::MATCH_EDGE_RIGHT) {
			select_node->cte_map.map["cte1"] =
			    CreateDirectedCSRCTE(edge_table, prev_binding, edge_binding, next_binding);
		} else if (edge_type == PGQMatchType::MATCH_EDGE_ANY) {
			select_node->cte_map.map["cte1"] = CreateUndirectedCSRCTE(edge_table, select_node);
		} else {
			throw NotImplementedException("Cannot do shortest path for edge type %s",
			                              edge_type == PGQMatchType::MATCH_EDGE_LEFT ? "MATCH_EDGE_LEFT"
			                                                                         : "MATCH_EDGE_LEFT_RIGHT");
		}
	}
	if (select_node->cte_map.map.find("shortest_path_cte") != select_node->cte_map.map.end()) {
		return;
	}
	auto temp_cte_select_subquery = CreateCountCTESubquery();
	if (select_node->from_table) {
		// create a cross join since there is already something in the
		// from clause
		auto from_join = make_uniq<JoinRef>(JoinRefType::CROSS);
		from_join->left = std::move(select_node->from_table);
		from_join->right = std::move(temp_cte_select_subquery);
		select_node->from_table = std::move(from_join);
	} else {
		select_node->from_table = std::move(temp_cte_select_subquery);
	}
	//! END
	//! FROM (SELECT count(cte1.temp) * 0 as temp from cte1) __x

	//! START
	//! WHERE __x.temp + iterativelength(<csr_id>, (SELECT count(c.id)
	//!       from dst c, a.rowid, b.rowid) between lower and upper
	conditions.push_back(AddPathQuantifierCondition(prev_binding, next_binding, edge_table, subpath));
	//! END
	//! WHERE __x.temp + iterativelength(<csr_id>, (SELECT count(s.id)
	//! from src s, a.rowid, b.rowid) between lower and upper
}

void PGQMatchFunction::AddPathModeFilter(PGQPathMode mode, const string &path_cte,
                                         vector<unique_ptr<ParsedExpression>> &conditions) {
	auto cond = CreatePathModeCondition(mode, path_cte);
	if (cond) {
		conditions.push_back(std::move(cond));
	}
}

void PGQMatchFunction::AddTraversalPathFinding(
    unique_ptr<SelectNode> &select_node, vector<unique_ptr<ParsedExpression>> &conditions, const string &prev_binding,
    const string &next_binding, const shared_ptr<PropertyGraphTable> &prev_vertex_table,
    const shared_ptr<PropertyGraphTable> &next_vertex_table,
    const vector<shared_ptr<PropertyGraphTable>> &edge_tables, bool undirected, idx_t lower, idx_t upper) {
	if (upper == static_cast<idx_t>(1 << 30)) {
		throw NotImplementedException(
		    "Unbounded traversal with compound edge labels is not yet supported — specify an upper bound.");
	}
	D_ASSERT(!edge_tables.empty());

	// All edge tables must share the same source/destination vertex tables.
	const auto &ref_tbl = edge_tables[0];
	for (size_t i = 1; i < edge_tables.size(); i++) {
		if (edge_tables[i]->source_reference != ref_tbl->source_reference ||
		    edge_tables[i]->destination_reference != ref_tbl->destination_reference) {
			throw NotImplementedException(
			    "Multi-label traversal across different source/destination vertex types is not yet supported.");
		}
	}

	const string &src_pk_col = ref_tbl->source_pk[0];
	const string &dst_pk_col = ref_tbl->destination_pk[0];

	// Generate unique CTE and alias names
	const auto cte_idx = select_node->cte_map.map.size();
	string cte_name = "pgq_traverse_" + std::to_string(cte_idx);
	string cte_alias = "__pgq_t_" + std::to_string(cte_idx);

	// === Anchor: seed every source vertex ===
	auto anchor = make_uniq<SelectNode>();
	anchor->from_table = prev_vertex_table->CreateBaseTableRef();

	auto a_src_rowid = make_uniq<ColumnRefExpression>("rowid");
	a_src_rowid->alias = "src_rowid";
	anchor->select_list.push_back(std::move(a_src_rowid));

	auto a_cur_pk = make_uniq<ColumnRefExpression>(src_pk_col);
	a_cur_pk->alias = "cur_pk";
	anchor->select_list.push_back(std::move(a_cur_pk));

	auto a_cur_rowid = make_uniq<ColumnRefExpression>("rowid");
	a_cur_rowid->alias = "cur_rowid";
	anchor->select_list.push_back(std::move(a_cur_rowid));

	auto a_depth = make_uniq<ConstantExpression>(Value::INTEGER(0));
	a_depth->alias = "depth";
	anchor->select_list.push_back(std::move(a_depth));

	vector<unique_ptr<ParsedExpression>> lv_args;
	lv_args.push_back(make_uniq<ColumnRefExpression>("rowid"));
	auto a_visited = make_uniq<FunctionExpression>("list_value", std::move(lv_args));
	a_visited->alias = "visited";
	anchor->select_list.push_back(std::move(a_visited));

	// === Recursive step ===
	auto recursive = make_uniq<SelectNode>();

	// FROM pgq_traverse_N t
	auto cte_ref = make_uniq<BaseTableRef>();
	cte_ref->table_name = cte_name;
	cte_ref->alias = "t";

	// JOIN (...edges union...) __edges ON __edges.src_pk = t.cur_pk
	auto join_edges = make_uniq<JoinRef>();
	join_edges->left = std::move(cte_ref);
	join_edges->right = BuildEdgeUnionSubquery(edge_tables, undirected);
	join_edges->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>("src_pk", "__edges"),
	    make_uniq<ColumnRefExpression>("cur_pk", "t"));

	// JOIN dst_vertex b ON b.<dst_pk_col> = __edges.dst_pk
	auto dst_tref = next_vertex_table->CreateBaseTableRef();
	dst_tref->alias = "b";
	auto join_dst = make_uniq<JoinRef>();
	join_dst->left = std::move(join_edges);
	join_dst->right = std::move(dst_tref);
	join_dst->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>(dst_pk_col, "b"),
	    make_uniq<ColumnRefExpression>("dst_pk", "__edges"));
	recursive->from_table = std::move(join_dst);

	// SELECT t.src_rowid, b.<dst_pk_col>, b.rowid, t.depth + 1, list_append(t.visited, b.rowid)
	auto r_src_rowid = make_uniq<ColumnRefExpression>("src_rowid", "t");
	r_src_rowid->alias = "src_rowid";
	recursive->select_list.push_back(std::move(r_src_rowid));

	auto r_cur_pk = make_uniq<ColumnRefExpression>(dst_pk_col, "b");
	r_cur_pk->alias = "cur_pk";
	recursive->select_list.push_back(std::move(r_cur_pk));

	auto r_cur_rowid = make_uniq<ColumnRefExpression>("rowid", "b");
	r_cur_rowid->alias = "cur_rowid";
	recursive->select_list.push_back(std::move(r_cur_rowid));

	vector<unique_ptr<ParsedExpression>> add_args;
	add_args.push_back(make_uniq<ColumnRefExpression>("depth", "t"));
	add_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));
	auto r_depth = make_uniq<FunctionExpression>("add", std::move(add_args));
	r_depth->alias = "depth";
	recursive->select_list.push_back(std::move(r_depth));

	vector<unique_ptr<ParsedExpression>> app_args;
	app_args.push_back(make_uniq<ColumnRefExpression>("visited", "t"));
	app_args.push_back(make_uniq<ColumnRefExpression>("rowid", "b"));
	auto r_visited = make_uniq<FunctionExpression>("list_append", std::move(app_args));
	r_visited->alias = "visited";
	recursive->select_list.push_back(std::move(r_visited));

	// WHERE t.depth < upper AND NOT list_contains(t.visited, b.rowid)
	vector<unique_ptr<ParsedExpression>> rec_where;
	rec_where.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_LESSTHAN, make_uniq<ColumnRefExpression>("depth", "t"),
	    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(upper)))));
	vector<unique_ptr<ParsedExpression>> lc_args;
	lc_args.push_back(make_uniq<ColumnRefExpression>("visited", "t"));
	lc_args.push_back(make_uniq<ColumnRefExpression>("rowid", "b"));
	auto lc_fn = make_uniq<FunctionExpression>("list_contains", std::move(lc_args));
	rec_where.push_back(make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(lc_fn)));
	recursive->where_clause = CreateWhereClause(rec_where);

	// === Build RecursiveCTENode ===
	auto rec_cte = make_uniq<RecursiveCTENode>();
	rec_cte->ctename = cte_name;
	rec_cte->union_all = true;
	rec_cte->left = std::move(anchor);
	rec_cte->right = std::move(recursive);
	rec_cte->aliases = {"src_rowid", "cur_pk", "cur_rowid", "depth", "visited"};

	auto cte_stmt = make_uniq<SelectStatement>();
	cte_stmt->node = std::move(rec_cte);
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	cte_info->query = std::move(cte_stmt);
	select_node->cte_map.map[cte_name] = std::move(cte_info);

	// Add a direct BaseTableRef for the CTE into the FROM clause (cross join)
	auto cte_scan = make_uniq<BaseTableRef>();
	cte_scan->table_name = cte_name;
	cte_scan->alias = cte_alias;
	if (select_node->from_table) {
		auto cross = make_uniq<JoinRef>(JoinRefType::CROSS);
		cross->left = std::move(select_node->from_table);
		cross->right = std::move(cte_scan);
		select_node->from_table = std::move(cross);
	} else {
		select_node->from_table = std::move(cte_scan);
	}

	// Outer WHERE: tie CTE rows to the vertex bindings and filter by depth
	conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>("rowid", prev_binding),
	    make_uniq<ColumnRefExpression>("src_rowid", cte_alias)));
	conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>("rowid", next_binding),
	    make_uniq<ColumnRefExpression>("cur_rowid", cte_alias)));
	conditions.push_back(make_uniq<BetweenExpression>(
	    make_uniq<ColumnRefExpression>("depth", cte_alias),
	    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(lower))),
	    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(upper)))));
}

void PGQMatchFunction::CheckNamedSubpath(SubPath &subpath, MatchExpression &original_ref,
                                         CreatePropertyGraphInfo &pg_table, unique_ptr<SelectNode> &final_select_node,
                                         vector<unique_ptr<ParsedExpression>> &conditions, bool all_shortest) {
	for (idx_t idx_i = 0; idx_i < original_ref.column_list.size(); idx_i++) {
		auto parsed_ref = dynamic_cast<FunctionExpression *>(original_ref.column_list[idx_i].get());
		if (parsed_ref == nullptr) {
			continue;
		}
		auto column_ref = dynamic_cast<ColumnRefExpression *>(parsed_ref->children[0].get());
		if (column_ref == nullptr) {
			continue;
		}

		if (column_ref->column_names[0] != subpath.path_variable) {
			continue;
		}
		// Trying to check parsed_ref->alias directly leads to a segfault
		string column_alias = parsed_ref->alias;
		if (parsed_ref->function_name == "element_id") {
			// Check subpath name matches the column referenced in the function -->
			// element_id(named_subpath)
			auto shortest_path_function = CreatePathFindingFunction(subpath.path_list, pg_table, subpath.path_variable,
			                                                        final_select_node, conditions, all_shortest);

			if (column_alias.empty()) {
				shortest_path_function->alias = "element_id(" + subpath.path_variable + ")";
			} else {
				shortest_path_function->alias = column_alias;
			}
			original_ref.column_list.erase(original_ref.column_list.begin() + static_cast<int64_t>(idx_i));
			original_ref.column_list.insert(original_ref.column_list.begin() + static_cast<int64_t>(idx_i),
			                                std::move(shortest_path_function));
		} else if (parsed_ref->function_name == "path_length") {
			auto shortest_path_function = CreatePathFindingFunction(subpath.path_list, pg_table, subpath.path_variable,
			                                                        final_select_node, conditions, all_shortest);
			auto path_len_children = vector<unique_ptr<ParsedExpression>>();
			path_len_children.push_back(std::move(shortest_path_function));
			auto path_len = make_uniq<FunctionExpression>("len", std::move(path_len_children));
			auto constant_two = make_uniq<ConstantExpression>(Value::INTEGER(2));
			vector<unique_ptr<ParsedExpression>> div_children;
			div_children.push_back(std::move(path_len));
			div_children.push_back(std::move(constant_two));
			auto path_length_function = make_uniq<FunctionExpression>("//", std::move(div_children));
			path_length_function->alias =
			    column_alias.empty() ? "path_length(" + subpath.path_variable + ")" : column_alias;
			original_ref.column_list.erase(original_ref.column_list.begin() + static_cast<int64_t>(idx_i));
			original_ref.column_list.insert(original_ref.column_list.begin() + static_cast<int64_t>(idx_i),
			                                std::move(path_length_function));
		} else if (parsed_ref->function_name == "vertices" || parsed_ref->function_name == "edges") {
			auto list_slice_children = vector<unique_ptr<ParsedExpression>>();
			auto shortest_path_function = CreatePathFindingFunction(subpath.path_list, pg_table, subpath.path_variable,
			                                                        final_select_node, conditions, all_shortest);
			list_slice_children.push_back(std::move(shortest_path_function));

			if (parsed_ref->function_name == "vertices") {
				list_slice_children.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));
			} else {
				list_slice_children.push_back(make_uniq<ConstantExpression>(Value::INTEGER(2)));
			}
			auto slice_end = make_uniq<ConstantExpression>(Value::INTEGER(-1));
			auto slice_step = make_uniq<ConstantExpression>(Value::INTEGER(2));

			list_slice_children.push_back(std::move(slice_end));
			list_slice_children.push_back(std::move(slice_step));
			auto list_slice = make_uniq<FunctionExpression>("list_slice", std::move(list_slice_children));
			if (parsed_ref->function_name == "vertices") {
				list_slice->alias = column_alias.empty() ? "vertices(" + subpath.path_variable + ")" : column_alias;
			} else {
				list_slice->alias = column_alias.empty() ? "edges(" + subpath.path_variable + ")" : column_alias;
			}
			original_ref.column_list.erase(original_ref.column_list.begin() + static_cast<int64_t>(idx_i));
			original_ref.column_list.insert(original_ref.column_list.begin() + static_cast<int64_t>(idx_i),
			                                std::move(list_slice));
		} else if (parsed_ref->function_name == "cost") {
			// Find edge subpath with cost_expr within this named subpath's path_list.
			SubPath *edge_sub = nullptr;
			PathElement *edge_elem = nullptr;
			PathElement *prev_elem = GetPathElement(subpath.path_list[0]);
			PathElement *next_elem = nullptr;
			for (idx_t k = 1; k + 1 < subpath.path_list.size(); k += 2) {
				auto *candidate = dynamic_cast<SubPath *>(subpath.path_list[k].get());
				if (candidate && candidate->cost_expr) {
					edge_sub = candidate;
					edge_elem = GetPathElement(edge_sub->path_list[0]);
					next_elem = GetPathElement(subpath.path_list[k + 1]);
					break;
				}
			}
			if (!edge_sub || !edge_elem || !prev_elem || !next_elem) {
				throw BinderException("COST(%s) requires a COST expression on the edge pattern",
				                      subpath.path_variable);
			}
			auto edge_table = ResolveTable(edge_elem->label, edge_elem->match_type, pg_table);
			// Build weighted CSR CTE if not already built.
			if (final_select_node->cte_map.map.find("cte1") == final_select_node->cte_map.map.end()) {
				if (edge_elem->match_type != PGQMatchType::MATCH_EDGE_RIGHT) {
					throw NotImplementedException("COST() is only supported for right-directed edges (->) currently");
				}
				final_select_node->cte_map.map["cte1"] = CreateWeightedDirectedCSRCTE(
				    edge_table, prev_elem->variable_binding, edge_elem->variable_binding,
				    next_elem->variable_binding, edge_sub->cost_expr->Copy());
			}
			// Generate cheapest_path_length(0, v_size, src_rowid, dst_rowid).
			vector<unique_ptr<ParsedExpression>> cpl_args;
			cpl_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(0)));
			cpl_args.push_back(GetCountTable(edge_table->source_pg_table, prev_elem->variable_binding,
			                                 edge_table->source_pk[0]));
			cpl_args.push_back(make_uniq<ColumnRefExpression>("rowid", prev_elem->variable_binding));
			cpl_args.push_back(make_uniq<ColumnRefExpression>("rowid", next_elem->variable_binding));
			auto cpl_func = make_uniq<FunctionExpression>("cheapest_path_length", std::move(cpl_args));
			cpl_func->alias = column_alias.empty() ? "cost(" + subpath.path_variable + ")" : column_alias;
			original_ref.column_list.erase(original_ref.column_list.begin() + static_cast<int64_t>(idx_i));
			original_ref.column_list.insert(original_ref.column_list.begin() + static_cast<int64_t>(idx_i),
			                                std::move(cpl_func));
		}
	}
}

void PGQMatchFunction::ProcessPathList(vector<unique_ptr<PathReference>> &path_list,
                                       vector<unique_ptr<ParsedExpression>> &conditions,
                                       unique_ptr<SelectNode> &final_select_node,
                                       case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                       CreatePropertyGraphInfo &pg_table, int32_t &extra_alias_counter,
                                       MatchExpression &original_ref, bool all_shortest) {
	PathElement *previous_vertex_element = GetPathElement(path_list[0]);
	if (!previous_vertex_element) {
		const auto previous_vertex_subpath = reinterpret_cast<SubPath *>(path_list[0].get());
		if (previous_vertex_subpath->where_clause) {
			conditions.push_back(std::move(previous_vertex_subpath->where_clause));
		}
		if (!previous_vertex_subpath->path_variable.empty() && previous_vertex_subpath->path_list.size() > 1) {
			CheckNamedSubpath(*previous_vertex_subpath, original_ref, pg_table, final_select_node, conditions,
			                  all_shortest);
		}
		if (previous_vertex_subpath->path_list.size() == 1) {
			previous_vertex_element = GetPathElement(previous_vertex_subpath->path_list[0]);
		} else {
			// Add the shortest path if the name is found in the column_list
			ProcessPathList(previous_vertex_subpath->path_list, conditions, final_select_node, alias_map, pg_table,
			                extra_alias_counter, original_ref, all_shortest);
			return;
		}
	}
	auto previous_vertex_table = ResolveTable(previous_vertex_element->label, previous_vertex_element->match_type, pg_table);
	if (!previous_vertex_element->label.empty()) {
		CheckInheritance(previous_vertex_table, previous_vertex_element, conditions);
	}
	alias_map[previous_vertex_element->variable_binding] = previous_vertex_table;

	for (idx_t idx_j = 1; idx_j < path_list.size(); idx_j = idx_j + 2) {
		PathElement *next_vertex_element = GetPathElement(path_list[idx_j + 1]);
		if (!next_vertex_element) {
			auto next_vertex_subpath = reinterpret_cast<SubPath *>(path_list[idx_j + 1].get());
			if (next_vertex_subpath->path_list.size() > 1) {
				throw NotImplementedException("Recursive patterns are not yet supported.");
			}
			if (next_vertex_subpath->where_clause) {
				conditions.push_back(std::move(next_vertex_subpath->where_clause));
			}
			next_vertex_element = GetPathElement(next_vertex_subpath->path_list[0]);
		}
		if (next_vertex_element->match_type != PGQMatchType::MATCH_VERTEX ||
		    previous_vertex_element->match_type != PGQMatchType::MATCH_VERTEX) {
			throw BinderException("Vertex and edge patterns must be alternated.");
		}
		auto next_vertex_table = ResolveTable(next_vertex_element->label, next_vertex_element->match_type, pg_table);
		if (!next_vertex_element->label.empty()) {
			CheckInheritance(next_vertex_table, next_vertex_element, conditions);
		}
		alias_map[next_vertex_element->variable_binding] = next_vertex_table;

		PathElement *edge_element = GetPathElement(path_list[idx_j]);
		if (!edge_element) {
			// We are dealing with a subpath
			auto edge_subpath = reinterpret_cast<SubPath *>(path_list[idx_j].get());
			if (edge_subpath->where_clause) {
				conditions.push_back(std::move(edge_subpath->where_clause));
			}
			if (edge_subpath->path_list.size() > 1) {
				throw NotImplementedException("Subpath on an edge is not yet supported.");
			}
			edge_element = GetPathElement(edge_subpath->path_list[0]);

			// Attempt to resolve the edge label to a single backing table.
			// Same-table compound labels (sub-labels like Colleague|Friend) resolve fine.
			// Cross-table compound labels (Knows|Likes) throw and need a recursive CTE.
			shared_ptr<PropertyGraphTable> edge_table;
			bool use_traversal_cte = false;
			if (IsCompoundLabel(edge_element->label)) {
				try {
					edge_table = ResolveTable(edge_element->label, edge_element->match_type, pg_table);
				} catch (const Exception &) {
					use_traversal_cte = true;
				}
			} else {
				edge_table = ResolveTable(edge_element->label, edge_element->match_type, pg_table);
			}

			if (use_traversal_cte) {
				// Cross-table multi-label: use recursive CTE traversal.
				auto edge_tbls =
				    CollectEdgeTablesFromLabel(edge_element->label, edge_element->match_type, pg_table);
				bool undirected = (edge_element->match_type == PGQMatchType::MATCH_EDGE_ANY);
				AddTraversalPathFinding(final_select_node, conditions,
				                        previous_vertex_element->variable_binding,
				                        next_vertex_element->variable_binding, previous_vertex_table,
				                        next_vertex_table, edge_tbls, undirected, edge_subpath->lower,
				                        edge_subpath->upper);
			} else if (edge_subpath->upper > 1) {
				if (original_ref.one_row_per != OneRowPerMode::NONE) {
					// ONE ROW PER: use shortestpath() so we can unnest per-step data.
					// Produce cte1 (CSR) + shortest_path_cte; ApplyOneRowPer handles FROM/WHERE.
					if (final_select_node->cte_map.map.find("cte1") == final_select_node->cte_map.map.end()) {
						if (edge_element->match_type == PGQMatchType::MATCH_EDGE_RIGHT) {
							final_select_node->cte_map.map["cte1"] = CreateDirectedCSRCTE(
							    edge_table, previous_vertex_element->variable_binding,
							    edge_element->variable_binding, next_vertex_element->variable_binding);
						} else if (edge_element->match_type == PGQMatchType::MATCH_EDGE_ANY) {
							final_select_node->cte_map.map["cte1"] =
							    CreateUndirectedCSRCTE(edge_table, final_select_node);
						} else {
							throw NotImplementedException("ONE ROW PER path finding is not supported for "
							                              "left-directed or bidirectional edges");
						}
					}
					static const string kPathCTE = "shortest_path_cte";
					if (final_select_node->cte_map.map.find(kPathCTE) == final_select_node->cte_map.map.end()) {
						vector<unique_ptr<ParsedExpression>> pfc;
						final_select_node->cte_map.map[kPathCTE] = GenerateShortestPathCTE(
						    pg_table, edge_subpath, previous_vertex_element, next_vertex_element, pfc, all_shortest);
					}
				} else {
					// Normal case: use iterativelength() — just checks hop-bound, no path materialised.
					AddPathFinding(final_select_node, conditions, previous_vertex_element->variable_binding,
					               edge_element->variable_binding, next_vertex_element->variable_binding,
					               edge_table, pg_table, edge_subpath, edge_element->match_type);
					if (edge_subpath->path_mode != PGQPathMode::NONE &&
					    edge_subpath->path_mode != PGQPathMode::WALK) {
						AddPathModeFilter(edge_subpath->path_mode, "shortest_path_cte", conditions);
					}
				}
			} else {
				// Single hop.
				AddEdgeJoins(edge_table, previous_vertex_table, next_vertex_table,
				             edge_element->match_type, edge_element->variable_binding,
				             previous_vertex_element->variable_binding,
				             next_vertex_element->variable_binding, conditions, alias_map,
				             extra_alias_counter, final_select_node->from_table);
			}
		} else {
			// The edge element is a path element without WHERE or path-finding.
			auto edge_table = ResolveTable(edge_element->label, edge_element->match_type, pg_table);
			if (!edge_element->label.empty()) {
				CheckInheritance(edge_table, edge_element, conditions);
			}
			// check aliases
			AddEdgeJoins(edge_table, previous_vertex_table, next_vertex_table, edge_element->match_type,
			             edge_element->variable_binding, previous_vertex_element->variable_binding,
			             next_vertex_element->variable_binding, conditions, alias_map, extra_alias_counter,
			             final_select_node->from_table);
			// Check the edge type
			// If (a)-[b]->(c) 	-> 	b.src = a.id AND b.dst = c.id
			// If (a)<-[b]-(c) 	-> 	b.dst = a.id AND b.src = c.id
			// If (a)-[b]-(c)  	-> 	(b.src = a.id AND b.dst = c.id)
			//              FROM (src, dst, * from b UNION ALL dst, src, * from b)
			// If (a)<-[b]->(c)	->  (b.src = a.id AND b.dst = c.id) AND
			//						(b.dst = a.id AND b.src
			//= c.id)
		}
		previous_vertex_element = next_vertex_element;
		previous_vertex_table = next_vertex_table;
	}
}

void PGQMatchFunction::ProcessOptionalPathList(
    const vector<unique_ptr<PathPattern>> &opt_patterns, unique_ptr<SelectNode> &select_node,
    case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &opt_alias_map, CreatePropertyGraphInfo &pg_table,
    int32_t &extra_alias_counter,
    const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &mandatory_alias_map) {
	for (auto &path_pattern : opt_patterns) {
		auto &path_elements = path_pattern->path_elements;
		if (path_elements.empty()) {
			continue;
		}

		PathElement *prev_elem = GetPathElement(path_elements[0]);
		if (!prev_elem) {
			throw NotImplementedException("SubPath as the first element of OPTIONAL MATCH is not yet supported.");
		}
		// Resolve the start vertex table. If it carries no label (reusing a mandatory binding),
		// look it up in the mandatory alias map rather than ResolveTable (which would fail on ambiguity).
		shared_ptr<PropertyGraphTable> prev_vertex_table;
		if (prev_elem->label.empty()) {
			auto it = mandatory_alias_map.find(prev_elem->variable_binding);
			if (it == mandatory_alias_map.end()) {
				auto it2 = opt_alias_map.find(prev_elem->variable_binding);
				if (it2 == opt_alias_map.end()) {
					throw BinderException(
					    "Variable '%s' in OPTIONAL MATCH is not bound by a preceding MATCH clause.",
					    prev_elem->variable_binding);
				}
				prev_vertex_table = it2->second;
			} else {
				prev_vertex_table = it->second;
			}
		} else {
			prev_vertex_table = ResolveTable(prev_elem->label, prev_elem->match_type, pg_table);
		}
		opt_alias_map.emplace(prev_elem->variable_binding, prev_vertex_table);

		for (idx_t idx_j = 1; idx_j < path_elements.size(); idx_j += 2) {
			if (idx_j + 1 >= path_elements.size()) {
				break;
			}
			PathElement *edge_elem = GetPathElement(path_elements[idx_j]);
			PathElement *next_elem = GetPathElement(path_elements[idx_j + 1]);

			if (!edge_elem || !next_elem) {
				throw NotImplementedException("Variable-length paths in OPTIONAL MATCH are not yet supported.");
			}
			if (edge_elem->match_type == PGQMatchType::MATCH_EDGE_ANY ||
			    edge_elem->match_type == PGQMatchType::MATCH_EDGE_LEFT_RIGHT) {
				throw NotImplementedException(
				    "Undirected or bidirectional edges in OPTIONAL MATCH are not yet supported.");
			}

			auto edge_table = ResolveTable(edge_elem->label, edge_elem->match_type, pg_table);
			auto next_vertex_table = ResolveTable(next_elem->label, next_elem->match_type, pg_table);

			const string &prev_binding = prev_elem->variable_binding;
			const string &edge_binding = edge_elem->variable_binding;
			const string &next_binding = next_elem->variable_binding;

			// Build the ON conditions for the edge LEFT JOIN and the vertex LEFT JOIN.
			// For MATCH_EDGE_RIGHT (a)-[e]->(b): e.src_fk=a.src_pk, b.dst_pk=e.dst_fk
			// For MATCH_EDGE_LEFT  (a)<-[e]-(b): e.dst_fk=a.dst_pk, b.src_pk=e.src_fk
			unique_ptr<ParsedExpression> edge_on_cond;
			unique_ptr<ParsedExpression> vertex_on_cond;

			if (edge_elem->match_type == PGQMatchType::MATCH_EDGE_RIGHT) {
				CheckEdgeTableConstraints(prev_vertex_table->table_name, next_vertex_table->table_name, edge_table);
				edge_on_cond =
				    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, prev_binding, edge_binding);
				vertex_on_cond = CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk,
				                                           next_binding, edge_binding);
			} else {
				// MATCH_EDGE_LEFT: (prev)<-[edge]-(next), so edge flows next→prev
				CheckEdgeTableConstraints(next_vertex_table->table_name, prev_vertex_table->table_name, edge_table);
				edge_on_cond = CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk,
				                                         prev_binding, edge_binding);
				vertex_on_cond =
				    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, next_binding, edge_binding);
			}

			// If the next vertex has a sub-label condition, fold it into the vertex ON clause.
			if (!next_elem->label.empty()) {
				vector<unique_ptr<ParsedExpression>> label_conds;
				CheckInheritance(next_vertex_table, next_elem, label_conds);
				if (!label_conds.empty()) {
					auto label_cond = CreateWhereClause(label_conds);
					vertex_on_cond = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
					                                                   std::move(vertex_on_cond),
					                                                   std::move(label_cond));
				}
			}

			// LEFT JOIN edge table ON edge_on_cond
			{
				auto edge_ref = edge_table->CreateBaseTableRef(edge_binding);
				auto left_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
				left_join->type = JoinType::LEFT;
				left_join->left = std::move(select_node->from_table);
				left_join->right = std::move(edge_ref);
				left_join->condition = std::move(edge_on_cond);
				select_node->from_table = std::move(left_join);
			}

			// LEFT JOIN next vertex table ON vertex_on_cond
			{
				auto vertex_ref = next_vertex_table->CreateBaseTableRef(next_binding);
				auto left_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
				left_join->type = JoinType::LEFT;
				left_join->left = std::move(select_node->from_table);
				left_join->right = std::move(vertex_ref);
				left_join->condition = std::move(vertex_on_cond);
				select_node->from_table = std::move(left_join);
			}

			opt_alias_map[edge_binding] = edge_table;
			opt_alias_map[next_binding] = next_vertex_table;

			prev_elem = next_elem;
			prev_vertex_table = next_vertex_table;
		}
	}
}

void PGQMatchFunction::PopulateGraphTableAliasMap(
    const CreatePropertyGraphInfo &pg_table, const unique_ptr<PathReference> &path_reference,
    case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_to_vertex_and_edge_tables) {
	PathElement *path_elem = GetPathElement(path_reference);

	// Populate binding from PathElement.
	if (path_elem != nullptr) {
		if (!path_elem->label.empty()) {
			shared_ptr<PropertyGraphTable> tbl;
			if (IsCompoundLabel(path_elem->label)) {
				// For compound expressions (A|B, !A, A&B), attempt to resolve to a single table.
				// Cross-table compound labels (e.g. multi-label traversal edges) are handled by a
				// recursive CTE — the variable has no single backing table, so skip registration.
				auto &pg_mut = const_cast<CreatePropertyGraphInfo &>(pg_table);
				try {
					tbl = ResolveLabelExprTable(path_elem->label, path_elem->match_type, pg_mut);
				} catch (const Exception &) {
					return; // Cross-table compound: variable not bindable to a single table.
				}
			} else {
				auto iter = pg_table.label_map.find(path_elem->label);
				if (iter == pg_table.label_map.end()) {
					throw BinderException("The label %s is not registered in property graph %s", path_elem->label,
					                      pg_table.property_graph_name);
				}
				tbl = iter->second;
			}
			alias_to_vertex_and_edge_tables[path_elem->variable_binding] = tbl;
		} else {
			bool is_vertex = (path_elem->match_type == PGQMatchType::MATCH_VERTEX);
			const auto &tables = is_vertex ? pg_table.vertex_tables : pg_table.edge_tables;
			if (tables.size() == 1) {
				alias_to_vertex_and_edge_tables[path_elem->variable_binding] = tables[0];
			} else {
				throw BinderException("Unlabeled %s pattern is ambiguous in property graph %s",
				                      is_vertex ? "vertex" : "edge", pg_table.property_graph_name);
			}
		}
		return;
	}

	// Recursively populate binding from SubPath.
	SubPath *sub_path = GetSubPath(path_reference);
	D_ASSERT(sub_path != nullptr);
	const auto &path_list = sub_path->path_list;
	for (const auto &cur_path : path_list) {
		PopulateGraphTableAliasMap(pg_table, cur_path, alias_to_vertex_and_edge_tables);
	}
}

case_insensitive_map_t<shared_ptr<PropertyGraphTable>>
PGQMatchFunction::PopulateGraphTableAliasMap(const CreatePropertyGraphInfo &pg_table,
                                             const MatchExpression &match_expr) {
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> alias_to_vertex_and_edge_tables;
	for (idx_t idx_i = 0; idx_i < match_expr.path_patterns.size(); idx_i++) {
		const auto &path_list = match_expr.path_patterns[idx_i]->path_elements;
		for (const auto &cur_path : path_list) {
			PopulateGraphTableAliasMap(pg_table, cur_path, alias_to_vertex_and_edge_tables);
		}
	}
	return alias_to_vertex_and_edge_tables;
}

void PGQMatchFunction::CheckColumnBinding(
    const CreatePropertyGraphInfo &pg_table, const MatchExpression &ref,
    const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_to_vertex_and_edge_tables) {
	// All fully-qualified column names for vertex tables and edge tables.
	const auto all_fq_col_names = GetFullyQualifiedColFromPg(pg_table, alias_to_vertex_and_edge_tables);

	for (auto &expression : ref.column_list) {
		// TODO(hjiang): `ColumnRefExpression` alone is not enough, we could have
		// more complicated expression.
		//
		// See issue for reference:
		// https://github.com/cwida/duckpgq-extension/issues/198
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(expression.get());
		if (column_ref == nullptr) {
			continue;
		}
		// 'shortest_path_cte' is a special table populated by pgq.
		if (column_ref->column_names[0] == "shortest_path_cte") {
			continue;
		}
		// 'rowid' is a column duckdb binds automatically.
		if (column_ref->column_names.back() == "rowid") {
			continue;
		}
		if (column_ref->column_names.size() == 1) {
			bool single_alias = false;
			for (const auto &alias : alias_to_vertex_and_edge_tables) {
				if (alias.first == column_ref->column_names[0]) {
					single_alias = true;
					break;
				}
			}
			if (single_alias) {
				continue;
			}
		}
		const auto cur_fq_col_name = StringUtil::Join(column_ref->column_names, /*separator=*/".");
		if (all_fq_col_names.find(cur_fq_col_name) == all_fq_col_names.end()) {
			throw BinderException("Property %s is never registered!", cur_fq_col_name);
		}
	}
}

// Build the __orp_steps CTE that unnests a shortest-path list into per-step tuples.
// path_cte_name is the CTE that contains a "path" column (list of rowids).
// For VERTEX: one column __orp_v_rowid  (vertices: positions 1,3,5,... i.e. slice(1,-1,2))
// For EDGE:   one column __orp_e_rowid  (edges:    positions 2,4,6,... i.e. slice(2,-1,2))
// For STEP:   three columns v1/e1/v2 rowids (slices 1, 2, 3)
static unique_ptr<CommonTableExpressionInfo> BuildORPStepsCTE(OneRowPerMode mode,
                                                               const string &path_cte_name) {
	auto sel = make_uniq<SelectNode>();

	auto make_unnest_slice = [&](int start, const string &alias_name) {
		vector<unique_ptr<ParsedExpression>> slice_args;
		slice_args.push_back(make_uniq<ColumnRefExpression>("path", path_cte_name));
		slice_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(start)));
		slice_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(-1)));
		slice_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(2)));
		auto slice = make_uniq<FunctionExpression>("list_slice", std::move(slice_args));

		vector<unique_ptr<ParsedExpression>> unnest_args;
		unnest_args.push_back(std::move(slice));
		auto fn = make_uniq<FunctionExpression>("unnest", std::move(unnest_args));
		fn->alias = alias_name;
		return fn;
	};

	switch (mode) {
	case OneRowPerMode::VERTEX:
		sel->select_list.push_back(make_unnest_slice(1, "__orp_v_rowid"));
		break;
	case OneRowPerMode::EDGE:
		sel->select_list.push_back(make_unnest_slice(2, "__orp_e_rowid"));
		break;
	case OneRowPerMode::STEP:
		sel->select_list.push_back(make_unnest_slice(1, "__orp_v1_rowid"));
		sel->select_list.push_back(make_unnest_slice(2, "__orp_e1_rowid"));
		sel->select_list.push_back(make_unnest_slice(3, "__orp_v2_rowid"));
		break;
	default:
		break;
	}

	// Always expose src/dst rowids so endpoint vars (a, b) can be rejoined for WHERE filtering.
	auto src_col = make_uniq<ColumnRefExpression>("src_rowid", path_cte_name);
	src_col->alias = "__orp_src_rowid";
	sel->select_list.push_back(std::move(src_col));
	auto dst_col = make_uniq<ColumnRefExpression>("dst_rowid", path_cte_name);
	dst_col->alias = "__orp_dst_rowid";
	sel->select_list.push_back(std::move(dst_col));

	// FROM path_cte WHERE path IS NOT NULL
	auto from_ref = make_uniq<BaseTableRef>();
	from_ref->table_name = path_cte_name;
	sel->from_table = std::move(from_ref);

	vector<unique_ptr<ParsedExpression>> not_null_args;
	not_null_args.push_back(make_uniq<ColumnRefExpression>("path", path_cte_name));
	sel->where_clause =
	    make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, std::move(not_null_args));

	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(sel);
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	cte_info->query = std::move(stmt);
	return cte_info;
}

// File-local helper: cast a PathReference to PathElement (returns nullptr for SubPath).
static PathElement *AsPathElement(const unique_ptr<PathReference> &ref) {
	return PGQMatchFunction::GetPathElement(ref);
}

// Given a path_elements vector, find the quantified SubPath and return the
// adjacent src/dst PathElements and the edge PathElement inside the SubPath.
// Returns false if no quantified subpath is found.
static bool FindQuantifiedPathElements(const vector<unique_ptr<PathReference>> &path_elements,
                                       PathElement *&out_src, PathElement *&out_edge, PathElement *&out_dst) {
	for (idx_t i = 1; i + 1 < path_elements.size(); i += 2) {
		auto *sp = dynamic_cast<SubPath *>(path_elements[i].get());
		if (sp && sp->upper > 1) {
			out_src = AsPathElement(path_elements[i - 1]);
			out_dst = AsPathElement(path_elements[i + 1]);
			out_edge = sp->path_list.empty() ? nullptr : AsPathElement(sp->path_list[0]);
			return out_src != nullptr && out_dst != nullptr && out_edge != nullptr;
		}
	}
	return false;
}

// Apply ONE ROW PER transformation after ProcessPathList has run.
// Replaces the from_clause and conditions with step-unnested equivalents.
static void ApplyOneRowPer(MatchExpression &ref, CreatePropertyGraphInfo &pg_table,
                           unique_ptr<SelectNode> &final_select_node,
                           vector<unique_ptr<ParsedExpression>> &conditions,
                           case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_to_vertex_and_edge_tables) {
	static const string kPathCTE = "shortest_path_cte";
	static const string kORPCTE = "__orp_steps";

	if (final_select_node->cte_map.map.find(kPathCTE) == final_select_node->cte_map.map.end()) {
		throw NotImplementedException(
		    "ONE ROW PER requires a quantified path pattern (e.g., ->{1,3}). "
		    "No shortest-path CTE was generated for this query.");
	}

	// Find the quantified subpath to get vertex/edge tables
	PathElement *src_elem = nullptr, *edge_elem = nullptr, *dst_elem = nullptr;
	if (ref.path_patterns.empty() ||
	    !FindQuantifiedPathElements(ref.path_patterns[0]->path_elements, src_elem, edge_elem, dst_elem)) {
		throw NotImplementedException("ONE ROW PER: could not locate quantified path element");
	}

	auto src_table = ResolveTable(src_elem->label, src_elem->match_type, pg_table);
	auto dst_table = ResolveTable(dst_elem->label, dst_elem->match_type, pg_table);
	auto edge_table = ResolveTable(edge_elem->label, edge_elem->match_type, pg_table);

	// Add __orp_steps CTE
	final_select_node->cte_map.map[kORPCTE] = BuildORPStepsCTE(ref.one_row_per, kPathCTE);

	// Replace from_clause: start from __orp_steps
	auto orp_ref = make_uniq<BaseTableRef>();
	orp_ref->table_name = kORPCTE;
	final_select_node->from_table = std::move(orp_ref);

	// Clear the old endpoint-matching conditions (they reference a/b and shortest_path_cte.src/dst_rowid)
	conditions.clear();

	// Helper: cross-join a table and add a rowid-matching condition
	auto add_step_join = [&](const string &var, const string &rowid_col,
	                          const shared_ptr<PropertyGraphTable> &pg_tbl) {
		auto tbl_ref = pg_tbl->CreateBaseTableRef();
		tbl_ref->alias = var;
		auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
		join->left = std::move(final_select_node->from_table);
		join->right = std::move(tbl_ref);
		final_select_node->from_table = std::move(join);
		conditions.push_back(make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>("rowid", var),
		    make_uniq<ColumnRefExpression>(rowid_col, kORPCTE)));
		alias_to_vertex_and_edge_tables[var] = pg_tbl;
	};

	// Re-join the endpoint tables (src/dst of the path) so WHERE conditions on a and b remain valid.
	// __orp_steps exposes __orp_src_rowid / __orp_dst_rowid for this purpose.
	add_step_join(src_elem->variable_binding, "__orp_src_rowid", src_table);
	if (dst_elem->variable_binding != src_elem->variable_binding) {
		add_step_join(dst_elem->variable_binding, "__orp_dst_rowid", dst_table);
	}

	const auto &vars = ref.one_row_per_vars;
	switch (ref.one_row_per) {
	case OneRowPerMode::VERTEX:
		add_step_join(vars[0], "__orp_v_rowid", src_table);
		break;
	case OneRowPerMode::EDGE:
		add_step_join(vars[0], "__orp_e_rowid", edge_table);
		break;
	case OneRowPerMode::STEP:
		add_step_join(vars[0], "__orp_v1_rowid", src_table);
		add_step_join(vars[1], "__orp_e1_rowid", edge_table);
		add_step_join(vars[2], "__orp_v2_rowid", dst_table);
		break;
	default:
		break;
	}
}

// Build an EXISTS (or NOT EXISTS) subquery from a PGQ path pattern.
// Variables already in outer_alias_map are correlated references — they are NOT
// added to the inner FROM clause.
static unique_ptr<ParsedExpression> BuildExistsSubquery(
    MatchExpression &inner_match, CreatePropertyGraphInfo &pg_table,
    const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &outer_alias_map,
    SubqueryType sub_type) {

	auto inner_select = make_uniq<SelectNode>();
	inner_select->select_list.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));

	// Pre-populate with outer aliases so ProcessPathList can resolve correlated vars.
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> inner_alias_map = outer_alias_map;

	vector<unique_ptr<ParsedExpression>> conditions;
	int32_t extra_alias_counter = 0;

	for (auto &path_pattern : inner_match.path_patterns) {
		bool all_shortest = path_pattern->all && path_pattern->shortest;
		PGQMatchFunction::ProcessPathList(path_pattern->path_elements, conditions, inner_select, inner_alias_map,
		                                  pg_table, extra_alias_counter, inner_match, all_shortest);
	}

	// Build FROM only for NEW variables (not in outer scope).
	for (auto &entry : inner_alias_map) {
		if (outer_alias_map.find(entry.first) != outer_alias_map.end()) {
			continue;
		}
		auto table_ref = entry.second->CreateBaseTableRef();
		table_ref->alias = entry.first;
		if (inner_select->from_table) {
			auto cross = make_uniq<JoinRef>(JoinRefType::CROSS);
			cross->left = std::move(inner_select->from_table);
			cross->right = std::move(table_ref);
			inner_select->from_table = std::move(cross);
		} else {
			inner_select->from_table = std::move(table_ref);
		}
	}

	if (inner_match.where_clause) {
		conditions.push_back(std::move(inner_match.where_clause));
	}
	inner_select->where_clause = PGQMatchFunction::CreateWhereClause(conditions);

	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(inner_select);

	auto subq = make_uniq<SubqueryExpression>();
	subq->subquery = std::move(stmt);
	// DuckDB's planner only handles SubqueryType::EXISTS; NOT EXISTS is EXISTS wrapped in NOT.
	subq->subquery_type = SubqueryType::EXISTS;

	if (sub_type == SubqueryType::NOT_EXISTS) {
		return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(subq));
	}
	return subq;
}

// Walk expr, replacing pgq_exists(MatchExpression) with EXISTS/NOT EXISTS subqueries.
static unique_ptr<ParsedExpression> RewritePGQExists(
    unique_ptr<ParsedExpression> expr, CreatePropertyGraphInfo &pg_table,
    const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &outer_alias_map) {

	if (!expr) {
		return expr;
	}

	// NOT EXISTS: OperatorExpression(NOT, FunctionExpression("pgq_exists", ...))
	auto *op = dynamic_cast<OperatorExpression *>(expr.get());
	if (op && op->type == ExpressionType::OPERATOR_NOT && op->children.size() == 1) {
		auto *inner_func = dynamic_cast<FunctionExpression *>(op->children[0].get());
		if (inner_func && inner_func->function_name == "pgq_exists" && inner_func->children.size() == 1) {
			auto *inner_match = dynamic_cast<MatchExpression *>(inner_func->children[0].get());
			if (inner_match) {
				return BuildExistsSubquery(*inner_match, pg_table, outer_alias_map, SubqueryType::NOT_EXISTS);
			}
		}
		for (auto &child : op->children) {
			child = RewritePGQExists(std::move(child), pg_table, outer_alias_map);
		}
		return expr;
	}

	// EXISTS: FunctionExpression("pgq_exists", [MatchExpression])
	auto *func = dynamic_cast<FunctionExpression *>(expr.get());
	if (func && func->function_name == "pgq_exists" && func->children.size() == 1) {
		auto *inner_match = dynamic_cast<MatchExpression *>(func->children[0].get());
		if (inner_match) {
			return BuildExistsSubquery(*inner_match, pg_table, outer_alias_map, SubqueryType::EXISTS);
		}
	}

	// Recurse into conjunction/disjunction children
	auto *conj = dynamic_cast<ConjunctionExpression *>(expr.get());
	if (conj) {
		for (auto &child : conj->children) {
			child = RewritePGQExists(std::move(child), pg_table, outer_alias_map);
		}
		return expr;
	}

	// Recurse into comparison children
	auto *comp = dynamic_cast<ComparisonExpression *>(expr.get());
	if (comp) {
		comp->left = RewritePGQExists(std::move(comp->left), pg_table, outer_alias_map);
		comp->right = RewritePGQExists(std::move(comp->right), pg_table, outer_alias_map);
		return expr;
	}

	// Recurse into other function children
	if (func) {
		for (auto &child : func->children) {
			child = RewritePGQExists(std::move(child), pg_table, outer_alias_map);
		}
		return expr;
	}

	return expr;
}

static vector<unique_ptr<ParsedExpression>>
BuildFinalColumnList(vector<unique_ptr<ParsedExpression>> col_list,
                     const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_to_vertex_and_edge_tables) {
	vector<unique_ptr<ParsedExpression>> final_column_list;
	for (auto &expression : col_list) {
		unordered_set<string> named_subpaths;

		auto *column_ref = dynamic_cast<ColumnRefExpression *>(expression.get());
		if (column_ref != nullptr) {
			if (named_subpaths.count(column_ref->column_names[0]) && column_ref->column_names.size() == 1) {
				final_column_list.emplace_back(make_uniq<ColumnRefExpression>("path", column_ref->column_names[0]));
			} else {
				final_column_list.push_back(std::move(expression));
			}
			continue;
		}

		auto *function_ref = dynamic_cast<FunctionExpression *>(expression.get());
		if (function_ref != nullptr) {
			if (function_ref->function_name == "path_length") {
				column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->children[0].get());
				if (column_ref == nullptr) {
					continue;
				}
				if (named_subpaths.count(column_ref->column_names[0]) && column_ref->column_names.size() == 1) {
					auto path_ref = make_uniq<ColumnRefExpression>("path", column_ref->column_names[0]);
					vector<unique_ptr<ParsedExpression>> path_children;
					path_children.push_back(std::move(path_ref));
					auto path_len = make_uniq<FunctionExpression>("len", std::move(path_children));
					auto constant_two = make_uniq<ConstantExpression>(Value::INTEGER(2));
					vector<unique_ptr<ParsedExpression>> div_children;
					div_children.push_back(std::move(path_len));
					div_children.push_back(std::move(constant_two));
					auto div_expression = make_uniq<FunctionExpression>("//", std::move(div_children));
					div_expression->alias = "path_length_" + column_ref->column_names[0];
					final_column_list.emplace_back(std::move(div_expression));
				}
			} else if (function_ref->function_name == "element_id" && function_ref->children.size() == 1) {
				column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->children[0].get());
				if (column_ref != nullptr && !column_ref->column_names.empty()) {
					string var = column_ref->column_names.back();
					auto rowid_ref = make_uniq<ColumnRefExpression>("rowid", var);
					rowid_ref->alias = function_ref->alias.empty() ? "element_id(" + var + ")" : function_ref->alias;
					final_column_list.emplace_back(std::move(rowid_ref));
				} else {
					final_column_list.push_back(std::move(expression));
				}
			} else if (function_ref->function_name == "label" && function_ref->children.size() == 1) {
				column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->children[0].get());
				if (column_ref != nullptr && !column_ref->column_names.empty()) {
					string var = column_ref->column_names.back();
					auto tbl_iter = alias_to_vertex_and_edge_tables.find(var);
					if (tbl_iter != alias_to_vertex_and_edge_tables.end()) {
						auto label_const = make_uniq<ConstantExpression>(Value(tbl_iter->second->main_label));
						label_const->alias = function_ref->alias.empty() ? "label(" + var + ")" : function_ref->alias;
						final_column_list.emplace_back(std::move(label_const));
					} else {
						final_column_list.push_back(std::move(expression));
					}
				} else {
					final_column_list.push_back(std::move(expression));
				}
			} else {
				final_column_list.push_back(std::move(expression));
			}
			continue;
		}

		auto *star_expression = dynamic_cast<StarExpression *>(expression.get());
		if (star_expression != nullptr) {
			if (!star_expression->relation_name.empty()) {
				auto tbl_iter = alias_to_vertex_and_edge_tables.find(star_expression->relation_name);
				if (tbl_iter == alias_to_vertex_and_edge_tables.end()) {
					continue;
				}
			}
			auto selected_col_exprs =
			    star_expression->relation_name.empty()
			        ? GetColRefExprFromPg(alias_to_vertex_and_edge_tables)
			        : GetColRefExprFromPg(alias_to_vertex_and_edge_tables, star_expression->relation_name);
			if (selected_col_exprs.empty()) {
				final_column_list.emplace_back(std::move(expression));
				continue;
			}
			final_column_list.reserve(final_column_list.size() + selected_col_exprs.size());
			for (auto &expr : selected_col_exprs) {
				final_column_list.emplace_back(std::move(expr));
			}
			continue;
		}

		final_column_list.emplace_back(std::move(expression));
	}
	return final_column_list;
}

unique_ptr<TableRef> PGQMatchFunction::MatchBindReplace(ClientContext &context, TableFunctionBindInput &bind_input) {
	auto duckpgq_state = GetDuckPGQState(context);

	auto match_index = bind_input.inputs[0].GetValue<int32_t>();
	auto *ref = dynamic_cast<MatchExpression *>(duckpgq_state->transform_expression[match_index].get());
	auto *pg_table = duckpgq_state->GetPropertyGraph(ref->pg_name);

	vector<unique_ptr<ParsedExpression>> conditions;

	auto final_select_node = make_uniq<SelectNode>();
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> alias_map;

	int32_t extra_alias_counter = 0;
	for (idx_t idx_i = 0; idx_i < ref->path_patterns.size(); idx_i++) {
		auto &path_pattern = ref->path_patterns[idx_i];
		// Check if the element is PathElement or a Subpath with potentially many
		// items
		bool all_shortest = path_pattern->all && path_pattern->shortest;
		ProcessPathList(path_pattern->path_elements, conditions, final_select_node, alias_map, *pg_table,
		                extra_alias_counter, *ref, all_shortest);
	}

	// Go through all aliases encountered
	for (auto &table_alias_entry : alias_map) {
		auto table_ref = table_alias_entry.second->CreateBaseTableRef();
		table_ref->alias = table_alias_entry.first;
		if (final_select_node->from_table) {
			auto new_root = make_uniq<JoinRef>(JoinRefType::CROSS);
			new_root->left = std::move(final_select_node->from_table);
			new_root->right = std::move(table_ref);
			final_select_node->from_table = std::move(new_root);
		} else {
			final_select_node->from_table = std::move(table_ref);
		}
	}

	// Process OPTIONAL MATCH groups — each group becomes a chain of LEFT JOINs.
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> optional_alias_map;
	for (auto &opt_group : ref->optional_match_patterns) {
		ProcessOptionalPathList(opt_group, final_select_node, optional_alias_map, *pg_table, extra_alias_counter, alias_map);
	}

	// Maps from table alias to table, including vertex and edge tables.
	auto alias_to_vertex_and_edge_tables = PopulateGraphTableAliasMap(*pg_table, *ref);
	// Merge optional aliases so column binding and function rewriting cover them too.
	for (auto &entry : optional_alias_map) {
		alias_to_vertex_and_edge_tables.emplace(entry.first, entry.second);
	}

	// ONE ROW PER: replace the from_clause and conditions with step-unnested equivalents.
	// Must run after alias_to_vertex_and_edge_tables is built (so we can extend it with step vars)
	// but before CheckColumnBinding (which validates the COLUMNS list).
	if (ref->one_row_per != OneRowPerMode::NONE) {
		ApplyOneRowPer(*ref, *pg_table, final_select_node, conditions, alias_to_vertex_and_edge_tables);
	}

	CheckColumnBinding(*pg_table, *ref, alias_to_vertex_and_edge_tables);

	if (ref->where_clause) {
		// Rewrite PGQ EXISTS/NOT EXISTS predicates first, then standard rewrites.
		ref->where_clause = RewritePGQExists(std::move(ref->where_clause), *pg_table, alias_map);
		auto where = RewriteAllDifferent(std::move(ref->where_clause));
		where = RewriteGraphFunctions(std::move(where), alias_to_vertex_and_edge_tables);
		conditions.push_back(std::move(where));
	}

	// Deep-copy the column list for each UNION branch before consuming it for the head branch.
	vector<vector<unique_ptr<ParsedExpression>>> union_col_copies;
	for (size_t ui = 0; ui < ref->union_matches.size(); ui++) {
		vector<unique_ptr<ParsedExpression>> branch_cols;
		for (auto &expr : ref->column_list) {
			branch_cols.push_back(expr->Copy());
		}
		union_col_copies.push_back(std::move(branch_cols));
	}

	final_select_node->where_clause = CreateWhereClause(conditions);
	final_select_node->select_list = BuildFinalColumnList(std::move(ref->column_list), alias_to_vertex_and_edge_tables);

	if (ref->union_matches.empty()) {
		auto subquery = make_uniq<SelectStatement>();
		subquery->node = std::move(final_select_node);
		return make_uniq<SubqueryRef>(std::move(subquery), ref->alias);
	}

	// Build UNION chain: head branch then each union branch.
	unique_ptr<QueryNode> current_node = std::move(final_select_node);

	for (size_t ui = 0; ui < ref->union_matches.size(); ui++) {
		auto &branch = *ref->union_matches[ui];
		bool all = ref->union_all_flags[ui];

		auto branch_select = make_uniq<SelectNode>();
		case_insensitive_map_t<shared_ptr<PropertyGraphTable>> branch_alias_map;
		vector<unique_ptr<ParsedExpression>> branch_conditions;
		int32_t branch_counter = 0;

		for (auto &path_pattern : branch.path_patterns) {
			bool all_shortest = path_pattern->all && path_pattern->shortest;
			ProcessPathList(path_pattern->path_elements, branch_conditions, branch_select,
			                branch_alias_map, *pg_table, branch_counter, branch, all_shortest);
		}

		for (auto &entry : branch_alias_map) {
			auto tbl_ref = entry.second->CreateBaseTableRef();
			tbl_ref->alias = entry.first;
			if (branch_select->from_table) {
				auto cross = make_uniq<JoinRef>(JoinRefType::CROSS);
				cross->left = std::move(branch_select->from_table);
				cross->right = std::move(tbl_ref);
				branch_select->from_table = std::move(cross);
			} else {
				branch_select->from_table = std::move(tbl_ref);
			}
		}

		case_insensitive_map_t<shared_ptr<PropertyGraphTable>> branch_opt_alias_map;
		for (auto &opt_group : branch.optional_match_patterns) {
			ProcessOptionalPathList(opt_group, branch_select, branch_opt_alias_map, *pg_table, branch_counter,
			                        branch_alias_map);
		}

		auto branch_alias_to_ve = PopulateGraphTableAliasMap(*pg_table, branch);
		for (auto &entry : branch_opt_alias_map) {
			branch_alias_to_ve.emplace(entry.first, entry.second);
		}

		if (branch.one_row_per != OneRowPerMode::NONE) {
			ApplyOneRowPer(branch, *pg_table, branch_select, branch_conditions, branch_alias_to_ve);
		}

		if (branch.where_clause) {
			branch.where_clause = RewritePGQExists(std::move(branch.where_clause), *pg_table, branch_alias_map);
			auto where = RewriteAllDifferent(std::move(branch.where_clause));
			where = RewriteGraphFunctions(std::move(where), branch_alias_to_ve);
			branch_conditions.push_back(std::move(where));
		}

		branch_select->where_clause = CreateWhereClause(branch_conditions);
		branch_select->select_list = BuildFinalColumnList(std::move(union_col_copies[ui]), branch_alias_to_ve);

		auto set_op = make_uniq<SetOperationNode>();
		set_op->setop_type = SetOperationType::UNION;
		set_op->setop_all = all;
		set_op->children.push_back(std::move(current_node));
		set_op->children.push_back(std::move(branch_select));
		current_node = std::move(set_op);
	}

	auto subquery = make_uniq<SelectStatement>();
	subquery->node = std::move(current_node);
	return make_uniq<SubqueryRef>(std::move(subquery), ref->alias);
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CoreTableFunctions::RegisterMatchTableFunction(ExtensionLoader &loader) {
	loader.RegisterFunction(PGQMatchFunction());
}

} // namespace duckdb
