CXX = g++
CPPFLAGS ?=
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?=
CONAN ?= conan
PKG_CONFIG ?= pkg-config
CONAN_FLAGS ?=

TARGETS := graph_worker supervisor
BUILD_DIR := build
CONAN_DIR := $(BUILD_DIR)/conan
CONAN_PC := $(CONAN_DIR)/nlohmann_json.pc
JSON_CPPFLAGS = $(shell PKG_CONFIG_PATH="$(abspath $(CONAN_DIR)):$$PKG_CONFIG_PATH" $(PKG_CONFIG) --cflags nlohmann_json)
SOURCES := Node.cpp InputNode.cpp OutputNode.cpp JsonGraphParser.cpp GraphWorker.cpp Supervisor.cpp
OBJECTS := $(SOURCES:%.cpp=$(BUILD_DIR)/%.o)
DEPS := $(OBJECTS:.o=.d)

.PHONY: all run clean deps test

all: $(TARGETS)

graph_worker: $(BUILD_DIR)/Node.o $(BUILD_DIR)/InputNode.o $(BUILD_DIR)/OutputNode.o $(BUILD_DIR)/JsonGraphParser.o $(BUILD_DIR)/GraphWorker.o
	$(CXX) $(LDFLAGS) -pthread $^ $(LDLIBS) -o $@

supervisor: $(BUILD_DIR)/Supervisor.o
	$(CXX) $(LDFLAGS) -pthread $^ $(LDLIBS) -o $@

deps: $(CONAN_PC)

$(CONAN_PC): conanfile.txt | $(BUILD_DIR)
	$(CONAN) install . --output-folder=$(CONAN_DIR) --build=missing $(CONAN_FLAGS)

$(BUILD_DIR)/JsonGraphParser.o: JsonGraphParser.cpp $(CONAN_PC) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(JSON_CPPFLAGS) $(CXXFLAGS) -std=c++20 -pthread -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -pthread -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

run: $(TARGETS)
	./supervisor ./graph_worker

test: graph_worker
	python3 tests/test_json_graph.py

clean:
	$(RM) -r $(BUILD_DIR) $(TARGETS) async_nodes

-include $(DEPS)
