CC=cc
CFLAGS=-Wall -O2
C99=gcc -std=c99

all: aio.o

test: aio.o test/test.o test/test2.o
	$(CC) $(CFLAGS) test/test.c aio.o -o test/test
	$(CC) $(CFLAGS) test/test2.c aio.o -o test/test2

aio.o: aio.c
	$(C99) $(CFLAGS) -c aio.c

clean:
	rm -rf aio.o test/test test/test2

