
subdir = src/gausskernel/storage/access
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = annvector cbtree common diskann heap hnsw index ivf nbtree ubtree psort rmgrdesc transam \
		  obs hash spgist gist gin hbstore redo table ustore bm25 hybridann rabitq embedding \
		  index_backend crux roar
ifeq ($(enable_lite_mode), no)
SUBDIRS += archive
endif

include $(top_srcdir)/src/gausskernel/common.mk
