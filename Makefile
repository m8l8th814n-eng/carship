CC       := clang
PREFIX   ?= /usr/local
BUILD    ?= build

WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wvla -Wno-unused-parameter
# Tunes for this machine's CPU. The binary stops being portable, so build with
# ARCH= when producing one for another machine.
ARCH     ?= -march=native
CFLAGS   ?= -O2
CFLAGS   += -std=c11 $(ARCH) $(WARN)
CPPFLAGS += -D_GNU_SOURCE -Isrc -I$(BUILD)

SRCS   := $(wildcard src/*.c) $(wildcard src/modules/*.c)
ASSETS := $(wildcard presets/*.toml) $(wildcard shell/*)
GEN_C  := $(BUILD)/assets.c
GEN_H  := $(BUILD)/assets.h
OBJS   := $(SRCS:%.c=$(BUILD)/%.o) $(BUILD)/assets.o
DEPS   := $(OBJS:.o=.d)
BIN    := $(BUILD)/carship

.PHONY: all clean install uninstall test debug asan static
all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: %.c $(GEN_H)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/assets.o: $(GEN_C)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

$(GEN_C) $(GEN_H) &: tools/embed.sh $(ASSETS)
	@mkdir -p $(BUILD)
	tools/embed.sh $(GEN_C) $(GEN_H) $(ASSETS)

# A prompt lives about a millisecond, so its cost is dominated by process
# startup rather than by anything the optimiser can reach. Linking statically
# removes the dynamic loader's symbol resolution from every exec and is worth
# roughly 20%; -O3, -Oz, ThinLTO and -march=native all measure as noise.
# Caveat: getpwuid goes through NSS, so a static build needs a matching glibc
# at runtime and will not see users from LDAP or SSSD.
static: CFLAGS := -O2 -std=c11 $(ARCH) -flto=thin $(WARN)
static: LDFLAGS += -flto=thin -static
static: clean $(BIN)

debug: CFLAGS := -std=c11 -O0 -g3 $(WARN)
debug: clean $(BIN)

asan: CFLAGS := -std=c11 -O1 -g3 $(ARCH) -fsanitize=address,undefined $(WARN)
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean $(BIN)

test: $(BIN)
	tests/run.sh $(abspath $(BIN))

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/carship

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/carship

clean:
	rm -rf $(BUILD)

-include $(DEPS)
