CC ?= gcc
CFLAGS ?= -std=c99 -O3 -Wall -Wextra -Wpedantic -Werror -Iinclude

SRCS = src/arena.c src/interval.c src/verifier.c src/backend.c src/main.c
OBJS = $(SRCS:.c=.o)
TARGET = oath-htse

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean