CC      ?= gcc
PKG_CONF := pkg-config

GTK_CFLAGS  := $(shell $(PKG_CONF) --cflags gtk4 | sed 's/-I/-isystem /g')
GTK_LIBS    := $(shell $(PKG_CONF) --libs gtk4)
JSON_CFLAGS := $(shell $(PKG_CONF) --cflags json-glib-1.0)
JSON_LIBS   := $(shell $(PKG_CONF) --libs json-glib-1.0)
SECRET_CFLAGS := $(shell $(PKG_CONF) --cflags libsecret-1)
SECRET_LIBS := $(shell $(PKG_CONF) --libs libsecret-1)
LSH_PKG := $(shell $(PKG_CONF) --exists gtk4-layer-shell-0 && echo gtk4-layer-shell-0 || echo gtk-layer-shell-0)
LSH_CFLAGS  := $(shell $(PKG_CONF) --cflags $(LSH_PKG) | sed 's/-I/-isystem /g')
LSH_LIBS    := $(shell $(PKG_CONF) --libs $(LSH_PKG))
SV_CFLAGS   := $(shell $(PKG_CONF) --cflags gtksourceview-5 | sed 's/-I/-isystem /g')
SV_LIBS     := $(shell $(PKG_CONF) --libs gtksourceview-5)

BUILD ?= release

ifeq ($(BUILD),debug)
  OPT_FLAGS  := -Og -ggdb3
  WARN_FLAGS := -Wall -Wextra -pedantic
  APP_ID     := com.toxdes.promptr-debug
  DEBUG_FLAGS  := -DDEBUG_BUILD
  VER_SUFFIX := -debug
else
  OPT_FLAGS  := -O3
  WARN_FLAGS := -Wall -Wextra -Werror -pedantic
  APP_ID     := com.toxdes.promptr
  DEBUG_FLAGS  :=
  VER_SUFFIX :=
endif

VERSION := $(shell cat VERSION)$(VER_SUFFIX)
TARGET  := promptr$(VER_SUFFIX)

CFLAGS  = -std=c11 $(WARN_FLAGS) $(OPT_FLAGS) -I. -Isrc $(GTK_CFLAGS) $(LSH_CFLAGS) $(SV_CFLAGS) $(JSON_CFLAGS) $(SECRET_CFLAGS) -DVERSION=\"$(VERSION)\" -DAPP_ID=\"$(APP_ID)\" -DPROMPTR_PLUGIN_DIR_DEFAULT=\"$(PLUGINSDIR)\" $(DEBUG_FLAGS)
LDFLAGS := $(GTK_LIBS) $(LSH_LIBS) $(SV_LIBS) $(JSON_LIBS) $(SECRET_LIBS)

SRCDIR   := src
BUILDDIR := build/$(BUILD)

PREFIX    ?= /usr/local
BINDIR     = $(PREFIX)/bin
DATADIR    = $(PREFIX)/share
ICONDIR    = $(DATADIR)/icons/hicolor/scalable/apps
APPDIR     = $(DATADIR)/applications
PLUGINSDIR = $(PREFIX)/lib/promptr/plugins

SOURCES := $(wildcard $(SRCDIR)/*.c)
JSONRPC_SRC := lib/promptr-protocol/jsonrpc.c
JSONRPC_OBJ := $(BUILDDIR)/jsonrpc.o
OBJECTS := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES)) $(JSONRPC_OBJ)
DEPS    := $(OBJECTS:.o=.d)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -MMD -MP -MF $(BUILDDIR)/$*.d -c $< -o $@

$(JSONRPC_OBJ): $(JSONRPC_SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -MMD -MP -MF $(BUILDDIR)/jsonrpc.d -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR) $(BUILDDIR)/providers

-include $(DEPS)

clean:
	rm -rf build dist promptr promptr-debug
	@for d in $(PROVIDERS_DIR)/promptr-*/; do \
	  if [ -d "$$d" ]; then $(MAKE) -C "$$d" clean; fi; \
	done

PROVIDERS_DIR := providers

PROVIDER_BINS := \
	$(PROVIDERS_DIR)/promptr-opencode/promptr-opencode \
	$(PROVIDERS_DIR)/promptr-openrouter/promptr-openrouter

$(PROVIDERS_DIR)/promptr-opencode/promptr-opencode:
	$(MAKE) -C $(PROVIDERS_DIR)/promptr-opencode

$(PROVIDERS_DIR)/promptr-openrouter/promptr-openrouter:
	$(MAKE) -C $(PROVIDERS_DIR)/promptr-openrouter

providers:
	$(MAKE) -C providers/promptr-opencode BUILD=$(BUILD)
	$(MAKE) -C providers/promptr-openrouter BUILD=$(BUILD)

install: $(TARGET) providers
	install -D -m755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -D -m644 data/promptr.svg $(DESTDIR)$(ICONDIR)/promptr.svg
	install -D -m644 com.toxdes.promptr.desktop $(DESTDIR)$(APPDIR)/com.toxdes.promptr.desktop
	rm -f $(DESTDIR)$(APPDIR)/promptr.desktop

	# Install provider plugins
	for p in opencode openrouter; do \
	  dir="$(DESTDIR)$(PLUGINSDIR)/$$p"; \
	  mkdir -p "$$dir"; \
	  install -m755 "$(PROVIDERS_DIR)/promptr-$$p/promptr-$$p" "$$dir/"; \
	  install -m644 "$(PROVIDERS_DIR)/promptr-$$p/plugin.json" "$$dir/"; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -rf $(DESTDIR)$(PLUGINSDIR)

.PHONY: clean install uninstall debug release r config providers test

test:
	python3 tests/run.py

debug:
	$(MAKE) BUILD=debug
	$(MAKE) BUILD=debug providers

release:
	$(MAKE) BUILD=release

r:
	$(MAKE) clean
	$(MAKE) BUILD=debug
	$(MAKE) BUILD=debug providers
	PROMPTR_PLUGIN_DIR=providers ./promptr-debug

config:
	python3 tests/check_config.py
