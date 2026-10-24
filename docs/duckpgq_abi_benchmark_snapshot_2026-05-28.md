# DuckPGQ ABI Benchmark Snapshot - 2026-05-28

This is a point-in-time smoke benchmark for the local DuckPGQ ABI rewrite work. It is intended as reproducible evidence for handoff/upstream discussion, not as a final performance claim.

## Environment

- DuckPGQ binary: `/Users/sturelygren/Development/pgq/duckpgq-extension/build/release/duckdb`
- DuckLake snapshot: `s3://datalake/frozen_20260527_233822.ducklake`
- Graph: `maintenance_graph`
- ABI path rows verified before benchmarking:
  - `edge`: 64
  - `fixed_path`: 68
  - `tc_chain`: 1
  - `tc_witness`: 1
  - `transitive_closure`: 1
  - `branch_summary`: 1
  - direct links: 48
- Verification sample counts:
  - `TAG -HAS_WORKORDER-> WORK_ORDER`: 213,915 rows
  - `tc_chain`: 1,126,752 rows

## Command

```bash
env PGHOST=127.0.0.1 \
    PGPORT=5436 \
    PGUSER=postgres \
    PGPASSWORD=postgres \
    PGDATABASE=s3lake \
    DUCKDB_S3_USE_SSL=FALSE \
    DUCKDB_S3_ENDPOINT=minio-api.test:8080 \
    AWS_REGION=us-east-1 \
    AWS_ACCESS_KEY_ID=minioadmin \
    AWS_SECRET_ACCESS_KEY=minioadmin123 \
    URL_STYLE=path \
    BUCKET_NAME=datalake \
  uv run python scripts/benchmark_duckpgq.py \
    --duckdb /Users/sturelygren/Development/pgq/duckpgq-extension/build/release/duckdb \
    --frozen-ducklake s3://datalake/frozen_20260527_233822.ducklake \
    --no-install-extensions \
    --iterations 1 \
    --warmup-runs 1
```

`profile_median_s` is DuckDB's profiled query latency after one same-connection warmup. `wall_median_s` includes process startup, extension/lake attach, graph registration, sample creation, warmup execution, and the profiled query.

## Results

| query | iterations | profile_median_s | wall_median_s |
| --- | ---: | ---: | ---: |
| `edge_has_workorder` | 1 | 0.014625 | 1.046503 |
| `edge_tag_engineering_core_attributes` | 1 | 0.094037 | 1.468726 |
| `direct_link_dot_projection` | 1 | 0.028875 | 1.105009 |
| `edge_tag_engineering_attributes` | 1 | 0.138745 | 2.996061 |
| `edge_tag_maintenance_attributes` | 1 | 0.061952 | 1.765034 |
| `edge_tag_all_attributes` | 1 | 0.162424 | 3.822671 |
| `edge_tag_attributes_domain_filter` | 1 | 0.136385 | 2.348830 |
| `edge_tag_all_attributes_egr_40pa001a` | 1 | 0.057481 | 2.624519 |
| `explain_attribute_covering_path` | 1 | 0.013595 | 0.879034 |
| `tc_chain_one_row_per_step` | 1 | 1.048870 | 3.008349 |
| `tc_chain_one_row_per_step_literal_egr` | 3 | 0.145222 | 1.184919 |
| `branch_shared_vertex` | 1 | 0.026838 | 1.100322 |
| `context_expansion_excluding_parent` | 1 | 0.286295 | 4.585328 |
| `explain_parent_path` | 1 | 0.015756 | 0.881839 |

## Interpretation

- One-hop ABI lookups, branch summary use, direct-link projection, and metadata-only explain are comfortably interactive after metadata/lake setup.
- Consolidated attribute-domain queries are now covered by ABI target properties and return `attributes_json` as JSON, avoiding a late vertex join for the common attribute columns.
- Bounded context expansion is still interactive but heavier because it expands across metadata-selected edge tables and returns path lists.
- Recursive parent `ONE ROW PER STEP` is fast when the source key is literal: `EGR-40PA001A` profiles around 145 ms on this snapshot.
- The scalar-subquery benchmark form still profiles around one second because the generated `c.v0 = (SELECT ...)` predicate does not prune the `tc_chain` table as well as a literal source key. This is now the clearest remaining performance target for verification/sample-driven queries.
