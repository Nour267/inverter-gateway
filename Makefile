CC     = gcc
CFLAGS = -Wall -Wextra -std=c11 -g

all: build/hello build/inverter_sim build/gateway

# Gateway (Linux only): main loop + all the common logic + the SocketCAN bus layer
GW_SRCS = src/gateway/main.c src/common/can_messages.c src/common/protocol.c \
          src/common/crc16.c src/bus/bus_socketcan.c

build/gateway: $(GW_SRCS) include/bus.h include/can_messages.h include/protocol.h include/crc16.h
	mkdir -p build
	$(CC) $(CFLAGS) -Iinclude $(GW_SRCS) -o build/gateway

# Inverter simulator (Linux only: uses SocketCAN). -lm = link the math library (sin)
SIM_SRCS = src/sim/inverter_sim.c src/common/can_messages.c src/bus/bus_socketcan.c

build/inverter_sim: $(SIM_SRCS) include/bus.h include/can_messages.h
	mkdir -p build
	$(CC) $(CFLAGS) -Iinclude $(SIM_SRCS) -lm -o build/inverter_sim

# Link: .o -> program
build/hello: build/hello.o
	$(CC) build/hello.o -o build/hello

# Compile: .c -> .o
build/hello.o: src/hello.c
	mkdir -p build
	$(CC) $(CFLAGS) -c src/hello.c -o build/hello.o

# Unit tests: each test program = test file + code under test + Unity
TEST_FLAGS = $(CFLAGS) -Iinclude -Itests/unity

CAN_TEST_SRCS = tests/test_can_messages.c src/common/can_messages.c tests/unity/unity.c
CRC_TEST_SRCS = tests/test_crc16.c src/common/crc16.c tests/unity/unity.c
PROTO_TEST_SRCS = tests/test_protocol.c src/common/protocol.c src/common/crc16.c tests/unity/unity.c

build/test_can_messages: $(CAN_TEST_SRCS) include/can_messages.h
	mkdir -p build
	$(CC) $(TEST_FLAGS) $(CAN_TEST_SRCS) -o build/test_can_messages

build/test_crc16: $(CRC_TEST_SRCS) include/crc16.h
	mkdir -p build
	$(CC) $(TEST_FLAGS) $(CRC_TEST_SRCS) -o build/test_crc16

build/test_protocol: $(PROTO_TEST_SRCS) include/protocol.h include/crc16.h
	mkdir -p build
	$(CC) $(TEST_FLAGS) $(PROTO_TEST_SRCS) -o build/test_protocol

test: build/test_can_messages build/test_crc16 build/test_protocol
	./build/test_can_messages
	./build/test_crc16
	./build/test_protocol

clean:
	rm -rf build

.PHONY: all test clean
