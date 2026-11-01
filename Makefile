CC      ?= cc
CFLAGS   = -std=c99 -Wall -Wextra -Werror -pedantic -O2 -Iinclude
SRC      = src/lexer.c src/ast.c src/parser.c src/cimport.c src/modules.c src/pkg.c src/sema.c src/borrow.c src/codegen.c src/driver.c src/main.c src/diag.c
OBJ      = $(SRC:.c=.o)
USE_LIBTCC ?= 1

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
LIBDIR   ?= $(PREFIX)/lib
STDLIBDIR = $(LIBDIR)/std

TCC_DIR   = vendor/tinycc
TCC_STAMP = $(TCC_DIR)/.zov-built
TCC_LIB   = $(TCC_DIR)/libtcc.a

ifdef USE_LIBTCC
LDLIBS   += $(TCC_LIB) -ldl -lm -lpthread
CFLAGS   += -DUSE_LIBTCC -I$(TCC_DIR)
endif

all: zovc zov-lsp

# dogfood: zovc compiles its own language server
zov-lsp: zovc tools/zov-lsp/main.zov
	./zovc tools/zov-lsp/main.zov -o zov-lsp

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

test: zovc zov-lsp
	sh tests/run.sh

# Install binaries and standard library system-wide.
install: zovc zov-lsp
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "Error: installation requires root privileges." >&2; \
		echo "Please run: sudo make install" >&2; \
		exit 1; \
	fi
	install -d "$(BINDIR)" "$(STDLIBDIR)"
	install -m 755 zovc "$(BINDIR)/zovc"
	install -m 755 zov-lsp "$(BINDIR)/zov-lsp"
	cp -R lib/std/. "$(STDLIBDIR)/"
	@echo "Zov installed successfully to $(PREFIX)"

clean:
	rm -f zovc zov-lsp $(OBJ)

.PHONY: all lsp test clean install