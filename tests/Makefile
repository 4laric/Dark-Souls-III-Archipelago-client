# tests/ — host (g++) unit tests for the ER Archipelago runtime client.
# NOT part of the MSVC DLL build: these validate the pure, platform-independent logic (decode
# contract, EquipParamGoods row offsets, param-table walk, version gate) on any C++17 compiler,
# so they can run in CI (Linux) alongside the Windows DLL build.
#
# Requires: the er_*.h headers in ../archipelago-client, and codec_vectors_generated.h in this dir
# (committed frozen golden vectors generated from spec-2's vagrant_codec.py — see tools/NOTES.md).

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -I../archipelago-client

SRCS    := tests.cpp row_test.cpp reconcile_test.cpp walk_test.cpp
BINS    := $(SRCS:.cpp=)
HEADERS := $(wildcard ../archipelago-client/er_*.h) codec_vectors_generated.h

.PHONY: all test clean
all: $(BINS)

$(BINS): %: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $< -o $@

test: $(BINS)
	@fail=0; for t in $(BINS); do \
	  echo "== $$t =="; ./$$t || fail=1; echo; \
	done; \
	if [ $$fail -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED"; exit 1; fi

clean:
	rm -f $(BINS)
