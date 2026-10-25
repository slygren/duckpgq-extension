#include "duckpgq/core/functions/table/vertex_elimination.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckpgq/core/utils/duckpgq_identifier.hpp"
#include "duckpgq/core/utils/duckpgq_utils.hpp"

namespace duckdb {

//------------------------------------------------------------------------------
// Recording
//------------------------------------------------------------------------------
static thread_local vector<PGQVertexEdgeJoin> *active_vertex_join_recorder = nullptr;

PGQVertexJoinRecorder::PGQVertexJoinRecorder(vector<PGQVertexEdgeJoin> &target)
    : previous(active_vertex_join_recorder) {
	active_vertex_join_recorder = &target;
}

PGQVertexJoinRecorder::~PGQVertexJoinRecorder() {
	active_vertex_join_recorder = previous;
}

void PGQVertexJoinRecorder::Record(const string &vertex_alias, const vector<Identifier> &vertex_keys,
                                   const string &edge_alias, const vector<Identifier> &edge_keys,
                                   const shared_ptr<PropertyGraphTable> &edge_table,
                                   const shared_ptr<PropertyGraphTable> &referenced_vertex_table, bool undirected) {
	if (!active_vertex_join_recorder || vertex_keys.size() != edge_keys.size() || vertex_keys.empty()) {
		return;
	}
	PGQVertexEdgeJoin join;
	join.vertex_alias = vertex_alias;
	join.edge_alias = edge_alias;
	join.edge_table = edge_table;
	join.referenced_vertex_table = referenced_vertex_table;
	join.vertex_keys = PGQStrs(vertex_keys);
	join.edge_keys = PGQStrs(edge_keys);
	join.undirected = undirected;
	active_vertex_join_recorder->push_back(std::move(join));
}

//------------------------------------------------------------------------------
// Elimination
//------------------------------------------------------------------------------
namespace {

vector<Identifier> PGQIds(const vector<string> &names) {
	vector<Identifier> result;
	for (auto &name : names) {
		result.emplace_back(name);
	}
	return result;
}

idx_t KeyPosition(const vector<string> &keys, const string &key) {
	for (idx_t i = 0; i < keys.size(); i++) {
		if (StringUtil::CIEquals(keys[i], key)) {
			return i;
		}
	}
	return DConstants::INVALID_INDEX;
}

bool SameKeySet(const vector<string> &left, const vector<string> &right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (idx_t i = 0; i < left.size(); i++) {
		if (KeyPosition(right, left[i]) == DConstants::INVALID_INDEX) {
			return false;
		}
		for (idx_t j = i + 1; j < left.size(); j++) {
			if (StringUtil::CIEquals(left[i], left[j])) {
				return false;
			}
		}
	}
	return true;
}

bool IsColumn(const ParsedExpression &expression, const string &table, const string &column) {
	if (expression.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	auto &names = expression.Cast<ColumnRefExpression>().ColumnNames();
	return names.size() == 2 && StringUtil::CIEquals(PGQStr(names[0]), table) &&
	       StringUtil::CIEquals(PGQStr(names[1]), column);
}

//! Is `expression` the equality vertex.vertex_key = edge.edge_key (in either order)?
bool IsJoinEquality(const ParsedExpression &expression, const PGQVertexEdgeJoin &join, idx_t key) {
	if (expression.GetExpressionClass() != ExpressionClass::COMPARISON ||
	    expression.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
		return false;
	}
	auto &comparison = expression.Cast<ComparisonExpression>();
	auto &vertex_key = join.vertex_keys[key];
	auto &edge_key = join.edge_keys[key];
	return (IsColumn(comparison.Left(), join.vertex_alias, vertex_key) &&
	        IsColumn(comparison.Right(), join.edge_alias, edge_key)) ||
	       (IsColumn(comparison.Right(), join.vertex_alias, vertex_key) &&
	        IsColumn(comparison.Left(), join.edge_alias, edge_key));
}

void FlattenConjunction(unique_ptr<ParsedExpression> expression, vector<unique_ptr<ParsedExpression>> &result) {
	if (!expression) {
		return;
	}
	if (expression->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : expression->Cast<ConjunctionExpression>().GetChildrenMutable()) {
			FlattenConjunction(std::move(child), result);
		}
		return;
	}
	result.push_back(std::move(expression));
}

bool IsBaseTableLeaf(const TableRef &ref, const string &alias) {
	return ref.type == TableReferenceType::BASE_TABLE && StringUtil::CIEquals(PGQStr(ref.alias), alias);
}

//! Find a FROM leaf that is part of the translated join: the cross-join chain and, below an outer join (OPTIONAL
//! MATCH), its preserved left side.
optional_ptr<TableRef> FindJoinLeaf(TableRef &ref, const string &alias) {
	if (ref.type == TableReferenceType::JOIN) {
		auto &join = ref.Cast<JoinRef>();
		auto left = FindJoinLeaf(*join.left, alias);
		if (left || join.ref_type != JoinRefType::CROSS) {
			return left;
		}
		return FindJoinLeaf(*join.right, alias);
	}
	if (StringUtil::CIEquals(PGQStr(ref.alias), alias)) {
		return &ref;
	}
	return nullptr;
}

//! Whether the base table leaf `alias` can be dropped: its parent must be a cross join, so the sibling simply takes
//! its place. With apply set, it is dropped.
bool RemoveVertexLeaf(unique_ptr<TableRef> &ref, const string &alias, bool apply) {
	if (ref->type != TableReferenceType::JOIN) {
		return false;
	}
	auto &join = ref->Cast<JoinRef>();
	if (join.ref_type == JoinRefType::CROSS) {
		if (IsBaseTableLeaf(*join.left, alias)) {
			if (apply) {
				auto sibling = std::move(join.right);
				ref = std::move(sibling);
			}
			return true;
		}
		if (IsBaseTableLeaf(*join.right, alias)) {
			if (apply) {
				auto sibling = std::move(join.left);
				ref = std::move(sibling);
			}
			return true;
		}
		return RemoveVertexLeaf(join.left, alias, apply) || RemoveVertexLeaf(join.right, alias, apply);
	}
	return RemoveVertexLeaf(join.left, alias, apply);
}

struct VertexPlan {
	string alias;
	//! Indices into the recorded joins; the first one is the representative
	vector<idx_t> joins;
	//! Indices of the WHERE conjuncts that are this vertex's recorded joins
	vector<idx_t> claimed_conjuncts;
	bool disqualified = false;
	bool eliminated = false;
	//! Per vertex key (in the representative's key order)
	vector<Identifier> output_names;
	vector<LogicalType> key_types;
	//! [join index in `joins`][key position]: whether that edge column's type differs from the vertex key's
	vector<vector<bool>> needs_cast;
	vector<bool> representative_nullable;
};

class VertexJoinEliminator {
public:
	VertexJoinEliminator(ClientContext &context, SelectNode &node, const vector<PGQVertexEdgeJoin> &joins,
	                     const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map)
	    : context(context), node(node), joins(joins), alias_map(alias_map) {
	}

	idx_t Run() {
		for (idx_t i = 0; i < joins.size(); i++) {
			auto entry = plan_index.find(joins[i].vertex_alias);
			if (entry == plan_index.end()) {
				entry = plan_index.emplace(joins[i].vertex_alias, plans.size()).first;
				plans.emplace_back();
				plans.back().alias = joins[i].vertex_alias;
			}
			plans[entry->second].joins.push_back(i);
		}

		vector<unique_ptr<ParsedExpression>> conjuncts;
		FlattenConjunction(std::move(node.where_clause), conjuncts);
		vector<bool> claimed(conjuncts.size(), false);
		ClaimJoinConjuncts(conjuncts, claimed);

		CollectAllReferences(conjuncts, claimed);

		idx_t eliminated_count = 0;
		for (auto &plan : plans) {
			if (TryPlan(plan)) {
				plan.eliminated = true;
				eliminated_count++;
			}
		}

		if (eliminated_count > 0) {
			Apply(conjuncts, claimed);
		}

		// Rebuild the WHERE clause: kept conjuncts in their original order, then the edge-to-edge joins.
		unique_ptr<ParsedExpression> where_clause;
		auto append = [&](unique_ptr<ParsedExpression> conjunct) {
			if (!conjunct) {
				return;
			}
			where_clause = where_clause ? make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
			                                                               std::move(where_clause), std::move(conjunct))
			                            : std::move(conjunct);
		};
		for (auto &conjunct : conjuncts) {
			append(std::move(conjunct));
		}
		for (auto &conjunct : new_conjuncts) {
			append(std::move(conjunct));
		}
		node.where_clause = std::move(where_clause);
		return eliminated_count;
	}

private:
	//! Mark the WHERE conjuncts that are exactly the recorded joins. A vertex any of whose joins is missing (moved
	//! into an ON clause, rewritten, ...) is kept.
	void ClaimJoinConjuncts(const vector<unique_ptr<ParsedExpression>> &conjuncts, vector<bool> &claimed) {
		for (auto &plan : plans) {
			for (auto join_index : plan.joins) {
				auto &join = joins[join_index];
				for (idx_t key = 0; key < join.vertex_keys.size(); key++) {
					bool found = false;
					for (idx_t c = 0; c < conjuncts.size(); c++) {
						if (!claimed[c] && IsJoinEquality(*conjuncts[c], join, key)) {
							claimed[c] = true;
							plan.claimed_conjuncts.push_back(c);
							found = true;
							break;
						}
					}
					if (!found) {
						plan.disqualified = true;
					}
				}
			}
		}
	}

	optional_ptr<VertexPlan> FindPlan(const string &alias) {
		auto entry = plan_index.find(alias);
		return entry == plan_index.end() ? nullptr : &plans[entry->second];
	}

	void Disqualify(const string &alias) {
		auto plan = FindPlan(alias);
		if (plan) {
			plan->disqualified = true;
		}
	}

	void DisqualifyAll() {
		for (auto &plan : plans) {
			plan.disqualified = true;
		}
	}

	bool IsVertexKey(const VertexPlan &plan, const string &column) const {
		for (auto join_index : plan.joins) {
			if (KeyPosition(joins[join_index].vertex_keys, column) != DConstants::INVALID_INDEX) {
				return true;
			}
		}
		return false;
	}

	optional_ptr<TableCatalogEntry> LookupTable(const PropertyGraphTable &table) {
		auto cache_key = table.FullTableName();
		auto cached = table_cache.find(cache_key);
		if (cached != table_cache.end()) {
			return cached->second;
		}
		optional_ptr<TableCatalogEntry> result;
		try {
			result = GetPropertyGraphTableEntry(context, table);
		} catch (CatalogException &) {
			// Views and tables outside the catalog cannot be checked for constraints: keep the join.
			result = nullptr;
		} catch (BinderException &) {
			result = nullptr;
		}
		table_cache[cache_key] = result;
		return result;
	}

	//! Could an unqualified column of this name resolve to the vertex table bound to `alias`?
	bool VertexHasColumn(const string &alias, const string &column) {
		auto table_entry = alias_map.find(alias);
		if (table_entry == alias_map.end()) {
			return true;
		}
		auto table = LookupTable(*table_entry->second);
		return !table || table->ColumnExists(Identifier(column));
	}

	void DisqualifyUnqualified(const string &name) {
		for (auto &plan : plans) {
			if (StringUtil::CIEquals(plan.alias, name) || VertexHasColumn(plan.alias, name)) {
				plan.disqualified = true;
			}
		}
	}

	//! Classify references in the SELECT list and the WHERE conjuncts, where a reference to the vertex key can be
	//! redirected to the edge key.
	void Collect(const ParsedExpression &expression) {
		switch (expression.GetExpressionClass()) {
		case ExpressionClass::COLUMN_REF: {
			auto &names = expression.Cast<ColumnRefExpression>().ColumnNames();
			if (names.size() == 1) {
				DisqualifyUnqualified(PGQStr(names[0]));
				return;
			}
			auto plan = FindPlan(PGQStr(names[0]));
			if (plan && (names.size() != 2 || !IsVertexKey(*plan, PGQStr(names[1])))) {
				// A property, rowid, a struct field of a property, ...
				plan->disqualified = true;
			}
			return;
		}
		case ExpressionClass::STAR: {
			auto &star = expression.Cast<StarExpression>();
			if (star.RelationName().empty()) {
				DisqualifyAll();
			} else {
				Disqualify(PGQStr(star.RelationName()));
			}
			ParsedExpressionIterator::EnumerateChildren(expression,
			                                            [&](const ParsedExpression &child) { Collect(child); });
			return;
		}
		case ExpressionClass::SUBQUERY:
		case ExpressionClass::LAMBDA:
			// A correlated subquery could reference anything, and a subquery alias or lambda parameter could shadow a
			// vertex variable, so references inside them cannot be classified.
			DisqualifyAll();
			return;
		default:
			ParsedExpressionIterator::EnumerateChildren(expression,
			                                            [&](const ParsedExpression &child) { Collect(child); });
		}
	}

	//! Everywhere else a vertex reference cannot be redirected: any mention of the vertex keeps it.
	void ScanNames(ParsedExpression &expression) {
		switch (expression.GetExpressionClass()) {
		case ExpressionClass::COLUMN_REF: {
			auto &names = expression.Cast<ColumnRefExpression>().ColumnNames();
			if (names.size() == 1) {
				DisqualifyUnqualified(PGQStr(names[0]));
			} else {
				Disqualify(PGQStr(names[0]));
			}
			return;
		}
		case ExpressionClass::STAR: {
			// An unqualified star inside a nested query expands that query's own FROM clause.
			auto &star = expression.Cast<StarExpression>();
			if (!star.RelationName().empty()) {
				Disqualify(PGQStr(star.RelationName()));
			}
			break;
		}
		case ExpressionClass::SUBQUERY: {
			auto &subquery = expression.Cast<SubqueryExpression>();
			if (subquery.Subquery() && subquery.Subquery()->node) {
				ScanNode(*subquery.Subquery()->node);
			}
			break;
		}
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateChildren(expression, [&](ParsedExpression &child) { ScanNames(child); });
	}

	void ScanNode(QueryNode &query_node) {
		for (auto &cte : query_node.cte_map.map) {
			if (cte.second && cte.second->query_node) {
				ScanNode(*cte.second->query_node);
			}
		}
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    query_node, [&](unique_ptr<ParsedExpression> &child) { ScanNames(*child); });
		ParsedExpressionIterator::EnumerateQueryNodeModifiers(
		    query_node, [&](unique_ptr<ParsedExpression> &child) { ScanNames(*child); });
	}

	void ScanFrom(TableRef &ref) {
		switch (ref.type) {
		case TableReferenceType::BASE_TABLE:
		case TableReferenceType::EMPTY_FROM:
			return;
		case TableReferenceType::JOIN: {
			auto &join = ref.Cast<JoinRef>();
			ScanFrom(*join.left);
			ScanFrom(*join.right);
			if (join.condition) {
				// Redirecting a key inside an ON clause would need the edge in scope there; keep the vertex.
				ScanNames(*join.condition);
			}
			if (!join.using_columns.empty()) {
				DisqualifyAll();
			}
			return;
		}
		case TableReferenceType::SUBQUERY: {
			auto &subquery = ref.Cast<SubqueryRef>();
			if (subquery.subquery && subquery.subquery->node) {
				ScanNode(*subquery.subquery->node);
			}
			return;
		}
		case TableReferenceType::TABLE_FUNCTION:
		case TableReferenceType::EXPRESSION_LIST:
			ParsedExpressionIterator::EnumerateTableRefChildren(
			    ref, [&](unique_ptr<ParsedExpression> &child) { ScanNames(*child); });
			return;
		default:
			DisqualifyAll();
			return;
		}
	}

	void CollectAllReferences(const vector<unique_ptr<ParsedExpression>> &conjuncts, const vector<bool> &claimed) {
		for (auto &expression : node.select_list) {
			Collect(*expression);
		}
		for (idx_t c = 0; c < conjuncts.size(); c++) {
			if (!claimed[c]) {
				Collect(*conjuncts[c]);
			}
		}
		for (auto &group : node.groups.group_expressions) {
			ScanNames(*group);
		}
		if (node.having) {
			ScanNames(*node.having);
		}
		if (node.qualify) {
			ScanNames(*node.qualify);
		}
		ParsedExpressionIterator::EnumerateQueryNodeModifiers(
		    node, [&](unique_ptr<ParsedExpression> &child) { ScanNames(*child); });
		if (node.from_table) {
			ScanFrom(*node.from_table);
		}
	}

	//! The FROM leaf an edge alias names must be the relation the join was recorded for.
	bool EdgeIsInFrom(const PGQVertexEdgeJoin &join) {
		auto leaf = FindJoinLeaf(*node.from_table, join.edge_alias);
		if (!leaf) {
			return false;
		}
		if (join.undirected) {
			return leaf->type == TableReferenceType::SUBQUERY;
		}
		if (leaf->type != TableReferenceType::BASE_TABLE) {
			return false;
		}
		auto table_entry = alias_map.find(join.edge_alias);
		return table_entry != alias_map.end() && table_entry->second.get() == join.edge_table.get();
	}

	//! The edge table columns that feed `join.edge_keys[position]`
	vector<string> BackingColumns(const PGQVertexEdgeJoin &join, idx_t position) const {
		if (!join.undirected) {
			return {join.edge_keys[position]};
		}
		// The UNION ALL exposes the source key under the source column name and the destination key under the
		// destination column name, each fed by both underlying columns.
		auto &edge = *join.edge_table;
		if (edge.source_fk.size() != 1 || edge.destination_fk.size() != 1) {
			return {};
		}
		return {PGQStr(edge.source_fk[0]), PGQStr(edge.destination_fk[0])};
	}

	static bool IsNotNull(const TableCatalogEntry &table, const string &column) {
		for (auto &constraint : table.GetConstraints()) {
			if (constraint->type != ConstraintType::NOT_NULL) {
				continue;
			}
			auto &not_null = constraint->Cast<NotNullConstraint>();
			if (StringUtil::CIEquals(PGQStr(table.GetColumn(not_null.index).Name()), column)) {
				return true;
			}
		}
		return false;
	}

	static bool HasUniqueConstraint(const TableCatalogEntry &table, const vector<string> &keys) {
		for (auto &constraint : table.GetConstraints()) {
			if (constraint->type != ConstraintType::UNIQUE) {
				continue;
			}
			auto &unique = constraint->Cast<UniqueConstraint>();
			vector<string> columns;
			if (unique.HasIndex()) {
				columns.push_back(PGQStr(table.GetColumn(unique.GetIndex()).Name()));
			} else {
				columns = PGQStrs(unique.GetColumnNames());
			}
			if (SameKeySet(columns, keys)) {
				return true;
			}
		}
		return false;
	}

	//! Whether DuckDB enforces edge_keys -> vertex_entry(vertex_keys) through a FOREIGN KEY on the edge table. DuckDB
	//! checks it on every insert and update of the edge table and every delete and key update of the vertex table.
	bool HasForeignKey(const TableCatalogEntry &edge_entry, const vector<Identifier> &edge_keys,
	                   const TableCatalogEntry &vertex_entry, const vector<Identifier> &vertex_keys) {
		if (edge_keys.empty() || edge_keys.size() != vertex_keys.size()) {
			return false;
		}
		for (auto &constraint : edge_entry.GetConstraints()) {
			if (constraint->type != ConstraintType::FOREIGN_KEY) {
				continue;
			}
			auto &foreign_key = constraint->Cast<ForeignKeyConstraint>();
			if (!foreign_key.info.IsAppendConstraint() || foreign_key.fk_columns.size() != edge_keys.size() ||
			    foreign_key.pk_columns.size() != edge_keys.size()) {
				continue;
			}
			// The referenced table must be this vertex table, not one of the same name elsewhere.
			auto schema = foreign_key.info.schema.empty() ? edge_entry.ParentSchema().name : foreign_key.info.schema;
			optional_ptr<TableCatalogEntry> referenced;
			try {
				referenced = Catalog::GetEntry<TableCatalogEntry>(
				    context, QualifiedName(edge_entry.ParentCatalog().GetName(), schema,
				                           foreign_key.info.table),
				    OnEntryNotFound::RETURN_NULL);
			} catch (std::exception &) {
				referenced = nullptr;
			}
			if (!referenced || referenced.get() != &vertex_entry) {
				continue;
			}
			bool pairs_match = true;
			for (idx_t i = 0; i < edge_keys.size() && pairs_match; i++) {
				pairs_match = false;
				for (idx_t j = 0; j < foreign_key.fk_columns.size(); j++) {
					if (StringUtil::CIEquals(PGQStr(foreign_key.fk_columns[j]), PGQStr(edge_keys[i])) &&
					    StringUtil::CIEquals(PGQStr(foreign_key.pk_columns[j]), PGQStr(vertex_keys[i]))) {
						pairs_match = true;
						break;
					}
				}
			}
			if (pairs_match) {
				return true;
			}
		}
		return false;
	}

	//! Whether every row of the join's edge table references a row of the vertex table on the join's keys, now and
	//! after any later modification: only a FOREIGN KEY guarantees that. A check made at CREATE PROPERTY GRAPH
	//! (pgq_verify_edges_on_create) does not: rows written later are not checked, and DuckDB tables record nowhere
	//! whether they changed since. (A DuckLake table's snapshots do, but DuckLake tables have no UNIQUE key, which
	//! elimination needs as well.)
	bool EdgesReferenceVertices(const PGQVertexEdgeJoin &join, const PropertyGraphTable &vertex_pg_table) {
		auto edge_entry = LookupTable(*join.edge_table);
		auto vertex_entry = LookupTable(vertex_pg_table);
		if (!edge_entry || !vertex_entry) {
			return false;
		}
		if (!join.undirected) {
			return HasForeignKey(*edge_entry, PGQIds(join.edge_keys), *vertex_entry, PGQIds(join.vertex_keys));
		}
		// Both endpoint columns feed the undirected edge's key.
		auto &edge = *join.edge_table;
		return HasForeignKey(*edge_entry, edge.source_fk, *vertex_entry, edge.source_pk) &&
		       HasForeignKey(*edge_entry, edge.destination_fk, *vertex_entry, edge.destination_pk);
	}

	bool TryPlan(VertexPlan &plan) {
		if (plan.disqualified || plan.joins.empty() || !node.from_table) {
			return false;
		}
		auto table_entry = alias_map.find(plan.alias);
		if (table_entry == alias_map.end() || !table_entry->second->is_vertex_table) {
			return false;
		}
		auto &vertex_pg_table = *table_entry->second;
		if (!RemoveVertexLeaf(node.from_table, plan.alias, false)) {
			return false;
		}
		auto vertex_table = LookupTable(vertex_pg_table);
		if (!vertex_table) {
			return false;
		}
		auto &representative = joins[plan.joins[0]];
		auto &keys = representative.vertex_keys;
		// Every incident edge must reference this vertex table (undirected and bidirectional edges are not checked
		// against the labels when the pattern is translated; the join did that), join on the same vertex key, and
		// still be the relation it was recorded for.
		for (auto join_index : plan.joins) {
			auto &join = joins[join_index];
			if (!join.referenced_vertex_table || !join.referenced_vertex_table->SameTableIdentity(vertex_pg_table)) {
				return false;
			}
			if (!SameKeySet(join.vertex_keys, keys) || !EdgeIsInFrom(join)) {
				return false;
			}
			if (join.undirected && keys.size() != 1) {
				return false;
			}
		}
		for (auto &key : keys) {
			if (!vertex_table->ColumnExists(Identifier(key))) {
				return false;
			}
		}
		if (!HasUniqueConstraint(*vertex_table, keys)) {
			return false;
		}
		for (auto join_index : plan.joins) {
			if (!EdgesReferenceVertices(joins[join_index], vertex_pg_table)) {
				return false;
			}
		}

		plan.needs_cast.assign(plan.joins.size(), vector<bool>(keys.size(), false));
		plan.representative_nullable.assign(keys.size(), false);
		for (idx_t k = 0; k < keys.size(); k++) {
			auto &vertex_column = vertex_table->GetColumn(Identifier(keys[k]));
			plan.output_names.push_back(vertex_column.Name());
			plan.key_types.push_back(vertex_column.Type());
		}
		for (idx_t j = 0; j < plan.joins.size(); j++) {
			auto &join = joins[plan.joins[j]];
			auto edge_table = LookupTable(*join.edge_table);
			if (!edge_table) {
				return false;
			}
			for (idx_t k = 0; k < keys.size(); k++) {
				auto position = KeyPosition(join.vertex_keys, keys[k]);
				auto backing = BackingColumns(join, position);
				if (backing.empty()) {
					return false;
				}
				for (auto &column_name : backing) {
					if (!edge_table->ColumnExists(Identifier(column_name))) {
						return false;
					}
					auto &column = edge_table->GetColumn(Identifier(column_name));
					if (column.Type() != plan.key_types[k]) {
						plan.needs_cast[j][k] = true;
					}
					if (j == 0 && !IsNotNull(*edge_table, column_name)) {
						plan.representative_nullable[k] = true;
					}
				}
			}
		}
		return true;
	}

	//! The expression that replaces vertex_alias.keys[k]: the representative edge column, cast to the key type when
	//! it differs.
	unique_ptr<ParsedExpression> EdgeKey(const VertexPlan &plan, idx_t join_in_plan, idx_t k) const {
		auto &join = joins[plan.joins[join_in_plan]];
		auto &keys = joins[plan.joins[0]].vertex_keys;
		auto position = KeyPosition(join.vertex_keys, keys[k]);
		unique_ptr<ParsedExpression> result = PGQColRef(join.edge_keys[position], join.edge_alias);
		if (plan.needs_cast[join_in_plan][k]) {
			result = make_uniq<CastExpression>(plan.key_types[k], std::move(result));
		}
		return result;
	}

	optional_ptr<VertexPlan> EliminatedPlan(const string &alias) {
		auto plan = FindPlan(alias);
		return plan && plan->eliminated ? plan : nullptr;
	}

	//! Replace references to eliminated vertex keys. Returns whether anything was replaced.
	bool Rewrite(unique_ptr<ParsedExpression> &expression) {
		if (expression->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			bool changed = false;
			ParsedExpressionIterator::EnumerateChildren(
			    *expression, [&](unique_ptr<ParsedExpression> &child) { changed |= Rewrite(child); });
			return changed;
		}
		auto &names = expression->Cast<ColumnRefExpression>().ColumnNames();
		if (names.size() != 2) {
			return false;
		}
		auto plan = EliminatedPlan(PGQStr(names[0]));
		if (!plan) {
			return false;
		}
		auto k = KeyPosition(joins[plan->joins[0]].vertex_keys, PGQStr(names[1]));
		if (k == DConstants::INVALID_INDEX) {
			throw InternalException("Vertex-join elimination: %s.%s is not a key of the eliminated vertex",
			                        PGQStr(names[0]), PGQStr(names[1]));
		}
		auto replacement = EdgeKey(*plan, 0, k);
		if (expression->HasAlias()) {
			replacement->SetAlias(expression->GetAlias());
		}
		expression = std::move(replacement);
		return true;
	}

	//! Rewrite a SELECT-list entry and keep the result column name it had.
	void RewriteSelectEntry(unique_ptr<ParsedExpression> &expression) {
		if (expression->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &names = expression->Cast<ColumnRefExpression>().ColumnNames();
			auto plan = names.size() == 2 ? EliminatedPlan(PGQStr(names[0])) : nullptr;
			if (!plan) {
				return;
			}
			auto k = KeyPosition(joins[plan->joins[0]].vertex_keys, PGQStr(names[1]));
			// DuckDB names a column reference after the catalog column when it has no alias or an alias that only
			// differs in case, so COLUMNS (v.ID) and COLUMNS (v.id AS ID) both yield the catalog spelling.
			auto &catalog_name = plan->output_names[k];
			Identifier name = catalog_name;
			if (expression->HasAlias() && !StringUtil::CIEquals(PGQStr(expression->GetAlias()), PGQStr(catalog_name))) {
				name = expression->GetAlias();
			}
			Rewrite(expression);
			expression->SetAlias(name);
			return;
		}
		auto original_name = expression->GetName();
		if (Rewrite(expression) && !expression->HasAlias()) {
			expression->SetAlias(original_name);
		}
	}

	void Apply(vector<unique_ptr<ParsedExpression>> &conjuncts, const vector<bool> &claimed) {
		for (auto &plan : plans) {
			if (!plan.eliminated) {
				continue;
			}
			RemoveVertexLeaf(node.from_table, plan.alias, true);
			for (auto c : plan.claimed_conjuncts) {
				conjuncts[c].reset();
			}
			auto &representative = joins[plan.joins[0]];
			auto &keys = representative.vertex_keys;
			for (idx_t k = 0; k < keys.size(); k++) {
				// A NULL edge key never joined with the vertex; keep dropping it.
				if (plan.representative_nullable[k]) {
					new_conjuncts.push_back(make_uniq<OperatorExpression>(
					    ExpressionType::OPERATOR_IS_NOT_NULL,
					    PGQColRef(representative.edge_keys[k], representative.edge_alias)));
				}
				// The other edges meet the representative at the vertex key.
				for (idx_t j = 1; j < plan.joins.size(); j++) {
					new_conjuncts.push_back(make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL, EdgeKey(plan, j, k), EdgeKey(plan, 0, k)));
				}
			}
		}
		for (auto &expression : node.select_list) {
			RewriteSelectEntry(expression);
		}
		for (idx_t c = 0; c < conjuncts.size(); c++) {
			if (conjuncts[c] && !claimed[c]) {
				Rewrite(conjuncts[c]);
			}
		}
	}

	ClientContext &context;
	SelectNode &node;
	const vector<PGQVertexEdgeJoin> &joins;
	const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map;
	vector<VertexPlan> plans;
	case_insensitive_map_t<idx_t> plan_index;
	vector<unique_ptr<ParsedExpression>> new_conjuncts;
	case_insensitive_map_t<optional_ptr<TableCatalogEntry>> table_cache;
};

} // namespace

idx_t PGQEliminateVertexJoins(ClientContext &context, SelectNode &node, const vector<PGQVertexEdgeJoin> &joins,
                              const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map) {
	if (joins.empty() || !node.from_table) {
		return 0;
	}
	Value value;
	if (context.TryGetCurrentSetting(PGQ_VERTEX_JOIN_ELIMINATION_SETTING, value) && !value.IsNull() &&
	    !value.GetValue<bool>()) {
		return 0;
	}
	return VertexJoinEliminator(context, node, joins, alias_map).Run();
}

} // namespace duckdb
