# This file is included by DuckDB's build system. It specifies which extension to load

# Optional local extensions, needed only to read frozen/live DuckLake artifacts
# in the native shell build. Point DUCKPGQ_LOCAL_EXT_DIR at the directory
# holding duckdb-httpfs/ and ducklake/ checkouts to include them:
#
#     export DUCKPGQ_LOCAL_EXT_DIR=/Users/sturelygren/Development/pgq
#
# Without it, the build produces duckpgq alone. That is what CI and a clean
# checkout do, and it is enough for the whole test suite -- every test under
# test/sql only declares "require duckpgq".
#
# Keep aligned with duckdb-wasm/extension_config_wasm.cmake.
set(DUCKPGQ_LOCAL_EXT_DIR "$ENV{DUCKPGQ_LOCAL_EXT_DIR}")

if(DUCKPGQ_LOCAL_EXT_DIR AND EXISTS "${DUCKPGQ_LOCAL_EXT_DIR}/duckdb-httpfs")
    duckdb_extension_load(httpfs
        SOURCE_DIR ${DUCKPGQ_LOCAL_EXT_DIR}/duckdb-httpfs
    )
endif()

if(DUCKPGQ_LOCAL_EXT_DIR AND EXISTS "${DUCKPGQ_LOCAL_EXT_DIR}/ducklake")
    duckdb_extension_load(ducklake
        SOURCE_DIR ${DUCKPGQ_LOCAL_EXT_DIR}/ducklake
    )
endif()

# Extension from this repo
duckdb_extension_load(duckpgq
    LOAD_TESTS
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
