CC = gcc
CFLAGS = -Wall -Wextra -pthread

BIN_DIR = bin
TARGET = $(BIN_DIR)/systeme

.PHONY: all run clean

all: $(TARGET)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(TARGET): systeme.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ systeme.c

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BIN_DIR)
