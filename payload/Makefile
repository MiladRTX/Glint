# Host build of the Glint payload HTTP/capture stack.
# Targets: Linux, macOS, MSYS2/MinGW (gcc). For the console build see ps5/.
# Everything built goes to build/ (objects in build/obj/).
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -pthread
CPPFLAGS += -DSL_HOST_BUILD -Iinclude -Isrc
LDFLAGS += -pthread

ifeq ($(OS),Windows_NT)
  LDFLAGS += -static
  LDLIBS += -lws2_32 -lm
else
  LDLIBS += -lm
endif

SRC := \
  src/state.c \
  src/server.c \
  src/media_http.c \
  src/capture_host.c \
  src/main.c \
  src/dashboard_blob.c \
  src/banner_blob.c

OBJ := $(SRC:src/%.c=build/obj/%.o)

all: build/glint-payload

build/glint-payload: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/obj/%.o: src/%.c include/glint.h src/http_handlers.h
	@mkdir -p build/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build/obj build/glint-payload build/glint-payload.exe

.PHONY: all clean
