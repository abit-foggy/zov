CC      ?= cc
CFLAGS   = -std=c99 -Wall -Wextra -Werror -pedantic -O2 -Iinclude
SRC      = src/lexer.c src/ast.c src/parser.c src/cimport.c src/modules.c src/pkg.c src/sema.c src/borrow.c src/codegen.c src/driver.c src/main.c src/diag.c
OBJ      = $(SRC:.c=.o)
USE_LIBTCC ?= 1

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
LIBDIR   ?= $(PREFIX)/lib
INCDIR   ?= $(PREFIX)/include
STDLIBDIR = $(LIBDIR)/std
RUNTIMEDIR = $(INCDIR)/runtime

TCC_DIR   = vendor/tinycc
TCC_STAMP = $(TCC_DIR)/.zov-built
TCC_LIB   = $(TCC_DIR)/libtcc.a

ifdef USE_LIBTCC
LDLIBS   += $(TCC_LIB) -ldl -lm -lpthread
CFLAGS   += -DUSE_LIBTCC -I$(TCC_DIR)
endif

all: zovc zov-lsp sklad

# dogfood: zovc compiles its own language server
zov-lsp: zovc tools/zov-lsp/main.zov
	./zovc tools/zov-lsp/main.zov -o zov-lsp

# dogfood: zovc compiles its own package and toolchain manager
sklad: zovc tools/sklad/main.zov
	./zovc tools/sklad/main.zov -o sklad

lsp: zov-lsp

# Upstream tcc builds with relaxed flags; zovc's strict flags do not apply.
$(TCC_STAMP):
	cd $(TCC_DIR) && ./configure --prefix=/usr >/dev/null \
	  && sed -i.bak 's/^CFLAGS=.*/CFLAGS=-w -O2/' config.mak \
	  && rm -f config.mak.bak \
	  && $(MAKE) libtcc.a libtcc1.a >/dev/null
	touch $@

zovc: $(OBJ) $(TCC_STAMP)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c include/common.h include/token.h include/ast.h $(TCC_STAMP)
	$(CC) $(CFLAGS) -c -o $@ $<

test: zovc zov-lsp sklad
	sh tests/run.sh

# Install binaries and standard library system-wide or to staged PREFIX.
install: zovc zov-lsp sklad
	install -d "$(BINDIR)" "$(STDLIBDIR)" "$(RUNTIMEDIR)"
	install -m 755 zovc "$(BINDIR)/zovc"
	install -m 755 sklad "$(BINDIR)/sklad"
	install -m 755 zov-lsp "$(BINDIR)/zov-lsp"
	cp -R lib/std/. "$(STDLIBDIR)/"
	cp -R runtime/. "$(RUNTIMEDIR)/"
	@echo "Zov installed successfully to $(PREFIX)"

clean:
	rm -f zovc zov-lsp sklad $(OBJ) .zov-test-bin

.PHONY: all lsp test clean install