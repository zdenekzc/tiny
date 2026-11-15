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

TARGET := nuklear-ide
SOURCES := main.c
HEADERS := vendor/nuklear.h vendor/nuklear_glfw_gl2.h

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(GLFW_CFLAGS) -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	./$(TARGET) demo/hello.c

clean:
	rm -f $(TARGET)
