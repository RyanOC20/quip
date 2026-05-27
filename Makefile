all: quip

quip: quip.c
	$(CC) -o quip quip.c -Wall -W -pedantic -std=c99

clean:
	rm quip
