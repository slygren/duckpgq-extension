#include "duckpgq/core/utils/element_relation.hpp"

#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/cte_node.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckpgq/core/utils/duckpgq_sql.hpp"
#include "duckpgq/core/utils/duckpgq_identifier.hpp"
#include "duckpgq/core/utils/duckpgq_utils.hpp"
#include "duckpgq/parser/parsed_data/create_property_graph_info.hpp"

namespace duckdb {

bool PGQElementRelations::Needs(const PropertyGraphTable &table) {
	if (table.is_view) {
		return true;
	}
	for (idx_t i = 0; i < table.column_names.size(); i++) {
		if (!table.PropertyExpression(i).empty() ||
		    !StringUtil::CIEquals(table.PropertyName(i).GetIdentifierName(),
		                          table.column_names[i].GetIdentifierName())) {
			return true;
		}
	}
	return false;
}

bool PGQElementRelations::Needs(const CreatePropertyGraphInfo &graph) {
	for (auto &table : graph.vertex_tables) {
		if (Needs(*table)) {
			return true;
		}
	}
	for (auto &table : graph.edge_tables) {
		if (Needs(*table)) {
			return true;
		}
	}
	return false;
}

string PGQElementRelations::RelationSQL(const PropertyGraphTable &table) {
	return RelationSQL(table, {});
}

string PGQElementRelations::RelationSQL(const PropertyGraphTable &table, const vector<string> &properties) {
	string id;
	if (table.is_view) {
		// A view has no rowid. Its rows are numbered densely (the CSR addresses vertices 0..n-1) in an order that is
		// the same on every scan, so that two reads of the view in one query agree on the ids: by the element key,
		// else by every column (rows that tie are then identical, so it does not matter which gets which id).
		vector<string> order;
		for (auto &column : table.identity_columns) {
			order.push_back(DuckPGQSQL::Identifier(column));
		}
		if (order.empty()) {
			throw InternalException("View %s of a property graph has no identity columns", table.table_name);
		}
		id = "row_number() OVER (ORDER BY " + StringUtil::Join(order, ", ") + ") - 1";
	} else {
		id = "rowid";
	}
	string extra;
	for (auto &property : properties) {
		extra += ", " + property;
	}
	return "SELECT *, " + id + " AS rowid" + extra + " FROM " + DuckPGQSQL::QualifiedTableName(table);
}

namespace {

struct ElementRelationRewriter {
	ElementRelationRewriter(ClientContext &context, const CreatePropertyGraphInfo &graph) : context(context) {
		auto add = [&](const shared_ptr<PropertyGraphTable> &table) {
			if (!PGQElementRelations::Needs(*table)) {
				return;
			}
			auto name = Resolve(table->catalog_name, table->schema_name, table->table_name);
			bool known = false;
			for (auto &entry : tables) {
				known = known || entry.first == name;
			}
			if (!known) {
				tables.emplace_back(name, table.get());
			}
			// Several element tables over one table share its relation, with all of their properties.
			auto &columns = properties[name];
			for (idx_t i = 0; i < table->column_names.size(); i++) {
				auto &property = table->PropertyName(i);
				auto expression = table->PropertyExpression(i);
				if (expression.empty() &&
				    StringUtil::CIEquals(property.GetIdentifierName(), table->column_names[i].GetIdentifierName())) {
					continue;
				}
				// A graph persisted before renamed properties were read may name one after another column of the
				// table; that name keeps reading the column, as it always did.
				if (expression.empty() &&
				    PGQRelationColumnType(context, PGQStr(table->catalog_name), PGQStr(table->schema_name),
				                          PGQStr(table->table_name), PGQStr(property))
				            .id() != LogicalTypeId::INVALID) {
					continue;
				}
				auto definition = (expression.empty() ? DuckPGQSQL::Identifier(table->column_names[i])
				                                      : "(" + expression + ")") +
				                  " AS " + DuckPGQSQL::Identifier(property);
				bool present = false;
				for (auto &column : columns) {
					present = present || column == definition;
				}
				if (!present) {
					columns.push_back(definition);
				}
			}
		};
		for (auto &table : graph.vertex_tables) {
			add(table);
		}
		for (auto &table : graph.edge_tables) {
			add(table);
		}
	}

	ClientContext &context;
	vector<std::pair<string, const PropertyGraphTable *>> tables;
	unordered_map<string, vector<string>> properties;

	//! catalog.schema.name the way DuckDB binds the reference (a lone qualifier names a catalog if one is attached under
	//! that name), so that differently spelled references to one table compare equal.
	string Resolve(const Identifier &catalog_p, const Identifier &schema_p, const Identifier &name) {
		auto catalog = catalog_p;
		auto schema = schema_p;
		try {
			Binder::BindSchemaOrCatalog(context, catalog, schema);
		} catch (std::exception &) {
		}
		if (catalog.empty()) {
			catalog = DatabaseManager::GetDefaultDatabase(context);
		}
		if (schema.empty()) {
			schema = Identifier::DefaultSchema();
		}
		return StringUtil::Lower(catalog.GetIdentifierName() + "." + schema.GetIdentifierName() + "." +
		                         name.GetIdentifierName());
	}

	const vector<string> &Properties(const PropertyGraphTable &table) {
		return properties[Resolve(table.catalog_name, table.schema_name, table.table_name)];
	}

	const PropertyGraphTable *Find(const BaseTableRef &ref) {
		auto &name = ref.GetQualifiedName();
		const PropertyGraphTable *candidate = nullptr;
		for (auto &entry : tables) {
			if (StringUtil::CIEquals(name.Name().GetIdentifierName(), entry.second->table_name.GetIdentifierName())) {
				candidate = entry.second;
			}
		}
		if (!candidate) {
			return nullptr;
		}
		auto resolved = Resolve(name.Catalog(), name.Schema(), name.Name());
		for (auto &entry : tables) {
			if (entry.first == resolved) {
				return entry.second;
			}
		}
		return nullptr;
	}

	void Rewrite(unique_ptr<ParsedExpression> &expression) {
		if (!expression) {
			return;
		}
		if (expression->GetExpressionClass() == ExpressionClass::SUBQUERY) {
			auto &subquery = expression->Cast<SubqueryExpression>();
			if (subquery.SubqueryMutable()) {
				Rewrite(*subquery.SubqueryMutable()->node);
			}
		}
		ParsedExpressionIterator::EnumerateChildren(
		    *expression, [&](unique_ptr<ParsedExpression> &child) { Rewrite(child); });
	}

	void Rewrite(unique_ptr<TableRef> &ref) {
		if (!ref) {
			return;
		}
		switch (ref->type) {
		case TableReferenceType::BASE_TABLE: {
			auto &base = ref->Cast<BaseTableRef>();
			auto table = Find(base);
			if (!table) {
				return;
			}
			auto alias = base.alias.empty() ? table->table_name : base.alias;
			auto relation = DuckPGQSQL::ParseSubqueryRef(
			    PGQElementRelations::RelationSQL(*table, Properties(*table)), PGQStr(alias), "element relation");
			relation->column_name_alias = std::move(base.column_name_alias);
			relation->sample = std::move(base.sample);
			ref = std::move(relation);
			return;
		}
		case TableReferenceType::JOIN: {
			auto &join = ref->Cast<JoinRef>();
			Rewrite(join.left);
			Rewrite(join.right);
			Rewrite(join.condition);
			return;
		}
		case TableReferenceType::SUBQUERY: {
			auto &subquery = ref->Cast<SubqueryRef>();
			Rewrite(*subquery.subquery->node);
			return;
		}
		case TableReferenceType::TABLE_FUNCTION: {
			Rewrite(ref->Cast<TableFunctionRef>().function);
			return;
		}
		case TableReferenceType::EXPRESSION_LIST: {
			for (auto &row : ref->Cast<ExpressionListRef>().values) {
				for (auto &value : row) {
					Rewrite(value);
				}
			}
			return;
		}
		case TableReferenceType::PIVOT: {
			Rewrite(ref->Cast<PivotRef>().source);
			return;
		}
		default:
			return;
		}
	}

	void Rewrite(QueryNode &node) {
		for (auto &entry : node.cte_map.map) {
			if (entry.second && entry.second->query_node) {
				Rewrite(*entry.second->query_node);
			}
		}
		switch (node.type) {
		case QueryNodeType::SELECT_NODE: {
			auto &select = node.Cast<SelectNode>();
			for (auto &expression : select.select_list) {
				Rewrite(expression);
			}
			for (auto &expression : select.groups.group_expressions) {
				Rewrite(expression);
			}
			Rewrite(select.where_clause);
			Rewrite(select.having);
			Rewrite(select.qualify);
			Rewrite(select.from_table);
			break;
		}
		case QueryNodeType::SET_OPERATION_NODE:
			for (auto &child : node.Cast<SetOperationNode>().children) {
				Rewrite(*child);
			}
			break;
		case QueryNodeType::RECURSIVE_CTE_NODE: {
			auto &recursive = node.Cast<RecursiveCTENode>();
			Rewrite(*recursive.left);
			Rewrite(*recursive.right);
			break;
		}
		case QueryNodeType::CTE_NODE: {
			auto &cte = node.Cast<CTENode>();
			if (cte.query) {
				Rewrite(*cte.query);
			}
			if (cte.child) {
				Rewrite(*cte.child);
			}
			break;
		}
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateQueryNodeModifiers(
		    node, [&](unique_ptr<ParsedExpression> &child) { Rewrite(child); });
	}
};

} // namespace

void PGQElementRelations::Apply(ClientContext &context, const CreatePropertyGraphInfo &graph,
                                unique_ptr<TableRef> &ref) {
	if (!Needs(graph)) {
		return;
	}

	ElementRelationRewriter rewriter(context, graph);
	rewriter.Rewrite(ref);
}

void PGQElementRelations::Apply(ClientContext &context, const CreatePropertyGraphInfo &graph, QueryNode &node) {
	if (!Needs(graph)) {
		return;
	}
	ElementRelationRewriter rewriter(context, graph);
	rewriter.Rewrite(node);
}

} // namespace duckdb
