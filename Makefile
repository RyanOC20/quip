all: quip

quip: quip.c
	$(CC) -Os -o quip quip.c -Wall -W -pedantic -std=c99
	strip quip

clean:
	rm quip
