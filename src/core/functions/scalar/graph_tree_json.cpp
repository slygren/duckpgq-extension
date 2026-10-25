#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckpgq/common.hpp"

#include <duckpgq/core/functions/scalar.hpp>

#include <algorithm>
#include <cctype>
#include <map>

namespace duckdb {

namespace {

struct GraphJsonPath {
	vector<string> vertex_keys;
	vector<string> vertex_labels;
	vector<string> edge_labels;
	vector<string> vertex_properties;
};

struct GraphJsonNode {
	string label;
	string key;
	string properties;
	map<string, vector<unique_ptr<GraphJsonNode>>> relationships;
};

static bool IsListValue(const Value &value) {
	return !value.IsNull() && value.type().id() == LogicalTypeId::LIST;
}

static string ValueToString(const Value &value) {
	if (value.IsNull()) {
		return "null";
	}
	return value.DefaultCastAs(LogicalType::VARCHAR).GetValue<string>();
}

static string JsonEscape(const string &input) {
	string result;
	result.reserve(input.size() + 8);
	for (const auto c : input) {
		switch (c) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buffer[7];
				snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
				result += buffer;
			} else {
				result += c;
			}
			break;
		}
	}
	return result;
}

static string JsonString(const string &input) {
	return "\"" + JsonEscape(input) + "\"";
}

static bool LooksLikeJsonObject(const string &input) {
	idx_t pos = 0;
	while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
		pos++;
	}
	return pos < input.size() && input[pos] == '{';
}

static string JsonObjectOrString(const string &input) {
	if (input.empty() || input == "null") {
		return "{}";
	}
	return LooksLikeJsonObject(input) ? input : JsonString(input);
}

static vector<string> ExtractStringList(const Value &value, const string &argument_name) {
	if (!IsListValue(value)) {
		throw InvalidInputException("%s must be a LIST", argument_name);
	}
	vector<string> result;
	auto &children = ListValue::GetChildren(value);
	result.reserve(children.size());
	for (const auto &child : children) {
		result.push_back(ValueToString(child));
	}
	return result;
}

static GraphJsonPath ExtractPath(const Value &vertices, const Value &vertex_labels, const Value &edge_labels) {
	GraphJsonPath path;
	path.vertex_keys = ExtractStringList(vertices, "vertices");
	path.vertex_labels = ExtractStringList(vertex_labels, "vertex_labels");
	path.edge_labels = ExtractStringList(edge_labels, "edge_labels");

	if (path.vertex_keys.size() != path.vertex_labels.size()) {
		throw InvalidInputException("vertices and vertex_labels must have the same length");
	}
	if (!path.vertex_keys.empty() && path.edge_labels.size() + 1 != path.vertex_keys.size()) {
		throw InvalidInputException("edge_labels length must be one less than vertices length");
	}
	return path;
}

static GraphJsonPath ExtractPath(const Value &vertices, const Value &vertex_labels, const Value &edge_labels,
                                 const Value &vertex_properties) {
	auto path = ExtractPath(vertices, vertex_labels, edge_labels);
	path.vertex_properties = ExtractStringList(vertex_properties, "vertex_properties");
	if (path.vertex_properties.size() != path.vertex_keys.size()) {
		throw InvalidInputException("vertex_properties length must match vertices length");
	}
	return path;
}

static vector<GraphJsonPath> ExtractPaths(const Value &vertices, const Value &vertex_labels, const Value &edge_labels,
                                          const Value *vertex_properties = nullptr) {
	vector<GraphJsonPath> paths;
	if (!IsListValue(vertices)) {
		throw InvalidInputException("vertices must be a LIST or LIST of LISTs");
	}

	auto &vertex_children = ListValue::GetChildren(vertices);
	if (vertex_children.empty()) {
		return paths;
	}

	if (!IsListValue(vertex_children[0])) {
		paths.push_back(vertex_properties ? ExtractPath(vertices, vertex_labels, edge_labels, *vertex_properties)
		                                  : ExtractPath(vertices, vertex_labels, edge_labels));
		return paths;
	}

	if (!IsListValue(vertex_labels) || !IsListValue(edge_labels)) {
		throw InvalidInputException("multi-path graph_tree_json expects all arguments to be LISTs of LISTs");
	}

	auto &vertex_label_children = ListValue::GetChildren(vertex_labels);
	auto &edge_label_children = ListValue::GetChildren(edge_labels);
	if (vertex_children.size() != vertex_label_children.size() || vertex_children.size() != edge_label_children.size()) {
		throw InvalidInputException("multi-path graph_tree_json arguments must contain the same number of paths");
	}
	const vector<Value> *vertex_property_children = nullptr;
	if (vertex_properties) {
		if (!IsListValue(*vertex_properties)) {
			throw InvalidInputException("multi-path graph_tree_json vertex_properties must be a LIST of LISTs");
		}
		vertex_property_children = &ListValue::GetChildren(*vertex_properties);
		if (vertex_children.size() != vertex_property_children->size()) {
			throw InvalidInputException("multi-path graph_tree_json vertex_properties must contain the same number of paths");
		}
	}

	paths.reserve(vertex_children.size());
	for (idx_t i = 0; i < vertex_children.size(); i++) {
		auto path = vertex_property_children
		                ? ExtractPath(vertex_children[i], vertex_label_children[i], edge_label_children[i],
		                              (*vertex_property_children)[i])
		                : ExtractPath(vertex_children[i], vertex_label_children[i], edge_label_children[i]);
		if (!path.vertex_keys.empty()) {
			paths.push_back(std::move(path));
		}
	}
	return paths;
}

static GraphJsonNode &FindOrCreateChild(GraphJsonNode &node, const string &edge_label, const string &label,
                                        const string &key, const string &properties) {
	auto &children = node.relationships[edge_label];
	for (auto &child : children) {
		if (child->label == label && child->key == key) {
			if (child->properties.empty() && !properties.empty()) {
				child->properties = properties;
			}
			return *child;
		}
	}
	auto child = make_uniq<GraphJsonNode>();
	child->label = label;
	child->key = key;
	child->properties = properties;
	children.push_back(std::move(child));
	return *children.back();
}

static void AddPath(vector<unique_ptr<GraphJsonNode>> &roots, const GraphJsonPath &path) {
	if (path.vertex_keys.empty()) {
		return;
	}

	GraphJsonNode *current = nullptr;
	for (auto &root : roots) {
		if (root->label == path.vertex_labels[0] && root->key == path.vertex_keys[0]) {
			current = root.get();
			break;
		}
	}
	if (!current) {
		auto root = make_uniq<GraphJsonNode>();
		root->label = path.vertex_labels[0];
		root->key = path.vertex_keys[0];
		if (!path.vertex_properties.empty()) {
			root->properties = path.vertex_properties[0];
		}
		roots.push_back(std::move(root));
		current = roots.back().get();
	} else if (current->properties.empty() && !path.vertex_properties.empty()) {
		current->properties = path.vertex_properties[0];
	}

	for (idx_t i = 1; i < path.vertex_keys.size(); i++) {
		const string properties = path.vertex_properties.empty() ? string() : path.vertex_properties[i];
		current = &FindOrCreateChild(*current, path.edge_labels[i - 1], path.vertex_labels[i], path.vertex_keys[i],
		                             properties);
	}
}

static void SortNode(GraphJsonNode &node) {
	for (auto &entry : node.relationships) {
		auto &children = entry.second;
		sort(children.begin(), children.end(), [](const unique_ptr<GraphJsonNode> &left,
		                                          const unique_ptr<GraphJsonNode> &right) {
			if (left->label != right->label) {
				return left->label < right->label;
			}
			return left->key < right->key;
		});
		for (auto &child : children) {
			SortNode(*child);
		}
	}
}

static string NodeToJson(const GraphJsonNode &node) {
	string json = "{";
	json += "\"label\":" + JsonString(node.label);
	json += ",\"key\":" + JsonString(node.key);
	if (!node.properties.empty()) {
		json += ",\"properties\":" + JsonObjectOrString(node.properties);
	}
	if (!node.relationships.empty()) {
		json += ",\"relationships\":{";
		bool first_relationship = true;
		for (const auto &relationship : node.relationships) {
			if (!first_relationship) {
				json += ",";
			}
			first_relationship = false;
			json += JsonString(relationship.first) + ":[";
			bool first_child = true;
			for (const auto &child : relationship.second) {
				if (!first_child) {
					json += ",";
				}
				first_child = false;
				json += NodeToJson(*child);
			}
			json += "]";
		}
		json += "}";
	}
	json += "}";
	return json;
}

static string BuildTreeJson(vector<GraphJsonPath> paths) {
	vector<unique_ptr<GraphJsonNode>> roots;
	for (const auto &path : paths) {
		AddPath(roots, path);
	}

	sort(roots.begin(), roots.end(), [](const unique_ptr<GraphJsonNode> &left, const unique_ptr<GraphJsonNode> &right) {
		if (left->label != right->label) {
			return left->label < right->label;
		}
		return left->key < right->key;
	});
	for (auto &root : roots) {
		SortNode(*root);
	}

	if (roots.empty()) {
		return "null";
	}
	if (roots.size() == 1) {
		return NodeToJson(*roots[0]);
	}

	string json = "{\"roots\":[";
	for (idx_t i = 0; i < roots.size(); i++) {
		if (i > 0) {
			json += ",";
		}
		json += NodeToJson(*roots[i]);
	}
	json += "]}";
	return json;
}

static void GraphTreeJsonFunction(DataChunk &args, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &result_validity = FlatVector::ValidityMutable(result);

	for (idx_t row = 0; row < args.size(); row++) {
		auto vertices = args.GetValue(0, row);
		auto vertex_labels = args.GetValue(1, row);
		auto edge_labels = args.GetValue(2, row);
		if (vertices.IsNull() || vertex_labels.IsNull() || edge_labels.IsNull()) {
			result_validity.SetInvalid(row);
			continue;
		}
		Value vertex_properties;
		Value *vertex_properties_ptr = nullptr;
		if (args.ColumnCount() > 3) {
			vertex_properties = args.GetValue(3, row);
			if (vertex_properties.IsNull()) {
				result_validity.SetInvalid(row);
				continue;
			}
			vertex_properties_ptr = &vertex_properties;
		}
		auto paths = ExtractPaths(vertices, vertex_labels, edge_labels, vertex_properties_ptr);
		auto json = BuildTreeJson(std::move(paths));
		if (json == "null") {
			result_validity.SetInvalid(row);
			continue;
		}
		result.SetValue(row, Value(json));
	}
}

} // namespace

void CoreScalarFunctions::RegisterGraphTreeJsonScalarFunction(ExtensionLoader &loader) {
	auto label_list = LogicalType::LIST(LogicalType::VARCHAR);
	vector<LogicalType> key_types = {LogicalType::BIGINT, LogicalType::INTEGER, LogicalType::VARCHAR};

	ScalarFunctionSet graph_tree("graph_tree_json");
	for (const auto &key_type : key_types) {
		graph_tree.AddFunction(ScalarFunction({LogicalType::LIST(LogicalType::LIST(key_type)),
		                                       LogicalType::LIST(label_list), LogicalType::LIST(label_list)},
		                                      LogicalType::VARCHAR, GraphTreeJsonFunction));
		graph_tree.AddFunction(ScalarFunction({LogicalType::LIST(LogicalType::LIST(key_type)),
		                                       LogicalType::LIST(label_list), LogicalType::LIST(label_list),
		                                       LogicalType::LIST(label_list)},
		                                      LogicalType::VARCHAR, GraphTreeJsonFunction));
		graph_tree.AddFunction(ScalarFunction({LogicalType::LIST(key_type), label_list, label_list},
		                                      LogicalType::VARCHAR, GraphTreeJsonFunction));
		graph_tree.AddFunction(ScalarFunction({LogicalType::LIST(key_type), label_list, label_list, label_list},
		                                      LogicalType::VARCHAR, GraphTreeJsonFunction));
	}
	loader.RegisterFunction(Fallible(graph_tree));

	ScalarFunctionSet path_tree("path_tree_json");
	for (const auto &key_type : key_types) {
		path_tree.AddFunction(ScalarFunction({LogicalType::LIST(key_type), label_list, label_list}, LogicalType::VARCHAR,
		                                     GraphTreeJsonFunction));
	}
	loader.RegisterFunction(Fallible(path_tree));
}

} // namespace duckdb
