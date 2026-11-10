CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-format-truncation -D_FORTIFY_SOURCE=2
PREFIX  ?= /usr/local

all: homecloud-uninstall

homecloud-uninstall: homecloud-uninstall.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	install -m 0755 homecloud-uninstall $(DESTDIR)$(PREFIX)/sbin/homecloud-uninstall

clean:
	rm -f homecloud-uninstall

.PHONY: all install clean
