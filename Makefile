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

# Unit tests: test file + code under test + Unity, built into one program
TEST_SRCS = tests/test_can_messages.c src/common/can_messages.c tests/unity/unity.c

build/test_can_messages: $(TEST_SRCS) include/can_messages.h
	mkdir -p build
	$(CC) $(CFLAGS) -Iinclude -Itests/unity $(TEST_SRCS) -o build/test_can_messages

test: build/test_can_messages
	./build/test_can_messages

clean:
	rm -rf build

.PHONY: all test clean
