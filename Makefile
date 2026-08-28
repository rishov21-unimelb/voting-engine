# voting-engine -- build system
#
#   make            build the optimised binary and static library
#   make debug      unoptimised build with debug info (SANITIZE=1 for ASan/UBSan)
#   make test       build and run the unit test suite
#   make valgrind   run the suite and a sample election under Valgrind
#   make install    install the binary, library and header under PREFIX
#   make clean      remove all build output
#
# SPDX-License-Identifier: MIT

CC       ?= cc
AR       ?= ar
PREFIX   ?= /usr/local

CSTD     := -std=c11
WARN     := -Wall -Wextra -Werror -pedantic \
            -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
            -Wold-style-definition -Wvla -Wpointer-arith -Wcast-qual
OPT      ?= -O2
CPPFLAGS += -Iinclude
CFLAGS   += $(CSTD) $(WARN) $(OPT)
LDLIBS   += -lm

# Windows toolchains want the suffix; everything else does not.
ifeq ($(OS),Windows_NT)
  EXE := .exe
else
  EXE :=
endif

BUILD_DIR ?= build
SRC_DIR   := src
TEST_DIR  := tests

LIB_SRCS  := $(filter-out $(SRC_DIR)/main.c,$(wildcard $(SRC_DIR)/*.c))
LIB_OBJS  := $(LIB_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
MAIN_OBJ  := $(BUILD_DIR)/main.o
TEST_OBJ  := $(BUILD_DIR)/test_voting.o

LIB       := $(BUILD_DIR)/libvoting.a
BIN       := $(BUILD_DIR)/vote$(EXE)
TEST_BIN  := $(BUILD_DIR)/test_voting$(EXE)

.PHONY: all debug test sanitize valgrind clean install uninstall help

all: $(BIN)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TEST_OBJ): $(TEST_DIR)/test_voting.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Everything but main() goes into the archive, so the same objects back
# the CLI, the test suite and any other front end.
$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(BIN): $(MAIN_OBJ) $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MAIN_OBJ) $(LIB) $(LDLIBS)

$(TEST_BIN): $(TEST_OBJ) $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_OBJ) $(LIB) $(LDLIBS)

# Recursive so the debug flags reach every object without stale mixing.
ifeq ($(SANITIZE),1)
  SAN := -fsanitize=address,undefined -fno-sanitize-recover=all
endif
DEBUG_FLAGS := -O0 -g3 -fno-omit-frame-pointer -DVOTE_DEBUG $(SAN)

debug:
	@$(MAKE) --no-print-directory BUILD_DIR=build/debug \
	         OPT="$(DEBUG_FLAGS)" LDFLAGS="$(LDFLAGS) $(SAN)" all
	@echo "built build/debug/vote$(EXE)"

test: $(TEST_BIN)
	@./$(TEST_BIN)

# The same suite under AddressSanitizer and UndefinedBehaviorSanitizer,
# in its own build directory so it never mixes with the normal objects.
SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all

sanitize:
	@$(MAKE) --no-print-directory BUILD_DIR=build/asan 	         OPT="-O1 -g3 -fno-omit-frame-pointer $(SAN_FLAGS)" 	         LDFLAGS="$(LDFLAGS) $(SAN_FLAGS)" test

# Zero leaks and zero invalid accesses are part of the build contract.
VALGRIND ?= valgrind --error-exitcode=1 --leak-check=full \
            --show-leak-kinds=all --track-origins=yes --errors-for-leak-kinds=all

valgrind: $(TEST_BIN) $(BIN)
	$(VALGRIND) ./$(TEST_BIN)
	$(VALGRIND) ./$(BIN) --color=never examples/election.txt
	$(VALGRIND) ./$(BIN) --system=borda --color=never examples/election.csv
	$(VALGRIND) ./$(BIN) --system=plurality --color=never \
	    --export=/dev/null examples/election.json

install: $(BIN) $(LIB)
	@mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib \
	          $(DESTDIR)$(PREFIX)/include
	cp $(BIN) $(DESTDIR)$(PREFIX)/bin/
	cp $(LIB) $(DESTDIR)$(PREFIX)/lib/
	cp include/voting.h $(DESTDIR)$(PREFIX)/include/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/vote$(EXE)
	rm -f $(DESTDIR)$(PREFIX)/lib/libvoting.a
	rm -f $(DESTDIR)$(PREFIX)/include/voting.h

clean:
	rm -rf build

help:
	@echo "targets: all debug test valgrind install uninstall clean"
