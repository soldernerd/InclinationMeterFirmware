# Host unit tests for the firmware's pure-logic pieces.
#
# Needs a native C compiler (gcc / clang / zig cc) — NOT the arm-none-eabi one.
# On the dev box:  CC="python -m ziglang cc" make   (pip install ziglang). See README.md.
#
#   make            build + run every test
#   make test_math_crc && ./test_math_crc     one suite
#   CC=clang make    pick the compiler

CC     ?= cc
CFLAGS ?= -std=c17 -Wall -Wextra -Werror -O1 -g \
          -I. -I../Config -I../Math -I../Services -I../Drivers_App -I../HAL_App

SUITES := test_math_crc test_math_phasor test_math_window test_txframe test_transfer

.PHONY: all clean $(addprefix run-,$(SUITES))

all: $(addprefix run-,$(SUITES))

$(SUITES): %: %.c test.h
	$(CC) $(CFLAGS) -o $@ $<

run-%: %
	@./$<

clean:
	rm -f $(SUITES)
