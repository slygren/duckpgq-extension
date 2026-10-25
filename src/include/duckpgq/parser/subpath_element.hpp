
#pragma once

#include "duckdb/parser/parsed_expression.hpp"
#include "duckpgq/parser/path_reference.hpp"

namespace duckdb {

enum class PGQPathMode : uint8_t { NONE, WALK, SIMPLE, TRAIL, ACYCLIC };

class SubPath : public PathReference {
public:
	vector<unique_ptr<PathReference>> path_list;

	unique_ptr<ParsedExpression> where_clause;

	PGQPathMode path_mode;

	bool single_bind;
	int64_t lower, upper;

	string path_variable;

	//! COST expression of a CHEAPEST path, and the cost used where it evaluates to NULL (when has_default).
	unique_ptr<ParsedExpression> cost_expr;
	double default_value = 1.0;
	bool has_default = false;
	//! Set during translation on the quantified edge of an ANY / ALL CHEAPEST pattern whose COST varies by edge:
	//! its paths are chosen by that cost, not by hop count.
	bool cheapest = false;
public:
	explicit SubPath(PGQPathReferenceType path_reference_type) : PathReference(path_reference_type) {
		path_mode = PGQPathMode::NONE;
		single_bind = true;
		lower = 1;
		upper = 1;
	}

	string ToString() const override;

	unique_ptr<PathReference> Copy() override;

	bool Equals(const PathReference *other_p) const override;

	void Serialize(Serializer &serializer) const override;

	static unique_ptr<PathReference> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
