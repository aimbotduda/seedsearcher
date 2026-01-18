CC = gcc
CFLAGS = -Wall -Wextra -O3 -flto -march=native
LDFLAGS = -lm -pthread -flto

.PHONY: all clean debug

all: groupfinder

debug: CFLAGS = -Wall -Wextra -O0 -ggdb3 -DDEBUG
debug: groupfinder

groupfinder: groupfinder.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f groupfinder
