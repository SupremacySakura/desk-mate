SHELL := /bin/bash

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR := $(ROOT)/build
MODULE_CACHE := $(BUILD_DIR)/module-cache
SOURCES := $(wildcard $(ROOT)/Sources/Gate0Probe/*.swift)
BIN_DIR := $(BUILD_DIR)
OBJC_SOURCE := $(ROOT)/Sources/Gate0Probe/Gate0VirtualDisplay.m
OBJC_OBJECT := $(BUILD_DIR)/Gate0VirtualDisplay.o
BRIDGE_HEADER := $(ROOT)/Sources/Gate0Probe/include/Gate0VirtualDisplay.h
BIN := $(BUILD_DIR)/gate0-probe
APP := $(BUILD_DIR)/Gate0Probe.app
SDK := $(shell xcrun --sdk macosx --show-sdk-path)
SWIFTC := xcrun --sdk macosx swiftc
SWIFT_TARGET := $(shell uname -m)-apple-macosx15.0
REFERENCE_MODEL ?=
REFERENCE_BUILD ?=
REFERENCE_ARGS := $(if $(REFERENCE_MODEL),--reference-model "$(REFERENCE_MODEL)",) $(if $(REFERENCE_BUILD),--reference-build "$(REFERENCE_BUILD)",)

FRAMEWORKS := \
	-framework AppKit \
	-framework CoreGraphics \
	-framework CoreMedia \
	-framework CoreVideo \
	-framework ScreenCaptureKit \
	-framework VideoToolbox

.PHONY: all build app test dry-run pending distribution-pending list-displays clean help

all: build

help:
	@printf '%s\n' \
		'make build    Build the standalone Gate 0 probe' \
		'make app      Build a signable .app wrapper' \
		'make test     Build and run the executable contract checks' \
		'make dry-run  Print a non-live pending report' \
		'make pending  Write a local pending JSON record' \
		'make distribution-pending  Check Developer ID notarization prerequisites' \
		'make list-displays  Read-only display inventory' \
		'make clean    Remove build output'

$(BUILD_DIR):
	@mkdir -p "$(BUILD_DIR)"

$(MODULE_CACHE):
	@mkdir -p "$(MODULE_CACHE)"

build: $(BIN)

$(OBJC_OBJECT): $(OBJC_SOURCE) $(BRIDGE_HEADER) | $(BUILD_DIR) $(MODULE_CACHE)
	@printf 'Building %s\n' "$@"
	@CLANG_MODULE_CACHE_PATH="$(MODULE_CACHE)/clang" \
		xcrun --sdk macosx clang \
		-isysroot "$(SDK)" \
		-mmacosx-version-min=15.0 \
		-fobjc-arc \
		-fmodules \
		-I "$(ROOT)/Sources/Gate0Probe/include" \
		-c "$(OBJC_SOURCE)" \
		-o "$@"

$(BIN): $(SOURCES) $(OBJC_OBJECT) $(BRIDGE_HEADER) $(ROOT)/Makefile | $(BUILD_DIR) $(MODULE_CACHE)
	@printf 'Building %s\n' "$@"
	@CLANG_MODULE_CACHE_PATH="$(MODULE_CACHE)/clang" \
		SWIFT_MODULE_CACHE_PATH="$(MODULE_CACHE)/swift" \
		$(SWIFTC) \
		-sdk "$(SDK)" \
		-target "$(SWIFT_TARGET)" \
		-swift-version 5 \
		-module-cache-path "$(MODULE_CACHE)/swift" \
		-import-objc-header "$(BRIDGE_HEADER)" \
		-O \
		$(FRAMEWORKS) \
		-o "$@" \
		$(SOURCES) \
		$(OBJC_OBJECT)

app: build
	@rm -rf "$(APP)"
	@mkdir -p "$(APP)/Contents/MacOS"
	@cp "$(BIN)" "$(APP)/Contents/MacOS/Gate0Probe"
	@cp "$(ROOT)/AppInfo.plist" "$(APP)/Contents/Info.plist"
	@printf 'Built %s\n' "$(APP)"

test: app
	@GATE0_BIN="$(BIN)" GATE0_APP="$(APP)" "$(ROOT)/tests/contract.sh"

dry-run: build
	@"$(BIN)" --dry-run --json

pending: build
	@mkdir -p "$(ROOT)/records"
	@"$(BIN)" --dry-run --report "$(ROOT)/records/local-pending.json" $(REFERENCE_ARGS)

distribution-pending: app
	@mkdir -p "$(ROOT)/records"
	@"$(ROOT)/scripts/distribution-check.sh" --dry-run --json --app "$(APP)" \
		--report "$(ROOT)/records/local-distribution-pending.json"

list-displays: build
	@"$(BIN)" --list-displays --json

clean:
	@rm -rf "$(BUILD_DIR)"
