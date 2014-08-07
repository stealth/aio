CC=cc
CFLAGS=-Wall -O2
C99=gcc -std=c99

all: aio.o

test: aio.o test.c test2.c
	$(CC) $(CFLAGS) test.c aio.o -o test
	$(CC) $(CFLAGS) test2.c aio.o -o test2

aio.o: aio.c
	$(C99) $(CFLAGS) -c aio.c

clean:
	rm -rf aio.o test test2

