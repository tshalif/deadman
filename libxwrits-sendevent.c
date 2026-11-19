/*
 * libxwrits-sendevent.c - LD_PRELOAD shim for xwrits under XWayland.
 *
 * Bridges Wayland activity from wayland-xwrits-bridge into xwrits via two
 * mechanisms:
 *   1. XQueryPointer(): xwrits with `+mouse` polls XQueryPointer() every
 *      3 seconds (A_MOUSE).  When wayland-xwrits-bridge records recent
 *      activity in /dev/shm/xwrits-wayland-activity-<uid>, XQueryPointer()
 *      toggles *root_x_return by 64px (> mouse_sensitivity=15) so xwrits's
 *      native A_MOUSE handler registers the activity in all states.
 *   2. XNextEvent(): clears ev->xany.send_event on synthetic KeyPress and
 *      MotionNotify events sent by wayland-xwrits-bridge, since xwrits 2.26
 *      otherwise ignores send_event!=0 in wait_x_loop().
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

struct xw_xany_event {
  int type;
  unsigned long serial;
  int send_event;
  void *display;
  unsigned long window;
};

struct xw_shm_state {
  uint64_t seq;
  uint64_t monotonic_ms;
};

#define XW_KEY_PRESS     2
#define XW_MOTION_NOTIFY 6

static uint64_t now_monotonic_ms(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static int check_shm_activity(void)
{
  static uint64_t last_seq;
  static char shm_path[128];
  struct xw_shm_state st;
  int fd;
  ssize_t n;

  if (shm_path[0] == '\0')
    snprintf(shm_path, sizeof(shm_path), "/dev/shm/xwrits-wayland-activity-%u",
             (unsigned int)getuid());

  fd = open(shm_path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  n = read(fd, &st, sizeof(st));
  close(fd);
  if (n != (ssize_t)sizeof(st))
    return 0;

  if (st.seq != last_seq) {
    uint64_t now = now_monotonic_ms();
    last_seq = st.seq;
    if (now >= st.monotonic_ms && now - st.monotonic_ms <= 5000ULL)
      return 1;
  }
  return 0;
}

int XNextEvent(void *dpy, void *event_return)
{
  static int (*real_XNextEvent)(void *, void *);
  struct xw_xany_event *ev = event_return;
  int ret;

  if (!real_XNextEvent)
    real_XNextEvent = dlsym(RTLD_NEXT, "XNextEvent");
  if (!real_XNextEvent)
    return -1;

  ret = real_XNextEvent(dpy, event_return);
  if (ret == 0 && ev && ev->send_event &&
      (ev->type == XW_MOTION_NOTIFY || ev->type == XW_KEY_PRESS))
    ev->send_event = 0;

  return ret;
}

int XQueryPointer(void *dpy, unsigned long w,
                  unsigned long *root_return, unsigned long *child_return,
                  int *root_x_return, int *root_y_return,
                  int *win_x_return, int *win_y_return,
                  unsigned int *mask_return)
{
  static int (*real_XQueryPointer)(void *, unsigned long,
                                   unsigned long *, unsigned long *,
                                   int *, int *, int *, int *,
                                   unsigned int *);
  static int jitter_offset;
  int ret;

  if (!real_XQueryPointer)
    real_XQueryPointer = dlsym(RTLD_NEXT, "XQueryPointer");
  if (!real_XQueryPointer)
    return 0;

  ret = real_XQueryPointer(dpy, w, root_return, child_return,
                           root_x_return, root_y_return,
                           win_x_return, win_y_return, mask_return);
  if (check_shm_activity())
    jitter_offset = (jitter_offset == 0) ? 64 : 0;

  if (root_x_return)
    *root_x_return += jitter_offset;
  if (win_x_return)
    *win_x_return += jitter_offset;

  return ret;
}
