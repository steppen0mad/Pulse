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
# -ffp-contract=off keeps floating-point results bit-identical between the server
# build and the cffi training shared library (no FMA contraction), which is what
# makes the trained policy behave the same in training and at deployment.
FPFLAGS := -ffp-contract=off
CFLAGS := $(CSTD) $(WARN) $(OPT) $(FPFLAGS) $(INC)

# The client needs OpenGL/GLFW; the server and tests are deliberately
# graphics-free so the netcode is verifiable on a headless machine.
GLFW_CFLAGS := $(shell pkg-config --cflags glfw3)
GLFW_LIBS   := $(shell pkg-config --libs glfw3) -lGL -lGLU -lm

BUILD  := build
# Shared, graphics-free core compiled into every binary.
CORE   := src/net.c src/world.c
# AI agent layer: the controller, the dependency-free inference, and the SHARED
# observation/decode unit (also compiled into the training .so via cffi).
AI     := src/agent.c src/policy.c src/agent_obs.c

.PHONY: all server client tests test run-server run-client clean sim-lib train export parity

# ---- AI training workflow (requires the Python venv: numpy, cffi, torch) ----
PYTHON ?= python

# Compile the C sim+perception into the cffi extension the training env imports.
sim-lib:
	$(PYTHON) training/build_sim.py

# Train a policy (navigation by default). Long-running; see training/ppo.py args.
train: sim-lib
	$(PYTHON) training/ppo.py

# Export the trained PyTorch policy to policy.bin + refresh the C parity fixtures.
export:
	$(PYTHON) training/export.py

# Regenerate fixtures and run the C parity test against them.
parity: export tests
	@$(BUILD)/test_policy

all: server client tests

$(BUILD):
	mkdir -p $(BUILD)

server: | $(BUILD)
	$(CC) $(CFLAGS) src/server.c $(CORE) $(AI) -o $(BUILD)/server -lm

client: | $(BUILD)
	$(CC) $(CFLAGS) $(GLFW_CFLAGS) src/client.c src/camera.c $(CORE) -o $(BUILD)/client $(GLFW_LIBS)

tests: | $(BUILD)
	$(CC) $(CFLAGS) tests/test_reliability.c $(CORE) -o $(BUILD)/test_reliability -lm
	$(CC) $(CFLAGS) tests/test_loopback.c    $(CORE) -o $(BUILD)/test_loopback    -lm
	$(CC) $(CFLAGS) tests/test_obs.c src/agent_obs.c -o $(BUILD)/test_obs -lm
	$(CC) $(CFLAGS) tests/test_policy.c src/policy.c src/agent_obs.c -o $(BUILD)/test_policy -lm
	$(CC) $(CFLAGS) tests/test_loopback_agents.c $(CORE) $(AI) -o $(BUILD)/test_loopback_agents -lm
	$(CC) $(CFLAGS) tests/bench_agent.c src/agent_obs.c src/policy.c -o $(BUILD)/bench_agent -lm

test: tests
	@echo "== reliability unit tests =="
	@$(BUILD)/test_reliability
	@echo
	@echo "== loopback integration tests =="
	@$(BUILD)/test_loopback
	@echo
	@echo "== observation + decode unit tests =="
	@$(BUILD)/test_obs
	@echo
	@echo "== C-vs-PyTorch policy parity tests =="
	@$(BUILD)/test_policy
	@echo
	@echo "== agent layer integration tests =="
	@$(BUILD)/test_loopback_agents
	@echo
	@echo "== agent decision microbenchmark =="
	@$(BUILD)/bench_agent

run-server: server
	$(BUILD)/server

run-client: client
	$(BUILD)/client 127.0.0.1

clean:
	rm -rf $(BUILD)
