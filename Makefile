CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
LDLIBS = -lglut -lGL -lm
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
INSTALL ?= install

.PHONY: all
all: neural-topology lsm

neural-topology: main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

lsm: monitor.c metrics.c metrics.h sampler.c sampler.h memory.c memory.h topology.c topology.h explore.c explore.h interaction.c interaction.h manual.c manual.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(shell pkg-config --cflags freetype2) $(LDFLAGS) -o $@ monitor.c metrics.c sampler.c memory.c topology.c explore.c interaction.c manual.c $(LDLIBS) $(shell pkg-config --libs freetype2) -ldl -pthread

.PHONY: check clean install
check: neural-topology lsm
	./neural-topology --check
	./lsm --check

install: lsm lsm.1
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(MANDIR)/man1"
	$(INSTALL) -m 755 lsm "$(DESTDIR)$(BINDIR)/lsm"
	$(INSTALL) -m 644 lsm.1 "$(DESTDIR)$(MANDIR)/man1/lsm.1"

clean:
	rm -f neural-topology lsm
