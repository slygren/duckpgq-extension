//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/parser/property_graph_table.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckpgq/parser/identifier.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/vector.hpp"
#include <duckdb/parser/tableref/basetableref.hpp>

namespace duckdb {

//! Represents a reference to a graph table from the CREATE PROPERTY GRAPH
class PropertyGraphTable {
public:
	//! Used for Copy
	PropertyGraphTable();
	//! Specify both the column and table name
	PropertyGraphTable(string table_name, const vector<string> &column_name, const vector<string> &label,
	                   string catalog_name = "", string schema = DEFAULT_SCHEMA);
	//! Specify both the column and table name with alias
	PropertyGraphTable(string table_name, string table_alias, const vector<string> &column_name,
	                   const vector<string> &label, string catalog_name = "", string schema = DEFAULT_SCHEMA);
	Identifier table_name;
	Identifier table_name_alias;

	//! The stack of names in order of which they appear (column_names[0], column_names[1], column_names[2], ...)
	vector<Identifier> column_names;
	vector<Identifier> column_aliases;

	vector<Identifier> except_columns;

	vector<Identifier> sub_labels;
	//! The first label declared for the table; label(x) returns it.
	Identifier main_label;
	//! The table's other labels, in declaration order (LABEL A LABEL B ...). Like main_label, every row has them.
	vector<Identifier> extra_labels;

	Identifier catalog_name;
	Identifier schema_name;

	//! Associated with the PROPERTIES keyword not mentioned in the creation of table, equalling SELECT * in some sense
	bool all_columns = false;

	//! Associated with the NO PROPERTIES functionality
	bool no_columns = false;

	bool is_vertex_table = false;

	Identifier discriminator;

	vector<Identifier> source_fk;

	vector<Identifier> source_pk;

	Identifier source_catalog;
	Identifier source_schema;
	Identifier source_reference;

	shared_ptr<PropertyGraphTable> source_pg_table;

	vector<Identifier> destination_fk;

	vector<Identifier> destination_pk;

	Identifier destination_catalog;
	Identifier destination_schema;
	Identifier destination_reference;

	shared_ptr<PropertyGraphTable> destination_pg_table;

	//! The element key declared with KEY (...): the columns that identify the table's elements. Empty when the table
	//! declares none (its key is then its PRIMARY KEY, or for a vertex table the columns its edges reference).
	vector<Identifier> element_key;

	//! The element table is a view. A view has no rowid, so the MATCH translation reads it through an element
	//! relation that numbers its rows by identity_columns (see PGQElementRelations).
	bool is_view = false;
	//! The columns that order a view's rows for its element ids: its element key, else every column.
	vector<Identifier> identity_columns;

public:
	string ToString() const;
	bool Equals(const PropertyGraphTable *other_p) const;
	bool SameTableIdentity(const PropertyGraphTable &other) const;

	shared_ptr<PropertyGraphTable> Copy() const;

	void Serialize(Serializer &serializer) const;

	static shared_ptr<PropertyGraphTable> Deserialize(Deserializer &deserializer);

	bool hasTableNameAlias() const {
		return !table_name_alias.empty();
	}

	string FullTableName() const {
		string full_table_name = catalog_name.empty() ? "" : catalog_name + ".";
		full_table_name += schema_name.empty() ? "" : schema_name + ".";
		full_table_name += table_name;
		return full_table_name;
	}

	unique_ptr<BaseTableRef> CreateBaseTableRef(const string &alias = "") const {
		auto base_table_ref = make_uniq<BaseTableRef>();
		base_table_ref->SetQualifiedName(catalog_name, schema_name, table_name);
		base_table_ref->alias = Identifier(alias.empty() ? "" : alias);
		return base_table_ref;
	}

	bool IsSourceTable(const Identifier &table_name);

	//! Whether every row of the table has this label: its main label or one of its extra labels.
	bool HasStaticLabel(const string &label) const;
	//! main_label followed by extra_labels.
	vector<Identifier> StaticLabels() const;
};
} // namespace duckdb
