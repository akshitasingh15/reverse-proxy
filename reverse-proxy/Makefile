CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread
SRCS = src/proxy.cpp
HDRS = $(wildcard src/*.h)

proxy: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o proxy $(SRCS)

test: proxy
	./tests/run_tests.sh

clean:
	rm -f proxy

.PHONY: test clean
