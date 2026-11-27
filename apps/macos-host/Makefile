SHELL := /bin/bash

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO := $(abspath $(ROOT)/../..)
BUILD_DIR := $(ROOT)/build
MODULE_CACHE := $(BUILD_DIR)/module-cache
APP := $(BUILD_DIR)/DeskMate.app
BIN := $(APP)/Contents/MacOS/DeskMate
VERSION ?= 1.7.1
ARCH := $(shell uname -m)
DMG := $(BUILD_DIR)/DeskMate-v$(VERSION)-macos-$(ARCH).dmg
APP_ZIP := $(BUILD_DIR)/DeskMate-v$(VERSION)-macos-$(ARCH).zip

CORE_SOURCES := $(wildcard $(REPO)/core/src/*.cpp)
CORE_OBJECTS := $(patsubst $(REPO)/core/src/%.cpp,$(BUILD_DIR)/core/%.o,$(CORE_SOURCES))
BRIDGE_OBJECT := $(BUILD_DIR)/Bridge.o
OBJC_OBJECT := $(BUILD_DIR)/DMVirtualDisplay.o
HID_OBJC_OBJECT := $(BUILD_DIR)/DMVirtualHID.o
USB_BYTE_IO_TEST := $(BUILD_DIR)/usb-byte-io-test

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
	-framework IOKit \
	-framework ScreenCaptureKit \
	-framework VideoToolbox \
	-framework ApplicationServices \
	-framework ServiceManagement

CORE_INCLUDES := -I$(REPO)/core/include -I$(ROOT)/include -I$(ROOT)/Sources/include \
	-I$(LIBUSB_PREFIX)/include
CORE_FLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Wno-deprecated-enum-enum-conversion \
	-O2 -mmacosx-version-min=15.0 -isysroot $(SDK) $(CORE_INCLUDES)

.PHONY: all build app dmg zip test clean list-displays help

all: app

help:
	@printf '%s\n' \
		'make app           Build build/DeskMate.app' \
		'make dmg           Bundle libusb and package DeskMate.dmg' \
		'make test          Build and run host contract checks' \
		'make list-displays Print the online display inventory' \
		'make device-foreground HDC=... Push and start the AOA foreground helper' \
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

$(HID_OBJC_OBJECT): $(ROOT)/Sources/DMVirtualHID.m $(BRIDGE_HEADER) | $(BUILD_DIR) $(MODULE_CACHE)
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

build: $(CORE_OBJECTS) $(BRIDGE_OBJECT) $(OBJC_OBJECT) $(HID_OBJC_OBJECT) $(ROOT)/Resources/AppIcon.icns | $(BUILD_DIR) $(MODULE_CACHE)
	@rm -rf "$(APP)"
	@mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	@cp "$(ROOT)/Info.plist" "$(APP)/Contents/Info.plist"
	@cp "$(ROOT)/Resources/AppIcon.icns" "$(APP)/Contents/Resources/AppIcon.icns"
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
		$(OBJC_OBJECT) \
		$(HID_OBJC_OBJECT)

app: build

dmg: app
	@bash "$(ROOT)/scripts/package-dmg.sh" "$(APP)" "$(DMG)" "$(LIBUSB_PREFIX)" "DeskMate" "$(APP_ZIP)"
	@mkdir -p "$(REPO)/dist/macos"
	@cp -f "$(DMG)" "$(REPO)/dist/macos/"
	@cp -f "$(APP_ZIP)" "$(REPO)/dist/macos/"
	@printf 'Copied macOS packages to %s\n' "$(REPO)/dist/macos/"

zip: dmg

$(USB_BYTE_IO_TEST): $(ROOT)/tests/usb_byte_io_test.cpp $(ROOT)/Sources/Bridge.cpp $(ROOT)/include/DMBridge.h $(CORE_OBJECTS) | $(BUILD_DIR)
	@xcrun --sdk macosx clang++ $(CORE_FLAGS) "$<" $(CORE_OBJECTS) \
		-L"$(LIBUSB_PREFIX)/lib" -lusb-1.0 -Wl,-rpath,"$(LIBUSB_PREFIX)/lib" -o "$@"

test: app $(USB_BYTE_IO_TEST)
	@bash "$(ROOT)/tests/contract.sh" "$(APP)"
	@"$(USB_BYTE_IO_TEST)"

list-displays: app
	@"$(BIN)" --list-displays

# Pushes the "keep the ability in the foreground across the AOA flip" helper and
# starts it detached on the tablet. Must be run while the tablet is still HDC.
device-foreground:
	@test -n "$(HDC)" || { echo 'usage: make device-foreground HDC=/path/to/hdc' >&2; exit 2; }
	@test -n "$(BUNDLE)" || { echo 'usage: make device-foreground HDC=... BUNDLE=com.example.app' >&2; exit 2; }
	@"$(HDC)" file send "$(ROOT)/tools/device-foreground.sh" /data/local/tmp/deskmate_fg.sh
	@"$(HDC)" shell "chmod 755 /data/local/tmp/deskmate_fg.sh"
	@"$(HDC)" shell "DESKMATE_BUNDLE=$(BUNDLE) nohup sh /data/local/tmp/deskmate_fg.sh >/dev/null 2>&1 &"
	@echo "foreground helper running on the tablet for bundle $(BUNDLE)"

clean:
	@rm -rf "$(BUILD_DIR)"
