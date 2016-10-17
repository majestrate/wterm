# wld: config.mk

CC                  = gcc
CFLAGS              = -pipe

ENABLE_DEBUG        = 1
ENABLE_STATIC       = 1
ENABLE_SHARED       = 0

ENABLE_PIXMAN       = 1
ENABLE_DRM          = 1
ENABLE_WAYLAND      = 1

DRM_DRIVERS         = intel nouveau
WAYLAND_INTERFACES  = shm

ifeq ($(ENABLE_DRM),1)
    WAYLAND_INTERFACES += drm
endif

