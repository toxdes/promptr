CC      ?= gcc
PKG_CONF := pkg-config

GTK_CFLAGS := $(shell $(PKG_CONF) --cflags gtk4 | sed 's/-I/-isystem /g')
GTK_LIBS   := $(shell $(PKG_CONF) --libs gtk4)

CFLAGS  := -std=c11 -pedantic -Wall -Wextra -Werror -O2 -I. $(GTK_CFLAGS)
LDFLAGS := $(GTK_LIBS)

SRCDIR   := src
BUILDDIR := build
TARGET   := promptr

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

.PHONY: clean
