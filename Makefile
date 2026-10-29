PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
LANGDIR ?= $(PREFIX)/share/quip/langs

all: quip

quip: quip.c
	$(CC) -Os -o quip quip.c -Wall -W -pedantic -std=c99
	strip quip

# quip finds its language files relative to its own binary, as
# $(BINDIR)/../share/quip/langs, so LANGDIR must stay beside BINDIR.
install: quip
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 quip $(DESTDIR)$(BINDIR)/quip
	install -d $(DESTDIR)$(LANGDIR)
	install -m 644 langs/*.lang $(DESTDIR)$(LANGDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/quip
	rm -f $(addprefix $(DESTDIR)$(LANGDIR)/,$(notdir $(wildcard langs/*.lang)))
	-rmdir $(DESTDIR)$(LANGDIR) $(DESTDIR)$(PREFIX)/share/quip 2>/dev/null

clean:
	rm -f quip

.PHONY: all install uninstall clean
