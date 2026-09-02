# ?=는 호출자가 CXX를 지정하지 않았을 때만 g++를 기본 컴파일러로 사용한다.
CXX ?= g++
# CPPFLAGS는 전처리기 옵션이며 -Iinclude가 프로젝트 헤더 검색 경로를 추가한다.
CPPFLAGS := -Iinclude
# CXXFLAGS는 C++ 컴파일 옵션이다. 외부 값이 없으면 최적화 수준 -O2를 사용한다.
CXXFLAGS ?= -O2
# +=는 기존 옵션 뒤에 C++20, 경고, pthread 옵션을 추가한다.
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread
# LDFLAGS는 library보다 먼저 전달하는 사용자 정의 linker 옵션 자리다.
LDFLAGS ?=
# LDLIBS는 실제 library 이름이며 -lboost_system은 libboost_system을 연결한다.
LDLIBS := -lboost_system -pthread

# :=는 오른쪽 식을 지금 즉시 계산해 Make 변수에 저장한다.
BUILD_DIR := build
TARGET := $(BUILD_DIR)/camera-client
TEST_TARGET := $(BUILD_DIR)/camera-client-tests

# wildcard는 src 아래 모든 .cpp 경로를 목록으로 반환한다.
SOURCES := $(wildcard src/*.cpp)
# patsubst는 src/name.cpp를 build/src/name.o 경로로 일괄 변환한다.
OBJECTS := $(patsubst src/%.cpp,$(BUILD_DIR)/src/%.o,$(SOURCES))
# 치환 참조 (.o=.d)는 각 object에 대응하는 자동 header dependency 파일을 만든다.
DEPENDENCIES := $(OBJECTS:.o=.d)
# 테스트는 카메라/네트워크 없이 config와 queue만 빌드한다.
TEST_SOURCES := tests/test_main.cpp src/config.cpp

# PHONY는 아래 이름이 실제 파일이 아니라 항상 실행 가능한 명령 target임을 알린다.
.PHONY: all clean test ci install uninstall help

# `make`가 target을 생략하면 첫 target인 all을 실행한다.
all: $(TARGET)

# 실행 파일은 모든 object가 준비된 뒤 linker 단계로 만들어진다.
$(TARGET): $(OBJECTS)
# $^는 모든 prerequisite, $@는 현재 target 경로로 Make가 치환한다.
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

# %는 같은 파일 이름 부분을 공유하는 pattern rule이다.
$(BUILD_DIR)/src/%.o: src/%.cpp
# @는 실행할 mkdir 명령 자체는 출력하지 않고, dir은 target의 디렉터리를 반환한다.
	@mkdir -p $(dir $@)
# -MMD/-MP는 include한 project header dependency를 .d 파일로 생성한다.
# -c는 link하지 않고 compile만 하며 $<는 첫 prerequisite .cpp 파일이다.
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

# 테스트 실행 파일은 간단하므로 두 source를 한 명령에서 compile/link한다.
$(TEST_TARGET): $(TEST_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -pthread -o $@

# test target은 테스트 binary를 먼저 최신 상태로 만든 다음 실행한다.
test: $(TEST_TARGET)
	./$(TEST_TARGET)

# CI에서는 이전 산출물을 지우고 production binary와 테스트를 모두 새로 확인한다.
ci:
# $(MAKE)는 현재 make의 옵션/jobserver 설정을 보존한 재귀 make 호출이다.
	$(MAKE) clean
	$(MAKE) all
	$(MAKE) test

# DESTDIR를 지정하면 package staging 경로를 앞에 붙일 수 있고 기본값은 빈 값이다.
install: $(TARGET)
# install -d는 디렉터리를 만들고, -m은 설치 파일의 Unix 권한 mode를 지정한다.
	install -d $(DESTDIR)/opt/camera-client/bin
	install -m 0755 $(TARGET) $(DESTDIR)/opt/camera-client/bin/camera-client
	install -d $(DESTDIR)/etc/systemd/system
	install -m 0644 packaging/camera-client.service \
		$(DESTDIR)/etc/systemd/system/camera-client.service

# uninstall은 설치된 binary/unit만 제거하며 source와 설정 파일은 건드리지 않는다.
uninstall:
	rm -f $(DESTDIR)/opt/camera-client/bin/camera-client
	rm -f $(DESTDIR)/etc/systemd/system/camera-client.service

# clean은 Git에서 무시되는 build 디렉터리만 삭제한다.
clean:
	rm -rf $(BUILD_DIR)

# recipe 줄의 @echo는 명령 문자열 자체 없이 안내 문장만 출력한다.
help:
	@echo "make          Build the C++20 camera client"
	@echo "make test     Build and run hardware-independent unit tests"
	@echo "make ci       Clean, build, and run all unit tests"
	@echo "make install  Install under /opt and install the systemd unit"
	@echo "make clean    Remove build outputs"

# 앞의 -는 .d 파일이 아직 없어도 오류를 무시한다. 존재하면 header 변경 시 관련
# object를 자동 재빌드하도록 dependency rule을 읽는다.
-include $(DEPENDENCIES)
