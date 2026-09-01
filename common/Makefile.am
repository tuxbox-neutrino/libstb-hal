noinst_LTLIBRARIES = libcommon.la

# The include path lives in AM_CPPFLAGS so that C sources see it too
# (AM_CXXFLAGS never reaches the C compiler).
AM_CPPFLAGS = \
	-I $(top_srcdir)/include

AM_CFLAGS = -Wall

AM_CXXFLAGS = -fno-rtti -fno-exceptions -fno-strict-aliasing

AM_LDFLAGS = -lpthread

if BOXTYPE_ARMBOX
AM_CXXFLAGS += \
	-I $(top_srcdir)/libdvbci
endif

if BOXTYPE_MIPSBOX
AM_CXXFLAGS += \
	-I $(top_srcdir)/libdvbci
endif

if BOXTYPE_MIPSBOX
libcommon_la_SOURCES = \
	ca_ci.cpp
else
if BOXTYPE_ARMBOX
if BOXMODEL_HD60
libcommon_la_SOURCES = \
	ca.cpp
else
if BOXMODEL_MULTIBOX
libcommon_la_SOURCES = \
	ca.cpp
else
if BOXMODEL_MULTIBOXSE
libcommon_la_SOURCES = \
	ca.cpp
else
if BOXMODEL_OSMIO4K
libcommon_la_SOURCES = \
	ca.cpp
else
if BOXMODEL_OSMIO4KPLUS
libcommon_la_SOURCES = \
	ca.cpp
else
libcommon_la_SOURCES = \
	ca_ci.cpp
endif
endif
endif
endif
endif
else
libcommon_la_SOURCES = \
	ca.cpp
endif
endif

libcommon_la_SOURCES += \
	hal_debug.cpp \
	proc_tools.c \
	pwrmngr.cpp \
	streaminput.c \
	version_hal.cpp
