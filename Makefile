.PHONY: build test ui-smoke conformance-smoke conformance-gba conformance-gba-tighten conformance-gba-swi-ab conformance-all

BUILD_DIR ?= build
ROM_ROOT ?= Test-Games
REPORT_PATH ?= tests/conformance_smoke_report.csv
PACKS ?= smoke

build:
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR) -j

test:
	ctest --test-dir $(BUILD_DIR) --output-on-failure

ui-smoke:
	./tests/ui_smoke.sh "$(ROM_ROOT)"

conformance-smoke:
	GBEMU_CONFORMANCE_PACKS="$(PACKS)" \
	./tests/run_seed_smoke_report.sh "$(ROM_ROOT)" "$(REPORT_PATH)"

conformance-gba:
	./tests/run_seed_gba_report.sh "$(ROM_ROOT)" tests/conformance_gba_report.csv

conformance-gba-tighten:
	GBEMU_CONFORMANCE_TIGHTEN_BASELINE=1 \
	./tests/run_seed_gba_report.sh "$(ROM_ROOT)" tests/conformance_gba_report.csv

conformance-gba-swi-ab:
	./tests/run_seed_gba_swi_ab_report.sh "$(ROM_ROOT)" tests

conformance-all:
	$(MAKE) conformance-smoke PACKS=all REPORT_PATH=tests/conformance_all_report.csv
