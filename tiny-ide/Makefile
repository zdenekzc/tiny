CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
NCURSES_CFLAGS := $(filter-out -D_XOPEN_SOURCE=%,$(shell $(PKG_CONFIG) --cflags ncursesw 2>/dev/null))
NCURSES_LIBS := $(shell $(PKG_CONFIG) --libs ncursesw 2>/dev/null)
ifeq ($(strip $(NCURSES_LIBS)),)
NCURSES_LIBS := -lncursesw
endif
LDLIBS += $(NCURSES_LIBS)

TARGET := tiny-ide.bin
SOURCES := main.c app.c editor.c runner.c ui.c dialogs.c
HEADERS := tiny_ide.h editor.h
TCC_SRC ?= ../_import/download/tinycc
TCC_BUILD ?= ../_build/host-tcc

.PHONY: all run tcc check check-unit check-smoke clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) Makefile
	$(CC) $(CFLAGS) $(NCURSES_CFLAGS) -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

run: $(TARGET) tcc
	./$(TARGET) demo/hello.c

$(TCC_BUILD)/include:
	mkdir -p $(TCC_BUILD)
	ln -sfn $(abspath $(TCC_SRC))/include $@

$(TCC_BUILD)/tcc: $(TCC_SRC)/configure | $(TCC_BUILD)/include
	mkdir -p $(TCC_BUILD)
	cd $(TCC_BUILD) && $(abspath $(TCC_SRC))/configure --prefix=$(abspath $(TCC_BUILD))/install
	$(MAKE) -C $(TCC_BUILD) tcc

$(TCC_BUILD)/libtcc1.a: $(TCC_BUILD)/tcc
	$(MAKE) -C $(TCC_BUILD)/lib ../libtcc1.a

tcc:
	@if command -v tcc >/dev/null 2>&1; then \
		echo "using system tcc: $$(command -v tcc)"; \
	else \
		$(MAKE) $(TCC_BUILD)/tcc $(TCC_BUILD)/libtcc1.a; \
		echo "using $(TCC_BUILD)/tcc"; \
	fi

check-unit: tests/test_editor.c editor.c editor.h
	mkdir -p ../_build/tiny-ide-tests
	$(CC) $(CFLAGS) -I. -o ../_build/tiny-ide-tests/test_editor tests/test_editor.c editor.c $(LDFLAGS)
	../_build/tiny-ide-tests/test_editor

check-smoke: $(TARGET) tcc
	@compiler=$$(command -v tcc 2>/dev/null || printf '%s' "$(abspath $(TCC_BUILD)/tcc)"); \
	TCC="$$compiler" python3 -I tests/smoke.py ./$(TARGET)

check: check-unit check-smoke

clean:
	rm -f $(TARGET)
