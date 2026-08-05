CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
LDLIBS  ?= -lm
PREFIX  ?= $(HOME)/.local

SRC  := $(wildcard src/*.c)
OBJ  := $(SRC:.c=.o)
BIN  := web

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/web.h vendor/repl.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

browser: install
	sh mkbrowser.sh

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all install browser clean
