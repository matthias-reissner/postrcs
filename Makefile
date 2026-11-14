EXTENSION = postrcs
MODULE_big = postrcs
DATA = postrcs--1.0.sql postrcs--1.1.sql postrcs--1.2.sql \
	postrcs--1.0--1.1.sql postrcs--1.1--1.2.sql
REGRESS = postrcs

PG_CONFIG ?= pg_config

ifeq ($(OS),Windows_NT)
OBJS = postrcs.obj
EXTRA_CLEAN = postrcs.lib postrcs.exp
export MSYS_NO_PATHCONV = 1
PG_CPPFLAGS += -I$(shell $(PG_CONFIG) --includedir) \
	-I$(shell $(PG_CONFIG) --includedir-server)/port/win32_msvc
else
OBJS = postrcs.o
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

ifeq ($(OS),Windows_NT)
postrcs.obj: postrcs.c
	$(CC) $(CPPFLAGS) $(CFLAGS) /c $< /Fo$@

postrcs.dll: $(OBJS)
	link /DLL /OUT:$@ /INCREMENTAL:NO /STACK:4194304 /NOEXP \
		$(OBJS) /LIBPATH:$(shell $(PG_CONFIG) --libdir) postgres.lib
endif
