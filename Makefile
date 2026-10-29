# Frontend to the CMake build (CMakeLists.txt). `make` configures on first
# use, builds, and links ./funk to the binary, which the benchmarks and the
# README use. One build directory, build/: changing SANITIZE rebuilds it.
#
#   make                           build ./funk
#   make test                      run the test suite in parallel; ARGS go to ctest,
#                                  e.g. make test ARGS="-R gcdeep" or ARGS="-L ffi"
#   make SANITIZE=undefined test   the same with funk built under UBSan
#   make bench | bench-report      time funk against C; see bench/README.md
#   make install PREFIX=DIR        install funk into DIR/bin
#   make clap                      build build/clap/funk-midi.clap (FUNK_CLAP)
#   make clap-dev                  run the plugin's editor in a minimal host,
#                                  MIDI to the first port
#   make clean                     remove build/ and ./funk

CMAKE ?= cmake
BUILD_TYPE ?= RelWithDebInfo
SANITIZE ?=
PREFIX ?= /usr/local
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
ARGS ?=
BUILD := build
GENERATOR := $(if $(shell command -v ninja 2>/dev/null),Ninja,Unix Makefiles)

all: funk

$(BUILD)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD) -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

funk: $(BUILD)/CMakeCache.txt
	$(CMAKE) -S . -B $(BUILD) -DFUNK_SANITIZE=$(SANITIZE) >/dev/null
	$(CMAKE) --build $(BUILD)
	ln -sf $(BUILD)/funk funk

test: funk
	ctest --test-dir $(BUILD) -j $(JOBS) --output-on-failure $(ARGS)

bench: funk
	python3 bench/bench.py run

bench-report:
	python3 bench/bench.py report

clap: $(BUILD)/CMakeCache.txt
	$(CMAKE) -S . -B $(BUILD) -DFUNK_CLAP=ON >/dev/null
	$(CMAKE) --build $(BUILD)

# On macOS a .clap is a bundle; dlopen needs the binary inside it
ifeq ($(shell uname),Darwin)
CLAP_BIN = $(BUILD)/clap/funk-midi.clap/Contents/MacOS/funk-midi
else
CLAP_BIN = $(BUILD)/clap/funk-midi.clap
endif
clap-dev: clap
	./$(BUILD)/clap_dev_host $(CLAP_BIN)

install: funk
	$(CMAKE) --install $(BUILD) --prefix $(PREFIX)

clean:
	rm -rf build funk

.PHONY: all funk test bench bench-report clap clap-dev install clean
