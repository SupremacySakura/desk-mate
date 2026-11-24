SHELL := /bin/bash

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO := $(abspath $(ROOT)/../..)
BUILD_DIR := $(ROOT)/build
MODULE_CACHE := $(BUILD_DIR)/module-cache
APP := $(BUILD_DIR)/DeskMate.app
BIN := $(APP)/Contents/MacOS/DeskMate

CORE_SOURCES := $(wildcard $(REPO)/core/src/*.cpp)
CORE_OBJECTS := $(patsubst $(REPO)/core/src/%.cpp,$(BUILD_DIR)/core/%.o,$(CORE_SOURCES))
BRIDGE_OBJECT := $(BUILD_DIR)/Bridge.o
OBJC_OBJECT := $(BUILD_DIR)/DMVirtualDisplay.o

SWIFT_SOURCES := $(wildcard $(ROOT)/Sources/*.swift)
BRIDGE_HEADER := $(ROOT)/Sources/include/DeskMateBridge.h

SDK := $(shell xcrun --sdk macosx --show-sdk-path)
SWIFTC := xcrun --sdk macosx swiftc
SWIFT_TARGET := $(shell uname -m)-apple-macosx15.0
LIBUSB_PREFIX ?= /opt/homebrew

FRAMEWORKS := \
	-framework AppKit \
	-framework CoreGraphics \
	-framework CoreMedia \
	-framework CoreVideo \
	-framework ScreenCaptureKit \
	-framework VideoToolbox \
	-framework ApplicationServices

CORE_INCLUDES := -I$(REPO)/core/include -I$(ROOT)/include -I$(ROOT)/Sources/include \
	-I$(LIBUSB_PREFIX)/include
CORE_FLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Wno-deprecated-enum-enum-conversion \
	-O2 -mmacosx-version-min=15.0 -isysroot $(SDK) $(CORE_INCLUDES)

.PHONY: all build app test clean list-displays help

all: app

help:
	@printf '%s\n' \
		'make app           Build build/DeskMate.app' \
		'make test          Build and run host contract checks' \
		'make list-displays Print the online display inventory' \
		'make clean         Remove build output'

$(BUILD_DIR):
	@mkdir -p "$(BUILD_DIR)"

$(MODULE_CACHE):
	@mkdir -p "$(MODULE_CACHE)"

$(BUILD_DIR)/core:
	@mkdir -p "$@"

$(BUILD_DIR)/core/%.o: $(REPO)/core/src/%.cpp | $(BUILD_DIR)/core
	@printf 'Building %s\n' "$@"
	@xcrun --sdk macosx clang++ $(CORE_FLAGS) -c "$<" -o "$@"

$(BRIDGE_OBJECT): $(ROOT)/Sources/Bridge.cpp $(ROOT)/include/DMBridge.h | $(BUILD_DIR)
	@printf 'Building %s\n' "$@"
	@xcrun --sdk macosx clang++ $(CORE_FLAGS) -c "$<" -o "$@"

$(OBJC_OBJECT): $(ROOT)/Sources/DMVirtualDisplay.m $(BRIDGE_HEADER) | $(BUILD_DIR) $(MODULE_CACHE)
	@printf 'Building %s\n' "$@"
	@CLANG_MODULE_CACHE_PATH="$(MODULE_CACHE)/clang" \
		xcrun --sdk macosx clang \
		-isysroot "$(SDK)" \
		-mmacosx-version-min=15.0 \
		-fobjc-arc \
		-fmodules \
		-I "$(ROOT)/Sources/include" \
		-c "$<" \
		-o "$@"

build: $(CORE_OBJECTS) $(BRIDGE_OBJECT) $(OBJC_OBJECT) | $(BUILD_DIR) $(MODULE_CACHE)
	@rm -rf "$(APP)"
	@mkdir -p "$(APP)/Contents/MacOS"
	@cp "$(ROOT)/Info.plist" "$(APP)/Contents/Info.plist"
	@printf 'Linking %s\n' "$(BIN)"
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
		-Xcc -I$(ROOT)/include \
		-Xlinker -L$(LIBUSB_PREFIX)/lib \
		-Xlinker -lusb-1.0 \
		-Xlinker -lc++ \
		-Xlinker -rpath -Xlinker $(LIBUSB_PREFIX)/lib \
		-o "$(BIN)" \
		$(SWIFT_SOURCES) \
		$(CORE_OBJECTS) \
		$(BRIDGE_OBJECT) \
		$(OBJC_OBJECT)

app: build

test: app
	@bash "$(ROOT)/tests/contract.sh" "$(APP)"

list-displays: app
	@"$(BIN)" --list-displays

clean:
	@rm -rf "$(BUILD_DIR)"
