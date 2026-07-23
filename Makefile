CC      ?= cc
PKG     ?= pkg-config
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man/man1

DBUS_CFLAGS := $(shell $(PKG) --cflags dbus-1)
DBUS_LIBS   := $(shell $(PKG) --libs dbus-1)

CFLAGS  ?= -O2 -g
CFLAGS  += -D_GNU_SOURCE -std=c11 -Wall -Wextra -Wpedantic $(DBUS_CFLAGS)
LDLIBS  += $(DBUS_LIBS)

BIN := caffeinate
SRC := caffeinate.c

.PHONY: all clean install uninstall check

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

# Quick smoke test: hold for 1s and confirm clean exit.
check: $(BIN)
	./$(BIN) -v -t 1
	@echo "OK: exited cleanly"

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -m 0644 caffeinate.1 $(DESTDIR)$(MANDIR)/caffeinate.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(MANDIR)/caffeinate.1

clean:
	rm -f $(BIN)
