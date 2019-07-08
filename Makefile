include common.mk

OBJECTS := libthreads.o schedule.o model.o threads.o librace.o action.o \
	   clockvector.o main.o snapshot-interface.o cyclegraph.o \
	   datarace.o impatomic.o cmodelint.o \
	   snapshot.o malloc.o mymemory.o common.o mutex.o conditionvariable.o \
	   context.o execution.o libannotate.o plugins.o pthread.o futex.o fuzzer.o \
	   sleeps.o history.o funcnode.o printf.o

CPPFLAGS += -Iinclude -I.
LDFLAGS := -ldl -lrt -rdynamic
SHARED := -shared

# Mac OSX options
ifeq ($(UNAME), Darwin)
LDFLAGS := -ldl
SHARED := -Wl,-undefined,dynamic_lookup -dynamiclib
endif

TESTS_DIR := test

MARKDOWN := doc/Markdown/Markdown.pl

all: $(LIB_SO) tests README.html

debug: CPPFLAGS += -DCONFIG_DEBUG
debug: all

PHONY += docs
docs: *.c *.cc *.h README.html
	doxygen

README.html: README.md
	$(MARKDOWN) $< > $@

malloc.o: malloc.c
	$(CC) -fPIC -c malloc.c -DMSPACES -DONLY_MSPACES -DHAVE_MMAP=1 $(CPPFLAGS) -Wno-unused-variable

futex.o: futex.cc
	$(CXX) -fPIC -c futex.cc -std=c++11 $(CPPFLAGS)

%.o : %.cc
	$(CXX) -MMD -MF .$@.d -fPIC -c $< $(CPPFLAGS)


$(LIB_SO): $(OBJECTS)
	$(CXX) $(SHARED) -o $(LIB_SO) $+ $(LDFLAGS)

%.pdf: %.dot
	dot -Tpdf $< -o $@

-include $(OBJECTS:%=.%.d)

PHONY += clean
clean:
	rm -f *.o *.so .*.d *.pdf *.dot
	$(MAKE) -C $(TESTS_DIR) clean

PHONY += mrclean
mrclean: clean
	rm -rf docs

PHONY += tags
tags:
	ctags -R

PHONY += tests
tests: $(LIB_SO)
#	$(MAKE) -C $(TESTS_DIR)

BENCH_DIR := benchmarks

PHONY += benchmarks
benchmarks: $(LIB_SO)
	@if ! test -d $(BENCH_DIR); then \
		echo "Directory $(BENCH_DIR) does not exist" && \
		echo "Please clone the benchmarks repository" && \
		echo && \
		exit 1; \
	fi
	$(MAKE) -C $(BENCH_DIR)

PHONY += pdfs
pdfs: $(patsubst %.dot,%.pdf,$(wildcard *.dot))

.PHONY: $(PHONY)

# A 1-inch margin PDF generated by 'pandoc'
%.pdf: %.md
	pandoc -o $@ $< -V header-includes='\usepackage[margin=1in]{geometry}'

tabbing:
	uncrustify -c C.cfg --no-backup --replace *.cc
	uncrustify -c C.cfg --no-backup --replace *.h
	uncrustify -c C.cfg --no-backup --replace include/*

