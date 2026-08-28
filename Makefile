# Build the tool.
#
#   make          the binary, at the top of the checkout
#   make check    the unit tests (fast, no repos or cluster involved)
#   make test     those, then the full suite in tests/test_tool.py
#   make clean
#
# There is nothing to install and nothing to fetch: a C++ compiler is the only
# thing needed to produce it, which is the point of it not being a script any
# more. It builds with the gcc that ships with RHEL8 (8.5, C++17) and anything
# newer.
#
# The binary is deliberately left beside ioc-restart and ioc-exec: it finds them
# next to itself, through a symlink if you put one on your PATH.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
# The C++ runtime is linked in rather than loaded: an updated or missing
# libstdc++ must not be able to stop the tool working.
LDFLAGS  ?= -static-libstdc++ -static-libgcc

BIN      := deployment-repo-tool
SRC      := $(wildcard src/*.cpp)
OBJ      := $(patsubst src/%.cpp,build/%.o,$(SRC))
# Everything but main, so the unit tests can link the same objects.
LIB_OBJ  := $(filter-out build/main.o,$(OBJ))
UNIT     := build/unit

.PHONY: all check test clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(LDFLAGS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(UNIT): tests/unit.cpp $(LIB_OBJ) | build
	$(CXX) $(CXXFLAGS) -Isrc tests/unit.cpp $(LIB_OBJ) -o $@ $(LDFLAGS)

build:
	mkdir -p build

# The pieces python used to get from its standard library -- globbing, the ini
# file, path handling, the diff -- are the ones worth testing directly.
check: $(UNIT)
	./$(UNIT)

# The full suite drives the built binary, so it has to exist first. It needs
# PyYAML, which uv fetches into a throwaway environment from the PEP 723 header
# in the test file; without uv, pip install pyyaml && python3 tests/test_tool.py.
test: $(BIN) check
	uv run tests/test_tool.py

clean:
	rm -rf build $(BIN)

-include $(OBJ:.o=.d)
