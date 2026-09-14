CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror -pedantic -O2
INCS     := $(shell pkg-config --cflags glib-2.0 json-glib-1.0)
LIBS     := $(shell pkg-config --libs glib-2.0 json-glib-1.0)

promptr-opencode: main.c ../../lib/promptr-protocol/jsonrpc.c
	$(CC) $(CFLAGS) $(INCS) -o $@ main.c ../../lib/promptr-protocol/jsonrpc.c $(LIBS)

clean:
	rm -f promptr-opencode

.PHONY: clean
