# This file is included by DuckDB's build system. It specifies which extension to load

# Local extensions needed to read frozen/live DuckLake artifacts in the native
# shell build. Keep these aligned with duckdb-wasm/extension_config_wasm.cmake.
duckdb_extension_load(httpfs
    SOURCE_DIR /Users/sturelygren/Development/pgq/duckdb-httpfs
)

duckdb_extension_load(ducklake
    SOURCE_DIR /Users/sturelygren/Development/pgq/ducklake
)

# Extension from this repo
duckdb_extension_load(duckpgq
    LOAD_TESTS
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
