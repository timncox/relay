# Native tests for the relay core. `make -C test` runs them.
CC      ?= cc
CFLAGS  ?= -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD   := build

all: $(BUILD)/test_relay
	./$(BUILD)/test_relay

$(BUILD)/test_relay: test_relay.c ../core/relay.c ../core/relay.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../core -o $@ test_relay.c ../core/relay.c -lm

clean:
	rm -rf $(BUILD)

.PHONY: all clean
