# Convenience targets over CMake. Everything here also works with plain cmake.
#
#   make             build
#   make test        unit tests
#   make powerloss   1,100 simulated power cuts, 300 of them with I/O errors (~12 min)
#   make check       everything CI runs (without sanitizers)
#   make bench       reproduce the README's performance figures (~12 min)
#   make bench-quick the 1M-key half of that (~2 min)

BUILD ?= build
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

.PHONY: all build test crash powerloss fuzz check bench bench-quick clean

all: build

build:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build $(BUILD) -j $(JOBS)

test: build
	./$(BUILD)/ledgerkv_tests

crash: build
	./$(BUILD)/crash_test --iterations 100
	./$(BUILD)/crash_test --iterations 50 --threads 8

# The 1,100 simulated power cuts the README quotes: 200 with sync on, 600
# with sync off, and 300 with sync on plus an injected I/O error each.
powerloss: build
	./$(BUILD)/powerloss_test --iterations 200 --sync
	./$(BUILD)/powerloss_test --iterations 600
	./$(BUILD)/powerloss_test --iterations 300 --sync --faults

fuzz: build
	for t in log write_batch table db; do ./$(BUILD)/fuzz_$$t --iterations 20000 || exit 1; done

check: test crash powerloss fuzz

bench: build
	bench/run.sh full $(BUILD)

bench-quick: build
	bench/run.sh quick $(BUILD)

clean:
	rm -rf $(BUILD)
