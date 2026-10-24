//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/core/utils/duckpgq_identifier.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"

namespace duckdb {

// Graph metadata and column names are Identifiers; the rewrite works on their raw names.
inline const string &PGQStr(const Identifier &identifier) {
	return identifier.GetIdentifierName();
}
inline const string &PGQStr(const string &value) {
	return value;
}
inline string PGQStr(const char *value) {
	return value;
}
inline Identifier PGQId(const Identifier &identifier) {
	return identifier;
}
inline Identifier PGQId(const string &value) {
	return Identifier(value);
}
inline Identifier PGQId(const char *value) {
	return Identifier(value);
}
//! Column references from raw or Identifier names: (column), (column, table) or a full name path.
template <class COLUMN>
inline unique_ptr<ColumnRefExpression> PGQColRef(const COLUMN &column) {
	return make_uniq<ColumnRefExpression>(PGQId(column));
}
template <class COLUMN, class TABLE>
inline unique_ptr<ColumnRefExpression> PGQColRef(const COLUMN &column, const TABLE &table) {
	return make_uniq<ColumnRefExpression>(PGQId(column), PGQId(table));
}
inline unique_ptr<ColumnRefExpression> PGQColRef(const vector<string> &names) {
	vector<Identifier> identifiers;
	for (auto &name : names) {
		identifiers.emplace_back(name);
	}
	return make_uniq<ColumnRefExpression>(std::move(identifiers));
}
inline unique_ptr<ColumnRefExpression> PGQColRef(vector<Identifier> names) {
	return make_uniq<ColumnRefExpression>(std::move(names));
}
inline vector<string> PGQStrs(const vector<Identifier> &identifiers) {
	vector<string> result;
	result.reserve(identifiers.size());
	for (auto &identifier : identifiers) {
		result.push_back(identifier.GetIdentifierName());
	}
	return result;
}

} // namespace duckdb
