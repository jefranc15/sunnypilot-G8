#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>

#define LOG_PATH "/data/local/tmp/g8_pageflip_trace_v2.log"

struct _drmModeModeInfo;
typedef struct _drmModeModeInfo drmModeModeInfo;
typedef drmModeModeInfo *drmModeModeInfoPtr;

static void trace_line(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;

  int fd = open(LOG_PATH, O_WRONLY | O_APPEND | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t r1 = write(fd, buf, (size_t)n);
    ssize_t r2 = write(fd, "\n", 1);
    (void)r1; (void)r2;
    close(fd);
  }
}

static void *libdrm_handle(void) {
  static void *h = NULL;
  static int tried = 0;
  if (!tried) {
    tried = 1;
    h = dlopen("/lib/aarch64-linux-gnu/libdrm.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libdrm.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!h) trace_line("TRACE_V2 dlopen(libdrm) FAIL: %s", dlerror());
  }
  return h;
}

static void *real_drm_sym(const char *name) {
  void *h = libdrm_handle();
  if (!h) return NULL;
  dlerror();
  void *p = dlsym(h, name);
  const char *e = dlerror();
  if (!p || e) {
    trace_line("TRACE_V2 dlsym(%s) FAIL: %s", name, e ? e : "NULL");
    return NULL;
  }
  return p;
}

int drmModeAddFB2(int fd, uint32_t width, uint32_t height,
                  uint32_t pixel_format, const uint32_t bo_handles[4],
                  const uint32_t pitches[4], const uint32_t offsets[4],
                  uint32_t *buf_id, uint32_t flags) {
  typedef int (*fn_t)(int, uint32_t, uint32_t, uint32_t,
                      const uint32_t[4], const uint32_t[4],
                      const uint32_t[4], uint32_t *, uint32_t);
  static fn_t real_fn = NULL;
  if (!real_fn) real_fn = (fn_t)real_drm_sym("drmModeAddFB2");
  if (!real_fn) {
    errno = ENOSYS;
    trace_line("TRACE_V2 AddFB2 REAL_MISSING");
    return -1;
  }

  errno = 0;
  int rc = real_fn(fd, width, height, pixel_format, bo_handles, pitches,
                   offsets, buf_id, flags);
  int e = errno;
  trace_line("TRACE_V2 AddFB2 pid=%d fd=%d w=%u h=%u fmt=0x%08x "
             "h0=%u p0=%u o0=%u flags=0x%x rc=%d errno=%d fb=%u",
             getpid(), fd, width, height, pixel_format,
             bo_handles ? bo_handles[0] : 0,
             pitches ? pitches[0] : 0,
             offsets ? offsets[0] : 0,
             flags, rc, e, (buf_id ? *buf_id : 0));
  errno = e;
  return rc;
}

int drmModeSetCrtc(int fd, uint32_t crtc_id, uint32_t buffer_id,
                   uint32_t x, uint32_t y, uint32_t *connectors,
                   int count, drmModeModeInfoPtr mode) {
  typedef int (*fn_t)(int, uint32_t, uint32_t, uint32_t, uint32_t,
                      uint32_t *, int, drmModeModeInfoPtr);
  static fn_t real_fn = NULL;
  if (!real_fn) real_fn = (fn_t)real_drm_sym("drmModeSetCrtc");
  if (!real_fn) {
    errno = ENOSYS;
    trace_line("TRACE_V2 SetCrtc REAL_MISSING");
    return -1;
  }

  errno = 0;
  int rc = real_fn(fd, crtc_id, buffer_id, x, y, connectors, count, mode);
  int e = errno;
  trace_line("TRACE_V2 SetCrtc pid=%d fd=%d crtc=%u fb=%u x=%u y=%u "
             "conn0=%u count=%d rc=%d errno=%d",
             getpid(), fd, crtc_id, buffer_id, x, y,
             (connectors && count > 0) ? connectors[0] : 0,
             count, rc, e);
  errno = e;
  return rc;
}

int drmModePageFlip(int fd, uint32_t crtc_id, uint32_t fb_id,
                    uint32_t flags, void *user_data) {
  typedef int (*fn_t)(int, uint32_t, uint32_t, uint32_t, void *);
  static fn_t real_fn = NULL;
  if (!real_fn) real_fn = (fn_t)real_drm_sym("drmModePageFlip");
  if (!real_fn) {
    errno = ENOSYS;
    trace_line("TRACE_V2 PageFlip REAL_MISSING");
    return -1;
  }

  errno = 0;
  int rc = real_fn(fd, crtc_id, fb_id, flags, user_data);
  int e = errno;
  trace_line("TRACE_V2 PageFlip pid=%d fd=%d crtc=%u fb=%u flags=0x%x "
             "user=%p rc=%d errno=%d",
             getpid(), fd, crtc_id, fb_id, flags, user_data, rc, e);
  errno = e;
  return rc;
}
