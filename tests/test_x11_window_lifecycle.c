#include <X11/Xlib.h>

#include <stdio.h>

int
main(void) {
  Display *display = XOpenDisplay(NULL);
  if (!display) {
    fprintf(stderr, "Unable to open DISPLAY.\n");
    return 1;
  }

  int screen = DefaultScreen(display);
  Window window = XCreateSimpleWindow(display, RootWindow(display, screen),
    20, 20, 240, 160, 0, BlackPixel(display, screen), WhitePixel(display, screen));
  if (!window) {
    fprintf(stderr, "Unable to create test window.\n");
    XCloseDisplay(display);
    return 1;
  }

  XStoreName(display, window, "fastcompmgr lifecycle test");
  XMapWindow(display, window);
  XSync(display, False);
  XUnmapWindow(display, window);
  XSync(display, False);
  XMapWindow(display, window);
  XMoveResizeWindow(display, window, 40, 40, 320, 200);
  XSync(display, False);
  XDestroyWindow(display, window);
  XSync(display, False);
  XCloseDisplay(display);
  return 0;
}
