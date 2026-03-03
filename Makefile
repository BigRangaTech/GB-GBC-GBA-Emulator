.PHONY: build test conformance-smoke conformance-all

BUILD_DIR ?= build
ROM_ROOT ?= Test-Games
REPORT_PATH ?= tests/conformance_smoke_report.csv
PACKS ?= smoke

build:
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR) -j

test:
	ctest --test-dir $(BUILD_DIR) --output-on-failure

conformance-smoke:
	GBEMU_CONFORMANCE_PACKS="$(PACKS)" \
	./tests/run_seed_smoke_report.sh "$(ROM_ROOT)" "$(REPORT_PATH)"

conformance-all:
	$(MAKE) conformance-smoke PACKS=all REPORT_PATH=tests/conformance_all_report.csv
