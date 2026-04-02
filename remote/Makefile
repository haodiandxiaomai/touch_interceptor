CC ?= aarch64-linux-gnu-gcc
CFLAGS ?= -static -O2 -Wall

all: remote_touch

remote_touch: remote_touch.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f remote_touch

.PHONY: all clean
