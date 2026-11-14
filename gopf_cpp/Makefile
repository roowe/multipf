CXX      ?= c++
CXXFLAGS ?= -O2 -std=c++20 -Wall -Wextra -Iinclude -Ithird_party
LDFLAGS  ?=

SRCS = \
	src/main.cpp \
	src/config.cpp \
	src/backend.cpp \
	src/ev.cpp \
	src/proxy.cpp

BIN = gopf_cpp

.PHONY: all clean debug

all: $(BIN)

$(BIN): $(SRCS) include/config.hpp include/backend.hpp include/ev.hpp include/proxy.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LDFLAGS)

debug:
	$(CXX) -O0 -g -std=c++20 -Wall -Wextra -Iinclude -Ithird_party -o $(BIN) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(BIN)
