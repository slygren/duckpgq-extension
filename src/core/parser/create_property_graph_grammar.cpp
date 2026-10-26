#include "duckpgq/core/parser/duckpgq_grammar.hpp"
#include "duckpgq/core/parser/duckpgq_parser.hpp"

#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/extension_statement.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckpgq/parser/parsed_data/create_property_graph_info.hpp"
#include "duckpgq_create_property_graph_grammar.hpp"

namespace duckdb {

namespace {

template <class T>
static vector<T> TransformParensList(PEGTransformer &transformer, ParseResult &parse_result) {
	vector<T> result;
	auto &list = PEGTransformerFactory::ExtractResultFromParens(parse_result);
	for (auto &item : PEGTransformerFactory::ExtractParseResultsFromList(list)) {
		result.push_back(transformer.Transform<T>(item.get()));
	}
	return result;
}

//! One PROPERTIES clause (or its absence, which SQL/PGQ reads as PROPERTIES ARE ALL COLUMNS).
struct PropertyClause {
	enum class Kind { ALL_COLUMNS, LIST, NONE } kind = Kind::ALL_COLUMNS;
	vector<Identifier> except_columns;
	vector<Identifier> column_names;
	vector<Identifier> column_aliases;
	vector<string> property_expressions;
};

//! A computed property's expression, as SQL text; refused where a property cannot be one value of its own row.
static string PropertyExpressionSQL(ParsedExpression &expression, const Identifier &name) {
	bool unsupported = false;
	std::function<void(ParsedExpression &)> check = [&](ParsedExpression &child) {
		if (child.GetExpressionClass() == ExpressionClass::SUBQUERY ||
		    child.GetExpressionClass() == ExpressionClass::WINDOW ||
		    child.GetExpressionClass() == ExpressionClass::STAR) {
			unsupported = true;
		}
		ParsedExpressionIterator::EnumerateChildren(child, check);
	};
	check(expression);
	if (unsupported) {
		throw ParserException("Property %s: a property expression cannot contain a subquery, a window function or *",
		                      name.GetIdentifierName());
	}
	return expression.ToString();
}

static PropertyClause TransformProperties(PEGTransformer &transformer, ParseResult &properties) {
	PropertyClause clause;
	auto &choice = DuckPGQGetChoice(properties);
	auto &list = choice.Cast<ListParseResult>();
	if (choice.Name() == "DuckPGQNoProperties") {
		clause.kind = PropertyClause::Kind::NONE;
	} else if (choice.Name() == "DuckPGQPropertyList") {
		clause.kind = PropertyClause::Kind::LIST;
		auto &items = PEGTransformerFactory::ExtractResultFromParens(list.GetChild(1));
		for (auto &item : PEGTransformerFactory::ExtractParseResultsFromList(items)) {
			auto &property = item.get().Cast<ListParseResult>();
			auto expression = transformer.Transform<unique_ptr<ParsedExpression>>(property.GetChild(0));
			if (expression->GetExpressionClass() == ExpressionClass::STAR) {
				throw ParserException("syntax error at or near \"*\": write PROPERTIES ARE ALL COLUMNS for every "
				                      "column, or leave the clause out");
			}
			auto &alias = property.Child<OptionalParseResult>(1);
			optional<Identifier> name;
			if (alias.HasResult()) {
				name = transformer.Transform<Identifier>(alias.GetResult().Cast<ListParseResult>().GetChild(1));
			}
			if (expression->GetExpressionClass() == ExpressionClass::COLUMN_REF &&
			    expression->Cast<ColumnRefExpression>().ColumnNames().size() == 1) {
				// A column, possibly renamed.
				auto column = expression->Cast<ColumnRefExpression>().ColumnNames()[0];
				clause.column_names.push_back(column);
				clause.column_aliases.push_back(name ? *name : column);
				clause.property_expressions.emplace_back();
				continue;
			}
			if (!name) {
				throw ParserException("Property %s is an expression and needs a name: %s AS <name>",
				                      expression->ToString(), expression->ToString());
			}
			// A computed property: its name stands in for the column it reads from.
			clause.column_names.push_back(*name);
			clause.column_aliases.push_back(*name);
			clause.property_expressions.push_back(PropertyExpressionSQL(*expression, *name));
		}
	} else if (choice.Name() == "DuckPGQAllColumnsExcept") {
		clause.except_columns = TransformParensList<Identifier>(transformer, list.GetChild(5));
	}
	return clause;
}

static bool ContainsIdentifier(const vector<Identifier> &list, const Identifier &value) {
	for (auto &entry : list) {
		if (StringUtil::CIEquals(entry.GetIdentifierName(), value.GetIdentifierName())) {
			return true;
		}
	}
	return false;
}

//! An element table's properties are those of all of its labels (SQL/PGQ: the element has every property of every
//! label it has). With one label this is that label's PROPERTIES clause, as before labels could be repeated.
static void ApplyPropertyUnion(const vector<PropertyClause> &clauses, PropertyGraphTable &table) {
	bool all_none = true;
	bool first_all = true;
	for (auto &clause : clauses) {
		all_none = all_none && clause.kind == PropertyClause::Kind::NONE;
		if (clause.kind == PropertyClause::Kind::ALL_COLUMNS) {
			if (first_all) {
				table.except_columns = clause.except_columns;
				first_all = false;
			} else {
				// A column is a property unless every ALL COLUMNS clause excepts it.
				vector<Identifier> kept;
				for (auto &column : table.except_columns) {
					if (ContainsIdentifier(clause.except_columns, column)) {
						kept.push_back(column);
					}
				}
				table.except_columns = std::move(kept);
			}
			table.all_columns = true;
		}
	}
	if (all_none) {
		table.no_columns = true;
		return;
	}
	for (auto &clause : clauses) {
		if (clause.kind != PropertyClause::Kind::LIST) {
			continue;
		}
		for (idx_t i = 0; i < clause.column_names.size(); i++) {
			auto &alias = clause.column_aliases[i];
			bool known = false;
			for (idx_t j = 0; j < table.column_aliases.size(); j++) {
				if (!StringUtil::CIEquals(table.column_aliases[j].GetIdentifierName(), alias.GetIdentifierName())) {
					continue;
				}
				if (!StringUtil::CIEquals(table.column_names[j].GetIdentifierName(),
				                          clause.column_names[i].GetIdentifierName()) ||
				    table.property_expressions[j] != clause.property_expressions[i]) {
					throw ParserException("Property %s of element table %s is defined twice, differently",
					                      alias.GetIdentifierName(), table.table_name.GetIdentifierName());
				}
				known = true;
			}
			if (known) {
				continue;
			}
			table.column_names.push_back(clause.column_names[i]);
			table.column_aliases.push_back(alias);
			table.property_expressions.push_back(clause.property_expressions[i]);
		}
	}
	if (table.all_columns) {
		// A column one label lists is a property even where another label's ALL COLUMNS clause excepts it.
		vector<Identifier> kept;
		for (auto &column : table.except_columns) {
			if (!ContainsIdentifier(table.column_aliases, column)) {
				kept.push_back(column);
			}
		}
		table.except_columns = std::move(kept);
	}
}

static void ApplySubLabels(PEGTransformer &transformer, ParseResult &parse_result, PropertyGraphTable &table) {
	if (!table.discriminator.empty()) {
		throw ParserException("Element table %s has more than one IN discriminator (...) clause",
		                      table.table_name.GetIdentifierName());
	}
	auto &list = parse_result.Cast<ListParseResult>();
	table.discriminator = transformer.Transform<Identifier>(list.GetChild(1));
	table.sub_labels = TransformParensList<Identifier>(transformer, list.GetChild(2));
}

//! [PROPERTIES ...] { LABEL l | DEFAULT LABEL } [IN d (...)] [PROPERTIES ...] ... [IN d (...)]
static void ApplyLabelsAndProperties(PEGTransformer &transformer, ParseResult &parse_result,
                                     PropertyGraphTable &table) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto &leading_properties = list.Child<OptionalParseResult>(0);
	auto &entries = list.Child<OptionalParseResult>(1);
	auto &trailing_sub_labels = list.Child<OptionalParseResult>(2);
	// DEFAULT LABEL, and no label clause at all, name the label after the element table's alias.
	auto default_label = table.table_name_alias.empty() ? table.table_name : table.table_name_alias;

	vector<Identifier> labels;
	vector<PropertyClause> clauses;
	if (entries.HasResult()) {
		for (auto &entry_result : entries.GetResult().Cast<RepeatParseResult>().GetChildren()) {
			auto &entry = entry_result.get().Cast<ListParseResult>();
			auto &name = DuckPGQGetChoice(entry.GetChild(0));
			auto label = name.Name() == "DuckPGQDefaultLabel"
			                 ? default_label
			                 : transformer.Transform<Identifier>(name.Cast<ListParseResult>().GetChild(1));
			if (ContainsIdentifier(labels, label)) {
				throw ParserException("Label %s is given twice for element table %s", label.GetIdentifierName(),
				                      table.table_name.GetIdentifierName());
			}
			auto &sub_labels = entry.Child<OptionalParseResult>(1);
			if (sub_labels.HasResult()) {
				ApplySubLabels(transformer, sub_labels.GetResult(), table);
			}
			auto &properties = entry.Child<OptionalParseResult>(2);
			PropertyClause clause;
			if (labels.empty() && leading_properties.HasResult()) {
				if (properties.HasResult()) {
					throw ParserException("Label %s of element table %s has two PROPERTIES clauses",
					                      label.GetIdentifierName(), table.table_name.GetIdentifierName());
				}
				clause = TransformProperties(transformer, leading_properties.GetResult());
			} else if (properties.HasResult()) {
				clause = TransformProperties(transformer, properties.GetResult());
			}
			labels.push_back(label);
			clauses.push_back(std::move(clause));
		}
	} else {
		labels.push_back(default_label);
		clauses.push_back(leading_properties.HasResult()
		                      ? TransformProperties(transformer, leading_properties.GetResult())
		                      : PropertyClause());
	}
	if (trailing_sub_labels.HasResult()) {
		ApplySubLabels(transformer, trailing_sub_labels.GetResult(), table);
	}
	table.main_label = labels[0];
	table.extra_labels.assign(labels.begin() + 1, labels.end());
	ApplyPropertyUnion(clauses, table);
}

static void ApplyReference(PEGTransformer &transformer, ParseResult &parse_result, PropertyGraphTable &edge,
                           bool source) {
	auto &reference = DuckPGQGetChoice(parse_result.Cast<ListParseResult>().GetChild(1));
	unique_ptr<BaseTableRef> base_table;
	auto &foreign_keys = source ? edge.source_fk : edge.destination_fk;
	auto &primary_keys = source ? edge.source_pk : edge.destination_pk;
	if (reference.Name() == "DuckPGQKeyReference") {
		auto &list = reference.Cast<ListParseResult>();
		foreign_keys = TransformParensList<Identifier>(transformer, list.GetChild(1));
		base_table = transformer.Transform<unique_ptr<BaseTableRef>>(list.GetChild(3));
		// Without a column list the vertex table's key is referenced; CREATE PROPERTY GRAPH resolves it.
		auto &referenced = list.Child<OptionalParseResult>(4);
		if (referenced.HasResult()) {
			primary_keys = TransformParensList<Identifier>(transformer, referenced.GetResult());
		}
	} else {
		base_table = transformer.Transform<unique_ptr<BaseTableRef>>(reference);
	}
	auto &name = base_table->GetQualifiedName();
	(source ? edge.source_catalog : edge.destination_catalog) = name.Catalog();
	(source ? edge.source_schema : edge.destination_schema) = name.Schema();
	(source ? edge.source_reference : edge.destination_reference) = name.Name();
}

// Called directly rather than through a registered transform: an extension-defined transform result type would
// instantiate the dllimport-declared TypedTransformResult template, which does not link on Windows.
static shared_ptr<PropertyGraphTable> TransformGraphTable(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto table = make_shared_ptr<PropertyGraphTable>();
	table->is_vertex_table = parse_result.Name() == "DuckPGQVertexTable";
	auto base_table = transformer.Transform<unique_ptr<BaseTableRef>>(list.GetChild(0));
	auto &name = base_table->GetQualifiedName();
	table->catalog_name = name.Catalog();
	table->schema_name = name.Schema();
	table->table_name = name.Name();
	auto &alias = list.Child<OptionalParseResult>(1);
	if (alias.HasResult()) {
		table->table_name_alias = transformer.Transform<TableAlias>(alias.GetResult()).name;
	}
	auto &element_key = list.Child<OptionalParseResult>(2);
	if (element_key.HasResult()) {
		table->element_key =
		    TransformParensList<Identifier>(transformer, element_key.GetResult().Cast<ListParseResult>().GetChild(1));
	}
	idx_t labels_index = 3;
	if (!table->is_vertex_table) {
		ApplyReference(transformer, list.GetChild(3), *table, true);
		ApplyReference(transformer, list.GetChild(4), *table, false);
		labels_index = 5;
	}
	ApplyLabelsAndProperties(transformer, list.GetChild(labels_index), *table);
	return table;
}

static vector<shared_ptr<PropertyGraphTable>> TransformGraphTables(PEGTransformer &transformer,
                                                                   ParseResult &parse_result) {
	vector<shared_ptr<PropertyGraphTable>> result;
	auto &list = PEGTransformerFactory::ExtractResultFromParens(parse_result);
	for (auto &item : PEGTransformerFactory::ExtractParseResultsFromList(list)) {
		result.push_back(TransformGraphTable(transformer, item.get()));
	}
	return result;
}

static arena_ptr<TransformResultValue> TransformCreateGraph(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto or_replace = list.Child<OptionalParseResult>(1).HasResult();
	auto if_not_exists = list.Child<OptionalParseResult>(4).HasResult();
	if (or_replace && if_not_exists) {
		throw ParserException("Cannot specify both OR REPLACE and IF NOT EXISTS within single create statement");
	}
	auto name = transformer.Transform<QualifiedName>(list.GetChild(5));
	if (name.Name().empty()) {
		throw ParserException("Empty property graph name not supported");
	}
	auto info = make_uniq<CreatePropertyGraphInfo>(name.Name().GetIdentifierName());
	info->SetQualifiedName(name);
	info->on_conflict = or_replace      ? OnCreateConflict::REPLACE_ON_CONFLICT
	                    : if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT
	                                    : OnCreateConflict::ERROR_ON_CONFLICT;
	info->vertex_tables = TransformGraphTables(transformer, list.GetChild(6).Cast<ListParseResult>().GetChild(2));
	auto &edges = list.Child<OptionalParseResult>(7);
	if (edges.HasResult()) {
		info->edge_tables = TransformGraphTables(transformer, edges.GetResult().Cast<ListParseResult>().GetChild(2));
	}
	auto &abi_clause = list.Child<OptionalParseResult>(8);
	if (abi_clause.HasResult()) {
		vector<string> options;
		auto &clause = abi_clause.GetResult().Cast<ListParseResult>();
		auto &option_list = PEGTransformerFactory::ExtractResultFromParens(clause.GetChild(2));
		for (auto &item : PEGTransformerFactory::ExtractParseResultsFromList(option_list)) {
			auto &option = item.get().Cast<ListParseResult>();
			auto name = transformer.Transform<Identifier>(option.GetChild(0));
			auto value = transformer.Transform<unique_ptr<ParsedExpression>>(option.GetChild(2));
			options.push_back(KeywordHelper::WriteQuoted(StringUtil::Lower(name.GetIdentifierName()), '\'') + ": " +
			                  value->ToString());
		}
		// Kept as one STRUCT literal: SELECT of it gives the option values back without re-parsing the clause.
		info->abi_options = "{" + StringUtil::Join(options, ", ") + "}";
	}
	info->ResolveEdgeReferences();
	info->RebuildLabelMap();
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	auto parse_data = make_uniq_base<ParserExtensionParseData, DuckPGQParseData>(std::move(statement));
	unique_ptr<SQLStatement> result = make_uniq<ExtensionStatement>(DuckPGQParserExtension(), std::move(parse_data));
	return transformer.MakeResult<unique_ptr<SQLStatement>>(std::move(result));
}

static bool IsCascade(OptionalParseResult &behavior) {
	return behavior.HasResult() && DuckPGQLiteralText(DuckPGQGetChoice(behavior.GetResult())) == "CASCADE";
}

// 'ALTER' 'PROPERTY' 'GRAPH' QualifiedName DuckPGQAlterGraphAction
static arena_ptr<TransformResultValue> TransformAlterGraph(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto name = transformer.Transform<QualifiedName>(list.GetChild(3));
	auto info = make_uniq<AlterPropertyGraphInfo>(name.Name().GetIdentifierName());
	info->SetQualifiedName(name);
	auto &action = DuckPGQGetChoice(list.GetChild(4));
	auto &action_list = action.Cast<ListParseResult>();
	if (action.Name() == "DuckPGQAddVertexTables" || action.Name() == "DuckPGQAddEdgeTables") {
		const bool vertex = action.Name() == "DuckPGQAddVertexTables";
		info->action = vertex ? PGQAlterGraphAction::ADD_VERTEX_TABLES : PGQAlterGraphAction::ADD_EDGE_TABLES;
		auto tables = TransformGraphTables(transformer, action_list.GetChild(1).Cast<ListParseResult>().GetChild(2));
		(vertex ? info->vertex_tables : info->edge_tables) = std::move(tables);
	} else if (action.Name() == "DuckPGQDropVertexTables" || action.Name() == "DuckPGQDropEdgeTables") {
		info->element_is_vertex = action.Name() == "DuckPGQDropVertexTables";
		info->action = info->element_is_vertex ? PGQAlterGraphAction::DROP_VERTEX_TABLES
		                                       : PGQAlterGraphAction::DROP_EDGE_TABLES;
		info->element_tables = TransformParensList<Identifier>(transformer, action_list.GetChild(3));
		info->cascade = IsCascade(action_list.Child<OptionalParseResult>(4));
	} else {
		// 'ALTER' DuckPGQElementKind 'TABLE' ColId DuckPGQAlterElementAction
		auto &kind = DuckPGQGetChoice(action_list.GetChild(1));
		info->element_is_vertex = kind.Name() == "DuckPGQVertexKeyword";
		info->element_tables.push_back(transformer.Transform<Identifier>(action_list.GetChild(3)));
		auto &element_action = DuckPGQGetChoice(action_list.GetChild(4));
		auto &element_list = element_action.Cast<ListParseResult>();
		info->label = transformer.Transform<Identifier>(element_list.GetChild(1).Cast<ListParseResult>().GetChild(1));
		if (element_action.Name() == "DuckPGQAddLabel") {
			info->action = PGQAlterGraphAction::ADD_LABEL;
			auto &properties = element_list.Child<OptionalParseResult>(2);
			info->label_properties = make_shared_ptr<PropertyGraphTable>();
			info->label_properties->table_name = info->element_tables[0];
			ApplyPropertyUnion({properties.HasResult() ? TransformProperties(transformer, properties.GetResult())
			                                           : PropertyClause()},
			                   *info->label_properties);
		} else {
			info->action = PGQAlterGraphAction::DROP_LABEL;
			info->cascade = IsCascade(element_list.Child<OptionalParseResult>(2));
		}
	}
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	auto parse_data = make_uniq_base<ParserExtensionParseData, DuckPGQParseData>(std::move(statement));
	unique_ptr<SQLStatement> result = make_uniq<ExtensionStatement>(DuckPGQParserExtension(), std::move(parse_data));
	return transformer.MakeResult<unique_ptr<SQLStatement>>(std::move(result));
}

static arena_ptr<TransformProcess> StartAlterGraphTransform(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, TransformAlterGraph);
}

static arena_ptr<TransformProcess> StartCreateGraphTransform(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, TransformCreateGraph);
}

} // namespace

void DuckPGQAddCreatePropertyGraphGrammar(vector<GrammarChange> &changes) {
	// Grammar literals that are not DuckDB keywords match by text, so no keywords need registering.
	for (auto rule : DUCKPGQ_CREATE_PROPERTY_GRAPH_RULES) {
		changes.push_back(GrammarChange::AddRule(rule));
	}
	changes.push_back(GrammarChange::SetTransformProcess("DuckPGQCreatePropertyGraph", StartCreateGraphTransform));
	// Return an ExtensionStatement directly, without intercepting ordinary CREATE statements.
	changes.push_back(GrammarChange::PrependChoice("Statement", "DuckPGQCreatePropertyGraph"));
	changes.push_back(GrammarChange::SetTransformProcess("DuckPGQAlterPropertyGraph", StartAlterGraphTransform));
	changes.push_back(GrammarChange::PrependChoice("Statement", "DuckPGQAlterPropertyGraph"));
}

} // namespace duckdb
