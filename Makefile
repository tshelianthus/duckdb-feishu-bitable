PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Cap compile parallelism at 4.  Callers (WriteTests CI) may lower this to
# cut RAM; values other than 1–4 are ignored so an inherited -j cannot
# silently spawn a larger compiler pool.
override BUILD_WORKERS := $(if $(filter 1 2 3 4,$(BUILD_WORKERS)),$(BUILD_WORKERS),4)
export CMAKE_BUILD_PARALLEL_LEVEL := $(BUILD_WORKERS)
export NINJAFLAGS := -j$(BUILD_WORKERS)
MAKEFLAGS += -j$(BUILD_WORKERS)

# Configuration of extension
EXT_NAME=feishu_bitable
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Auto-source .env.local for SQLLogicTest integration tests (no-op if file absent).
# Keep Debug and Release on the same credential-loading path. Capture the runner
# output and redact every injected value before writing it to the terminal: the
# SQLLogicTest runner includes the expanded query in failure diagnostics.
define RUN_SQLLOGICTEST
	@bash -c 'set +x; set -a; if [ -f .env.local ]; then source .env.local; fi; set +a; \
		tmp=$$(mktemp); trap '\''rm -f "$$tmp"'\'' EXIT; \
		./build/$(1)/$(TEST_PATH) "$(TESTS_BASE_DIRECTORY)*" >"$$tmp" 2>&1; status=$$?; \
		while IFS= read -r line || [ -n "$$line" ]; do \
			line=$${line//"$${FEISHU_TEST_APP_ID}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_TEST_APP_SECRET}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_TEST_APP_TOKEN}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_TEST_URL}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_WRITE_TEST_APP_TOKEN}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_WRITE_TEST_TABLE_ID}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_WRITE_TEST_APP_ID}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_WRITE_TEST_APP_SECRET}"/[REDACTED]}; \
			printf "%s\\n" "$$line"; \
		done <"$$tmp"; exit "$$status"'
endef

# Run only the protected, real-API integration suite. Credentials are supplied
# by the caller environment (or .env.local through RUN_SQLLOGICTEST); they are
# never accepted as command-line arguments or echoed by this target.
define RUN_ONLINE_SQLLOGICTEST
	@bash -c 'set +x; set -a; if [ -f .env.local ]; then source .env.local; fi; set +a; \
		tmp=$$(mktemp); trap '\''rm -f "$$tmp"'\'' EXIT; \
		./build/$(1)/$(TEST_PATH) "test/sql/integration_first_table.test" >"$$tmp" 2>&1; status=$$?; \
		if [ "$$status" -eq 0 ]; then ./build/$(1)/$(TEST_PATH) "test/sql/integration_pushdown.test" >>"$$tmp" 2>&1; status=$$?; fi; \
		if [ "$$status" -eq 0 ]; then ./build/$(1)/$(TEST_PATH) "test/sql/integration_search_pushdown.test" >>"$$tmp" 2>&1; status=$$?; fi; \
		while IFS= read -r line || [ -n "$$line" ]; do \
			line=$${line//"$${FEISHU_TEST_APP_ID}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_TEST_APP_SECRET}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_TEST_APP_TOKEN}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_TEST_URL}"/[REDACTED]}; \
			printf "%s\\n" "$$line"; \
		done <"$$tmp"; exit "$$status"'
endef

test_debug_internal:
	$(call RUN_SQLLOGICTEST,debug)

test_release_internal:
	$(call RUN_SQLLOGICTEST,release)

test_sql_online_debug: debug
	$(call RUN_ONLINE_SQLLOGICTEST,debug)

test_sql_online_release: release
	$(call RUN_ONLINE_SQLLOGICTEST,release)

# Protected write-side suite. It targets a DEDICATED write base
# (.specs/04_TESTING_SPEC.md section 6) and is only ever invoked from the
# manually triggered WriteTests workflow; public CI never receives these
# variables, so the file skips as a whole there.
define RUN_WRITE_SQLLOGICTEST
	@bash -c 'set +x; set -a; if [ -f .env.local ]; then source .env.local; fi; set +a; \
		tmp=$$(mktemp); trap '\''rm -f "$$tmp"'\'' EXIT; \
		./build/$(1)/$(TEST_PATH) "test/sql/integration_copy_writeback.test" >"$$tmp" 2>&1; status=$$?; \
		while IFS= read -r line || [ -n "$$line" ]; do \
			line=$${line//"$${FEISHU_WRITE_TEST_APP_TOKEN}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_WRITE_TEST_TABLE_ID}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_WRITE_TEST_APP_ID}"/[REDACTED]}; \
			line=$${line//"$${FEISHU_WRITE_TEST_APP_SECRET}"/[REDACTED]}; \
			printf "%s\\n" "$$line"; \
		done <"$$tmp"; exit "$$status"'
endef

test_sql_write_debug: debug
	$(call RUN_WRITE_SQLLOGICTEST,debug)

# Custom test targets
.PHONY: test_unit test_unit_build test_all test_all_debug test_sql_online_debug test_sql_online_release clean_unit_tests

# Build the standalone unit tests (no full DuckDB build required)
test_unit_build:
	mkdir -p build/unit_tests
	cmake -S test/unit -B build/unit_tests
	cmake --build build/unit_tests --parallel $(BUILD_WORKERS)

# Run the unit tests (builds if needed)
test_unit: test_unit_build
	./build/unit_tests/unit_tests

# Run every test tier
test_all: test_unit test_release
test_all_debug: test_unit test_debug

clean_unit_tests:
	rm -rf build/unit_tests
