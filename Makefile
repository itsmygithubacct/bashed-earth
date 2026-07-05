CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
LDLIBS   = -lz -lm

SRC = src/main.c src/game.c src/terrain.c src/render.c src/term.c src/config.c
OBJ = $(SRC:.c=.o)
BIN = bashed-earth

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/bashed_earth.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/render.o: src/font8x16.h

test: $(BIN)
	./$(BIN) --selftest 1337 3
	./$(BIN) --selftest 42 2
	./$(BIN) --selftest 7 2

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all test clean
