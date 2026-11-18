/*
 * wayland-xwrits-bridge.c - Forward Wayland user activity to xwrits.
 *
 * Under Wayland, xwrits runs inside XWayland and cannot see keyboard or
 * pointer activity in native Wayland windows.  This daemon monitors user
 * activity across Wayland via:
 *   1. Linux input devices (/dev/input/event*, /dev/input/mice) when readable
 *   2. GNOME / Mutter Session DBus (org.gnome.Mutter.IdleMonitor.GetIdletime)
 *   3. Freedesktop / KDE Session DBus (org.freedesktop.ScreenSaver.GetSessionIdleTime)
 *
 * Whenever activity is detected within a poll interval, it sends a synthetic
 * MotionNotify XEvent to xwrits's window on $DISPLAY.
 *
 * Uses dlopen() for libX11 and libgio/libglib so it compiles with only gcc/libc.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Minimal X11 ABI definitions (avoids requiring libx11-dev at build time) */
typedef unsigned long XID;
typedef XID Window;
typedef XID Atom;
typedef void Display;

#define XW_NONE          0UL
#define XW_XA_INTEGER    ((Atom)19)
#define XW_MOTION_NOTIFY 6
#define XW_KEY_PRESS_MASK            (1L << 0)
#define XW_SUBSTRUCTURE_NOTIFY_MASK  (1L << 19)

union xw_xevent {
  int type;
  struct {
    int type;
    unsigned long serial;
    int send_event;
    Display *display;
    Window window;
    Window root;
    Window subwindow;
    unsigned long time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    char is_hint;
    int same_screen;
  } xmotion;
  long pad[24];
};

struct x11_api {
  void *handle;
  Display *(*XOpenDisplay)(const char *);
  int (*XCloseDisplay)(Display *);
  Window (*XDefaultRootWindow)(Display *);
  Atom (*XInternAtom)(Display *, const char *, int);
  int (*XQueryTree)(Display *, Window, Window *, Window *, Window **, unsigned int *);
  int (*XGetWindowProperty)(Display *, Window, Atom, long, long, int, Atom,
                            Atom *, int *, unsigned long *, unsigned long *,
                            unsigned char **);
  int (*XFree)(void *);
  int (*XSendEvent)(Display *, Window, int, long, void *);
  int (*XFlush)(Display *);
  void *(*XSetErrorHandler)(int (*)(Display *, void *));
};

/* Minimal GIO/GLib ABI definitions for Session DBus idle queries */
struct gio_api {
  void *gio_handle;
  void *glib_handle;
  void *conn;
  void *(*g_bus_get_sync)(int, void *, void **);
  void *(*g_dbus_connection_call_sync)(void *, const char *, const char *,
                                       const char *, const char *, void *,
                                       const void *, int, int, void *, void **);
  void *(*g_variant_get_child_value)(void *, size_t);
  int (*g_variant_classify)(void *);
  uint64_t (*g_variant_get_uint64)(void *);
  uint32_t (*g_variant_get_uint32)(void *);
  void (*g_variant_unref)(void *);
  void (*g_error_free)(void *);
};

#define G_BUS_TYPE_SESSION 2
#define MAX_INPUT_FDS      64
#define RESCAN_INTERVAL_S  10

static volatile sig_atomic_t stop_requested;
static bool verbose;

static void handle_sig(int sig)
{
  (void)sig;
  stop_requested = 1;
}

static int ignore_x_error(Display *dpy, void *err)
{
  (void)dpy;
  (void)err;
  return 0;
}

static bool load_x11(struct x11_api *x)
{
  memset(x, 0, sizeof(*x));
  x->handle = dlopen("libX11.so.6", RTLD_LAZY);
  if (!x->handle)
    x->handle = dlopen("libX11.so", RTLD_LAZY);
  if (!x->handle) {
    fprintf(stderr, "wayland-xwrits-bridge: cannot load libX11.so.6: %s\n", dlerror());
    return false;
  }

#define LOAD_SYM(name) do { \
    x->name = dlsym(x->handle, #name); \
    if (!x->name) { \
      fprintf(stderr, "wayland-xwrits-bridge: missing X11 symbol %s\n", #name); \
      return false; \
    } \
  } while (0)

  LOAD_SYM(XOpenDisplay);
  LOAD_SYM(XCloseDisplay);
  LOAD_SYM(XDefaultRootWindow);
  LOAD_SYM(XInternAtom);
  LOAD_SYM(XQueryTree);
  LOAD_SYM(XGetWindowProperty);
  LOAD_SYM(XFree);
  LOAD_SYM(XSendEvent);
  LOAD_SYM(XFlush);
  LOAD_SYM(XSetErrorHandler);
#undef LOAD_SYM

  return true;
}

static bool load_gio(struct gio_api *g)
{
  memset(g, 0, sizeof(*g));
  g->glib_handle = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
  g->gio_handle = dlopen("libgio-2.0.so.0", RTLD_LAZY);
  if (!g->glib_handle || !g->gio_handle)
    return false;

  g->g_bus_get_sync = dlsym(g->gio_handle, "g_bus_get_sync");
  g->g_dbus_connection_call_sync = dlsym(g->gio_handle, "g_dbus_connection_call_sync");
  g->g_variant_get_child_value = dlsym(g->glib_handle, "g_variant_get_child_value");
  g->g_variant_classify = dlsym(g->glib_handle, "g_variant_classify");
  g->g_variant_get_uint64 = dlsym(g->glib_handle, "g_variant_get_uint64");
  g->g_variant_get_uint32 = dlsym(g->glib_handle, "g_variant_get_uint32");
  g->g_variant_unref = dlsym(g->glib_handle, "g_variant_unref");
  g->g_error_free = dlsym(g->glib_handle, "g_error_free");

  if (!g->g_bus_get_sync || !g->g_dbus_connection_call_sync ||
      !g->g_variant_get_child_value || !g->g_variant_classify ||
      !g->g_variant_get_uint64 || !g->g_variant_get_uint32 ||
      !g->g_variant_unref || !g->g_error_free)
    return false;

  void *err = NULL;
  g->conn = g->g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (err)
    g->g_error_free(err);
  return g->conn != NULL;
}

static bool query_dbus_idle_ms(struct gio_api *g, const char *bus_name,
                               const char *obj_path, const char *iface,
                               const char *method, uint64_t *idle_ms_out)
{
  void *err = NULL;
  void *res;
  void *child;
  int cls;
  bool ok = false;

  if (!g->conn)
    return false;

  res = g->g_dbus_connection_call_sync(g->conn, bus_name, obj_path, iface,
                                       method, NULL, NULL, 0, 300, NULL, &err);
  if (err) {
    g->g_error_free(err);
    return false;
  }
  if (!res)
    return false;

  child = g->g_variant_get_child_value(res, 0);
  if (child) {
    cls = g->g_variant_classify(child);
    if (cls == 't') {
      *idle_ms_out = g->g_variant_get_uint64(child);
      ok = true;
    } else if (cls == 'u') {
      *idle_ms_out = (uint64_t)g->g_variant_get_uint32(child);
      ok = true;
    }
    g->g_variant_unref(child);
  }
  g->g_variant_unref(res);
  return ok;
}

static int open_input_devices(struct pollfd *pfds, int max_fds)
{
  DIR *dir;
  struct dirent *de;
  int count = 0;
  char path[280];

  dir = opendir("/dev/input");
  if (!dir)
    return 0;

  while ((de = readdir(dir)) != NULL && count < max_fds) {
    if (strncmp(de->d_name, "event", 5) != 0 &&
        strcmp(de->d_name, "mice") != 0)
      continue;

    snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
      pfds[count].fd = fd;
      pfds[count].events = POLLIN;
      pfds[count].revents = 0;
      count++;
    }
  }
  closedir(dir);
  return count;
}

static void close_input_devices(struct pollfd *pfds, int count)
{
  for (int i = 0; i < count; i++) {
    if (pfds[i].fd >= 0)
      close(pfds[i].fd);
  }
}

/*
 * Find xwrits's permanent hand window via the XWRITS_WINDOW integer property
 * that xwrits sets on its top-level windows.
 */
static Window find_xwrits_window(struct x11_api *x, Display *dpy, Window root,
                                 Atom xwrits_atom)
{
  Window r = XW_NONE, parent = XW_NONE, *children = NULL;
  unsigned int nchildren = 0;
  Window found = XW_NONE;

  if (!x->XQueryTree(dpy, root, &r, &parent, &children, &nchildren))
    return XW_NONE;

  for (unsigned int i = 0; i < nchildren; i++) {
    Atom actual_type = XW_NONE;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;

    if (x->XGetWindowProperty(dpy, children[i], xwrits_atom, 0, 1, 0,
                              XW_XA_INTEGER, &actual_type, &actual_format,
                              &nitems, &bytes_after, &prop) == 0) {
      if (prop) {
        if (actual_format != 0 && nitems >= 1) {
          long val = *(long *)prop;
          found = (val != 0) ? (Window)val : children[i];
        }
        x->XFree(prop);
      }
      if (found != XW_NONE)
        break;
    }
  }

  if (children)
    x->XFree(children);
  return found;
}

static void notify_xwrits_activity(struct x11_api *x, Display *dpy, Window root,
                                   Atom xwrits_atom, Window *cached_win)
{
  union xw_xevent ev;

  if (*cached_win == XW_NONE)
    *cached_win = find_xwrits_window(x, dpy, root, xwrits_atom);

  memset(&ev, 0, sizeof(ev));
  ev.type = XW_MOTION_NOTIFY;
  ev.xmotion.display = dpy;
  ev.xmotion.root = root;
  ev.xmotion.same_screen = 1;

  if (*cached_win != XW_NONE) {
    ev.xmotion.window = *cached_win;
    if (!x->XSendEvent(dpy, *cached_win, 0, 0L, &ev)) {
      *cached_win = find_xwrits_window(x, dpy, root, xwrits_atom);
      if (*cached_win != XW_NONE) {
        ev.xmotion.window = *cached_win;
        x->XSendEvent(dpy, *cached_win, 0, 0L, &ev);
      }
    }
  } else {
    ev.xmotion.window = root;
    x->XSendEvent(dpy, root, 0,
                  XW_KEY_PRESS_MASK | XW_SUBSTRUCTURE_NOTIFY_MASK, &ev);
  }
  x->XFlush(dpy);
}

static void usage(const char *prog)
{
  fprintf(stderr,
          "Usage: %s [-i INTERVAL_MS] [-v]\n"
          "Forward Wayland user input activity to xwrits over X11/XWayland.\n",
          prog);
}

int main(int argc, char **argv)
{
  int poll_ms = 1000;
  struct x11_api x11;
  struct gio_api gio;
  bool has_gio;
  Display *dpy;
  Window root, cached_xwrits_win = XW_NONE;
  Atom xwrits_atom;
  struct pollfd pfds[MAX_INPUT_FDS];
  int nfds = 0;
  time_t last_rescan = 0;
  uint64_t prev_mutter_idle = UINT64_MAX;
  uint64_t prev_fdo_idle = UINT64_MAX;

  for (int i = 1; i < argc; i++) {
    if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interval") == 0) &&
        i + 1 < argc) {
      poll_ms = atoi(argv[++i]);
      if (poll_ms < 100)
        poll_ms = 100;
    } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
      verbose = true;
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  signal(SIGINT, handle_sig);
  signal(SIGTERM, handle_sig);

  if (!load_x11(&x11))
    return 1;

  x11.XSetErrorHandler(ignore_x_error);
  dpy = x11.XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "wayland-xwrits-bridge: cannot open X display '%s'\n",
            getenv("DISPLAY") ? getenv("DISPLAY") : "");
    return 1;
  }

  root = x11.XDefaultRootWindow(dpy);
  xwrits_atom = x11.XInternAtom(dpy, "XWRITS_WINDOW", 0);
  has_gio = load_gio(&gio);

  nfds = open_input_devices(pfds, MAX_INPUT_FDS);
  last_rescan = time(NULL);

  if (verbose) {
    fprintf(stderr,
            "wayland-xwrits-bridge: started (input_fds=%d, dbus=%s, interval=%dms)\n",
            nfds, has_gio ? "yes" : "no", poll_ms);
  }

  while (!stop_requested) {
    bool active = false;
    time_t now = time(NULL);

    if (now - last_rescan >= RESCAN_INTERVAL_S) {
      close_input_devices(pfds, nfds);
      nfds = open_input_devices(pfds, MAX_INPUT_FDS);
      last_rescan = now;
    }

    if (nfds > 0) {
      int r = poll(pfds, (nfds_t)nfds, poll_ms);
      if (r > 0) {
        char drain[512];
        for (int i = 0; i < nfds; i++) {
          if (pfds[i].revents & POLLIN) {
            while (read(pfds[i].fd, drain, sizeof(drain)) > 0)
              ;
            active = true;
          } else if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            last_rescan = 0; /* trigger rescan next iteration */
          }
        }
      }
    } else {
      usleep((useconds_t)poll_ms * 1000U);
    }

    if (!active && has_gio) {
      uint64_t idle_ms = 0;
      if (query_dbus_idle_ms(&gio,
                             "org.gnome.Mutter.IdleMonitor",
                             "/org/gnome/Mutter/IdleMonitor/Core",
                             "org.gnome.Mutter.IdleMonitor",
                             "GetIdletime",
                             &idle_ms)) {
        if (idle_ms < (uint64_t)poll_ms + 250 || idle_ms < prev_mutter_idle)
          active = true;
        prev_mutter_idle = idle_ms;
      } else if (query_dbus_idle_ms(&gio,
                                    "org.freedesktop.ScreenSaver",
                                    "/org/freedesktop/ScreenSaver",
                                    "org.freedesktop.ScreenSaver",
                                    "GetSessionIdleTime",
                                    &idle_ms)) {
        if (idle_ms == 0 || idle_ms < prev_fdo_idle)
          active = true;
        prev_fdo_idle = idle_ms;
      }
    }

    if (active) {
      if (verbose)
        fprintf(stderr, "wayland-xwrits-bridge: activity detected, notifying xwrits\n");
      notify_xwrits_activity(&x11, dpy, root, xwrits_atom, &cached_xwrits_win);
    }
  }

  close_input_devices(pfds, nfds);
  x11.XCloseDisplay(dpy);
  return 0;
}
