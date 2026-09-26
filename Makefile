CC ?= gcc
CFLAGS ?= -std=c99 -O3 -Wall -Wextra -Wpedantic -Werror -Iinclude

SRCS = src/arena.c src/interval.c src/lexer.c src/parser.c src/diagnostic.c src/sepe.c src/backend.c src/oir.c src/lower.c src/polyglot.c src/ingest.c src/main.c
OBJS = $(SRCS:.c=.o)
TARGET = oath

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) __oath_*

.PHONY: all clean