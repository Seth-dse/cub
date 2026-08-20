# Build the Cub compiler.
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter
PREFIX  ?= /usr/local

SRC     := src/util.c src/types.c src/lexer.c src/parser.c src/check.c \
           src/codegen.c src/format.c src/main.c src/runtime_embed.c
OBJ     := $(SRC:.c=.o)

all: cubc

cubc: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/runtime_embed.c: runtime/cub_rt.h tools/embed.sh
	./tools/embed.sh runtime/cub_rt.h > $@

%.o: %.c src/cub.h
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

test: cubc
	@./tests/run_tests.sh

examples: cubc
	@for f in examples/*.cub; do echo "== $$f"; ./cubc run $$f < /dev/null || exit 1; done

install: cubc
	install -d $(PREFIX)/bin
	install -m 755 cubc $(PREFIX)/bin/cubc

# Link the editor extension into every VS Code-family editor that is present.
EXT_NAME := sethtech.cub-0.2.0
install-vscode:
	@found=0; \
	for dir in $$HOME/.vscode/extensions $$HOME/.vscode-insiders/extensions \
	           $$HOME/.cursor/extensions $$HOME/.antigravity-ide/extensions \
	           $$HOME/.windsurf/extensions; do \
	    if [ -d "$$dir" ]; then \
	        rm -rf "$$dir/$(EXT_NAME)"; \
	        ln -s "$(CURDIR)/editors/vscode" "$$dir/$(EXT_NAME)"; \
	        echo "linked into $$dir"; \
	        found=1; \
	    fi; \
	done; \
	if [ $$found -eq 0 ]; then \
	    echo "no VS Code extensions folder found; is VS Code installed?"; \
	    exit 1; \
	fi; \
	echo "now restart your editor, or run: Developer: Reload Window"

uninstall-vscode:
	@for dir in $$HOME/.vscode/extensions $$HOME/.vscode-insiders/extensions \
	            $$HOME/.cursor/extensions $$HOME/.antigravity-ide/extensions \
	            $$HOME/.windsurf/extensions; do \
	    [ -e "$$dir/$(EXT_NAME)" ] && rm -rf "$$dir/$(EXT_NAME)" && echo "removed from $$dir"; \
	done; true

fmt: cubc
	@for f in examples/*.cub tests/run/*.cub tests/errors/*.cub tests/runtime/*.cub; do \
	    ./cubc fmt -w $$f; \
	done; echo "formatted"

fmt-check: cubc
	@bad=0; \
	for f in examples/*.cub tests/run/*.cub tests/errors/*.cub tests/runtime/*.cub; do \
	    ./cubc fmt --check $$f || bad=1; \
	done; \
	if [ $$bad -eq 0 ]; then echo "all files are formatted"; else exit 1; fi

clean:
	rm -f cubc $(OBJ) src/runtime_embed.c
	rm -f examples/*.c tests/*.c
	rm -rf tests/tmp

.PHONY: all test examples install install-vscode uninstall-vscode fmt fmt-check clean
