# Build and install the desktop-GL -> GLES GLX shim.
#
# The shim gives ordinary desktop GL/GLX programs (glxgears, glxinfo, GLFW
# apps, glmark2) a working GLX implementation on top of libhybris' EGL +
# GLES, so they render through the real GPU without any per-command
# LD_PRELOAD. It is installed over the system GL lookup paths, which is why
# `install` removes the destination first: gl-shims/libGL.so.1 is a symlink
# to libGLESv2.so.2, and writing through it would clobber the real driver.

PREFIX      ?= /usr
HYBRIS_DIR  ?= $(PREFIX)/lib/hybris
SHIMS_DIR   ?= $(HYBRIS_DIR)/gl-shims
SYS_LIB_DIR ?= $(PREFIX)/lib/aarch64-linux-gnu

CC     ?= gcc
CFLAGS ?= -O2 -fPIC -Wall
LDFLAGS ?= -shared
LDLIBS ?= -ldl -lX11 -lm

STAMP  := $(shell date +%Y%m%d-%H%M%S)
BACKUP ?= backup-$(STAMP)

# The three places a GL/GLX consumer can end up loading libGL from:
#   1. gl-shims/libGL.so     - libhybris' own (non-functional) GLX shim
#   2. gl-shims/libGL.so.1   - currently a symlink to the GLES driver
#   3. $(SYS_LIB_DIR)/libGL.so.1.7.0 - behind the distro's libGL.so.1 symlink
TARGETS := $(SHIMS_DIR)/libGL.so $(SHIMS_DIR)/libGL.so.1 \
           $(SYS_LIB_DIR)/libGL.so.1.7.0

# Backup names are flattened, leading-slash-free (e.g. the target
# /usr/lib/hybris/gl-shims/libGL.so is backed up as
# usr_lib_hybris_gl-shims_libGL.so). The mapping has to happen in the shell,
# where the target path actually lives, so it is a sed script rather than a
# make $(call) -- make would expand the argument before $t is set.
BACKUP_NAME = printf '%s' "$$t" | sed -e 's|^/||' -e 's|/|_|g'

.PHONY: all install uninstall verify clean

all: libGL.so.1 libGL.so

libGL.so.1: glxshim.c
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-soname,libGL.so.1 -Wl,--no-undefined -o $@ $< $(LDLIBS)

libGL.so: glxshim.c
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-soname,libGL.so -Wl,--no-undefined -o $@ $< $(LDLIBS)

install: all
	@mkdir -p $(BACKUP)
	@for t in $(TARGETS); do \
	    if [ -e "$$t" ] || [ -L "$$t" ]; then \
	        cp -a "$$t" "$(BACKUP)/$$($(BACKUP_NAME))" && \
	        echo "  backed up $$t"; \
	    fi; \
	done
	@rm -f $(TARGETS)
	@install -m 0755 libGL.so.1 $(SHIMS_DIR)/libGL.so.1
	@install -m 0755 libGL.so   $(SHIMS_DIR)/libGL.so
	@install -m 0755 libGL.so.1 $(SYS_LIB_DIR)/libGL.so.1.7.0
	@ldconfig
	@$(MAKE) --no-print-directory verify
	@echo "installed; replaced files are in $(BACKUP)/"

verify:
	@for t in $(TARGETS); do \
	    printf '  %-46s soname=%s\n' "$$t" \
	        "$$(readelf -d $$t | sed -n 's/.*SONAME.*\[\(.*\)\]/\1/p')"; \
	done
	@ldd /usr/bin/glxinfo 2>/dev/null | grep -E "libGL\.so|libEGL\.so" || true

uninstall:
	@test -d $(BACKUP) || { echo "no backup at $(BACKUP); nothing to restore" >&2; exit 1; }
	@for t in $(TARGETS); do \
	    b="$(BACKUP)/$$($(BACKUP_NAME))"; \
	    { [ -e "$$b" ] || [ -L "$$b" ]; } || { \
	        echo "MISSING BACKUP: $$b -- refusing to touch $$t" >&2; exit 1; }; \
	done
	@rm -f $(TARGETS)
	@for t in $(TARGETS); do \
	    cp -a "$(BACKUP)/$$($(BACKUP_NAME))" "$$t" && echo "  restored $$t"; \
	done
	@ldconfig

clean:
	rm -f libGL.so.1 libGL.so
