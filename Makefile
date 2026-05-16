CC      ?= gcc
PKG_CONF := pkg-config

GTK_CFLAGS  := $(shell $(PKG_CONF) --cflags gtk4 | sed 's/-I/-isystem /g')
GTK_LIBS    := $(shell $(PKG_CONF) --libs gtk4)
LSH_PKG := $(shell $(PKG_CONF) --exists gtk4-layer-shell-0 && echo gtk4-layer-shell-0 || echo gtk-layer-shell-0)
LSH_CFLAGS  := $(shell $(PKG_CONF) --cflags $(LSH_PKG) | sed 's/-I/-isystem /g')
LSH_LIBS    := $(shell $(PKG_CONF) --libs $(LSH_PKG))
SV_CFLAGS   := $(shell $(PKG_CONF) --cflags gtksourceview-5 | sed 's/-I/-isystem /g')
SV_LIBS     := $(shell $(PKG_CONF) --libs gtksourceview-5)

CFLAGS  := -std=c11 -pedantic -Wall -Wextra -Werror -O2 -I. $(GTK_CFLAGS) $(LSH_CFLAGS) $(SV_CFLAGS)
LDFLAGS := $(GTK_LIBS) $(LSH_LIBS) $(SV_LIBS)

SRCDIR   := src
BUILDDIR := build
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
	rm -rf $(BUILDDIR) $(TARGET)

install: $(TARGET)
	install -D -m755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -D -m644 data/promptr.svg $(DESTDIR)$(ICONDIR)/promptr.svg
	install -D -m644 promptr.desktop $(DESTDIR)$(APPDIR)/promptr.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

.PHONY: clean install uninstall
