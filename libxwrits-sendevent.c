/*
 * libxwrits-sendevent.c - LD_PRELOAD shim for xwrits under XWayland.
 *
 * xwrits 2.26 ignores synthetic XSendEvent events in wait_x_loop() by
 * checking `if (e->xany.send_event) return 0;`.  Clearing the send_event
 * flag on MotionNotify events in XNextEvent() lets wayland-xwrits-bridge
 * forward Wayland keyboard/mouse activity to xwrits without warping the
 * user's cursor or injecting fake keystrokes into focused windows.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>

struct xw_xany_event {
  int type;
  unsigned long serial;
  int send_event;
  void *display;
  unsigned long window;
};

#define XW_MOTION_NOTIFY 6

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
  if (ret == 0 && ev && ev->type == XW_MOTION_NOTIFY && ev->send_event)
    ev->send_event = 0;

  return ret;
}
