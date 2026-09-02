# Native test build for the link codec. No board, no Arduino core.
#   make          build and run
#   make asan     build and run under ASan/UBSan

CC      ?= cc
CFLAGS  := -std=c99 -Wall -Wextra -Wpedantic -Werror -O1 -g
SRC     := test_frame.c ../../protocol/ss_frame.c
BIN     := test_frame

.PHONY: all run asan clean

all: run

$(BIN): $(SRC) ../../protocol/ss_frame.h ../../protocol/ss_protocol.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

run: $(BIN)
	./$(BIN)

asan: $(SRC)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $(BIN)-asan $(SRC)
	./$(BIN)-asan

clean:
	rm -f $(BIN) $(BIN)-asan
