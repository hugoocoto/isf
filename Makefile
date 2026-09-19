OUT     = isf
SRC     = $(wildcard src/*.c)
HDR     = $(wildcard src/*.h) thirdparty/cum.h/cum.h thirdparty/flag.h/flag.h thirdparty/bt.h/bt.h
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

CFLAGS ?= -O2
CFLAGS += -std=gnu17 -Wall -Wextra -DVERSION='"$(VERSION)"'
LDFLAGS += -pthread
INC    += -Ithirdparty/cum.h
INC    += -Ithirdparty/flag.h
INC    += -Ithirdparty/bt.h

# make STATIC=1: a static binary, which runs on any Linux of its architecture
ifdef STATIC
LDFLAGS += -static
endif

.PHONY: all clean test appimage

all: $(OUT)

$(OUT): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $(LDFLAGS)

# Everything but main.c, for the unit tests
test/unit: test/unit.c $(filter-out src/main.c,$(SRC)) $(HDR)
	$(CC) $(CFLAGS) $(INC) -Isrc -o $@ test/unit.c $(filter-out src/main.c,$(SRC)) $(LDFLAGS)

test: $(OUT) test/unit
	test/unit
	test/run.sh

appimage: $(OUT)
	./appimage.sh $(OUT)

clean:
	rm -f $(OUT) test/unit isf-*.AppImage AppDir/usr/bin/isf
