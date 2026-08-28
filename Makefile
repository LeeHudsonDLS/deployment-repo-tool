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
# next to itself, through a symlink if you put one on your PATH. That means one
# binary per checkout, so on a shared filesystem the last machine to build wins.
# Object files are kept apart per toolchain (see below), but the binary cannot
# be; run make on the machine you are going to use it from. Getting this wrong
# fails loudly at exec, never quietly.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic

# Linking the C++ runtime in rather than loading it is worth having: it is one
# fewer thing that can change underneath the tool. But it needs libstdc++.a,
# which RHEL8 keeps in a separate libstdc++-static package that is usually not
# installed, and needing a package installed to build is exactly the sort of
# dependency this tool is not supposed to have.
#
# So it is probed for rather than assumed. Without it the build falls back to
# the ordinary shared link, which is no worse than any other program on the
# machine. `dnf install libstdc++-static` (or the CXXFLAGS/LDFLAGS below) if
# you want the static one.
STATIC    := -static-libstdc++ -static-libgcc
STATIC_OK := $(shell echo 'int main(){}' | $(CXX) -x c++ - $(STATIC) -o /dev/null 2>/dev/null && echo '$(STATIC)')
LDFLAGS   ?= $(STATIC_OK)

BIN      := ioc
SRC      := $(wildcard src/*.cpp)

# Object files go under the compiler that made them. A checkout on /dls_sw is
# shared between machines, and building it on two of them put RHEL8 and Ubuntu
# objects in one directory -- make saw nothing wrong with that, and the link
# failed with a relocation error that says nothing about the real cause. Each
# toolchain now gets its own directory and they cannot mix.
TOOLCHAIN := $(shell $(CXX) -dumpmachine)-$(shell $(CXX) -dumpversion)
BUILD     := build/$(TOOLCHAIN)

OBJ      := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRC))
# Everything but main, so the unit tests can link the same objects.
LIB_OBJ  := $(filter-out $(BUILD)/main.o,$(OBJ))
UNIT     := $(BUILD)/unit

.PHONY: all check test clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(LDFLAGS)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(UNIT): tests/unit.cpp $(LIB_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc tests/unit.cpp $(LIB_OBJ) -o $@ $(LDFLAGS)

$(BUILD):
	mkdir -p $(BUILD)

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
