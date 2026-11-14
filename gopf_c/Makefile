CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Isrc
LDFLAGS ?=

SRCS = \
	src/main.c \
	src/config.c \
	src/backend.c \
	src/ev.c \
	src/proxy.c \
	src/vendor/cJSON.c

BIN = gopf_c

.PHONY: all clean debug

all: $(BIN)

$(BIN): $(SRCS) src/config.h src/backend.h src/ev.h src/proxy.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

debug:
	$(CC) -O0 -g -std=c11 -Wall -Wextra -Isrc -o $(BIN) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(BIN)
