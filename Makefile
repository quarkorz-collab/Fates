CXX ?= g++
CXXFLAGS ?= -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic
LDFLAGS ?= -pthread

ifeq ($(NATIVE),1)
CXXFLAGS += -march=native
endif

.PHONY: all test clean
all: fates pries

fates: src/fates.cpp src/extension_api.h src/fast_containers.h src/user_extensions.h third_party/unordered_dense/include/ankerl/unordered_dense.h third_party/unordered_dense/include/ankerl/stl.h
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

pries: fates
	cp fates pries

test: fates
	./fates --self-test

clean:
	rm -f fates pries
