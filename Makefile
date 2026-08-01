# Chaaya — convenience wrapper around CMake.
#
#   make          # configure (if needed) + build
#   make test     # build + ctest
#   make run      # REPL
#   make clean    # remove build directory

BUILD_DIR ?= build
BUILD_TYPE ?= Release
CMAKE ?= cmake
CTEST ?= ctest
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

CHAAYA := $(BUILD_DIR)/chaaya

.PHONY: all configure build test bootstrap-scheme run clean distclean install help

all: build

$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

configure: $(BUILD_DIR)/CMakeCache.txt

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

bootstrap-scheme: build
	bash tests/scheme/run-bootstrap.sh $(CHAAYA)

run: build
	$(CHAAYA)

install: build
	$(CMAKE) --install $(BUILD_DIR)

clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean 2>/dev/null || true

distclean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Targets:"
	@echo "  make / make build   Configure and build ($(BUILD_TYPE) in $(BUILD_DIR)/)"
	@echo "  make test           Build and run ctest"
	@echo "  make bootstrap-scheme  Run Kaappi-shaped bootstrap Scheme suites"
	@echo "  make run            Build and start the REPL"
	@echo "  make install        Install chaaya (DESTDIR / CMAKE_INSTALL_PREFIX apply)"
	@echo "  make clean          Remove built objects (keep CMake cache)"
	@echo "  make distclean      Delete $(BUILD_DIR)/"
	@echo ""
	@echo "Variables: BUILD_DIR=$(BUILD_DIR) BUILD_TYPE=$(BUILD_TYPE) JOBS=$(JOBS)"
	@echo "Example:   make BUILD_TYPE=Debug test"
