ifeq ($(origin CXX),default)
CXX := clang++
endif
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Werror -Icore/include
CPPFLAGS ?=
LDFLAGS ?=

BUILD_DIR := build
LIB := $(BUILD_DIR)/libdeskmate-protocol.a
TEST_BIN := $(BUILD_DIR)/deskmate-tests

LIB_SOURCES := $(wildcard core/src/*.cpp)
TEST_SOURCES := $(wildcard core/tests/*.cpp)
LIB_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(LIB_SOURCES))
TEST_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(TEST_SOURCES))
DEPS := $(LIB_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)

.PHONY: all lib test clean

all: $(LIB) $(TEST_BIN)

lib: $(LIB)

$(LIB): $(LIB_OBJECTS)
	mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(TEST_BIN): $(TEST_OBJECTS) $(LIB)
	mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $(TEST_OBJECTS) $(LIB) -o $@

$(BUILD_DIR)/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

test: $(TEST_BIN)
	./$(TEST_BIN) testdata/golden

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
