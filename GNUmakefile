# Makefile for umlwrap.

prefix = /usr/local
libdir = $(prefix)/lib
ourlibdir = $(prefix)/lib/umlwrap
bindir = $(prefix)/bin
mandir = $(prefix)/man
man1dir = $(mandir)/man1

INSTALL = install

CFLAGS = -Wall --std=c99 -pedantic $(XFLAGS)

-include Makefile.local

AGEDU_MODULES := agedu du alloc trie index html httpd fgetline licence
AGEDU_OBJS := $(patsubst %,%.o,$(AGEDU_MODULES))

ALLMODULES := $(sort $(AGEDU_MODULES))
ALLOBJS := $(patsubst %,%.o,$(ALLMODULES))
ALLDEPS := $(patsubst %,%.d,$(ALLMODULES))

BINARIES = agedu

binaries: $(BINARIES)

agedu: config.h $(AGEDU_OBJS)
	gcc $(LFLAGS) -o agedu $(AGEDU_OBJS)

config.h: configure
	./configure
	rm -f Makefile # we keep using _this_ Makefile

configure: configure.ac
	aclocal
	autoconf
	autoheader
	automake -a --foreign

INTERNALFLAGS=#

$(ALLOBJS): %.o: %.c
	gcc $(CFLAGS) -MM $*.c > $*.d
	gcc $(CFLAGS) $(INTERNALFLAGS) -c $*.c

MANPAGES = agedu.1

doc: $(MANPAGES)
$(MANPAGES): %.1: %.but
	halibut --man=$*.1 $*.but

clean:
	rm -f $(ALLOBJS) $(ALLDEPS) $(MANPAGES) $(BINARIES)

spotless: clean
	rm -f config.h config.h.in config.log config.status configure
	rm -f depcomp install-sh missing stamp-h1
	rm -f Makefile.in aclocal.m4
	rm -rf autom4te.cache .deps

-include $(ALLDEPS)
