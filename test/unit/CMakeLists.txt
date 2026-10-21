set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

cmake_minimum_required(VERSION 3.5)
project(feishu_bitable_unit_tests CXX)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Derive extension root from current location
get_filename_component(EXT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

include_directories(${EXT_ROOT}/duckdb/third_party/catch)
include_directories(${EXT_ROOT}/duckdb/third_party/yyjson/include)
include_directories(${EXT_ROOT}/duckdb/src/include)
include_directories(${EXT_ROOT}/src/include)

# Only self-contained translation units belong here: they must not pull in the
# DuckDB library (see stubs/duckdb_stubs.cpp).
set(UNIT_TEST_SOURCES
    test_main.cpp
    # Stubs for DuckDB types (avoids linking the full DuckDB library)
    stubs/duckdb_stubs.cpp
    # URL parser tests
    bitable/test_url_parser.cpp
    ${EXT_ROOT}/src/bitable/url_parser.cpp
    # Feishu API response parsing tests
    bitable/test_api_response.cpp
    ${EXT_ROOT}/src/bitable/api_response.cpp
    # Single-record lookup planning (H-3)
    bitable/test_record_id_lookup.cpp
    ${EXT_ROOT}/src/bitable/record_id_lookup.cpp
    # Search filter/sort pushdown planning (H-2)
    bitable/test_search_pushdown.cpp
    ${EXT_ROOT}/src/bitable/search_pushdown.cpp
    # Field type mapping tests
    bitable/test_field_type.cpp
    ${EXT_ROOT}/src/bitable/field_type.cpp
    # Record value coercion tests
    bitable/test_value_coercion.cpp
    ${EXT_ROOT}/src/bitable/value_coercion.cpp
    # Write-side value encoding tests (self-contained TU)
    bitable/test_write_value_encoding.cpp
    ${EXT_ROOT}/src/bitable/write_value_encoding.cpp
    # Phase 1 resilience, redaction, and bounded token-cache tests
    common/test_retry.cpp
    common/test_write_retry.cpp
    ${EXT_ROOT}/src/common/write_retry.cpp
    # Write-side state machine tests
    common/test_write_state_machine.cpp
    ${EXT_ROOT}/src/common/write_state_machine.cpp
    common/test_error.cpp
    auth/test_token_cache.cpp
    auth/test_token_provider.cpp
    ${EXT_ROOT}/src/common/retry.cpp
    ${EXT_ROOT}/src/common/error.cpp
    ${EXT_ROOT}/src/auth/token_cache.cpp
    ${EXT_ROOT}/src/auth/token_provider.cpp
    ${EXT_ROOT}/duckdb/third_party/yyjson/yyjson.cpp)

add_executable(unit_tests ${UNIT_TEST_SOURCES})
target_link_libraries(unit_tests PRIVATE OpenSSL::Crypto Threads::Threads)

enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)
