subdir = src/gausskernel/storage/access/bm25
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif

OBJS = bm25.o bm25_utils.o bm25_inverted_list.o bm25_token_index.o bm25_score.o bm25_doc_store.o \
       bm25_internal.o bm25_parse.o bm25_parameters.o bm25_inspect.o bm25_scan.o bm25xlog.o \
       bm25_expr.o
SUBDIRS = tokenizer

override CPPFLAGS += -Wno-volatile -Wno-register -fno-exceptions -std=c++17
include $(top_srcdir)/src/gausskernel/common.mk
