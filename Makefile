# pikasys: one C translation unit, only system libraries.
# Build: make        Install: sudo make install
# User install: make install PREFIX="$HOME/.local" (add its bin to PATH)
# Tests: make check  Sanitizers: make sanitize
CC ?= cc
PREFIX ?= /usr/local
DESTDIR ?=
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
WARN = -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes
SYSTEM := $(shell uname -s)
ifeq ($(SYSTEM),Darwin)
LDLIBS += -framework IOKit -framework CoreFoundation
else ifeq ($(SYSTEM),Linux)
LDLIBS += -ldl -lm
else
$(error Supported systems are Linux, WSL2, and macOS)
endif

.PHONY: all install uninstall clean check sanitize
all: pikasys

pikasys: pikasys.c Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 pikasys.c $(LDFLAGS) $(LDLIBS) -o $@

install: pikasys
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 pikasys "$(DESTDIR)$(PREFIX)/bin/pikasys"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/pikasys"

check: pikasys
	./pikasys --self-test
	./pikasys --no-config --json --count 2 --interval 0.1 > /dev/null

sanitize:
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer pikasys.c $(LDFLAGS) $(LDLIBS) -o pikasys-sanitize
	./pikasys-sanitize --self-test
	./pikasys-sanitize --no-config --json --count 2 --interval 0.1 > /dev/null

clean:
	rm -f pikasys pikasys-sanitize
