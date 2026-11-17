CC ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter
GLFW_CFLAGS := $(shell $(PKG_CONFIG) --cflags glfw3 2>/dev/null)
GLFW_LIBS := $(shell $(PKG_CONFIG) --libs glfw3 2>/dev/null)
ifeq ($(strip $(GLFW_LIBS)),)
GLFW_LIBS := -lglfw
endif

LDLIBS += $(GLFW_LIBS) -lGL -lm -lpthread -ldl

TARGET := nuklear-ide.bin
TCC_SRC ?= ../_build/download/tinycc
SOURCES := main.c
HEADERS := ../_import/nuklear-ide/nuklear.h ../_import/nuklear-ide/nuklear_glfw_gl2.h

# The Nuklear headers are a download (see fetch-nuklear.sh), not tracked files:
# this rule fetches them when they are missing.
../_import/nuklear-ide/%.h:
	./fetch-nuklear.sh

.PHONY: all run tcc toaru clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(GLFW_CFLAGS) -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

run: $(TARGET) tcc
	./$(TARGET) demo/hello.c

toaru:
	./toaru/build.sh

# Build a host TinyCC from the tree that the ToaruOS setup already downloaded.
# A system-wide tcc or TCC=/path/to/tcc is used instead when available.
tcc:
	@if command -v tcc >/dev/null 2>&1; then \
		echo "using system tcc: $$(command -v tcc)"; \
	elif [ -x "$(TCC_SRC)/tcc" ]; then \
		echo "using $(TCC_SRC)/tcc"; \
	elif [ -f "$(TCC_SRC)/configure" ]; then \
		echo "building host TinyCC in $(TCC_SRC)"; \
		cd "$(TCC_SRC)" && ./configure --prefix="$(abspath ../_build/host-tcc)" >/dev/null && $(MAKE) -j"$$(nproc)"; \
	else \
		echo "error: no tcc found." >&2; \
		echo "Install TinyCC or set TCC=/path/to/tcc; this repo normally" >&2; \
		echo "has sources in $(TCC_SRC) after ./tests/run-toaru.sh." >&2; \
		exit 1; \
	fi

clean:
	rm -f $(TARGET)
