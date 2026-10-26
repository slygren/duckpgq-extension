#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include <duckpgq/parser/parsed_data/create_property_graph_info.hpp>

namespace duckdb {

CreatePropertyGraphInfo::CreatePropertyGraphInfo() : CreateInfo(CatalogType::INVALID) {
}

CreatePropertyGraphInfo::CreatePropertyGraphInfo(string property_graph_name)
    : CreateInfo(CatalogType::INVALID), property_graph_name(std::move(property_graph_name)) {
}

unique_ptr<CreateInfo> CreatePropertyGraphInfo::Copy() const {
	auto result = make_uniq<CreatePropertyGraphInfo>(property_graph_name);
	CopyGraph(*result);
	return std::move(result);
}

unique_ptr<CreateInfo> AlterPropertyGraphInfo::Copy() const {
	auto result = make_uniq<AlterPropertyGraphInfo>(property_graph_name);
	CopyGraph(*result);
	result->action = action;
	result->element_tables = element_tables;
	result->element_is_vertex = element_is_vertex;
	result->cascade = cascade;
	result->label = label;
	result->label_properties = label_properties ? label_properties->Copy() : nullptr;
	return std::move(result);
}

void CreatePropertyGraphInfo::CopyGraph(CreatePropertyGraphInfo &result_p) const {
	auto result = &result_p;
	CopyProperties(*result);
	result->verified = verified;
	result->abi_options = abi_options;
	result->abi_disabled = abi_disabled;
	// The copies' edges point at the copies of their vertex tables.
	unordered_map<const PropertyGraphTable *, shared_ptr<PropertyGraphTable>> copies;

	for (auto &vertex_table : vertex_tables) {
		auto copied_vertex_table = vertex_table->Copy();
		for (auto &label : copied_vertex_table->sub_labels) {
			result->label_map[label.GetIdentifierName()] = copied_vertex_table;
		}
		for (auto &label : copied_vertex_table->StaticLabels()) {
			result->label_map[label.GetIdentifierName()] = copied_vertex_table;
		}
		copies[vertex_table.get()] = copied_vertex_table;
		result->vertex_tables.push_back(std::move(copied_vertex_table));
	}
	for (auto &edge_table : edge_tables) {
		auto copied_edge_table = edge_table->Copy();
		for (auto &label : copied_edge_table->sub_labels) {
			result->label_map[label.GetIdentifierName()] = copied_edge_table;
		}
		for (auto &label : copied_edge_table->StaticLabels()) {
			result->label_map[label.GetIdentifierName()] = copied_edge_table;
		}
		auto source = copies.find(copied_edge_table->source_pg_table.get());
		if (source != copies.end()) {
			copied_edge_table->source_pg_table = source->second;
		}
		auto destination = copies.find(copied_edge_table->destination_pg_table.get());
		if (destination != copies.end()) {
			copied_edge_table->destination_pg_table = destination->second;
		}
		result->edge_tables.push_back(std::move(copied_edge_table));
	}
}

static shared_ptr<PropertyGraphTable> FindVertexTable(const CreatePropertyGraphInfo &info, const Identifier &catalog,
                                                      const Identifier &schema, const Identifier &name) {
	for (auto &table : info.vertex_tables) {
		if ((!catalog.empty() && table->catalog_name != catalog) || (!schema.empty() && table->schema_name != schema)) {
			continue;
		}
		if (table->table_name == name || (!table->table_name_alias.empty() && table->table_name_alias == name)) {
			return table;
		}
	}
	return nullptr;
}

static void ResolveReference(const shared_ptr<PropertyGraphTable> &vertex, Identifier &catalog, Identifier &schema,
                             Identifier &reference) {
	if (!vertex) {
		return; // left as written so the binder can report the unknown reference
	}
	catalog = vertex->catalog_name;
	schema = vertex->schema_name;
	reference = vertex->table_name;
}

void CreatePropertyGraphInfo::ResolveEdgeReferences() {
	for (auto &table : edge_tables) {
		if (!table->source_pg_table) {
			table->source_pg_table =
			    FindVertexTable(*this, table->source_catalog, table->source_schema, table->source_reference);
		}
		if (!table->destination_pg_table) {
			table->destination_pg_table =
			    FindVertexTable(*this, table->destination_catalog, table->destination_schema, table->destination_reference);
		}
		// A reference may use the vertex table's statement alias; the rest of the extension compares physical names.
		ResolveReference(table->source_pg_table, table->source_catalog, table->source_schema, table->source_reference);
		ResolveReference(table->destination_pg_table, table->destination_catalog, table->destination_schema,
		                 table->destination_reference);
	}
}

void CreatePropertyGraphInfo::RebuildLabelMap() {
	label_map.clear();
	auto register_table = [&](const shared_ptr<PropertyGraphTable> &table) {
		auto register_label = [&](const Identifier &label) {
			auto name = label.GetIdentifierName();
			if (!label_map.emplace(name, table).second) {
				throw ConstraintException("Label %s is not unique, make sure all labels are unique",
				                          StringUtil::Lower(name));
			}
		};
		for (auto &label : table->StaticLabels()) {
			register_label(label);
		}
		for (auto &label : table->sub_labels) {
			register_label(label);
		}
	};
	for (auto &table : vertex_tables) {
		register_table(table);
	}
	for (auto &table : edge_tables) {
		register_table(table);
	}
}

void CreatePropertyGraphInfo::Serialize(Serializer &serializer) const {
	serializer.WriteProperty<string>(100, "property_graph_name", property_graph_name);
	serializer.WriteList(101, "vertex_tables", vertex_tables.size(), [&](Serializer::List &list, idx_t i) {
		auto &entry = vertex_tables[i];
		list.WriteObject([&](Serializer &obj) { entry->Serialize(obj); });
	});
	serializer.WriteList(102, "edge_tables", edge_tables.size(), [&](Serializer::List &list, idx_t i) {
		auto &entry = edge_tables[i];
		list.WriteObject([&](Serializer &obj) { entry->Serialize(obj); });
	});
	serializer.WriteProperty(103, "label_map", label_map);
	serializer.WritePropertyWithDefault(104, "verified", verified, false);
	serializer.WritePropertyWithDefault(105, "abi_options", abi_options, string());
}

unique_ptr<CreateInfo> CreatePropertyGraphInfo::Deserialize(Deserializer &deserializer) {
	auto result = make_uniq<CreatePropertyGraphInfo>();
	deserializer.ReadProperty<string>(100, "property_graph_name", result->property_graph_name);

	deserializer.ReadList(101, "vertex_tables", [&](Deserializer::List &list, idx_t i) {
		list.ReadObject([&](Deserializer &obj) { result->vertex_tables[i]->Deserialize(obj); });
	});

	deserializer.ReadProperty(103, "label_map", result->label_map);
	deserializer.ReadPropertyWithExplicitDefault(104, "verified", result->verified, false);
	deserializer.ReadPropertyWithExplicitDefault(105, "abi_options", result->abi_options, string());
	return std::move(result);
}

} // namespace duckdb
