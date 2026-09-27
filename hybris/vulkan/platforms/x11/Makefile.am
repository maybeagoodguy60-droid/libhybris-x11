pkglib_LTLIBRARIES = vulkanplatform_x11.la

vulkanplatform_x11_la_SOURCES = vulkanplatform_x11.cpp $(top_srcdir)/platforms/x11/x11_window.cpp
vulkanplatform_x11_la_CXXFLAGS = \
	-I$(top_srcdir)/common \
	-I$(top_srcdir)/platforms/common \
	-I$(top_srcdir)/platforms/x11 \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/vulkan \
	-I$(top_srcdir)/vulkan/platforms/common \
	-I$(top_builddir)/vulkan/platforms/common \
	$(ANDROID_HEADERS_CFLAGS) \
	$(X11_CFLAGS) \
	$(XCB_CFLAGS) \
	$(X11_XCB_CFLAGS) \
	$(DRI3_CFLAGS) \
	$(PRESENT_CFLAGS)

if WANT_DEBUG
vulkanplatform_x11_la_CXXFLAGS += -ggdb -O0
endif

if WANT_TRACE
vulkanplatform_x11_la_CXXFLAGS += -DDEBUG
endif

vulkanplatform_x11_la_LDFLAGS = \
	-avoid-version -module -shared -export-dynamic \
	$(top_builddir)/platforms/common/libhybris-platformcommon.la \
	$(top_builddir)/vulkan/platforms/common/libhybris-vulkanplatformcommon.la \
	$(top_builddir)/gralloc/libgralloc.la \
	$(X11_LIBS) \
	$(XCB_LIBS) \
	$(X11_XCB_LIBS) \
	$(DRI3_LIBS) \
	$(PRESENT_LIBS)

if HAS_ANDROID_4_2_0
vulkanplatform_x11_la_LDFLAGS += $(top_builddir)/libsync/libsync.la
endif

if HAS_ANDROID_5_0_0
vulkanplatform_x11_la_LDFLAGS += $(top_builddir)/libsync/libsync.la
endif
