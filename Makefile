CC      ?= cc
CFLAGS   = -std=c99 -Wall -Wextra -Werror -pedantic -O2 -Iinclude
SRC      = src/lexer.c src/ast.c src/parser.c src/cimport.c src/modules.c src/pkg.c src/sema.c src/borrow.c src/codegen.c src/driver.c src/main.c
OBJ      = $(SRC:.c=.o)
USE_LIBTCC ?= 1

TCC_DIR   = vendor/tinycc
TCC_STAMP = $(TCC_DIR)/.zov-built
TCC_LIB   = $(TCC_DIR)/libtcc.a

ifdef USE_LIBTCC
LDLIBS   += $(TCC_LIB) -ldl -lm -lpthread
CFLAGS   += -DUSE_LIBTCC -I$(TCC_DIR)
endif

all: zovc

# Upstream tcc builds with relaxed flags; zovc's strict flags do not apply.
$(TCC_STAMP):
	cd $(TCC_DIR) && ./configure --prefix=/usr >/dev/null \
	  && sed -i 's/^CFLAGS=.*/CFLAGS=-w -O2/' config.mak \
	  && $(MAKE) libtcc.a libtcc1.a >/dev/null
	touch $@

zovc: $(OBJ) $(TCC_STAMP)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c include/common.h include/token.h include/ast.h $(TCC_STAMP)
	$(CC) $(CFLAGS) -c -o $@ $<

test: zovc
	sh tests/run.sh

clean:
	rm -f zovc $(OBJ)

.PHONY: all test clean
