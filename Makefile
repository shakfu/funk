CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
LDLIBS = -lm -lpthread
B = build

SRCS = main util lex parse check elab resolve interp cgen rt
OBJS = $(SRCS:%=$(B)/%.o) $(B)/embedded.o
HDRS = src/funk.h src/rt.h src/prims.def

all: funk

funk: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(B)/%.o: src/%.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/embed: tools/embed.c | $(B)
	$(CC) -O2 -o $@ $<

$(B)/embedded.c: $(B)/embed src/rt.c $(HDRS) lib/prelude.fk
	$(B)/embed fk_rt_src src/rt.c fk_prelude_src lib/prelude.fk > $@

$(B)/embedded.o: $(B)/embedded.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(B):
	mkdir -p $(B)

test: funk
	sh tests/run.sh

bench: funk
	python3 bench/bench.py run

bench-report:
	python3 bench/bench.py report

clean:
	rm -rf $(B) funk

.PHONY: all test bench bench-report clean
