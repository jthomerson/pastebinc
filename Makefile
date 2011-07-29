VERSION=0.9-BETA
GIT_VERSION := $(shell [ -d .git ] && echo "-$$(git describe --always)")

PROGNAME ?= pastebinc

prefix ?= /usr/local
bindir ?= $(prefix)/bin
CONFDIR ?= /etc/pastebinc
DESTDIR ?= .
INSTALL	?= install

CFLAGS ?= -g
CFLAGS += -DPROGNAME=\"$(PROGNAME)\"
CFLAGS += -DVERSION=\"$(VERSION)$(GIT_VERSION)\"
CFLAGS += -DCONFDIR=\"$(CONFDIR)\"
CFLAGS += $(shell pkg-config --cflags glib-2.0)

LIBS   ?= -lcurl
LIBS   += $(shell pkg-config --libs   glib-2.0)

CC     ?= gcc

TARGETS  = pastebinc


all: $(TARGETS)

#pastebinc: pastebinc.o
#	$(CC) -fPIC $(LDFLAGS) -o $@ $^
#
#.c.o:
#	$(CC) -fPIC $(CFLAGS) $(LIBS) $^ -o $@
#

pastebinc: 
	$(CC) -fPIC $(CFLAGS) $(LIBS) -o $(PROGNAME) pastebinc.c

clean:
	rm -f *.o *.out $(PROGNAME)

install: $(TARGETS)
	$(INSTALL) -d $(DESTDIR)$(bindir)
	$(INSTALL) $(PROGNAME) $(DESTDIR)$(bindir)
	$(INSTALL) -d $(DESTDIR)$(CONFDIR)
	$(INSTALL) -m644 ./etc/*.conf $(DESTDIR)$(CONFDIR)
