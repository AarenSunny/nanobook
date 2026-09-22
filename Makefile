CXX ?= c++
CPPFLAGS := -Iinclude
CXXFLAGS := -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic
TESTFLAGS := -std=c++20 -O1 -g -Wall -Wextra -Wpedantic
SRC := src/order_book.cpp src/feed.cpp

.PHONY: all test clean sanitize
all: build/nanobook-bench

build:
	mkdir -p build

build/nanobook-bench: $(SRC) src/bench.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

build/nanobook-test: $(SRC) tests/test.cpp | build
	$(CXX) $(CPPFLAGS) $(TESTFLAGS) $^ -o $@

test: build/nanobook-test
	./build/nanobook-test

sanitize: | build
	$(CXX) $(CPPFLAGS) $(TESTFLAGS) -fsanitize=address,undefined $(SRC) tests/test.cpp -o build/nanobook-sanitize
	./build/nanobook-sanitize

clean:
	rm -rf build
