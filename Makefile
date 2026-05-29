# Pulse -- multiplayer sandbox with a UDP netcode layer.
#
#   make            build server, client, and tests
#   make server     headless authoritative server         -> build/server
#   make client     OpenGL client (prediction/interp/HUD)  -> build/client
#   make tests      build the test harness binaries
#   make test       build AND run the test harness (headless, CI-friendly)
#   make run-server / run-client
#   make clean

CC     ?= gcc
CSTD   := -std=gnu11
WARN   := -Wall -Wextra
OPT    := -O2
INC    := -Iinclude
CFLAGS := $(CSTD) $(WARN) $(OPT) $(INC)

# The client needs OpenGL/GLFW; the server and tests are deliberately
# graphics-free so the netcode is verifiable on a headless machine.
GLFW_CFLAGS := $(shell pkg-config --cflags glfw3)
GLFW_LIBS   := $(shell pkg-config --libs glfw3) -lGL -lGLU -lm

BUILD  := build
# Shared, graphics-free core compiled into every binary.
CORE   := src/net.c src/world.c

.PHONY: all server client tests test run-server run-client clean

all: server client tests

$(BUILD):
	mkdir -p $(BUILD)

server: | $(BUILD)
	$(CC) $(CFLAGS) src/server.c $(CORE) -o $(BUILD)/server -lm

client: | $(BUILD)
	$(CC) $(CFLAGS) $(GLFW_CFLAGS) src/client.c src/camera.c $(CORE) -o $(BUILD)/client $(GLFW_LIBS)

tests: | $(BUILD)
	$(CC) $(CFLAGS) tests/test_reliability.c $(CORE) -o $(BUILD)/test_reliability -lm
	$(CC) $(CFLAGS) tests/test_loopback.c    $(CORE) -o $(BUILD)/test_loopback    -lm

test: tests
	@echo "== reliability unit tests =="
	@$(BUILD)/test_reliability
	@echo
	@echo "== loopback integration tests =="
	@$(BUILD)/test_loopback

run-server: server
	$(BUILD)/server

run-client: client
	$(BUILD)/client 127.0.0.1

clean:
	rm -rf $(BUILD)
