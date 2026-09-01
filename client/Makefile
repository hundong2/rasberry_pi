CXX ?= g++
CPPFLAGS := -Iinclude
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread
LDFLAGS ?=
LDLIBS := -lboost_system -pthread

BUILD_DIR := build
TARGET := $(BUILD_DIR)/camera-client
TEST_TARGET := $(BUILD_DIR)/camera-client-tests

SOURCES := $(wildcard src/*.cpp)
OBJECTS := $(patsubst src/%.cpp,$(BUILD_DIR)/src/%.o,$(SOURCES))
DEPENDENCIES := $(OBJECTS:.o=.d)
TEST_SOURCES := tests/test_main.cpp src/config.cpp

.PHONY: all clean test ci install uninstall help

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(TEST_TARGET): $(TEST_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -pthread -o $@

test: $(TEST_TARGET)
	./$(TEST_TARGET)

ci:
	$(MAKE) clean
	$(MAKE) all
	$(MAKE) test

install: $(TARGET)
	install -d $(DESTDIR)/opt/camera-client/bin
	install -m 0755 $(TARGET) $(DESTDIR)/opt/camera-client/bin/camera-client
	install -d $(DESTDIR)/etc/systemd/system
	install -m 0644 packaging/camera-client.service \
		$(DESTDIR)/etc/systemd/system/camera-client.service

uninstall:
	rm -f $(DESTDIR)/opt/camera-client/bin/camera-client
	rm -f $(DESTDIR)/etc/systemd/system/camera-client.service

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "make          Build the C++20 camera client"
	@echo "make test     Build and run hardware-independent unit tests"
	@echo "make ci       Clean, build, and run all unit tests"
	@echo "make install  Install under /opt and install the systemd unit"
	@echo "make clean    Remove build outputs"

-include $(DEPENDENCIES)
