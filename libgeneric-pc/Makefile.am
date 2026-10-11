noinst_LTLIBRARIES = libgeneric.la

AM_CPPFLAGS = -D__STDC_FORMAT_MACROS -D__STDC_CONSTANT_MACROS
AM_CPPFLAGS += -Wfatal-errors
AM_CPPFLAGS += \
	-I$(top_srcdir)/common \
	-I$(top_srcdir)/include \
	@AVUTIL_CFLAGS@ \
	@SDL3_CFLAGS@ \
	@GLES2_CFLAGS@ \
	@MPV_CFLAGS@

AM_CXXFLAGS = -fno-rtti -fno-exceptions -fno-strict-aliasing

AM_LDFLAGS = \
	-lrt \
	-lOpenThreads \
	@AVFORMAT_LIBS@ \
	@AVUTIL_LIBS@ \
	@AVCODEC_LIBS@ \
	@SWRESAMPLE_LIBS@ \
	@SWSCALE_LIBS@ \
	@SDL3_LIBS@ \
	@GLES2_LIBS@ \
	@MPV_LIBS@

libgeneric_la_SOURCES = \
	hardware_caps.c \
	dmx.cpp \
	video.cpp \
	audio.cpp \
	init.cpp \
	record.cpp \
	glfb.cpp \
	mpv_player.cpp \
	playback_mpv.cpp
