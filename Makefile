# Frontend to the CMake build (CMakeLists.txt). `make` configures on first
# use, builds, and links ./funk to the binary, which the benchmarks and the
# README use. A sanitizer build has its own directory and leaves ./funk alone.
#
#   make                           build ./funk
#   make test                      run the test suite in parallel; ARGS go to ctest,
#                                  e.g. make test ARGS="-R gcdeep" or ARGS="-L ffi"
#   make SANITIZE=undefined test   the same with funk built under UBSan, in build-undefined/
#   make bench | bench-report      time funk against C; see bench/README.md
#   make install PREFIX=DIR        install funk into DIR/bin
#   make clean                     remove build directories and ./funk

CMAKE ?= cmake
BUILD_TYPE ?= RelWithDebInfo
SANITIZE ?=
PREFIX ?= /usr/local
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
ARGS ?=
comma := ,
BUILD := build$(if $(SANITIZE),-$(subst $(comma),-,$(SANITIZE)))
GENERATOR := $(if $(shell command -v ninja 2>/dev/null),Ninja,Unix Makefiles)

all: funk

$(BUILD)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD) -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DFUNK_SANITIZE=$(SANITIZE)

funk: $(BUILD)/CMakeCache.txt
	$(CMAKE) --build $(BUILD)
	$(if $(SANITIZE),,ln -sf $(BUILD)/funk funk)

test: funk
	ctest --test-dir $(BUILD) -j $(JOBS) --output-on-failure $(ARGS)

bench: funk
	python3 bench/bench.py run

bench-report:
	python3 bench/bench.py report

install: funk
	$(CMAKE) --install $(BUILD) --prefix $(PREFIX)

clean:
	rm -rf build build-* funk

.PHONY: all funk test bench bench-report install clean
