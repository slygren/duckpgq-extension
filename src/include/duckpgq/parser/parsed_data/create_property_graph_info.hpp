//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckpgq/parser/parsed_data/create_property_graph_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <vector>
#include "duckdb/catalog/catalog_entry/column_dependency_manager.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckpgq/parser/property_graph_table.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/catalog/catalog_entry.hpp"

namespace duckdb {

struct CreatePropertyGraphInfo : public CreateInfo {
	CreatePropertyGraphInfo();
	explicit CreatePropertyGraphInfo(string property_graph_name);

	//! Property graph name
	string property_graph_name;
	//! List of vector tables
	vector<shared_ptr<PropertyGraphTable>> vertex_tables;

	vector<shared_ptr<PropertyGraphTable>> edge_tables;

	//! Dictionary to point label to vector or edge table
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> label_map;

	//! Whether CREATE PROPERTY GRAPH verified that every edge key references a vertex row, as of that moment. Graphs
	//! persisted before the check existed, or created over tables the check could not read, are not verified.
	bool verified = false;

	//! The WITH ABI (...) clause: what pgq_abi_build builds for this graph, as a STRUCT literal in SQL text
	//! ({'max_hops': 3, ...}). Empty when the statement has none.
	string abi_options;

	//! Set by ALTER PROPERTY GRAPH: the graph's ABI artifacts were built for another definition, so none of them
	//! answers until pgq_abi_build builds them again.
	bool abi_disabled = false;

	//! Points each edge table at its source and destination among vertex_tables, by the name or alias the edge
	//! references, and resolves those references to the vertex tables' physical names.
	void ResolveEdgeReferences();
	//! label_map from the tables' labels; throws when a label is not unique.
	void RebuildLabelMap();

public:
	unique_ptr<CreateInfo> Copy() const override;

	string ToString() const override {
		string result = "-CREATE PROPERTY GRAPH " + property_graph_name + " ";
		result += "VERTEX TABLES ( ";
		for (idx_t i = 0; i < vertex_tables.size(); i++) {
			result += vertex_tables[i]->ToString();
			if (i != vertex_tables.size() - 1) {
				result += ", ";
			}
		}
		result += " ) ";
		result += "EDGE TABLES ( ";
		for (idx_t i = 0; i < edge_tables.size(); i++) {
			result += edge_tables[i]->ToString();
			if (i != edge_tables.size() - 1) {
				result += ", ";
			}
		}
		result += " ) ";
		return result;
	}

	static size_t LevenshteinDistance(const string &s1, const string &s2) {
		const size_t len1 = s1.size(), len2 = s2.size();
		std::vector<std::vector<size_t>> d(len1 + 1, std::vector<size_t>(len2 + 1));

		// Initialize the first row and column
		d[0][0] = 0;
		for (size_t i = 1; i <= len1; ++i) {
			d[i][0] = i;
		}
		for (size_t i = 1; i <= len2; ++i) {
			d[0][i] = i;
		}

		// Compute the Levenshtein distance
		for (size_t i = 1; i <= len1; ++i) {
			for (size_t j = 1; j <= len2; ++j) {
				// Calculate the cost for substitution
				size_t substitution_cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;

				// Compute deletion, insertion, and substitution costs
				size_t deletion = d[i - 1][j] + 1;
				size_t insertion = d[i][j - 1] + 1;
				size_t substitution = d[i - 1][j - 1] + substitution_cost;

				// Find the minimum of the three
				size_t min_cost = deletion; // Start with deletion
				if (insertion < min_cost) {
					min_cost = insertion;
				}
				if (substitution < min_cost) {
					min_cost = substitution;
				}

				// Set the minimum cost in the matrix
				d[i][j] = min_cost;
			}
		}

		return d[len1][len2];
	}

	shared_ptr<PropertyGraphTable> GetTableByName(const string &catalog_name, const string &schema_name,
	                                              const string &table_name, bool error_not_found = true,
	                                              bool is_vertex_table = true) {
		if (is_vertex_table) {
			// search vertex tables
			for (const auto &vertex_table : vertex_tables) {
				if (vertex_table->catalog_name != catalog_name) {
					continue;
				}
				if (vertex_table->schema_name != schema_name) {
					continue;
				}
				if (vertex_table->table_name != table_name) {
					continue;
				}
				return vertex_table;
			}
		} else {
			// Search edge tables
			for (const auto &edge_table : edge_tables) {
				if (edge_table->catalog_name != catalog_name) {
					continue;
				}
				if (edge_table->schema_name != schema_name) {
					continue;
				}
				if (edge_table->table_name != table_name) {
					continue;
				}
				return edge_table;
			}
		}
		if (error_not_found) {
			string full_table_name = (catalog_name.empty() ? "" : catalog_name + ".") +
			                         (schema_name.empty() ? "" : schema_name + ".") + table_name;
			throw Exception(ExceptionType::INVALID, "Table '" + full_table_name + "' not found in the property graph " +
			                                            property_graph_name + ".");
		}
		return nullptr; // Return nullptr if no match is found and error_not_found is false
	}

	shared_ptr<PropertyGraphTable> GetTableByLabel(const string &label, bool error_not_found = true,
	                                               bool is_vertex_table = true) {
		// First, check if there is an exact match for the table name in label_map
		auto table_entry = label_map.find(label);
		if (table_entry != label_map.end()) {
			// Exact table match found, but verify if it matches the vertex/edge type
			if (table_entry->second->is_vertex_table == is_vertex_table) {
				return table_entry->second;
			}
			if (error_not_found) {
				throw Exception(ExceptionType::INVALID, "Exact label '" + label + "' found, but it is not a " +
				                                            (is_vertex_table ? "vertex" : "edge") + " table.");
			}
			return nullptr;
		}

		// If no exact table match is found, search for the closest label match using Levenshtein distance
		string closest_label;
		auto min_distance = std::numeric_limits<size_t>::max();

		for (const auto &pair : label_map) {
			const auto &pg_table = pair.second;

			// Only consider tables of the correct type (vertex or edge)
			if (pg_table->is_vertex_table != is_vertex_table) {
				continue;
			}
			if (pg_table->table_name.GetIdentifierName() == label) {
				throw Exception(
				    ExceptionType::INVALID,
				    "Table '" + label +
				        "' found in the property graph, but does not have the correct label. Did you mean the label '" +
				        pg_table->main_label + "' instead?");
			}

			// Use int64_t for the distance calculations
			auto distance_main_label = LevenshteinDistance(label, pg_table->main_label.GetIdentifierName());
			if (distance_main_label < min_distance) {
				min_distance = distance_main_label;
				closest_label = pg_table->main_label.GetIdentifierName();
			}

			for (const auto &sub_label : pg_table->sub_labels) {
				auto distance_sub_label = LevenshteinDistance(label, sub_label.GetIdentifierName());
				if (distance_sub_label < min_distance) {
					min_distance = distance_sub_label;
					closest_label = sub_label.GetIdentifierName();
				}
			}
		}

		// If a close label match is found, suggest it in the error message
		if (min_distance < std::numeric_limits<size_t>::max() && error_not_found) {
			throw Exception(ExceptionType::INVALID, "Label '" + label + "' not found. Did you mean the " +
			                                            (is_vertex_table ? "vertex" : "edge") + " label '" +
			                                            StringUtil::Lower(closest_label) + "'?");
		}

		// If no match is found and error_not_found is true, throw an error
		if (error_not_found) {
			throw Exception(ExceptionType::INVALID, "Label '" + label + "' not found in the property graph for a " +
			                                            (is_vertex_table ? "vertex" : "edge") + " table.");
		}

		// Return nullptr if no match is found and error_not_found is false
		return nullptr;
	}

	//! Serializes a blob into a CreatePropertyGraphInfo
	void Serialize(Serializer &serializer) const override;
	//! Deserializes a blob back into a CreatePropertyGraphInfo
	static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

protected:
	void CopyGraph(CreatePropertyGraphInfo &result) const;
};

enum class PGQAlterGraphAction : uint8_t {
	ADD_VERTEX_TABLES,
	ADD_EDGE_TABLES,
	DROP_VERTEX_TABLES,
	DROP_EDGE_TABLES,
	ADD_LABEL,
	DROP_LABEL
};

//! ALTER PROPERTY GRAPH: the graph (property_graph_name, the qualified name) and the change. ADD ... TABLES carries
//! the new element tables in vertex_tables / edge_tables, as CREATE PROPERTY GRAPH does.
struct AlterPropertyGraphInfo : public CreatePropertyGraphInfo {
	explicit AlterPropertyGraphInfo(string property_graph_name) : CreatePropertyGraphInfo(std::move(property_graph_name)) {
	}

	PGQAlterGraphAction action = PGQAlterGraphAction::ADD_VERTEX_TABLES;
	//! DROP ... TABLES: the element tables, by alias; ALTER ... TABLE: the one table.
	vector<Identifier> element_tables;
	bool element_is_vertex = true;
	//! DROP ... CASCADE (the default, RESTRICT, refuses what would leave a dangling reference).
	bool cascade = false;
	//! ADD LABEL / DROP LABEL
	Identifier label;
	//! ADD LABEL: the label's PROPERTIES clause, held as a table's property fields (all_columns, except_columns,
	//! column_names, ...); no clause means all columns.
	shared_ptr<PropertyGraphTable> label_properties;

	unique_ptr<CreateInfo> Copy() const override;
};
} // namespace duckdb
