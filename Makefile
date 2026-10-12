# Convenience wrapper over CMake presets. Everything here is a one-liner you
# can also run by hand; see README.md.
PRESET ?= release
JOBS   ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

.PHONY: help build test bench asan tsan clean format tidy run run replay
.DEFAULT_GOAL := help

help: ## List targets
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-10s %s\n", $$1, $$2}'

build: ## Configure + build (PRESET=release|debug|native)
	cmake --preset $(PRESET)
	cmake --build --preset $(PRESET) -j $(JOBS)

test: build ## Run the test suite
	ctest --preset $(PRESET)

bench: ## Build Release and run benchmarks
	$(MAKE) build PRESET=release
	./build/release/bench/limitbook_bench --benchmark_counters_tabular=true

asan: ## Tests under AddressSanitizer + UndefinedBehaviorSanitizer
	cmake --preset asan && cmake --build --preset asan -j $(JOBS) && ctest --preset asan

tsan: ## Tests under ThreadSanitizer
	cmake --preset tsan && cmake --build --preset tsan -j $(JOBS) && ctest --preset tsan

format: ## clang-format every source file in place
	find include tools tests bench -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

tidy: build ## clang-tidy over the compile database
	run-clang-tidy -p build/$(PRESET) -quiet

clean: ## Remove all build trees
	rm -rf build

run: build ## Generate a synthetic ITCH day and replay it with latency histograms
	./build/release/tools/limitbook-gen-synthetic --out build/synthetic.itch --messages 3000000 --symbols 8
	./build/release/tools/limitbook-replay build/synthetic.itch --validate --symbol NVDA

replay: build ## Replay a real NASDAQ file: make replay FILE=data/01302019.NASDAQ_ITCH50 SYMBOL=AAPL
	./build/release/tools/limitbook-replay $(FILE) --validate --symbol $(or $(SYMBOL),AAPL) --max-orders 8388608
