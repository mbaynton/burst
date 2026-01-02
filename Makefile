# BURST Test Targets
#
# Configurable parallel test execution via CTEST_PARALLEL_LEVEL environment variable.
# Default: 4 jobs (suitable for most CI and local development environments)

# Default parallel job count (can be overridden via environment variable)
CTEST_PARALLEL_LEVEL ?= 4

.PHONY: test test-unit test-integration test-slow help

help:
	@echo "BURST Test Targets:"
	@echo "  make test              - Run all tests (~20-25min)"
	@echo "  make test-unit         - Run unit tests only (11 tests, ~5s)"
	@echo "  make test-integration  - Run fast integration tests (14 tests, ~3-5min with 4 parallel jobs)"
	@echo "  make test-slow         - Run slow E2E tests (5 tests, ~5-8min with 4 parallel jobs)"
	@echo ""
	@echo "Parallel execution: Default $(CTEST_PARALLEL_LEVEL) jobs (set via CTEST_PARALLEL_LEVEL)"
	@echo "Example: CTEST_PARALLEL_LEVEL=8 make test-integration"

test: test-unit test-integration

test-unit:
	@if [ ! -d build ]; then \
		echo "Error: build/ not found. Run 'mkdir build && cd build && cmake .. && make' first."; \
		exit 1; \
	fi
	@echo "Running unit tests..."
	cd build && ctest -LE integration --output-on-failure

test-integration:
	@if [ ! -d build ]; then \
		echo "Error: build/ not found. Run 'mkdir build && cd build && cmake .. && make' first."; \
		exit 1; \
	fi
	@echo "Caching sudo credentials for integration tests..."
	bash -c 'sudo -v || /bin/true'
	@echo "Running integration tests with $(CTEST_PARALLEL_LEVEL) parallel jobs..."
	cd build && ctest -L integration -LE slow --output-on-failure --parallel $(CTEST_PARALLEL_LEVEL)

test-slow:
	@if [ ! -d build ]; then \
		echo "Error: build/ not found. Run 'mkdir build && cd build && cmake .. && make' first."; \
		exit 1; \
	fi
	@echo "Running slow E2E tests with $(CTEST_PARALLEL_LEVEL) parallel jobs..."
	cd build && ctest -L slow --output-on-failure --parallel $(CTEST_PARALLEL_LEVEL)
