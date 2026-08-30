PACKAGES = x11 xcomposite xfixes xdamage xrender xrandr
PKG_CONFIG ?= pkg-config
LIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm
INCS = $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2 -flto -pipe
CFLAGS += -Wall -fno-plt
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1
INSTALL ?= install

OBJS = fastcompmgr.o comp_rect.o cm-root.o cm-global.o cm-util.o cm-window.o cm-event.o
DEPS = $(OBJS:.o=.d) $(TEST_OBJS:.o=.d)
TEST_BINS = tests/test_comp_rect tests/test_ringbuffer tests/test_ringbuffer_no_modulo
TEST_OBJS = tests/test_comp_rect.o tests/comp_rect.o tests/test_ringbuffer.o \
	tests/test_ringbuffer_no_modulo.o

.DEFAULT_GOAL := all

all: fastcompmgr

$(OBJS): %.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCS) -MMD -MP -c -o $@ $<

fastcompmgr: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

tests/%.o: tests/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

tests/comp_rect.o: comp_rect.c comp_rect.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

tests/test_comp_rect: tests/test_comp_rect.o tests/comp_rect.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tests/test_ringbuffer: tests/test_ringbuffer.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tests/test_ringbuffer_no_modulo.o: tests/test_ringbuffer.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -DRINGBUFFER_AVOID_MODULO=1 -MMD -MP -c -o $@ $<

tests/test_ringbuffer_no_modulo: tests/test_ringbuffer_no_modulo.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test: $(TEST_BINS)
	./tests/test_comp_rect
	./tests/test_ringbuffer
	./tests/test_ringbuffer_no_modulo

check: all test
	sh tests/xvfb-smoke.sh ./fastcompmgr

install: fastcompmgr
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 0755 fastcompmgr "$(DESTDIR)$(BINDIR)/fastcompmgr"
	$(INSTALL) -d "$(DESTDIR)$(MANDIR)"
	$(INSTALL) -m 0644 fastcompmgr.1 "$(DESTDIR)$(MANDIR)/fastcompmgr.1"

uninstall:
	$(RM) "$(DESTDIR)$(BINDIR)/fastcompmgr"
	$(RM) "$(DESTDIR)$(MANDIR)/fastcompmgr.1"

clean:
	$(RM) $(OBJS) $(DEPS) $(TEST_OBJS) $(TEST_BINS) fastcompmgr

.PHONY: all check test install uninstall clean

-include $(DEPS)
