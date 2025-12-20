CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -std=c11 -Iinclude `pkg-config --cflags glfw3`
CXXFLAGS = -Wall -Wextra -std=c++17 -Iinclude `pkg-config --cflags glfw3`
LDFLAGS = `pkg-config --libs glfw3` -lGL -lGLU -lm
SRC = src/main.c
OBJ = build/main.o
TARGET = build/sandbox
MULTI_TARGET = build/multiplayer

all: $(TARGET) $(MULTI_TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

build/main.o: src/main.c
	$(CC) $(CFLAGS) -c src/main.c -o build/main.o

$(MULTI_TARGET): build/multiplayer.o
	$(CXX) build/multiplayer.o -o $(MULTI_TARGET) $(LDFLAGS)

build/multiplayer.o: src/multiplayer.cpp include/net_common.hpp include/net_host.hpp include/net_client.hpp
	$(CXX) $(CXXFLAGS) -c src/multiplayer.cpp -o build/multiplayer.o

clean:
	rm -f build/*

run: $(TARGET)
	./$(TARGET)

run-host: $(MULTI_TARGET)
	./$(MULTI_TARGET) host

run-client: $(MULTI_TARGET)
	./$(MULTI_TARGET) client 127.0.0.1

# Test target
TEST_TARGET = build/net_test

$(TEST_TARGET): build/net_test.o
	$(CXX) build/net_test.o -o $(TEST_TARGET) -lpthread

build/net_test.o: src/net_test.cpp include/net_common.hpp include/net_host.hpp include/net_client.hpp
	$(CXX) $(CXXFLAGS) -c src/net_test.cpp -o build/net_test.o

test: $(TEST_TARGET)
	./$(TEST_TARGET)

.PHONY: all clean run run-host run-client test
