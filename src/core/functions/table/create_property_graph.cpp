#include "duckpgq/core/functions/table/abi_build.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckpgq/core/functions/table/create_property_graph.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckpgq/common.hpp"
#include <duckpgq/core/functions/table.hpp>
#include <duckpgq/core/utils/duckpgq_utils.hpp>
#include "duckdb/main/connection_manager.hpp"
#include <duckpgq/core/parser/duckpgq_parser.hpp>
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckpgq/core/utils/duckpgq_sql.hpp"

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

		pg_table->column_names = ExpandAllColumnsExcept(IdentifiersToStrings(table.names), pg_table->except_columns);
		pg_table->column_aliases = pg_table->column_names;
		return;
	}

	for (auto &column : pg_table->column_names) {
		if (!table.Exists(PGQIdentifier(column))) {
			throw Exception(ExceptionType::INVALID, "Column " + column + " not found in table " + pg_table->table_name);
		}
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

	case_insensitive_set_t v_table_names;
	case_insensitive_map_t<ElementColumns> vertex_columns;
	for (auto &vertex_table : info->vertex_tables) {
		ElementColumns table;
		try {
			BindPropertyGraphTable(context, vertex_table);
			table = ResolveElementColumns(context, *vertex_table);
			CheckPropertyGraphTableColumns(vertex_table, table);
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

	for (auto &edge_table : info->edge_tables) {
		try {
			BindPropertyGraphTable(context, edge_table);
			auto table = ResolveElementColumns(context, *edge_table);
			edge_table->is_view = table.is_view;
			CheckPropertyGraphTableColumns(edge_table, table);
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
				ThrowMissingVertexReference(*info, edge_table->source_catalog, edge_table->source_schema,
				                            edge_table->source_reference);
			}
			ValidateVertexTableRegistration(edge_table->source_pg_table, v_table_names);

			// Validate primary keys in the source table
			ValidatePrimaryKeyInTable(context, edge_table->source_pg_table, edge_table->source_pk);

			// Validate destination table registration
			if (!edge_table->destination_pg_table) {
				ThrowMissingVertexReference(*info, edge_table->destination_catalog, edge_table->destination_schema,
				                            edge_table->destination_reference);
			}
			ValidateVertexTableRegistration(edge_table->destination_pg_table, v_table_names);

			// Validate primary keys in the destination table
			ValidatePrimaryKeyInTable(context, edge_table->destination_pg_table, edge_table->destination_pk);
			if (table.is_view) {
				edge_table->identity_columns = IdentityColumns(vector<Identifier>(), table.names);
			}
		} catch (CatalogException &e) {
			throw Exception(ExceptionType::INVALID, e.what());
		} catch (BinderException &e) {
			throw Exception(ExceptionType::INVALID, "Catalog '" + edge_table->catalog_name + "' does not exist!");
		}
	}
	for (auto &vertex_table : info->vertex_tables) {
		if (!vertex_table->is_view) {
			continue;
		}
		// The vertex key of a view: the columns every edge references it by, when they agree; otherwise every
		// column (rows are then numbered in the order of their whole contents).
		vector<Identifier> referenced;
		bool agree = true;
		for (auto &edge_table : info->edge_tables) {
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

//! The values of the __duckpgq_internal columns after abi_options, for one element table.
static string ElementColumnsSQL(const PropertyGraphTable &table) {
	return string(table.is_view ? "true" : "NULL") + ", " + StringListSQL(table.identity_columns);
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

	// Verify the data before anything is registered, so that a failed CREATE leaves no trace and CREATE OR REPLACE
	// keeps the previous definition.
	// The check scans every edge table, so it is opt-in.
	Value verify_edges;
	const bool verify = context.TryGetCurrentSetting("pgq_verify_edges_on_create", verify_edges) &&
	                    !verify_edges.IsNull() && verify_edges.GetValue<bool>();
	pg_info->verified = verify;
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
		insert_info += ", " + ElementColumnsSQL(*v_table);
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
		insert_info += ", " + ElementColumnsSQL(*e_table);
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
// Register functions
//------------------------------------------------------------------------------
void CoreTableFunctions::RegisterCreatePropertyGraphTableFunction(ExtensionLoader &loader) {
	loader.RegisterFunction(CreatePropertyGraphFunction());
}

} // namespace duckdb
