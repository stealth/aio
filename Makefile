CC=cc
CFLAGS=-Wall -O2
C99=gcc -std=c99

all: aio.o

test: aio.o test/test.o test/test2.o test/test3.o
	$(CC) $(CFLAGS) test/test.c aio.o -o test/test
	$(CC) $(CFLAGS) test/test2.c aio.o -o test/test2
	$(CC) $(CFLAGS) test/test3.c aio.o -o test/test3


aio.o: aio.c
	$(C99) $(CFLAGS) -c aio.c

clean:
	rm -rf aio.o test/test test/test2 test/test3 test/*.o

