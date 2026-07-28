# Fixture for tools/inguest/make-test.py: the smallest Makefile that still
# exercises the parts of a make the in-guest build depends on — immediate and
# deferred variable expansion, a prerequisite chain, an automatic variable, and
# spawning a recipe through /bin/sh.
#
# Written for bmake (b1nix's /bin/make since M98): ${.ALLSRC} rather than GNU's
# $<, which BSD make only defines inside suffix rules.
VAR := hello
GREETING = ${VAR} world

all: dep
	@echo MK-VARS ${GREETING};

dep: in.txt
	@echo MK-AUTOVAR dep from ${.ALLSRC};
