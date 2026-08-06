CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
LDLIBS  ?= -lm -lsqlite3
PREFIX  ?= $(HOME)/.local

SRC  := $(wildcard src/*.c)
OBJ  := $(SRC:.c=.o)
GEN  := src/start_html.h
BIN  := web

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

# The start page, carried in the binary so a first run has one to write out.
$(GEN): start.html
	printf 'static const char START_HTML[] = {\n' > $@
	xxd -i < $< >> $@
	printf '\n};\n' >> $@

src/%.o: src/%.c src/web.h vendor/repl.h $(GEN)
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

browser: install
	sh mkbrowser.sh

clean:
	rm -f $(OBJ) $(BIN) $(GEN)

.PHONY: all install browser clean
