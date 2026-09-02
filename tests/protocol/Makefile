# Native test build for the link codec and transport. No board, no Arduino core.
#   make          build and run everything
#   make asan     same, under ASan/UBSan

CC      ?= cc
CFLAGS  := -std=c99 -Wall -Wextra -Wpedantic -Werror -O1 -g
PROTO   := ../../protocol
HDRS    := $(PROTO)/ss_frame.h $(PROTO)/ss_protocol.h $(PROTO)/ss_link.h
BINS    := test_frame test_link

.PHONY: all run asan clean

all: run

test_frame: test_frame.c $(PROTO)/ss_frame.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_frame.c $(PROTO)/ss_frame.c

test_link: test_link.c $(PROTO)/ss_frame.c $(PROTO)/ss_link.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_link.c $(PROTO)/ss_frame.c $(PROTO)/ss_link.c

run: $(BINS)
	@for t in $(BINS); do ./$$t || exit 1; echo; done

asan: test_frame.c test_link.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o test_frame-asan \
		test_frame.c $(PROTO)/ss_frame.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o test_link-asan \
		test_link.c $(PROTO)/ss_frame.c $(PROTO)/ss_link.c
	./test_frame-asan && echo && ./test_link-asan

clean:
	rm -f $(BINS) test_frame-asan test_link-asan
