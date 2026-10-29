# PS5 payload build (ps5-payload-sdk / prospero-clang).
# Usage:
#   PS5_PAYLOAD_SDK=/root/ps5-payload-sdk make
PS5_PAYLOAD_SDK ?= /root/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

# The ELFs go to payload/build, next to the host build.
OUT := ../build
ELF := $(OUT)/glint-payload.elf

CFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -DSL_PS5 \
          -I../include -I../src
# The 120 Hz Output setting (main.c).
LDLIBS := -lSceRegMgr

SRC := \
  ../src/state.c \
  ../src/server.c \
  ../src/media_http.c \
  ../src/capture_ps5.c \
  ../src/inject.c \
  ../src/main.c \
  ../src/dashboard_blob.c \
  ../src/banner_blob.c

SRC_TEST := \
  ../src/state.c \
  ../src/server.c \
  ../src/media_http.c \
  ../src/capture_host.c \
  ../src/main.c \
  ../src/dashboard_blob.c \
  ../src/banner_blob.c

all: $(ELF) $(OUT)/glint-payload-test.elf

$(ELF): $(SRC) ../include/glint.h ../src/http_handlers.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

$(OUT)/glint-payload-test.elf: $(SRC_TEST) ../include/glint.h ../src/http_handlers.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DSL_TEST_CAPTURE -o $@ $(SRC_TEST) $(LDLIBS)

$(OUT)/glint-stall-test.elf: $(SRC) ../include/glint.h ../src/http_handlers.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DSL_FORCE_STALL -o $@ $(SRC) $(LDLIBS)

$(OUT)/glint-reset-ra.elf: $(SRC) ../include/glint.h ../src/http_handlers.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DSL_RESET_RA -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(ELF) $(OUT)/glint-payload-test.elf $(OUT)/glint-stall-test.elf $(OUT)/glint-reset-ra.elf

.PHONY: all clean
