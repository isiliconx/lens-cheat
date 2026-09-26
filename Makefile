# lens - plain make, no cmake.
#   make            release
#   make DEBUG=1    debug
#   make selftest   build then run the self-test
#   make clean

CXX      ?= g++
# -MMD -MP emits a .d sidecar per object so a header edit rebuilds everything
# that includes it. Without this, editing a struct in a header leaves stale
# objects with the old layout and the link succeeds into a segfault.
CXXFLAGS  = -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -MMD -MP -Isrc -DLENS_VERSION=\"1.0.0\"
LDFLAGS   =
LDLIBS    =
DEP       = $(CORE_OBJ:.o=.d) src/main.d src/sim/main.d

ifeq ($(DEBUG),1)
  CXXFLAGS = -std=c++20 -O0 -g3 -Wall -Wextra -Wno-unused-parameter -MMD -MP -Isrc -DLENS_DEBUG -DLENS_VERSION=\"1.0.0\"
endif

ifeq ($(OS),Windows_NT)
  LDLIBS += -luser32 -lpsapi -ladvapi32
else
  LDLIBS += -lrt
endif

CORE_SRC = $(wildcard src/core/*.cpp) $(wildcard src/descriptor/*.cpp) \
           $(wildcard src/runtime/*.cpp) $(wildcard src/overlay/*.cpp) \
           $(wildcard src/features/*.cpp) $(wildcard src/script/*.cpp) \
           $(wildcard src/config/*.cpp) src/selftest.cpp
CORE_OBJ = $(CORE_SRC:.cpp=.o)

all: lens lens_sim

lens: $(CORE_OBJ) src/main.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

lens_sim: $(CORE_OBJ) src/sim/main.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

selftest: lens lens_sim
	./lens selftest

clean:
	rm -f $(CORE_OBJ) $(DEP) src/main.o src/sim/main.o lens lens_sim

-include $(DEP)

.PHONY: all selftest clean
