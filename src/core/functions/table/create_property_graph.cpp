#include "duckpgq/core/functions/table/abi_build.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckpgq/core/functions/table/create_property_graph.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckpgq/common.hpp"
#include <duckpgq/core/functions/table.hpp>
#include <duckpgq/core/utils/duckpgq_utils.hpp>
#include "duckdb/main/connection_manager.hpp"
#include <duckpgq/core/parser/duckpgq_parser.hpp>
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckpgq/core/utils/duckpgq_sql.hpp"
#include "duckpgq/core/utils/duckpgq_identifier.hpp"

namespace duckdb {

static Identifier PGQIdentifier(const string &value) {
	return Identifier(value);
}

static Identifier PGQIdentifier(const Identifier &value) {
	return value;
}

static void BindPropertyGraphTable(ClientContext &context, const shared_ptr<PropertyGraphTable> &table) {
	auto catalog = table->catalog_name;
	auto schema = table->schema_name;
	Binder::BindSchemaOrCatalog(context, catalog, schema);
	table->catalog_name = catalog;
	table->schema_name = schema;
}

static void BindPropertyGraphReference(ClientContext &context, Identifier &catalog_name, Identifier &schema_name) {
	auto catalog = catalog_name;
	auto schema = schema_name;
	Binder::BindSchemaOrCatalog(context, catalog, schema);
	catalog_name = catalog;
	schema_name = schema;
}

static optional_ptr<TableCatalogEntry> GetPropertyGraphTable(ClientContext &context,
                                                             const shared_ptr<PropertyGraphTable> &table) {
	return GetPropertyGraphTableEntry(context, *table);
}

//! The columns that number a view's rows (see PGQElementRelations): the key first, then every other column, so that
//! only identical rows tie even where the key is not unique.
static vector<Identifier> IdentityColumns(const vector<Identifier> &key, const vector<Identifier> &columns) {
	vector<Identifier> result = key;
	for (auto &column : columns) {
		bool in_key = false;
		for (auto &key_column : key) {
			in_key = in_key || StringUtil::CIEquals(key_column.GetIdentifierName(), column.GetIdentifierName());
		}
		if (!in_key) {
			result.push_back(column);
		}
	}
	return result;
}

//! The columns of a UNIQUE or PRIMARY KEY constraint.
static vector<Identifier> UniqueColumns(const TableCatalogEntry &table, const UniqueConstraint &unique) {
	if (unique.HasIndex()) {
		return {table.GetColumn(unique.GetIndex()).Name()};
	}
	return unique.GetColumnNames();
}

static bool SameColumnSet(const vector<Identifier> &left, const vector<Identifier> &right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (auto &column : left) {
		bool found = false;
		for (auto &other : right) {
			found = found || StringUtil::CIEquals(column.GetIdentifierName(), other.GetIdentifierName());
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

//! Whether the table's constraints guarantee that these columns identify its rows (a PRIMARY KEY or UNIQUE
//! constraint on exactly them), so a KEY over them needs no check.
static bool KeyIsEnforced(const CreatePropertyGraphFunction::ElementColumns &table, const vector<Identifier> &key) {
	if (!table.table) {
		return false;
	}
	for (auto &constraint : table.table->GetConstraints()) {
		if (constraint->type == ConstraintType::UNIQUE &&
		    SameColumnSet(UniqueColumns(*table.table, constraint->Cast<UniqueConstraint>()), key)) {
			return true;
		}
	}
	return false;
}

//! The key a vertex table is referenced by when an edge names no columns (REFERENCES v): its KEY, else its PRIMARY
//! KEY; empty when it has neither.
static vector<Identifier> ReferencedKey(const PropertyGraphTable &vertex,
                                        const CreatePropertyGraphFunction::ElementColumns &columns) {
	if (!vertex.element_key.empty()) {
		return vertex.element_key;
	}
	if (columns.table) {
		for (auto &constraint : columns.table->GetConstraints()) {
			if (constraint->type == ConstraintType::UNIQUE && constraint->Cast<UniqueConstraint>().IsPrimaryKey()) {
				return UniqueColumns(*columns.table, constraint->Cast<UniqueConstraint>());
			}
		}
	}
	return {};
}

//! REFERENCES v without a column list references v's key (its KEY, else its PRIMARY KEY).
static void ResolveReferencedKey(const PropertyGraphTable &edge, const string &side, const PropertyGraphTable &vertex,
                                 const vector<Identifier> &edge_columns, vector<Identifier> &vertex_columns,
                                 case_insensitive_map_t<CreatePropertyGraphFunction::ElementColumns> &columns) {
	if (!vertex_columns.empty() || edge_columns.empty()) {
		return;
	}
	vertex_columns = ReferencedKey(vertex, columns[vertex.FullTableName()]);
	if (vertex_columns.empty()) {
		throw Exception(ExceptionType::INVALID, side + " KEY of edge table " + edge.table_name + " references " +
		                                            vertex.table_name +
		                                            " without a column list, but that table has neither a KEY nor a "
		                                            "PRIMARY KEY: name the referenced columns");
	}
	if (vertex_columns.size() != edge_columns.size()) {
		throw Exception(ExceptionType::INVALID, side + " KEY of edge table " + edge.table_name + " has " +
		                                            to_string(edge_columns.size()) + " columns, but the key of " +
		                                            vertex.table_name + " has " + to_string(vertex_columns.size()));
	}
}

static void CheckElementKey(const PropertyGraphTable &table, const CreatePropertyGraphFunction::ElementColumns &columns) {
	for (idx_t i = 0; i < table.element_key.size(); i++) {
		auto &column = table.element_key[i];
		if (!columns.Exists(column)) {
			throw Exception(ExceptionType::INVALID,
			                "KEY column " + column + " not found in table " + table.table_name);
		}
		for (idx_t j = 0; j < i; j++) {
			if (StringUtil::CIEquals(table.element_key[j].GetIdentifierName(), column.GetIdentifierName())) {
				throw Exception(ExceptionType::INVALID,
				                "KEY column " + column + " is given twice for table " + table.table_name);
			}
		}
	}
}

static void ThrowMissingVertexReference(CreatePropertyGraphInfo &info, const Identifier &catalog_name,
                                        const Identifier &schema_name, const Identifier &table_name) {
	info.GetTableByName(catalog_name.GetIdentifierName(), schema_name.GetIdentifierName(),
	                    table_name.GetIdentifierName());
}

static vector<Identifier> ExpandAllColumnsExcept(vector<string> column_names,
                                                 const vector<Identifier> &except_columns) {
	auto excluded_columns = IdentifiersToStrings(except_columns);
	vector<string> selected_columns;

	// Preserve the historical output order for expanded property columns.
	std::sort(column_names.begin(), column_names.end());
	std::sort(excluded_columns.begin(), excluded_columns.end());
	std::set_difference(column_names.begin(), column_names.end(), excluded_columns.begin(), excluded_columns.end(),
	                    std::back_inserter(selected_columns));
	return StringsToIdentifiers(selected_columns);
}

bool CreatePropertyGraphFunction::ElementColumns::Exists(const Identifier &name) const {
	return Type(name).id() != LogicalTypeId::INVALID;
}

LogicalType CreatePropertyGraphFunction::ElementColumns::Type(const Identifier &name) const {
	for (idx_t i = 0; i < names.size(); i++) {
		if (StringUtil::CIEquals(names[i].GetIdentifierName(), name.GetIdentifierName())) {
			return types[i];
		}
	}
	return LogicalType::INVALID;
}

CreatePropertyGraphFunction::ElementColumns
CreatePropertyGraphFunction::ResolveElementColumns(ClientContext &context, const PropertyGraphTable &pg_table) {
	ElementColumns result;
	auto entry = GetPropertyGraphCatalogEntry(context, pg_table);
	if (entry && entry->type == CatalogType::TABLE_ENTRY) {
		auto &table = entry->Cast<TableCatalogEntry>();
		result.table = &table;
		for (auto &column : table.GetColumns().Logical()) {
			result.names.push_back(column.Name());
			result.types.push_back(column.Type());
		}
		return result;
	}
	if (entry && entry->type == CatalogType::VIEW_ENTRY) {
		auto &view = entry->Cast<ViewCatalogEntry>();
		view.BindView(context);
		auto columns = view.GetColumnInfo();
		if (!columns) {
			throw Exception(ExceptionType::INVALID, "Could not determine the columns of view " + pg_table.table_name);
		}
		result.is_view = true;
		result.names = columns->names;
		result.types = columns->types;
		for (auto &name : result.names) {
			// The view's element id is exposed as rowid (see PGQElementRelations).
			if (StringUtil::CIEquals(name.GetIdentifierName(), "rowid")) {
				throw Exception(ExceptionType::INVALID, "View " + pg_table.table_name +
				                                            " has a column named rowid, which a property graph "
				                                            "element table over a view cannot have");
			}
		}
		return result;
	}
	throw Exception(ExceptionType::INVALID, "Table with name " + pg_table.table_name + " does not exist");
}

void CreatePropertyGraphFunction::CheckPropertyGraphTableLabels(const shared_ptr<PropertyGraphTable> &pg_table,
                                                                const ElementColumns &table) {
	if (!pg_table->discriminator.empty()) {
		if (!table.Exists(PGQIdentifier(pg_table->discriminator))) {
			throw Exception(ExceptionType::INVALID,
			                "Column " + pg_table->discriminator + " not found in table " + pg_table->table_name);
		}
		auto type = table.Type(PGQIdentifier(pg_table->discriminator));
		if (!(type == LogicalType::BIGINT || type == LogicalType::INTEGER)) {
			throw Exception(ExceptionType::INVALID, "The discriminator column " + pg_table->discriminator +
			                                            " of table " + pg_table->table_name +
			                                            " should be of type BIGINT or INTEGER");
		}
	}
}

void CreatePropertyGraphFunction::CheckPropertyGraphTableColumns(const shared_ptr<PropertyGraphTable> &pg_table,
                                                                 const ElementColumns &table) {
	if (pg_table->no_columns) {
		return;
	}

	if (pg_table->all_columns) {
		for (auto &except_column : pg_table->except_columns) {
			if (!table.Exists(PGQIdentifier(except_column))) {
				throw Exception(ExceptionType::INVALID,
				                "EXCEPT column " + except_column + " not found in table " + pg_table->table_name);
			}
		}

		// The properties another label of the table lists come after the expanded columns.
		auto listed_names = std::move(pg_table->column_names);
		auto listed_aliases = std::move(pg_table->column_aliases);
		auto listed_expressions = std::move(pg_table->property_expressions);
		listed_expressions.resize(listed_names.size());
		pg_table->column_names = ExpandAllColumnsExcept(IdentifiersToStrings(table.names), pg_table->except_columns);
		pg_table->column_aliases = pg_table->column_names;
		pg_table->property_expressions.assign(pg_table->column_names.size(), string());
		for (idx_t i = 0; i < listed_names.size(); i++) {
			if (listed_expressions[i].empty() && !table.Exists(PGQIdentifier(listed_names[i]))) {
				throw Exception(ExceptionType::INVALID,
				                "Column " + listed_names[i] + " not found in table " + pg_table->table_name);
			}
			bool known = false;
			for (idx_t j = 0; j < pg_table->column_aliases.size(); j++) {
				if (!StringUtil::CIEquals(pg_table->column_aliases[j].GetIdentifierName(),
				                          listed_aliases[i].GetIdentifierName())) {
					continue;
				}
				if (!StringUtil::CIEquals(pg_table->column_names[j].GetIdentifierName(),
				                          listed_names[i].GetIdentifierName()) ||
				    !listed_expressions[i].empty()) {
					throw Exception(ExceptionType::INVALID, "Property " + listed_aliases[i] + " of element table " +
					                                            pg_table->table_name + " is defined twice, differently");
				}
				known = true;
			}
			if (!known) {
				pg_table->column_names.push_back(listed_names[i]);
				pg_table->column_aliases.push_back(listed_aliases[i]);
				pg_table->property_expressions.push_back(listed_expressions[i]);
			}
		}
		return;
	}

	for (idx_t i = 0; i < pg_table->column_names.size(); i++) {
		auto &column = pg_table->column_names[i];
		if (pg_table->PropertyExpression(i).empty() && !table.Exists(PGQIdentifier(column))) {
			throw Exception(ExceptionType::INVALID, "Column " + column + " not found in table " + pg_table->table_name);
		}
	}
}

//! A property that is not a plain column of its own name -- a renamed column or an expression -- becomes a column of
//! the table's element relation (see PGQElementRelations), next to the table's own columns: its name must not be one
//! of them. A computed property must bind against the table.
static void CheckPropertyRelation(ClientContext &context, const PropertyGraphTable &pg_table,
                                  const CreatePropertyGraphFunction::ElementColumns &table) {
	vector<string> expressions;
	for (idx_t i = 0; i < pg_table.column_names.size(); i++) {
		auto &name = pg_table.PropertyName(i);
		auto expression = pg_table.PropertyExpression(i);
		const bool renamed = !StringUtil::CIEquals(name.GetIdentifierName(), pg_table.column_names[i].GetIdentifierName());
		if (expression.empty() && !renamed) {
			continue;
		}
		if (table.Exists(name) || StringUtil::CIEquals(name.GetIdentifierName(), "rowid")) {
			throw Exception(ExceptionType::INVALID, "Property " + name + " of element table " + pg_table.table_name +
			                                            " is also the name of a column of the table: name it "
			                                            "differently");
		}
		if (!expression.empty()) {
			expressions.push_back("(" + expression + ") AS " + DuckPGQSQL::Identifier(name));
		}
	}
	if (expressions.empty()) {
		return;
	}
	// Bound as the element relation reads it, next to the table's columns: an aggregate is refused there.
	auto statement = DuckPGQSQL::ParseSelect("SELECT *, " + StringUtil::Join(expressions, ", ") + " FROM " +
	                                             DuckPGQSQL::TableRef(pg_table) + " LIMIT 0",
	                                         "computed properties");
	try {
		auto binder = Binder::CreateBinder(context);
		binder->Bind(static_cast<SQLStatement &>(*statement));
	} catch (std::exception &e) {
		ErrorData error(e);
		throw Exception(ExceptionType::INVALID,
		                "A computed property of element table " + pg_table.table_name + " is invalid: " + error.RawMessage());
	}
}

// Helper function to validate source/destination keys
void CreatePropertyGraphFunction::ValidateKeys(shared_ptr<PropertyGraphTable> &edge_table, const Identifier &reference,
                                               const string &key_type, vector<Identifier> &pk_columns,
                                               vector<Identifier> &fk_columns,
                                               const vector<unique_ptr<Constraint>> &table_constraints) {
	// todo(dtenwolde) add test case for attached databases or different schema that has pk-fk relationships
	if (fk_columns.empty() && pk_columns.empty()) {
		if (table_constraints.empty()) {
			throw Exception(ExceptionType::INVALID, "No primary key - foreign key relationship found in " +
			                                            edge_table->table_name + " with " +
			                                            StringUtil::Upper(key_type) + " table " + reference);
		}

		for (const auto &constraint : table_constraints) {
			if (constraint->type == ConstraintType::FOREIGN_KEY) {
				auto fk_constraint = constraint->Cast<ForeignKeyConstraint>();
				if (fk_constraint.info.table != reference) {
					continue;
				}
				// If a PK-FK relationship was found earlier, throw an ambiguity
				// exception
				if (!pk_columns.empty() && !fk_columns.empty()) {
					throw Exception(ExceptionType::INVALID, "Multiple primary key - foreign key relationships "
					                                        "detected between " +
					                                            edge_table->table_name + " and " + reference +
					                                            ". "
					                                            "Please explicitly define the primary key and "
					                                            "foreign key columns using `" +
					                                            StringUtil::Upper(key_type) +
					                                            " KEY <primary key> REFERENCES " + reference +
					                                            " <foreign key>`");
				}
				pk_columns = fk_constraint.pk_columns;
				fk_columns = fk_constraint.fk_columns;
			}
		}

		if (pk_columns.empty()) {
			throw Exception(ExceptionType::INVALID, "The primary key for the " + StringUtil::Upper(key_type) +
			                                            " table " + reference + " is not defined in the edge table " +
			                                            edge_table->table_name);
		}

		if (fk_columns.empty()) {
			throw Exception(ExceptionType::INVALID, "The foreign key for the " + StringUtil::Upper(key_type) +
			                                            " table " + reference + " is not defined in the edge table " +
			                                            edge_table->table_name);
		}
	}
}

void CreatePropertyGraphFunction::ValidateForeignKeyColumns(shared_ptr<PropertyGraphTable> &edge_table,
                                                            const vector<Identifier> &fk_columns,
                                                            const ElementColumns &table) {
	for (const auto &fk : fk_columns) {
		if (!table.Exists(PGQIdentifier(fk))) {
			throw Exception(ExceptionType::INVALID,
			                "Foreign key " + fk + " does not exist in table " + edge_table->table_name);
		}
	}
}

// Helper function to check if the vertex table is registered
void CreatePropertyGraphFunction::ValidateVertexTableRegistration(shared_ptr<PropertyGraphTable> &pg_table,
                                                                  const case_insensitive_set_t &v_table_names) {
	if (v_table_names.find(pg_table->FullTableName()) == v_table_names.end()) {
		throw Exception(ExceptionType::INVALID, "Referenced vertex table " + pg_table->FullTableName() +
		                                            " is not registered in the vertex tables.");
	}
}

// Helper function to validate primary keys in the source or destination tables
void CreatePropertyGraphFunction::ValidatePrimaryKeyInTable(ClientContext &context,
                                                            shared_ptr<PropertyGraphTable> &pg_table,
                                                            const vector<Identifier> &pk_columns) {
	auto table = ResolveElementColumns(context, *pg_table);
	for (const auto &pk : pk_columns) {
		if (!table.Exists(PGQIdentifier(pk))) {
			throw Exception(ExceptionType::INVALID,
			                "Primary key " + pk + " does not exist in table " + pg_table->table_name);
		}
	}
}

//! Checks a property graph's definition against the catalog and completes it: binds the tables' catalogs and
//! schemas, expands ALL COLUMNS, resolves keys (FOREIGN KEY, REFERENCES v without a list), and records views.
void CreatePropertyGraphFunction::ValidatePropertyGraph(ClientContext &context, CreatePropertyGraphInfo &info) {
	case_insensitive_set_t v_table_names;
	case_insensitive_map_t<ElementColumns> vertex_columns;
	for (auto &vertex_table : info.vertex_tables) {
		ElementColumns table;
		try {
			BindPropertyGraphTable(context, vertex_table);
			table = ResolveElementColumns(context, *vertex_table);
			CheckElementKey(*vertex_table, table);
			CheckPropertyGraphTableColumns(vertex_table, table);
			CheckPropertyRelation(context, *vertex_table, table);
			CheckPropertyGraphTableLabels(vertex_table, table);
		} catch (CatalogException &e) {
			throw Exception(ExceptionType::INVALID, e.what());
		} catch (BinderException &e) {
			throw Exception(ExceptionType::INVALID, "Catalog '" + vertex_table->catalog_name + "' does not exist!");
		}
		vertex_table->is_view = table.is_view;
		vertex_columns[vertex_table->FullTableName()] = table;
		v_table_names.insert(vertex_table->FullTableName());
		if (vertex_table->hasTableNameAlias()) {
			v_table_names.insert(vertex_table->table_name_alias.GetIdentifierName());
		}
	}

	for (auto &edge_table : info.edge_tables) {
		try {
			BindPropertyGraphTable(context, edge_table);
			auto table = ResolveElementColumns(context, *edge_table);
			CheckElementKey(*edge_table, table);
			edge_table->is_view = table.is_view;
			CheckPropertyGraphTableColumns(edge_table, table);
			CheckPropertyRelation(context, *edge_table, table);
			CheckPropertyGraphTableLabels(edge_table, table);
			BindPropertyGraphReference(context, edge_table->source_catalog, edge_table->source_schema);
			BindPropertyGraphReference(context, edge_table->destination_catalog, edge_table->destination_schema);

			// A view has no constraints to infer the keys from.
			const vector<unique_ptr<Constraint>> no_constraints;
			auto &table_constraints = table.table ? table.table->GetConstraints() : no_constraints;
			if (table.is_view && ((edge_table->source_fk.empty() && edge_table->source_pk.empty()) ||
			                      (edge_table->destination_fk.empty() && edge_table->destination_pk.empty()))) {
				throw Exception(ExceptionType::INVALID,
				                "Edge table " + edge_table->table_name +
				                    " is a view, which has no foreign keys to take the endpoint keys from: declare "
				                    "them with SOURCE KEY (...) REFERENCES ... (...) and DESTINATION KEY (...) "
				                    "REFERENCES ... (...)");
			}

			ValidateKeys(edge_table, edge_table->source_reference, "source", edge_table->source_pk,
			             edge_table->source_fk, table_constraints);

			// Check source foreign key columns exist in the table
			ValidateForeignKeyColumns(edge_table, edge_table->source_fk, table);

			// Validate destination keys
			ValidateKeys(edge_table, edge_table->destination_reference, "destination", edge_table->destination_pk,
			             edge_table->destination_fk, table_constraints);

			// Check destination foreign key columns exist in the table
			ValidateForeignKeyColumns(edge_table, edge_table->destination_fk, table);

			// Validate source table registration
			if (!edge_table->source_pg_table) {
				ThrowMissingVertexReference(info, edge_table->source_catalog, edge_table->source_schema,
				                            edge_table->source_reference);
			}
			ValidateVertexTableRegistration(edge_table->source_pg_table, v_table_names);
			ResolveReferencedKey(*edge_table, "SOURCE", *edge_table->source_pg_table, edge_table->source_fk,
			                     edge_table->source_pk, vertex_columns);

			// Validate primary keys in the source table
			ValidatePrimaryKeyInTable(context, edge_table->source_pg_table, edge_table->source_pk);

			// Validate destination table registration
			if (!edge_table->destination_pg_table) {
				ThrowMissingVertexReference(info, edge_table->destination_catalog, edge_table->destination_schema,
				                            edge_table->destination_reference);
			}
			ValidateVertexTableRegistration(edge_table->destination_pg_table, v_table_names);
			ResolveReferencedKey(*edge_table, "DESTINATION", *edge_table->destination_pg_table,
			                     edge_table->destination_fk, edge_table->destination_pk, vertex_columns);

			// Validate primary keys in the destination table
			ValidatePrimaryKeyInTable(context, edge_table->destination_pg_table, edge_table->destination_pk);
			if (table.is_view) {
				edge_table->identity_columns = IdentityColumns(edge_table->element_key, table.names);
			}
		} catch (CatalogException &e) {
			throw Exception(ExceptionType::INVALID, e.what());
		} catch (BinderException &e) {
			throw Exception(ExceptionType::INVALID, "Catalog '" + edge_table->catalog_name + "' does not exist!");
		}
	}
	for (auto &vertex_table : info.vertex_tables) {
		if (!vertex_table->is_view) {
			continue;
		}
		// The vertex key of a view: its KEY, else the columns every edge references it by when they agree; otherwise
		// every column (rows are then numbered in the order of their whole contents).
		if (!vertex_table->element_key.empty()) {
			vertex_table->identity_columns =
			    IdentityColumns(vertex_table->element_key, vertex_columns[vertex_table->FullTableName()].names);
			continue;
		}
		vector<Identifier> referenced;
		bool agree = true;
		for (auto &edge_table : info.edge_tables) {
			for (int side = 0; side < 2; side++) {
				auto &vertex = side == 0 ? edge_table->source_pg_table : edge_table->destination_pg_table;
				auto &keys = side == 0 ? edge_table->source_pk : edge_table->destination_pk;
				if (vertex.get() != vertex_table.get()) {
					continue;
				}
				if (referenced.empty()) {
					referenced = keys;
				} else if (referenced != keys) {
					agree = false;
				}
			}
		}
		vertex_table->identity_columns = IdentityColumns(agree ? referenced : vector<Identifier>(),
		                                                 vertex_columns[vertex_table->FullTableName()].names);
	}
}

unique_ptr<FunctionData> CreatePropertyGraphFunction::CreatePropertyGraphBind(ClientContext &context,
                                                                              TableFunctionBindInput &input,
                                                                              vector<LogicalType> &return_types,
                                                                              vector<Identifier> &names) {
	names.emplace_back("Success");
	return_types.emplace_back(LogicalType::BOOLEAN);
	auto duckpgq_state = GetDuckPGQState(context);

	const auto duckpgq_parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state->parse_data.get());

	if (!duckpgq_parse_data) {
		return {};
	}
	auto statement = dynamic_cast<CreateStatement *>(duckpgq_parse_data->statement.get());
	auto info = dynamic_cast<CreatePropertyGraphInfo *>(statement->info.get());
	// Record where the graph lives so qualified references can be checked; the schema must exist like for a table.
	auto graph_name = DuckPGQState::ResolvePropertyGraphName(context, info->GetQualifiedName());
	Catalog::GetSchema(context, graph_name.Catalog(), graph_name.Schema());
	info->SetQualifiedName(graph_name);
	auto pg_table = duckpgq_state->registered_property_graphs.find(info->property_graph_name);

	if (pg_table != duckpgq_state->registered_property_graphs.end() &&
	    info->on_conflict == OnCreateConflict::ERROR_ON_CONFLICT) {
		throw Exception(ExceptionType::INVALID,
		                "Property graph table with name " + info->property_graph_name + " already exists");
	}

	ValidatePropertyGraph(context, *info);
	return make_uniq<CreatePropertyGraphBindData>(info);
}

//! The check runs on a separate connection, so the table is named by the catalog entry it resolved to in the user's
//! context (the property graph itself keeps an unqualified name when the search path resolved it).
static string ResolvedTableRef(ClientContext &context, const shared_ptr<PropertyGraphTable> &table,
                               const string &alias) {
	auto entry = GetPropertyGraphCatalogEntry(context, *table);
	if (!entry) {
		return DuckPGQSQL::TableRef(*table, alias);
	}
	return DuckPGQSQL::TableRef(entry->ParentCatalog().GetName(), entry->ParentSchema().name, entry->name, alias);
}

//! Whether DuckDB itself enforces edge_keys -> vertex_table(vertex_keys) through a FOREIGN KEY constraint.
static bool HasEnforcedForeignKey(const TableCatalogEntry &edge_entry, const vector<Identifier> &edge_keys,
                                  const PropertyGraphTable &vertex_table, const vector<Identifier> &vertex_keys) {
	for (auto &constraint : edge_entry.GetConstraints()) {
		if (constraint->type != ConstraintType::FOREIGN_KEY) {
			continue;
		}
		auto &foreign_key = constraint->Cast<ForeignKeyConstraint>();
		if (foreign_key.info.type == ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE ||
		    foreign_key.info.table != vertex_table.table_name ||
		    (!foreign_key.info.schema.empty() && !vertex_table.schema_name.empty() &&
		     foreign_key.info.schema != vertex_table.schema_name) ||
		    foreign_key.fk_columns.size() != edge_keys.size() || foreign_key.pk_columns.size() != edge_keys.size()) {
			continue;
		}
		bool pairs_match = true;
		for (idx_t i = 0; i < edge_keys.size() && pairs_match; i++) {
			pairs_match = false;
			for (idx_t j = 0; j < foreign_key.fk_columns.size(); j++) {
				if (foreign_key.fk_columns[j] == edge_keys[i] && foreign_key.pk_columns[j] == vertex_keys[i]) {
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

//! The property graph declares that every edge key references a vertex row (SOURCE KEY ... REFERENCES ...). SQL/PGQ
//! calls an edge whose key matches no vertex an "incomplete edge" (ISO/IEC 9075-16, 9.5). Verify the declaration once,
//! when the graph is created. Rows modified after creation are not re-checked, so the MATCH translation does not rely
//! on it (vertex-join elimination needs a FOREIGN KEY). An edge with a NULL key component references nothing and is
//! never matched; it is accepted. A FOREIGN KEY constraint already guarantees the reference and skips the scan.
//! Returns false when the data could not be inspected (tables that are not yet committed are invisible to the
//! separate connection); the graph is then created unverified.
static bool CheckEdgeKeysReferenceVertices(ClientContext &context, Connection &connection,
                                           const shared_ptr<PropertyGraphTable> &edge_table_p, const string &side,
                                           const vector<Identifier> &edge_keys,
                                           const shared_ptr<PropertyGraphTable> &vertex_table_p,
                                           const vector<Identifier> &vertex_keys) {
	if (edge_keys.empty() || edge_keys.size() != vertex_keys.size()) {
		return true;
	}
	auto &edge_table = *edge_table_p;
	auto &vertex_table = *vertex_table_p;
	auto edge_entry = GetPropertyGraphTable(context, edge_table_p);
	if (edge_entry && HasEnforcedForeignKey(*edge_entry, edge_keys, vertex_table, vertex_keys)) {
		return true;
	}
	vector<string> not_null;
	vector<string> matches;
	vector<string> projections;
	for (idx_t i = 0; i < edge_keys.size(); i++) {
		auto edge_column = DuckPGQSQL::Column(edge_keys[i], string("e"));
		not_null.push_back(edge_column + " IS NOT NULL");
		matches.push_back(DuckPGQSQL::Column(vertex_keys[i], string("v")) + " = " + edge_column);
		projections.push_back(edge_column);
	}
	std::ostringstream query;
	query << "SELECT " << StringUtil::Join(projections, ", ") << " FROM "
	      << ResolvedTableRef(context, edge_table_p, "e") << " WHERE " << StringUtil::Join(not_null, " AND ")
	      << " AND NOT EXISTS (SELECT 1 FROM " << ResolvedTableRef(context, vertex_table_p, "v") << " WHERE "
	      << StringUtil::Join(matches, " AND ") << ") LIMIT 1";
	auto result = connection.Query(query.str());
	if (result->HasError()) {
		if (result->GetErrorObject().Type() == ExceptionType::CATALOG) {
			return false;
		}
		throw Exception(ExceptionType::INVALID, "Could not verify that the " + side + " key of edge table " +
		                                            edge_table.table_name + " references " + vertex_table.table_name +
		                                            ": " + result->GetError());
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0) {
		return true;
	}
	vector<string> values;
	for (idx_t i = 0; i < edge_keys.size(); i++) {
		values.push_back(chunk->GetValue(i, 0).ToString());
	}
	throw ConstraintException("Incomplete edge: a row of edge table %s has %s key (%s) = (%s), which references no row "
	                          "in vertex table %s (%s). Every edge key must reference an existing vertex; fix or "
	                          "remove the row before creating the property graph.",
	                          edge_table.table_name.GetIdentifierName(), side, StringUtil::Join(edge_keys, ", "),
	                          StringUtil::Join(values, ", "), vertex_table.table_name.GetIdentifierName(),
	                          StringUtil::Join(vertex_keys, ", "));
}

static string StringListSQL(const vector<Identifier> &values) {
	if (values.empty()) {
		return "NULL";
	}
	vector<string> literals;
	for (auto &value : values) {
		literals.push_back(KeywordHelper::WriteQuoted(value.GetIdentifierName(), '\''));
	}
	return "[" + StringUtil::Join(literals, ", ") + "]::VARCHAR[]";
}

//! The values of the __duckpgq_internal columns after abi_options, for one element table of the graph.
static string ElementColumnsSQL(const CreatePropertyGraphInfo &graph, const PropertyGraphTable &table) {
	string expressions = "NULL";
	for (idx_t i = 0; i < table.column_names.size(); i++) {
		if (!table.PropertyExpression(i).empty()) {
			vector<string> values;
			for (idx_t j = 0; j < table.column_names.size(); j++) {
				auto expression = table.PropertyExpression(j);
				values.push_back(expression.empty() ? "NULL" : KeywordHelper::WriteQuoted(expression, '\''));
			}
			expressions = "[" + StringUtil::Join(values, ", ") + "]::VARCHAR[]";
			break;
		}
	}
	return string(table.is_view ? "true" : "NULL") + ", " + StringListSQL(table.identity_columns) + ", " +
	       StringListSQL(table.extra_labels) + ", " + StringListSQL(table.element_key) + ", " + expressions + ", " +
	       (table.table_name_alias.empty() ? string("NULL")
	                                       : KeywordHelper::WriteQuoted(PGQStr(table.table_name_alias), '\'')) +
	       ", " + (graph.abi_disabled ? "true" : "NULL");
}

//! A KEY must identify the table's rows (SQL/PGQ element key). A PRIMARY KEY or UNIQUE constraint on the same columns
//! guarantees it; otherwise it is trusted, and checked here when pgq_verify_edges_on_create asks for the data checks:
//! no key value may occur twice or contain NULL. Returns false when the data could not be read.
static bool CheckElementKeyIdentifies(ClientContext &context, Connection &connection,
                                      const shared_ptr<PropertyGraphTable> &table) {
	if (table->element_key.empty()) {
		return true;
	}
	auto columns = CreatePropertyGraphFunction::ResolveElementColumns(context, *table);
	if (KeyIsEnforced(columns, table->element_key)) {
		return true;
	}
	vector<string> keys;
	vector<string> nulls;
	for (auto &column : table->element_key) {
		keys.push_back(DuckPGQSQL::Column(column, string("t")));
		nulls.push_back(DuckPGQSQL::Column(column, string("t")) + " IS NULL");
	}
	auto result = connection.Query("SELECT " + StringUtil::Join(keys, ", ") + ", count(*) FROM " +
	                               ResolvedTableRef(context, table, "t") + " GROUP BY ALL HAVING count(*) > 1 OR " +
	                               StringUtil::Join(nulls, " OR ") + " LIMIT 1");
	if (result->HasError()) {
		if (result->GetErrorObject().Type() == ExceptionType::CATALOG) {
			return false;
		}
		throw Exception(ExceptionType::INVALID,
		                "Could not verify the KEY of element table " + table->table_name + ": " + result->GetError());
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0) {
		return true;
	}
	vector<string> values;
	for (idx_t i = 0; i < table->element_key.size(); i++) {
		values.push_back(chunk->GetValue(i, 0).ToString());
	}
	throw ConstraintException("KEY (%s) of element table %s does not identify its rows: (%s) occurs %s times. A key "
	                          "must be unique and not NULL.",
	                          StringUtil::Join(IdentifiersToStrings(table->element_key), ", "),
	                          table->table_name.GetIdentifierName(), StringUtil::Join(values, ", "),
	                          chunk->GetValue(table->element_key.size(), 0).ToString());
}

unique_ptr<GlobalTableFunctionState>
CreatePropertyGraphFunction::CreatePropertyGraphInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<CreatePropertyGraphGlobalData>();
}

void CreatePropertyGraphFunction::CreatePropertyGraphFunc(ClientContext &context, TableFunctionInput &data_p,
                                                          DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<CreatePropertyGraphBindData>();
	auto pg_info = bind_data.create_pg_info;
	auto duckpgq_state = GetDuckPGQState(context);

	duckpgq_state->InitializeInternalTable(context);
	auto new_conn = make_shared_ptr<Connection>(*context.db);
	auto retrieve_query = new_conn->Query("SELECT * FROM __duckpgq_internal where property_graph = '" +
	                                      pg_info->property_graph_name + "';");
	if (retrieve_query->HasError()) {
		throw TransactionException(retrieve_query->GetError());
	}
	bool replace_existing = false;
	if (retrieve_query->RowCount() > 0) {
		if (pg_info->on_conflict == OnCreateConflict::ERROR_ON_CONFLICT) {
			throw Exception(ExceptionType::INVALID,
			                "Property graph " + pg_info->property_graph_name + " is already registered");
		}
		if (pg_info->on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
			return; // Do nothing and silently return
		}
		replace_existing = pg_info->on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT;
	}

	StorePropertyGraph(context, *pg_info, *new_conn, replace_existing);
}

//! Verifies the data (when pgq_verify_edges_on_create asks for it), registers the graph on every connection and
//! writes its __duckpgq_internal rows, replacing the graph's previous rows when replace_existing.
void CreatePropertyGraphFunction::StorePropertyGraph(ClientContext &context, CreatePropertyGraphInfo &info,
                                                     Connection &connection, bool replace_existing) {
	auto pg_info = &info;
	auto new_conn = &connection;
	// Verify the data before anything is registered, so that a failed CREATE leaves no trace and CREATE OR REPLACE
	// keeps the previous definition.
	// The check scans every edge table, so it is opt-in.
	Value verify_edges;
	const bool verify = context.TryGetCurrentSetting("pgq_verify_edges_on_create", verify_edges) &&
	                    !verify_edges.IsNull() && verify_edges.GetValue<bool>();
	pg_info->verified = verify;
	for (auto &table : pg_info->vertex_tables) {
		if (verify) {
			pg_info->verified &= CheckElementKeyIdentifies(context, *new_conn, table);
		}
	}
	for (auto &edge_table : pg_info->edge_tables) {
		if (verify) {
			pg_info->verified &= CheckElementKeyIdentifies(context, *new_conn, edge_table);
		}
	}
	for (auto &edge_table : pg_info->edge_tables) {
		if (!verify) {
			break;
		}
		pg_info->verified &=
		    CheckEdgeKeysReferenceVertices(context, *new_conn, edge_table, "source", edge_table->source_fk,
		                                   edge_table->source_pg_table, edge_table->source_pk);
		pg_info->verified &=
		    CheckEdgeKeysReferenceVertices(context, *new_conn, edge_table, "destination", edge_table->destination_fk,
		                                   edge_table->destination_pg_table, edge_table->destination_pk);
	}

	for (auto &local_client_context : ConnectionManager::Get(*context.db).GetConnectionList()) {
		auto local_state = GetDuckPGQState(*local_client_context);
		local_state->registered_property_graphs[pg_info->property_graph_name] = pg_info->Copy();
	}
	if (replace_existing) {
		// DELETE the old property graph and insert new one.
		new_conn->Query("DELETE FROM __duckpgq_internal WHERE property_graph = '" + pg_info->property_graph_name +
		                "';");
	}

	auto graph_catalog = pg_info->GetQualifiedName().Catalog().GetIdentifierName();
	auto graph_schema = pg_info->GetQualifiedName().Schema().GetIdentifierName();
	const string abi_options_sql =
	    pg_info->abi_options.empty() ? string("NULL") : KeywordHelper::WriteQuoted(pg_info->abi_options, '\'');
	string insert_info = "INSERT INTO __duckpgq_internal VALUES ";
	for (const auto &v_table : pg_info->vertex_tables) {
		insert_info += "(";
		insert_info += "'" + pg_info->property_graph_name + "', ";
		insert_info += "'" + v_table->table_name + "', ";
		insert_info += "'" + v_table->main_label + "', ";
		insert_info += "true, "; // is_vertex_table
		insert_info += "NULL, "; // source_table
		insert_info += "NULL, "; // source_pk
		insert_info += "NULL, "; // source_fk
		insert_info += "NULL, "; // destination_table
		insert_info += "NULL, "; // destination_pk
		insert_info += "NULL, "; // destination_fk
		insert_info += v_table->discriminator.empty() ? "NULL, " : "'" + v_table->discriminator + "', ";
		if (!v_table->discriminator.empty()) {
			insert_info += "[";
			for (idx_t i = 0; i < v_table->sub_labels.size(); i++) {
				insert_info += "'" + v_table->sub_labels[i] + (i == v_table->sub_labels.size() - 1 ? "'" : "', ");
			}
			insert_info += "],";
		} else {
			insert_info += "NULL,";
		}
		insert_info += "'" + v_table->catalog_name + "', ";
		insert_info += "'" + v_table->schema_name + "', ";
		insert_info += "NULL,"; // source table catalog
		insert_info += "NULL,"; // source table schema
		insert_info += "NULL,"; // destination table catalog
		insert_info += "NULL,"; // destination table schema
		insert_info += "[";     // Start of column names
		for (idx_t i = 0; i < v_table->column_names.size(); i++) {
			insert_info += "'" + v_table->column_names[i] + (i == v_table->column_names.size() - 1 ? "'" : "', ");
		}
		insert_info += "],"; // End of column names
		insert_info += "[";  // Start of column aliases
		for (idx_t i = 0; i < v_table->column_aliases.size(); i++) {
			insert_info += "'" + v_table->column_aliases[i] + (i == v_table->column_aliases.size() - 1 ? "'" : "', ");
		}
		insert_info += "], "; // End of column aliases
		insert_info += "'" + graph_catalog + "', '" + graph_schema + "', ";
		insert_info += pg_info->verified ? "true" : "false";
		insert_info += ", " + abi_options_sql;
		insert_info += ", " + ElementColumnsSQL(*pg_info, *v_table);
		insert_info += "), ";
	}

	for (const auto &e_table : pg_info->edge_tables) {
		insert_info += "(";
		insert_info += "'" + pg_info->property_graph_name + "', ";
		insert_info += "'" + e_table->table_name + "', ";
		insert_info += "'" + e_table->main_label + "', ";
		insert_info += "false, "; // is_vertex_table
		// Persist physical vertex names: table aliases only exist in the CREATE statement.
		insert_info += "'" + e_table->source_pg_table->table_name + "', ";
		insert_info += "[";
		for (const auto &source_pk : e_table->source_pk) {
			insert_info += "'" + source_pk + "', ";
		}
		insert_info += "], ";
		insert_info += "[";
		for (const auto &source_fk : e_table->source_fk) {
			insert_info += "'" + source_fk + "', ";
		}
		insert_info += "], ";

		insert_info += "'" + e_table->destination_pg_table->table_name + "', ";
		insert_info += "[";
		for (const auto &destination_pk : e_table->destination_pk) {
			insert_info += "'" + destination_pk + "', ";
		}
		insert_info += "], ";
		insert_info += "[";
		for (const auto &destination_fk : e_table->destination_fk) {
			insert_info += "'" + destination_fk + "', ";
		}
		insert_info += "], ";

		insert_info += e_table->discriminator.empty() ? "NULL, " : "'" + e_table->discriminator + "', ";
		if (!e_table->discriminator.empty()) {
			insert_info += "[";
			for (idx_t i = 0; i < e_table->sub_labels.size(); i++) {
				insert_info += "'" + e_table->sub_labels[i] + (i == e_table->sub_labels.size() - 1 ? "'" : "', ");
			}
			insert_info += "], ";
		} else {
			insert_info += "NULL, ";
		}
		insert_info += "'" + e_table->catalog_name + "', ";
		insert_info += "'" + e_table->schema_name + "', ";
		insert_info += "'" + e_table->source_pg_table->catalog_name + "', ";
		insert_info += "'" + e_table->source_pg_table->schema_name + "', ";
		insert_info += "'" + e_table->destination_pg_table->catalog_name + "', ";
		insert_info += "'" + e_table->destination_pg_table->schema_name + "', ";
		insert_info += "["; // Start of column names
		for (idx_t i = 0; i < e_table->column_names.size(); i++) {
			insert_info += "'" + e_table->column_names[i] + (i == e_table->column_names.size() - 1 ? "'" : "', ");
		}
		insert_info += "],"; // End of column names
		insert_info += "[";  // Start of column aliases
		for (idx_t i = 0; i < e_table->column_aliases.size(); i++) {
			insert_info += "'" + e_table->column_aliases[i] + (i == e_table->column_aliases.size() - 1 ? "'" : "', ");
		}
		insert_info += "], "; // End of column aliases
		insert_info += "'" + graph_catalog + "', '" + graph_schema + "', ";
		insert_info += pg_info->verified ? "true" : "false";
		insert_info += ", " + abi_options_sql;
		insert_info += ", " + ElementColumnsSQL(*pg_info, *e_table);
		insert_info += "), ";
	}
	auto insert_query = new_conn->Query(insert_info);
	if (insert_query->HasError()) {
		throw TransactionException(insert_query->GetError());
	}
	// pgq_abi_mode = 'auto' builds the graph's artifacts now, when that is safe; other modes leave it to pgq_abi_build.
	PGQAutoBuildAbi(context, *pg_info);
}

//------------------------------------------------------------------------------
// ALTER PROPERTY GRAPH
//------------------------------------------------------------------------------
//! The element table an ALTER names: by its alias, or by its table name when that is unambiguous.
static shared_ptr<PropertyGraphTable> FindElementTable(const vector<shared_ptr<PropertyGraphTable>> &tables,
                                                       const Identifier &name, bool vertex, const string &graph) {
	shared_ptr<PropertyGraphTable> by_alias;
	vector<shared_ptr<PropertyGraphTable>> by_name;
	for (auto &table : tables) {
		if (!table->table_name_alias.empty() &&
		    StringUtil::CIEquals(table->table_name_alias.GetIdentifierName(), name.GetIdentifierName())) {
			by_alias = table;
		}
		if (StringUtil::CIEquals(table->table_name.GetIdentifierName(), name.GetIdentifierName())) {
			by_name.push_back(table);
		}
	}
	if (by_alias) {
		return by_alias;
	}
	if (by_name.size() == 1) {
		return by_name[0];
	}
	throw BinderException("%s %s of property graph %s %s", vertex ? "Vertex table" : "Edge table",
	                      name.GetIdentifierName(), graph,
	                      by_name.empty() ? "does not exist" : "is ambiguous: name it by its alias");
}

static string ElementName(const PropertyGraphTable &table) {
	return PGQStr(table.table_name_alias.empty() ? table.table_name : table.table_name_alias);
}

static void RemoveTable(vector<shared_ptr<PropertyGraphTable>> &tables, const shared_ptr<PropertyGraphTable> &table) {
	tables.erase(std::remove(tables.begin(), tables.end(), table), tables.end());
}

//! ADD LABEL l [PROPERTIES ...]: the table gets the label, and the label's properties (SQL/PGQ: all columns when it
//! has no PROPERTIES clause) join the table's.
static void AddLabel(PropertyGraphTable &table, const AlterPropertyGraphInfo &alter) {
	auto &label = alter.label;
	if (table.HasStaticLabel(label.GetIdentifierName())) {
		throw BinderException("Element table %s already has label %s", ElementName(table), label.GetIdentifierName());
	}
	table.extra_labels.push_back(label);
	auto &clause = *alter.label_properties;
	if (clause.no_columns) {
		return;
	}
	table.no_columns = false;
	if (clause.all_columns) {
		// Expanded again, with the table's current properties after them (see CheckPropertyGraphTableColumns).
		if (!table.all_columns) {
			table.except_columns = clause.except_columns;
		} else {
			vector<Identifier> kept;
			for (auto &column : table.except_columns) {
				for (auto &other : clause.except_columns) {
					if (StringUtil::CIEquals(column.GetIdentifierName(), other.GetIdentifierName())) {
						kept.push_back(column);
					}
				}
			}
			table.except_columns = std::move(kept);
		}
		table.all_columns = true;
	}
	for (idx_t i = 0; i < clause.column_names.size(); i++) {
		bool known = false;
		for (idx_t j = 0; j < table.column_names.size(); j++) {
			if (!StringUtil::CIEquals(table.PropertyName(j).GetIdentifierName(),
			                          clause.PropertyName(i).GetIdentifierName())) {
				continue;
			}
			if (!StringUtil::CIEquals(table.column_names[j].GetIdentifierName(),
			                          clause.column_names[i].GetIdentifierName()) ||
			    table.PropertyExpression(j) != clause.PropertyExpression(i)) {
				throw BinderException("Property %s of element table %s is defined twice, differently",
				                      clause.PropertyName(i).GetIdentifierName(), ElementName(table));
			}
			known = true;
		}
		if (!known) {
			table.property_expressions.resize(table.column_names.size());
			table.column_names.push_back(clause.column_names[i]);
			table.column_aliases.push_back(clause.PropertyName(i));
			table.property_expressions.push_back(clause.PropertyExpression(i));
		}
	}
}

//! DROP LABEL l: the table's first label is the next one when l was the first. Its properties stay.
static void DropLabel(PropertyGraphTable &table, const Identifier &label) {
	auto name = label.GetIdentifierName();
	for (auto &sub_label : table.sub_labels) {
		if (StringUtil::CIEquals(sub_label.GetIdentifierName(), name)) {
			throw BinderException("Label %s of element table %s is a discriminator sub-label (IN %s (...)); recreate "
			                      "the graph to change those",
			                      name, ElementName(table), PGQStr(table.discriminator));
		}
	}
	if (!table.HasStaticLabel(name)) {
		throw BinderException("Element table %s has no label %s", ElementName(table), name);
	}
	if (StringUtil::CIEquals(table.main_label.GetIdentifierName(), name)) {
		if (table.extra_labels.empty()) {
			throw BinderException("Label %s is the only label of element table %s; an element table has at least one",
			                      name, ElementName(table));
		}
		table.main_label = table.extra_labels[0];
		table.extra_labels.erase(table.extra_labels.begin());
		return;
	}
	for (idx_t i = 0; i < table.extra_labels.size(); i++) {
		if (StringUtil::CIEquals(table.extra_labels[i].GetIdentifierName(), name)) {
			table.extra_labels.erase(table.extra_labels.begin() + i);
			return;
		}
	}
}

//! The WITH ABI clause may name labels (closures, branch summaries, ...); after a DROP it must still name existing
//! ones, or pgq_abi_build would refuse the graph.
static void CheckAbiOptionLabels(ClientContext &context, const CreatePropertyGraphInfo &graph) {
	if (graph.abi_options.empty()) {
		return;
	}
	auto options = PGQAbiOptionsForGraph(context, graph, {});
	vector<string> labels;
	for (auto &closure : options.closures) {
		labels.push_back(closure.relationship);
	}
	for (auto &branch : options.branch_summaries) {
		labels.push_back(branch.anchor);
		labels.insert(labels.end(), branch.branches.begin(), branch.branches.end());
	}
	for (auto &path : options.exclude_paths) {
		labels.insert(labels.end(), path.begin(), path.end());
	}
	for (auto &entry : options.cover) {
		labels.push_back(entry.first);
	}
	for (auto &entry : options.edge_domains) {
		labels.push_back(entry.first);
	}
	for (auto &label : labels) {
		if (graph.label_map.find(label) == graph.label_map.end()) {
			throw BinderException("The WITH ABI clause of property graph %s names label %s, which the altered graph "
			                      "would not have; recreate the graph (CREATE OR REPLACE) with a WITH ABI clause "
			                      "that does not name it",
			                      graph.property_graph_name, label);
		}
	}
}

unique_ptr<FunctionData> AlterPropertyGraphFunction::AlterPropertyGraphBind(ClientContext &context,
                                                                            TableFunctionBindInput &input,
                                                                            vector<LogicalType> &return_types,
                                                                            vector<Identifier> &names) {
	names.emplace_back("Success");
	return_types.emplace_back(LogicalType::BOOLEAN);
	auto duckpgq_state = GetDuckPGQState(context);
	auto parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state->parse_data.get());
	if (!parse_data) {
		return {};
	}
	auto &statement = parse_data->statement->Cast<CreateStatement>();
	auto &alter = statement.info->Cast<AlterPropertyGraphInfo>();
	auto current = duckpgq_state->TryGetPropertyGraph(context, alter.GetQualifiedName());
	if (!current) {
		throw BinderException("Property graph %s does not exist", alter.GetQualifiedName().ToString());
	}
	auto altered = unique_ptr_cast<CreateInfo, CreatePropertyGraphInfo>(current->Copy());
	altered->on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
	auto &graph = altered->property_graph_name;

	switch (alter.action) {
	case PGQAlterGraphAction::ADD_VERTEX_TABLES:
	case PGQAlterGraphAction::ADD_EDGE_TABLES: {
		const bool vertex = alter.action == PGQAlterGraphAction::ADD_VERTEX_TABLES;
		auto &added = vertex ? alter.vertex_tables : alter.edge_tables;
		auto &tables = vertex ? altered->vertex_tables : altered->edge_tables;
		for (auto &table : added) {
			for (auto &existing : altered->vertex_tables) {
				if (StringUtil::CIEquals(ElementName(*existing), ElementName(*table))) {
					throw BinderException("Property graph %s already has an element table %s", graph,
					                      ElementName(*table));
				}
			}
			for (auto &existing : altered->edge_tables) {
				if (StringUtil::CIEquals(ElementName(*existing), ElementName(*table))) {
					throw BinderException("Property graph %s already has an element table %s", graph,
					                      ElementName(*table));
				}
			}
			auto copy = table->Copy();
			copy->source_pg_table = nullptr;
			copy->destination_pg_table = nullptr;
			tables.push_back(std::move(copy));
		}
		break;
	}
	case PGQAlterGraphAction::DROP_VERTEX_TABLES:
		for (auto &name : alter.element_tables) {
			auto table = FindElementTable(altered->vertex_tables, name, true, graph);
			vector<shared_ptr<PropertyGraphTable>> referencing;
			for (auto &edge : altered->edge_tables) {
				if (edge->source_pg_table == table || edge->destination_pg_table == table) {
					referencing.push_back(edge);
				}
			}
			if (!referencing.empty() && !alter.cascade) {
				vector<string> edge_names;
				for (auto &edge : referencing) {
					edge_names.push_back(ElementName(*edge));
				}
				throw BinderException("Vertex table %s of property graph %s is referenced by edge table(s) %s: drop "
				                      "them first, or DROP VERTEX TABLES (...) CASCADE to drop them with it",
				                      ElementName(*table), graph, StringUtil::Join(edge_names, ", "));
			}
			for (auto &edge : referencing) {
				RemoveTable(altered->edge_tables, edge);
			}
			RemoveTable(altered->vertex_tables, table);
		}
		if (altered->vertex_tables.empty()) {
			throw BinderException("Property graph %s would have no vertex table left; DROP PROPERTY GRAPH drops it",
			                      graph);
		}
		break;
	case PGQAlterGraphAction::DROP_EDGE_TABLES:
		for (auto &name : alter.element_tables) {
			RemoveTable(altered->edge_tables, FindElementTable(altered->edge_tables, name, false, graph));
		}
		break;
	case PGQAlterGraphAction::ADD_LABEL:
	case PGQAlterGraphAction::DROP_LABEL: {
		auto &tables = alter.element_is_vertex ? altered->vertex_tables : altered->edge_tables;
		auto table = FindElementTable(tables, alter.element_tables[0], alter.element_is_vertex, graph);
		if (alter.action == PGQAlterGraphAction::ADD_LABEL) {
			AddLabel(*table, alter);
		} else {
			DropLabel(*table, alter.label);
		}
		break;
	}
	}
	altered->ResolveEdgeReferences();
	altered->RebuildLabelMap();
	CreatePropertyGraphFunction::ValidatePropertyGraph(context, *altered);
	CheckAbiOptionLabels(context, *altered);
	// Artifacts were built for the previous definition: a dropped table's, or a family the new one adds to.
	altered->abi_disabled = true;
	return make_uniq<AlterPropertyGraphBindData>(std::move(altered));
}

void AlterPropertyGraphFunction::AlterPropertyGraphFunc(ClientContext &context, TableFunctionInput &data_p,
                                                        DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<AlterPropertyGraphBindData>();
	auto &global = data_p.global_state->Cast<CreatePropertyGraphFunction::CreatePropertyGraphGlobalData>();
	if (global.done) {
		return;
	}
	global.done = true;
	auto duckpgq_state = GetDuckPGQState(context);
	duckpgq_state->InitializeInternalTable(context);
	Connection connection(*context.db);
	CreatePropertyGraphFunction::StorePropertyGraph(context, *bind_data.altered, connection, true);
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CoreTableFunctions::RegisterCreatePropertyGraphTableFunction(ExtensionLoader &loader) {
	loader.RegisterFunction(CreatePropertyGraphFunction());
	loader.RegisterFunction(AlterPropertyGraphFunction());
}

} // namespace duckdb
