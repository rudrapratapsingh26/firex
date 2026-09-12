# Host build for the fusion engine: unit tests and the shared library the
# Python simulator loads. Nothing here needs ESP-IDF or a board.
#
#   make test   run the Unity tests
#   make sim    build the harness tools/simulator drives
#   make lib    build a shared library (optional; the simulator uses the harness)
#   make all    tests and harness
#
# The ESP-IDF component build (idf.py) arrives in M1; this Makefile stays for CI.

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2
INC     := -Icomponents/fusion/include
SRC     := components/fusion/fusion.c
BUILD   := build

ifeq ($(OS),Windows_NT)
  LIBEXT := .dll
  EXEEXT := .exe
else
  LIBEXT := .so
  EXEEXT :=
endif

LIB  := $(BUILD)/libfusion$(LIBEXT)
TEST := $(BUILD)/test_fusion$(EXEEXT)
SIM  := $(BUILD)/fusion_cli$(EXEEXT)

.PHONY: all test lib sim clean

all: sim test

$(BUILD):
	@mkdir -p $(BUILD) 2>/dev/null || true

lib: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -shared -fPIC $(SRC) -o $(LIB)

$(TEST): | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -Itest/unity -DUNITY_OUTPUT_COLOR \
		$(SRC) test/unity/unity.c test/test_fusion.c -o $(TEST)

$(SIM): | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(SRC) sim/fusion_cli.c -o $(SIM)

sim: $(SIM)

test: $(TEST)
	@$(TEST)

clean:
	rm -rf $(BUILD)
