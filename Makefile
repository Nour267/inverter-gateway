CC     = gcc
CFLAGS = -Wall -Wextra -std=c11 -g

all: build/hello

# Link: .o -> program
build/hello: build/hello.o
	$(CC) build/hello.o -o build/hello

# Compile: .c -> .o
build/hello.o: src/hello.c
	mkdir -p build
	$(CC) $(CFLAGS) -c src/hello.c -o build/hello.o

clean:
	rm -rf build

.PHONY: all clean
