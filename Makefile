CC=gcc
OUTPUT=server.out

build:
	$(CC) -ansi server.c -lm -o $(OUTPUT)

test: build
	./$(OUTPUT)
