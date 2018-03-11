/* wld: drm.c
 *
 * Copyright (c) 2013, 2014 Michael Forney
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "wld/drm.h"
#include "wld/drm-private.h"

#include <sys/sysmacros.h>

const static struct drm_driver *drivers[] = {
#if WITH_DRM_INTEL
    &intel_drm_driver,
#endif
#if WITH_DRM_NOUVEAU
    &nouveau_drm_driver,
#endif
    &dumb_drm_driver};

static const struct drm_driver *find_driver(int fd) {
  char path[64], id[32];
  uint32_t vendor_id, device_id;
  char *path_part;
  struct stat st;
  FILE *file;
  uint32_t index;

  if (fstat(fd, &st) == -1)
    return NULL;

  path_part = path + snprintf(path, sizeof path, "/sys/dev/char/%u:%u/device/",
                              major(st.st_rdev), minor(st.st_rdev));

  strcpy(path_part, "vendor");
  file = fopen(path, "r");
  fgets(id, sizeof id, file);
  fclose(file);
  vendor_id = strtoul(id, NULL, 0);

  strcpy(path_part, "device");
  file = fopen(path, "r");
  fgets(id, sizeof id, file);
  fclose(file);
  device_id = strtoul(id, NULL, 0);

  if (getenv("WLD_DRM_DUMB"))
    return &dumb_drm_driver;

  for (index = 0; index < ARRAY_LENGTH(drivers); ++index) {
    DEBUG("Trying DRM driver `%s'\n", drivers[index]->name);
    if (drivers[index]->device_supported(vendor_id, device_id))
      return drivers[index];
  }

  DEBUG("No DRM driver supports device 0x%x:0x%x\n", vendor_id, device_id);

  return NULL;
}

EXPORT
struct wld_context *wld_drm_create_context(int fd) {
  const struct drm_driver *driver;

  if (!(driver = find_driver(fd)))
    return NULL;

  return driver->create_context(fd);
}

EXPORT
bool wld_drm_is_dumb(struct wld_context *context) {
  return context->impl == dumb_context_impl;
}
