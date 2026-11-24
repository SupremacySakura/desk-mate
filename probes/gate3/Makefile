ifeq ($(origin CXX),default)
CXX := clang++
endif
BUILD_DIR ?= $(CURDIR)/.build
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread
INCLUDES := -I$(CURDIR)/core/include
CORE := $(CURDIR)/core

.PHONY: host-check build test records clean

host-check: test records

build:
	mkdir -p "$(BUILD_DIR)"

$(BUILD_DIR)/surface-lease-tests: $(CORE)/tests/surface_lease_tests.cpp $(CORE)/src/surface_lease.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(BUILD_DIR)/recovery-output-tests: $(CORE)/tests/recovery_output_tests.cpp $(CORE)/src/recovery_output.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(BUILD_DIR)/lifecycle-controller-tests: $(CORE)/tests/lifecycle_controller_tests.cpp \
		$(CORE)/src/lifecycle_controller.cpp $(CORE)/src/recovery_output.cpp $(CORE)/src/surface_lease.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(BUILD_DIR)/annexb-tests: $(CORE)/tests/annexb_tests.cpp $(CORE)/src/annexb.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) -DGATE3_TESTDATA='"$(CURDIR)/testdata/gate3-testsrc.h264"' $^ -o $@

test: $(BUILD_DIR)/surface-lease-tests $(BUILD_DIR)/recovery-output-tests \
		$(BUILD_DIR)/lifecycle-controller-tests $(BUILD_DIR)/annexb-tests
	cd "$(CURDIR)" && "$(BUILD_DIR)/surface-lease-tests"
	cd "$(CURDIR)" && "$(BUILD_DIR)/recovery-output-tests"
	cd "$(CURDIR)" && "$(BUILD_DIR)/lifecycle-controller-tests"
	"$(BUILD_DIR)/annexb-tests"

records:
	python3 "$(CURDIR)/scripts/validate-records.py" "$(CURDIR)/records"

clean:
	rm -rf "$(BUILD_DIR)"
