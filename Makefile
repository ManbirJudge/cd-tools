CC := g++
BUILD ?= debug

SRC := $(wildcard *.cpp)
TARGET := cd-tools

ifeq ($(BUILD),debug)
	CFLAGS := -g -O0
else
	CFLAGS := -O2 -DNDEBUG -march=native
endif
CFLAGS += -Wall -Wextra -std=c++20

BUILD_DIR := build/$(BUILD)
OBJ_DIR := $(BUILD_DIR)/obj

OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SRC))

$(BUILD_DIR)/$(TARGET): $(OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJ) -o $@

$(OBJ): $(SRC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

debug:
	$(MAKE) BUILD=debug

release:
	$(MAKE) BUILD=release

clean:
	rm -r build
