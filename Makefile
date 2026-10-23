CC      ?= cc
CFLAGS   = -std=c99 -Wall -Wextra -Werror -pedantic -O2 -Iinclude
SRC      = src/lexer.c src/ast.c src/parser.c src/sema.c src/codegen.c src/driver.c src/main.c
OBJ      = $(SRC:.c=.o)

ifdef USE_LIBTCC
CFLAGS  += -DUSE_LIBTCC
LDLIBS  += -ltcc
endif

all: zovc

zovc: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c include/common.h include/token.h include/ast.h
	$(CC) $(CFLAGS) -c -o $@ $<

test: zovc
	sh tests/run.sh

clean:
	rm -f zovc $(OBJ)

.PHONY: all test clean
