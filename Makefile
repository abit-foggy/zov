CC      ?= cc
CFLAGS   = -std=c99 -Wall -Wextra -Werror -pedantic -O2 -Iinclude
SRC      = src/lexer.c src/ast.c src/parser.c src/cimport.c src/modules.c src/pkg.c src/sema.c src/borrow.c src/codegen.c src/driver.c src/main.c src/diag.c
OBJ      = $(SRC:.c=.o)
UNAME_S ?= $(shell uname -s)
UNAME_M ?= $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
  USE_LIBTCC ?= 0
else
  USE_LIBTCC ?= 1
endif

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
LIBDIR   ?= $(PREFIX)/lib
INCDIR   ?= $(PREFIX)/include
STDLIBDIR = $(LIBDIR)/std
RUNTIMEDIR = $(INCDIR)/runtime

TCC_DIR   = vendor/tinycc
TCC_STAMP = $(TCC_DIR)/.zov-built
TCC_LIB   = $(TCC_DIR)/libtcc.a

LDLIBS   += -lm
ifeq ($(USE_LIBTCC),1)
TCC_DEP   = $(TCC_STAMP)
LDLIBS   += $(TCC_LIB) -ldl -lpthread
CFLAGS   += -DUSE_LIBTCC -I$(TCC_DIR)
else
TCC_DEP   =
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

zovc: $(OBJ) $(TCC_DEP)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c include/common.h include/token.h include/ast.h $(TCC_DEP)
	$(CC) $(CFLAGS) -c -o $@ $<

test: zovc zov-lsp sklad
	sh tests/run.sh

# OS and architecture detection for release packaging
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
  DETECTED_OS = macos
else ifeq ($(UNAME_S),Linux)
  DETECTED_OS = linux
else
  DETECTED_OS = $(shell echo $(UNAME_S) | tr '[:upper:]' '[:lower:]')
endif

ifeq ($(UNAME_M),x86_64)
  DETECTED_ARCH = x86_64
else ifeq ($(UNAME_M),amd64)
  DETECTED_ARCH = x86_64
else ifeq ($(UNAME_M),aarch64)
  DETECTED_ARCH = aarch64
else ifeq ($(UNAME_M),arm64)
  DETECTED_ARCH = aarch64
else
  DETECTED_ARCH = $(UNAME_M)
endif

TARGET  ?= $(DETECTED_OS)-$(DETECTED_ARCH)
DISTDIR ?= dist

# Install only zovc compiler, zov-lsp, standard library, and runtime.
install-zovc: zovc zov-lsp
	install -d "$(BINDIR)" "$(STDLIBDIR)" "$(RUNTIMEDIR)"
	install -m 755 zovc "$(BINDIR)/zovc"
	install -m 755 zov-lsp "$(BINDIR)/zov-lsp"
	cp -R lib/std/. "$(STDLIBDIR)/"
	cp -R runtime/. "$(RUNTIMEDIR)/"
	@echo "zovc installed successfully to $(PREFIX)"

# Install only sklad package manager.
install-sklad: sklad
	install -d "$(BINDIR)"
	install -m 755 sklad "$(BINDIR)/sklad"
	@echo "sklad installed successfully to $(PREFIX)"

# Install both zovc and sklad.
install: install-zovc install-sklad
	@echo "Zov installed successfully to $(PREFIX)"

package-zovc: zovc zov-lsp
	@mkdir -p "$(DISTDIR)/staging-zovc"
	$(MAKE) install-zovc PREFIX="$(CURDIR)/$(DISTDIR)/staging-zovc"
	tar -czf "$(DISTDIR)/zovc-$(TARGET).tar.gz" -C "$(DISTDIR)/staging-zovc" .
	@rm -rf "$(DISTDIR)/staging-zovc"
	@echo "Created $(DISTDIR)/zovc-$(TARGET).tar.gz"

package-sklad: sklad
	@mkdir -p "$(DISTDIR)/staging-sklad"
	$(MAKE) install-sklad PREFIX="$(CURDIR)/$(DISTDIR)/staging-sklad"
	tar -czf "$(DISTDIR)/sklad-$(TARGET).tar.gz" -C "$(DISTDIR)/staging-sklad" .
	@rm -rf "$(DISTDIR)/staging-sklad"
	@echo "Created $(DISTDIR)/sklad-$(TARGET).tar.gz"

dist: package-zovc package-sklad

clean:
	rm -rf zovc zov-lsp sklad $(OBJ) .zov-test-bin $(DISTDIR)

.PHONY: all lsp test clean install install-zovc install-sklad dist package-zovc package-sklad