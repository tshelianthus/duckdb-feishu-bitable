cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME feishu_bitable)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package.
find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# DuckDB 1.5.5's bundled httplib retries SSLPeerCouldBeClosed_ inside one
# request, even after a POST body may have been sent. Generate a private copy
# with that implicit replay removed; read-side retry policy remains in
# api_client.cpp, while write-side calls can conservatively report an unknown
# result. The pinned source is never modified in the DuckDB submodule.
set(FEISHU_HTTPLIB_DIR "${CMAKE_CURRENT_BINARY_DIR}/feishu_httplib")
file(MAKE_DIRECTORY "${FEISHU_HTTPLIB_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib/httplib.hpp
          -DOUTPUT=${FEISHU_HTTPLIB_DIR}/feishu_httplib.hpp
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/prepare_httplib.cmake
  RESULT_VARIABLE FEISHU_HTTPLIB_RESULT)
if(NOT FEISHU_HTTPLIB_RESULT EQUAL 0)
  message(FATAL_ERROR "Could not prepare the private httplib header")
endif()

set(EXTENSION_SOURCES
    src/feishu_bitable_extension.cpp
    src/auth/token_provider.cpp
    src/auth/token_cache.cpp
    src/bitable/api_client.cpp
    src/bitable/api_response.cpp
    src/bitable/tables_function.cpp
    src/bitable/fields_function.cpp
    src/bitable/scan_function.cpp
    src/bitable/record_id_lookup.cpp
    src/bitable/search_pushdown.cpp
    src/bitable/copy_function.cpp
    src/bitable/field_type.cpp
    src/bitable/type_mapping.cpp
    src/bitable/url_parser.cpp
    src/bitable/value_coercion.cpp
    src/bitable/write_value_encoding.cpp
    src/secrets/feishu_secret.cpp
    src/common/error.cpp
    src/common/retry.cpp
    src/common/write_retry.cpp
    src/common/write_state_machine.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link OpenSSL in both the static library and the loadable extension
target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PRIVATE cxx_std_17)
target_include_directories(${EXTENSION_NAME} PRIVATE "${FEISHU_HTTPLIB_DIR}")
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE "${FEISHU_HTTPLIB_DIR}")
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto duckdb_yyjson)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto duckdb_yyjson)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
