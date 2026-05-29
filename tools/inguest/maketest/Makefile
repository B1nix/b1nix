VAR := hello
GREETING = $(VAR) world

all: dep
	@echo MK-VARS $(GREETING);

dep: in.txt
	@echo MK-AUTOVAR dep from $<;
