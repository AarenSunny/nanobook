CXX ?= c++
CPPFLAGS := -Iinclude
CXXFLAGS := -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic
TESTFLAGS := -std=c++20 -O1 -g -Wall -Wextra -Wpedantic
ENGINE_SRC := src/order_book.cpp src/feed.cpp
GATEWAY_SRC := src/order_book.cpp src/protocol.cpp src/gateway.cpp
JOURNAL_SRC := $(GATEWAY_SRC) src/journal.cpp
SHARDED_SRC := $(GATEWAY_SRC) src/sharded_exchange.cpp
PIPELINE_SRC := $(JOURNAL_SRC) src/sharded_exchange.cpp
THREADFLAGS := -pthread

.PHONY: all test clean sanitize
all: build/nanobook-bench build/nanobook-gateway build/nanobook-exchange-bench

build:
	mkdir -p build

build/nanobook-bench: $(ENGINE_SRC) src/bench.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

build/nanobook-test: $(ENGINE_SRC) tests/test.cpp | build
	$(CXX) $(CPPFLAGS) $(TESTFLAGS) $^ -o $@

build/nanobook-gateway-test: $(GATEWAY_SRC) tests/test_gateway.cpp | build
	$(CXX) $(CPPFLAGS) $(TESTFLAGS) $^ -o $@

build/nanobook-journal-test: $(JOURNAL_SRC) tests/test_journal.cpp | build
	$(CXX) $(CPPFLAGS) $(TESTFLAGS) $^ -o $@

build/nanobook-sharded-test: $(SHARDED_SRC) tests/test_sharded.cpp | build
	$(CXX) $(CPPFLAGS) $(TESTFLAGS) $(THREADFLAGS) $^ -o $@

build/nanobook-gateway: $(JOURNAL_SRC) src/gateway_server.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

build/nanobook-exchange-bench: $(PIPELINE_SRC) src/exchange_bench.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(THREADFLAGS) $^ -o $@

test: build/nanobook-test build/nanobook-gateway-test build/nanobook-journal-test build/nanobook-sharded-test
	./build/nanobook-test
	./build/nanobook-gateway-test
	./build/nanobook-journal-test
	./build/nanobook-sharded-test

sanitize: | build
	$(CXX) $(CPPFLAGS) $(TESTFLAGS) -fsanitize=address,undefined $(ENGINE_SRC) tests/test.cpp -o build/nanobook-sanitize
	./build/nanobook-sanitize
	$(CXX) $(CPPFLAGS) $(TESTFLAGS) -fsanitize=address,undefined $(GATEWAY_SRC) tests/test_gateway.cpp -o build/nanobook-gateway-sanitize
	./build/nanobook-gateway-sanitize
	$(CXX) $(CPPFLAGS) $(TESTFLAGS) -fsanitize=address,undefined $(JOURNAL_SRC) tests/test_journal.cpp -o build/nanobook-journal-sanitize
	./build/nanobook-journal-sanitize
	$(CXX) $(CPPFLAGS) $(TESTFLAGS) $(THREADFLAGS) -fsanitize=address,undefined $(SHARDED_SRC) tests/test_sharded.cpp -o build/nanobook-sharded-sanitize
	./build/nanobook-sharded-sanitize

clean:
	rm -rf build
