ifeq ($(origin CXX),default)
CXX := clang++
endif
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Werror -Icore/include
CPPFLAGS ?=
LDFLAGS ?=

BUILD_DIR := build
LIB := $(BUILD_DIR)/libdeskmate-protocol.a
PROTOCOL_TEST_BIN := $(BUILD_DIR)/deskmate-tests
SESSION_TEST_BIN := $(BUILD_DIR)/deskmate-session-tests
SCHEDULING_TEST_BIN := $(BUILD_DIR)/deskmate-scheduling-tests
TEST_BINS := $(PROTOCOL_TEST_BIN) $(SESSION_TEST_BIN) $(SCHEDULING_TEST_BIN)

LIB_SOURCES := $(wildcard core/src/*.cpp)
TEST_SOURCES := $(wildcard core/tests/*.cpp)
LIB_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(LIB_SOURCES))
TEST_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(TEST_SOURCES))
DEPS := $(LIB_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)

.PHONY: all lib test clean

all: $(LIB) $(TEST_BINS)

lib: $(LIB)

$(LIB): $(LIB_OBJECTS)
	mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(PROTOCOL_TEST_BIN): $(BUILD_DIR)/core/tests/protocol_tests.o $(LIB)
	mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $^ -o $@

$(SESSION_TEST_BIN): $(BUILD_DIR)/core/tests/session_tests.o $(LIB)
	mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $^ -o $@

$(SCHEDULING_TEST_BIN): $(BUILD_DIR)/core/tests/scheduling_tests.o $(LIB)
	mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

test: $(TEST_BINS)
	./$(PROTOCOL_TEST_BIN) testdata/golden
	./$(SESSION_TEST_BIN)
	./$(SCHEDULING_TEST_BIN)

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
