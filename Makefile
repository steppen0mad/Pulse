CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -Iinclude `pkg-config --cflags glfw3`
LDFLAGS = `pkg-config --libs glfw3` -lGL -lGLU -lm
SRC = src/main.c
OBJ = build/main.o
TARGET = build/sandbox

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

build/main.o: src/main.c
	$(CC) $(CFLAGS) -c src/main.c -o build/main.o

clean:
	rm -f build/*

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
