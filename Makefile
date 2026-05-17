CC      ?= gcc
PKG_CONF := pkg-config

GTK_CFLAGS  := $(shell $(PKG_CONF) --cflags gtk4 | sed 's/-I/-isystem /g')
GTK_LIBS    := $(shell $(PKG_CONF) --libs gtk4)
LSH_PKG := $(shell $(PKG_CONF) --exists gtk4-layer-shell-0 && echo gtk4-layer-shell-0 || echo gtk-layer-shell-0)
LSH_CFLAGS  := $(shell $(PKG_CONF) --cflags $(LSH_PKG) | sed 's/-I/-isystem /g')
LSH_LIBS    := $(shell $(PKG_CONF) --libs $(LSH_PKG))
SV_CFLAGS   := $(shell $(PKG_CONF) --cflags gtksourceview-5 | sed 's/-I/-isystem /g')
SV_LIBS     := $(shell $(PKG_CONF) --libs gtksourceview-5)

BUILD ?= debug

ifeq ($(BUILD),debug)
  OPT_FLAGS  := -Og -ggdb3
  WARN_FLAGS := -Wall -Wextra -pedantic
  VER_SUFFIX := -debug
else
  OPT_FLAGS  := -O2
  WARN_FLAGS := -Wall -Wextra -Werror -pedantic
  VER_SUFFIX :=
endif

VERSION := $(shell cat VERSION)$(VER_SUFFIX)

CFLAGS  := -std=c11 $(WARN_FLAGS) $(OPT_FLAGS) -I. $(GTK_CFLAGS) $(LSH_CFLAGS) $(SV_CFLAGS) -DVERSION=\"$(VERSION)\"
LDFLAGS := $(GTK_LIBS) $(LSH_LIBS) $(SV_LIBS)

SRCDIR   := src
BUILDDIR := build/$(BUILD)
TARGET   := promptr

PREFIX ?= /usr/local
BINDIR  = $(PREFIX)/bin
DATADIR = $(PREFIX)/share
ICONDIR = $(DATADIR)/icons/hicolor/scalable/apps
APPDIR  = $(DATADIR)/applications

SOURCES := $(wildcard $(SRCDIR)/*.c)
OBJECTS := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES))
DEPS    := $(OBJECTS:.o=.d)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -MMD -MP -MF $(BUILDDIR)/$*.d -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

-include $(DEPS)

clean:
	rm -rf build dist $(TARGET)

install: $(TARGET)
	install -D -m755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -D -m644 data/promptr.svg $(DESTDIR)$(ICONDIR)/promptr.svg
	install -D -m644 promptr.desktop $(DESTDIR)$(APPDIR)/promptr.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

.PHONY: clean install uninstall debug release

debug:
	$(MAKE) BUILD=debug

release:
	$(MAKE) BUILD=release
