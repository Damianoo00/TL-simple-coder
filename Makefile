CXX = g++
CPPFLAGS ?=
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?=

TARGETS := graph_worker supervisor
BUILD_DIR := build
SOURCES := Node.cpp InputNode.cpp GraphWorker.cpp Supervisor.cpp
OBJECTS := $(SOURCES:%.cpp=$(BUILD_DIR)/%.o)
DEPS := $(OBJECTS:.o=.d)

.PHONY: all run clean

all: $(TARGETS)

graph_worker: $(BUILD_DIR)/Node.o $(BUILD_DIR)/InputNode.o $(BUILD_DIR)/GraphWorker.o
	$(CXX) $(LDFLAGS) -pthread $^ $(LDLIBS) -o $@

supervisor: $(BUILD_DIR)/Supervisor.o
	$(CXX) $(LDFLAGS) -pthread $^ $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -pthread -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

run: $(TARGETS)
	./supervisor ./graph_worker

clean:
	$(RM) -r $(BUILD_DIR) $(TARGETS) async_nodes

-include $(DEPS)
