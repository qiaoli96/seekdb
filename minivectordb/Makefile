CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
LDFLAGS ?=

SRC := src/main.cpp src/db.cpp src/parser.cpp
HEADERS := $(wildcard src/*.h)
BIN := bin/mvd

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC) $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -rf bin
