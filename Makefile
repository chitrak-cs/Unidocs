CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -pedantic -std=c99 -O2

cli_editor: cli_editor.c
	$(CC) $(CFLAGS) -o $@ $<

all: cli_editor

clean:
	rm -f cli_editor

.PHONY: all clean
