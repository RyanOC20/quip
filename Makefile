PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

all: quip

quip: quip.c
	$(CC) -Os -o quip quip.c -Wall -W -pedantic -std=c99
	strip quip

install: quip
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 quip $(DESTDIR)$(BINDIR)/quip

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/quip

clean:
	rm -f quip

.PHONY: all install uninstall clean
