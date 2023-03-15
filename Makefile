CC=gcc
OUTPUT=server.out

build:
	$(CC) -ansi server.c -lm -lcrypt -Wall -D_DEFAULT_SOURCE -o $(OUTPUT)

test: build
	./$(OUTPUT)
