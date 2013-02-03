## Process this file with automake to produce Makefile.in
CC=clang
AM_CFLAGS=-O3 -pipe -B/usr/lib/gold-ld/ -fpic -ffunction-sections -fdata-sections -flto
AM_LDFLAGS=-Wl,--gc-sections,-O1,--as-needed -flto -fuse-linker-plugin
noinst_LIBRARIES = libtre.a
libtre_a_CFLAGS = ${AM_CFLAGS}

noinst_HEADERS =		\
	tre-ast.h		\
	tre-compile.h		\
	tre-internal.h		\
	tre-match-utils.h	\
	tre-mem.h		\
	tre-parse.h		\
	tre-stack.h		\
	xmalloc.h

libtre_a_SOURCES =		\
	tre-ast.c		\
	tre-compile.c		\
	tre-match-backtrack.c	\
	tre-match-parallel.c	\
	tre-mem.c		\
	tre-parse.c		\
	tre-stack.c		\
	regcomp.c		\
	regexec.c		\
	regerror.c              \
        tre-match-approx.c



dist_pkginclude_HEADERS = tre.h regex.h
nodist_pkginclude_HEADERS = tre-config.h
