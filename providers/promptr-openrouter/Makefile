CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror -pedantic -O2
ifeq ($(BUILD),debug)
CFLAGS = -std=c11 -Wall -Wextra -pedantic -Og -ggdb3
endif

INCS     := $(shell pkg-config --cflags gio-2.0 json-glib-1.0 libcurl)
LIBS     := $(shell pkg-config --libs gio-2.0 json-glib-1.0 libcurl)

promptr-openrouter: main.c ../../lib/promptr-protocol/jsonrpc.c ../../lib/promptr-protocol/provider-server.c
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LIBS)

clean:
	rm -f promptr-openrouter

.PHONY: clean
