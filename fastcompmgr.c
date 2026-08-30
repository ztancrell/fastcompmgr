/*
 * fastcompmgr - a _fast_ compositor for X11
 *
 * based on xcompmgr - copyright (c) 2003, keith packard
 *                                   2008, Dana Jansens
 *          compton                  2011, Christopher Jeffrey
 *          fastcompmgr              2023, Tycho Kirchner
 *
 * copyright (c) 2023, Tycho Kirchner
 * See LICENSE for more information.
 *
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xrandr.h>

#include "cm-global.h"
#include "cm-event.h"
#include "cm-root.h"
#include "cm-util.h"
#include "cm-window.h"
#include "comp_rect.h"
#include "ringbuffer.h"


typedef struct _ignore {
  struct _ignore *next;
  unsigned long sequence;
} ignore;


typedef struct _conv {
  int size;
  double *data;
} conv;

typedef struct _fade {
  struct _fade *next;
  win *w;
  double cur;
  double finish;
  double step;
  void (*callback) (Display *dpy, win *w);
  Display *dpy;
} fade;

fade *fades;
Display *dpy;
Picture black_picture;
Picture cshadow_picture;
Picture root_tile;
XserverRegion all_damage;
XserverRegion g_xregion_tmp;
XserverRegion screen_damage;
int g_paint_interval_ms;
int64_t g_last_paint_ms = 0;
Bool all_damage_is_dirty;
Bool clip_changed;
#if HAS_NAME_WINDOW_PIXMAP
Bool has_name_pixmap;
#endif
int xfixes_event, xfixes_error;
int damage_event, damage_error;
int composite_event, composite_error;
int render_event, render_error;
Bool synchronize;
int composite_opcode;
static Bool g_paint_ignore_region_is_dirty = True;

Atom win_type[NUM_WINTYPES];
double win_type_opacity[NUM_WINTYPES];
Bool win_type_shadow[NUM_WINTYPES];
Bool win_type_fade[NUM_WINTYPES];

#define REGISTER_PROP "_NET_WM_CM_S"

#define OPAQUE 0xffffffff

conv *gaussian_map;

#define WINDOW_SOLID 0
#define WINDOW_TRANS 1
#define WINDOW_ARGB 2

#ifndef DEBUG_REPAINT
#define DEBUG_REPAINT 0
#endif
#ifndef DEBUG_EVENTS
#define DEBUG_EVENTS 0
#endif
#ifndef MONITOR_REPAINT
#define MONITOR_REPAINT 0
#endif

static void
determine_mode(Display *dpy, win *w);
static bool
is_gtk_frame_extent(Display *dpy, Window w);

static double
get_opacity_percent(Display *dpy, win *w);
static void
do_configure_win(Display *dpy, win* w);
static void
set_paint_ignore_region_dirty(void);

static XserverRegion
win_extents(Display *dpy, win *w);
static void
invalidate_shadow(Display *dpy, win *w);
static void
add_damage(Display *dpy, XserverRegion damage);

int shadow_radius = 12;
int shadow_offset_x = -15;
int shadow_offset_y = -15;
double shadow_opacity = .75;

double fade_in_step = 0.028;
double fade_out_step = 0.03;
int fade_delta = 10;
int64_t fade_time = 0;
Bool fade_trans = False;

double inactive_opacity = 0;
double frame_opacity = 0;

#define INACTIVE_OPACITY \
(unsigned long)((double)inactive_opacity * OPAQUE)

#define IS_NORMAL_WIN(w) \
((w) && ((w)->window_type == WINTYPE_NORMAL \
         || (w)->window_type == WINTYPE_UTILITY))

#define HAS_FRAME_OPACITY(w) (frame_opacity && (w)->top_width)

/* For shadow precomputation */
int Gsize = -1;
unsigned char *shadow_corner = NULL;
unsigned char *shadow_top = NULL;


fade *
find_fade(win *w) {
  fade *f;

  for (f = fades; f; f = f->next) {
    if (f->w == w) return f;
  }

  return 0;
}

void
dequeue_fade(Display *dpy, fade *f) {
  fade **prev;

  for (prev = &fades; *prev; prev = &(*prev)->next) {
    if (*prev == f) {
      *prev = f->next;
      if (f->callback) {
        (*f->callback)(dpy, f->w);
      }
      free(f);
      break;
    }
  }
}

void
cleanup_fade(Display *dpy, win *w) {
  fade *f = find_fade (w);
  if (f) {
    dequeue_fade(dpy, f);
  }
}

void
enqueue_fade(Display *dpy, fade *f) {
  if (!fades) {
    fade_time = get_time_in_milliseconds() + fade_delta;
  }
  f->next = fades;
  fades = f;
}

static void
set_fade(Display *dpy, win *w, double start,
         double finish, double step,
         void(*callback) (Display *dpy, win *w),
         Bool exec_callback, Bool override) {
  fade *f;

  f = find_fade(w);
  if (!f) {
    f = malloc(sizeof(fade));
    if (!f) return;
    f->next = 0;
    f->w = w;
    f->cur = start;
    enqueue_fade(dpy, f);
  } else if (!override) {
    return;
  } else {
    if (exec_callback && f->callback) {
      (*f->callback)(dpy, f->w);
    }
  }

  if (finish < 0) finish = 0;
  if (finish > 1) finish = 1;
  f->finish = finish;

  if (f->cur < finish) {
    f->step = step;
  } else if (f->cur > finish) {
    f->step = -step;
  } else {
    f->step = 0;
  }

  f->callback = callback;
  w->opacity = f->cur * OPAQUE;

#if 0
  printf("set_fade start %g step %g\n", f->cur, f->step);
#endif

  determine_mode(dpy, w);

  invalidate_shadow(dpy, w);

  /* fading windows need to be drawn, mark
     them as damaged.  when a window maps,
     if it tries to fade in but it already
     at the right opacity (map/unmap/map fast)
     then it will never get drawn without this
     until it repaints */
  w->damaged = 1;
}

int
fade_timeout(void) {
  int64_t now;
  int64_t delta;

  if (!fades) return -1;

  now = get_time_in_milliseconds();
  delta = fade_time - now;

  if (delta < 0) delta = 0;
/* printf("timeout %d\n", delta); */

  return delta > INT_MAX ? INT_MAX : (int)delta;
}

void
run_fades(Display *dpy) {
  int64_t now = get_time_in_milliseconds();
  fade *next = fades;
  int64_t steps;
  Bool need_dequeue;

#if 0
  printf("run fades\n");
#endif

  if (fade_time - now > 0) return;
  steps = 1 + (now - fade_time) / fade_delta;

  while (next) {
    fade *f = next;
    win *w = f->w;
    next = f->next;

    f->cur += f->step * steps;
    if (f->cur >= 1) {
      f->cur = 1;
    } else if (f->cur < 0) {
      f->cur = 0;
    }

#if 0
    printf("opacity now %g\n", f->cur);
#endif

    w->opacity = f->cur * OPAQUE;
    need_dequeue = False;
    if (f->step > 0) {
      if (f->cur >= f->finish) {
        w->opacity = f->finish * OPAQUE;
        need_dequeue = True;
      }
    } else {
      if (f->cur <= f->finish) {
        w->opacity = f->finish * OPAQUE;
        need_dequeue = True;
      }
    }

    determine_mode(dpy, w);

    invalidate_shadow(dpy, w);

    /* Must do this last as it might
       destroy f->w in callbacks */
    if (need_dequeue) dequeue_fade(dpy, f);
  }

  fade_time = now + fade_delta;
}

static double
gaussian(double r, double x, double y) {
  return ((1 / (sqrt(2 * M_PI * r))) *
      exp((- (x * x + y * y)) / (2 * r * r)));
}


static conv *
make_gaussian_map(Display *dpy, double r) {
  conv *c;
  int size = ((int) ceil((r * 3)) + 1) & ~1;
  int center = size / 2;
  int x, y;
  double t;
  double g;

  c = malloc(sizeof(conv) + size * size * sizeof(double));
  if (!c) return NULL;
  c->size = size;
  c->data = (double *) (c + 1);
  t = 0.0;

  for (y = 0; y < size; y++) {
    for (x = 0; x < size; x++) {
      g = gaussian(r, (double) (x - center), (double) (y - center));
      t += g;
      c->data[y * size + x] = g;
    }
  }

/*  printf("gaussian total %f\n", t); */

  for (y = 0; y < size; y++) {
    for (x = 0; x < size; x++) {
      c->data[y * size + x] /= t;
    }
  }

  return c;
}

/*
 * A picture will help
 *
 *      -center   0                width  width+center
 *  -center +-----+-------------------+-----+
 *          |     |                   |     |
 *          |     |                   |     |
 *        0 +-----+-------------------+-----+
 *          |     |                   |     |
 *          |     |                   |     |
 *          |     |                   |     |
 *   height +-----+-------------------+-----+
 *          |     |                   |     |
 * height+  |     |                   |     |
 *  center  +-----+-------------------+-----+
 */

static unsigned char
sum_gaussian(conv *map, double opacity,
             int x, int y, int width, int height) {
  int fx, fy;
  double *g_data;
  double *g_line = map->data;
  int g_size = map->size;
  int center = g_size / 2;
  int fx_start, fx_end;
  int fy_start, fy_end;
  double v;

  /*
   * Compute set of filter values which are "in range",
   * that's the set with:
   *    0 <= x + (fx-center) && x + (fx-center) < width &&
   *  0 <= y + (fy-center) && y + (fy-center) < height
   *
   *  0 <= x + (fx - center)    x + fx - center < width
   *  center - x <= fx    fx < width + center - x
   */

  fx_start = center - x;
  if (fx_start < 0) fx_start = 0;
  fx_end = width + center - x;
  if (fx_end > g_size) fx_end = g_size;

  fy_start = center - y;
  if (fy_start < 0) fy_start = 0;
  fy_end = height + center - y;
  if (fy_end > g_size) fy_end = g_size;

  g_line = g_line + fy_start * g_size + fx_start;

  v = 0;

  for (fy = fy_start; fy < fy_end; fy++) {
    g_data = g_line;
    g_line += g_size;

    for (fx = fx_start; fx < fx_end; fx++) {
      v += *g_data++;
    }
  }

  if (v > 1) v = 1;

  return ((unsigned char) (v * opacity * 255.0));
}

/* precompute shadow corners and sides
   to save time for large windows */
static void
presum_gaussian(conv *map) {
  int center = map->size/2;
  int opacity, x, y;

  Gsize = map->size;

  if (shadow_corner) free((void *)shadow_corner);
  if (shadow_top) free((void *)shadow_top);

  shadow_corner = (unsigned char *)(malloc((Gsize + 1) * (Gsize + 1) * 26));
  shadow_top = (unsigned char *)(malloc((Gsize + 1) * 26));
  if (!shadow_corner || !shadow_top) {
    free(shadow_corner);
    free(shadow_top);
    shadow_corner = NULL;
    shadow_top = NULL;
    Gsize = 0;
    return;
  }

  for (x = 0; x <= Gsize; x++) {
    shadow_top[25 * (Gsize + 1) + x] =
      sum_gaussian(map, 1, x - center, center, Gsize * 2, Gsize * 2);

    for (opacity = 0; opacity < 25; opacity++) {
      shadow_top[opacity * (Gsize + 1) + x] =
        shadow_top[25 * (Gsize + 1) + x] * opacity / 25;
    }

    for (y = 0; y <= x; y++) {
      shadow_corner[25 * (Gsize + 1) * (Gsize + 1) + y * (Gsize + 1) + x]
        = sum_gaussian(map, 1, x - center, y - center, Gsize * 2, Gsize * 2);
      shadow_corner[25 * (Gsize + 1) * (Gsize + 1) + x * (Gsize + 1) + y]
        = shadow_corner[25 * (Gsize + 1) * (Gsize + 1) + y * (Gsize + 1) + x];

      for (opacity = 0; opacity < 25; opacity++) {
        shadow_corner[opacity * (Gsize + 1) * (Gsize + 1)
                      + y * (Gsize + 1) + x]
          = shadow_corner[opacity * (Gsize + 1) * (Gsize + 1)
                          + x * (Gsize + 1) + y]
          = shadow_corner[25 * (Gsize + 1) * (Gsize + 1)
                          + y * (Gsize + 1) + x] * opacity / 25;
      }
    }
  }
}

/// Make that part of the shadow transparent that is immediately below its window
static void make_transparent_shadowcenter(int width, int height, int swidth, int sheight,
                                          unsigned char *data){
  int ylimit;
  int x, y;
  int x_diff;

  if(shadow_offset_x < 0){
    x = -shadow_offset_x;
    if(swidth - x < width){
      x_diff = swidth-x;
    } else {
      x_diff = width;
    }
  } else {
    x = 0;
    x_diff = width - shadow_offset_x;
  }

  if(shadow_offset_y < 0){
    y = -shadow_offset_y;
    if(sheight - y < height){
      ylimit = sheight;
    } else {
      ylimit = height + y;
    }
  } else {
    y = 0;
    ylimit = height - shadow_offset_y;
  }

  if(likely(x_diff > 0)){
    assert(y >=0);
    assert(ylimit <= sheight);
    assert(x >=0 && x < swidth);
    assert(x+x_diff <= swidth);
    for(; y < ylimit ; y++){
      memset(&data[y * swidth + x], 0, x_diff);
    }
  }
}

static XImage *
make_shadow(Display *dpy, double opacity,
            int width, int height, shadowtype shadow_type) {
  XImage *ximage;
  unsigned char *data;
  int gsize = gaussian_map->size;
  int ylimit, xlimit;
  int swidth = width + gsize;
  int sheight = height + gsize;
  int center = gsize / 2;
  int x, y;
  unsigned char d;
  int x_diff;
  int opacity_int = (int)(opacity * 25);

  data = malloc(swidth * sheight * sizeof(unsigned char));
  if (!data) return 0;

  ximage = XCreateImage(
    dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 8,
    ZPixmap, 0, (char *) data, swidth, sheight, 8,
    swidth * sizeof(unsigned char));

  if (!ximage) {
    free(data);
    return 0;
  }

  /*
   * Build the gaussian in sections
   */

  /*
   * center (fill the complete data array)
   */

  if (Gsize > 0) {
    d = shadow_top[opacity_int * (Gsize + 1) + Gsize];
  } else {
    d = sum_gaussian(gaussian_map,
      opacity, center, center, width, height);
  }

  memset(data, d, sheight * swidth);

  /*
   * corners
   */

  ylimit = gsize;
  if (ylimit > sheight / 2) ylimit = (sheight + 1) / 2;

  xlimit = gsize;
  if (xlimit > swidth / 2) xlimit = (swidth + 1) / 2;

  for (y = 0; y < ylimit; y++)
    for (x = 0; x < xlimit; x++) {
      if (xlimit == Gsize && ylimit == Gsize) {
        d = shadow_corner[opacity_int * (Gsize + 1) * (Gsize + 1)
                          + y * (Gsize + 1) + x];
      } else {
        d = sum_gaussian(gaussian_map,
          opacity, x - center, y - center, width, height);
      }
      data[y * swidth + x] = d;
      data[(sheight - y - 1) * swidth + x] = d;
      data[(sheight - y - 1) * swidth + (swidth - x - 1)] = d;
      data[y * swidth + (swidth - x - 1)] = d;
    }

  /*
   * top/bottom
   */

  x_diff = swidth - (gsize * 2);
  if (x_diff > 0 && ylimit > 0) {
    for (y = 0; y < ylimit; y++) {
      if (ylimit == Gsize) {
        d = shadow_top[opacity_int * (Gsize + 1) + y];
      } else {
        d = sum_gaussian(gaussian_map,
          opacity, center, y - center, width, height);
      }
      memset(&data[y * swidth + gsize], d, x_diff);
      memset(&data[(sheight - y - 1) * swidth + gsize], d, x_diff);
    }
  }

  /*
   * sides
   */

  for (x = 0; x < xlimit; x++) {
    if (xlimit == Gsize) {
      d = shadow_top[opacity_int * (Gsize + 1) + x];
    } else {
      d = sum_gaussian(gaussian_map,
        opacity, x - center, center, width, height);
    }
    for (y = gsize; y < sheight - gsize; y++) {
      data[y * swidth + x] = d;
      data[y * swidth + (swidth - x - 1)] = d;
    }
  }

  switch (shadow_type) {
  case SHADOW_UNKNOWN:
  case SHADOW_NO: assert(false);
  case SHADOW_FULL: break;
  case SHADOW_NOCENTER:
    make_transparent_shadowcenter(width, height, swidth, sheight, data);
  }
  return ximage;
}

static Picture
shadow_picture(Display *dpy, double opacity, shadowtype shadow_type,
               int width, int height, int *wp, int *hp) {
  XImage *shadowImage;
  Pixmap shadowPixmap;
  Picture shadow_picture;
  GC gc;

  shadowImage = make_shadow(dpy, opacity, width, height, shadow_type);
  if (!shadowImage) return None;

  shadowPixmap = XCreatePixmap(dpy, root,
    shadowImage->width, shadowImage->height, 8);

  if (!shadowPixmap) {
    XDestroyImage(shadowImage);
    return None;
  }

  shadow_picture = XRenderCreatePicture(dpy, shadowPixmap,
    XRenderFindStandardFormat(dpy, PictStandardA8), 0, 0);
  if (!shadow_picture) {
    XDestroyImage(shadowImage);
    XFreePixmap(dpy, shadowPixmap);
    return None;
  }

  gc = XCreateGC(dpy, shadowPixmap, 0, 0);
  if (!gc) {
    XDestroyImage(shadowImage);
    XFreePixmap(dpy, shadowPixmap);
    XRenderFreePicture(dpy, shadow_picture);
    return None;
  }

  XPutImage(
    dpy, shadowPixmap, gc, shadowImage, 0, 0, 0, 0,
    shadowImage->width, shadowImage->height);

  *wp = shadowImage->width;
  *hp = shadowImage->height;
  XFreeGC(dpy, gc);
  XDestroyImage(shadowImage);
  XFreePixmap(dpy, shadowPixmap);

  return shadow_picture;
}

Picture
solid_picture(Display *dpy, Bool argb, double a,
              double r, double g, double b) {
  Pixmap pixmap;
  Picture picture;
  XRenderPictureAttributes pa;
  XRenderColor c;

  pixmap = XCreatePixmap(dpy, root, 1, 1, argb ? 32 : 8);

  if (!pixmap) return None;

  pa.repeat = True;
  picture = XRenderCreatePicture(dpy, pixmap,
    XRenderFindStandardFormat(dpy, argb
      ? PictStandardARGB32 : PictStandardA8),
    CPRepeat,
    &pa);

  if (!picture) {
    XFreePixmap(dpy, pixmap);
    return None;
  }

  c.alpha = a * 0xffff;
  c.red = r * 0xffff;
  c.green = g * 0xffff;
  c.blue = b * 0xffff;

  XRenderFillRectangle(dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);
  XFreePixmap(dpy, pixmap);

  return picture;
}


// 9-patch shadow tiles: size-independent shadow pieces, built lazily per
// opacity level (0..25) and shared by every window. For large SHADOW_FULL
// windows this replaces the per-window full-size alpha mask, so resizing a big
// window no longer mallocs+memsets+uploads an O(w*h) XImage every frame.
typedef struct {
  Picture corner_tl, corner_tr, corner_bl, corner_br;
  Picture edge_top, edge_bottom, edge_left, edge_right;
  Picture center;
  Bool built;
} shadow_tiles;

static shadow_tiles g_shadow_tiles[26];

// Wrap an 8-bit alpha buffer as an A8 Picture. Mirrors shadow_picture()'s
// proven pixmap->picture->free-pixmap dance; XDestroyImage frees `data`.
static Picture
a8_picture_from_data(Display *dpy, unsigned char *data,
                     int width, int height, Bool repeat) {
  XImage *img;
  Pixmap pixmap;
  Picture picture;
  GC gc;
  XRenderPictureAttributes pa;

  img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 8,
    ZPixmap, 0, (char *) data, width, height, 8, width);
  if (!img) { free(data); return None; }

  pixmap = XCreatePixmap(dpy, root, width, height, 8);
  if (!pixmap) { XDestroyImage(img); return None; }

  pa.repeat = repeat;
  picture = XRenderCreatePicture(dpy, pixmap,
    XRenderFindStandardFormat(dpy, PictStandardA8),
    repeat ? CPRepeat : 0, &pa);
  if (!picture) { XDestroyImage(img); XFreePixmap(dpy, pixmap); return None; }

  gc = XCreateGC(dpy, pixmap, 0, 0);
  if (!gc) {
    XRenderFreePicture(dpy, picture);
    XDestroyImage(img);
    XFreePixmap(dpy, pixmap);
    return None;
  }

  XPutImage(dpy, pixmap, gc, img, 0, 0, 0, 0, width, height);
  XFreeGC(dpy, gc);
  XDestroyImage(img);
  XFreePixmap(dpy, pixmap);
  return picture;
}

// Build the 9 tiles for opacity level `op` (once). Values come straight from
// sum_gaussian() with a 2*gsize filter window, which is exactly what make_shadow
// / presum_gaussian produce for a large window's corners/edges/center - so the
// 9-patch is pixel-identical to the old full-size shadow for eligible windows.
static void
ensure_shadow_tiles(Display *dpy, int op) {
  if (g_shadow_tiles[op].built) return;

  const int g = gaussian_map->size;
  if (g <= 0) return;  // ponytail: degenerate blur radius (e.g. -r 0); caller falls back
  const int center = g / 2;
  const int win = 2 * g;
  const double od = (double) op / 25.0;
  int x, y;
  unsigned char *d;

  // corners (g x g): TL, and h/v mirrors for the other three
  d = malloc(g * g);
  for (y = 0; y < g; y++) for (x = 0; x < g; x++)
    d[y * g + x] = sum_gaussian(gaussian_map, od, x - center, y - center, win, win);
  g_shadow_tiles[op].corner_tl = a8_picture_from_data(dpy, d, g, g, False);

  d = malloc(g * g);
  for (y = 0; y < g; y++) for (x = 0; x < g; x++)
    d[y * g + x] = sum_gaussian(gaussian_map, od, (g - 1 - x) - center, y - center, win, win);
  g_shadow_tiles[op].corner_tr = a8_picture_from_data(dpy, d, g, g, False);

  d = malloc(g * g);
  for (y = 0; y < g; y++) for (x = 0; x < g; x++)
    d[y * g + x] = sum_gaussian(gaussian_map, od, x - center, (g - 1 - y) - center, win, win);
  g_shadow_tiles[op].corner_bl = a8_picture_from_data(dpy, d, g, g, False);

  d = malloc(g * g);
  for (y = 0; y < g; y++) for (x = 0; x < g; x++)
    d[y * g + x] = sum_gaussian(gaussian_map, od, (g - 1 - x) - center, (g - 1 - y) - center, win, win);
  g_shadow_tiles[op].corner_br = a8_picture_from_data(dpy, d, g, g, False);

  // top/bottom edges (1 wide x g tall, repeated horizontally)
  d = malloc(g);
  for (y = 0; y < g; y++) d[y] = sum_gaussian(gaussian_map, od, y - center, center, win, win);
  g_shadow_tiles[op].edge_top = a8_picture_from_data(dpy, d, 1, g, True);

  d = malloc(g);
  for (y = 0; y < g; y++) d[y] = sum_gaussian(gaussian_map, od, (g - 1 - y) - center, center, win, win);
  g_shadow_tiles[op].edge_bottom = a8_picture_from_data(dpy, d, 1, g, True);

  // left/right edges (g wide x 1 tall, repeated vertically)
  d = malloc(g);
  for (x = 0; x < g; x++) d[x] = sum_gaussian(gaussian_map, od, x - center, center, win, win);
  g_shadow_tiles[op].edge_left = a8_picture_from_data(dpy, d, g, 1, True);

  d = malloc(g);
  for (x = 0; x < g; x++) d[x] = sum_gaussian(gaussian_map, od, (g - 1 - x) - center, center, win, win);
  g_shadow_tiles[op].edge_right = a8_picture_from_data(dpy, d, g, 1, True);

  // solid center
  d = malloc(1);
  d[0] = sum_gaussian(gaussian_map, od, center, center, win, win);
  g_shadow_tiles[op].center = a8_picture_from_data(dpy, d, 1, 1, True);

  g_shadow_tiles[op].built = True;
}

// Composite a window's shadow from the 9 shared tiles (replaces the single
// full-size composite). The tiled rects exactly partition the sw x sh shadow.
static void
render_shadow_9patch(Display *dpy, win *w) {
  const shadow_tiles *t = &g_shadow_tiles[w->shadow_opacity_int];
  const int g = gaussian_map->size;
  const int sx = w->a.x + w->shadow_dx;
  const int sy = w->a.y + w->shadow_dy;
  const int sw = w->shadow_width;
  const int sh = w->shadow_height;
  const int mw = sw - 2 * g;   // middle (stretched) width  > 0 by eligibility
  const int mh = sh - 2 * g;   // middle (stretched) height > 0 by eligibility

#define COMP(mask, dx, dy, ww, hh) \
  XRenderComposite(dpy, PictOpOver, cshadow_picture, (mask), root_buffer, \
    0, 0, 0, 0, sx + (dx), sy + (dy), (ww), (hh))

  COMP(t->corner_tl, 0,      0,      g,  g);
  COMP(t->corner_tr, sw - g, 0,      g,  g);
  COMP(t->corner_bl, 0,      sh - g, g,  g);
  COMP(t->corner_br, sw - g, sh - g, g,  g);
  COMP(t->edge_top,    g,      0,      mw, g);
  COMP(t->edge_bottom, g,      sh - g, mw, g);
  COMP(t->edge_left,   0,      g,      g,  mh);
  COMP(t->edge_right,  sw - g, g,      g,  mh);
  COMP(t->center,      g,      g,      mw, mh);

#undef COMP
}


static void
paint_root(Display *dpy) {
  if (!root_tile) {
    root_tile = root_create_tile();
  }

  XRenderComposite(
    dpy, PictOpSrc, root_tile, None,
    root_buffer, 0, 0, 0, 0, 0, 0,
    root_width, root_height);
}

static shadowtype shadow_find_type(win *w){
  if(unlikely(!w->window_type) ||
     ! win_type_shadow[w->window_type] ||
     is_gtk_frame_extent(dpy, w->id)){
    return SHADOW_NO;
  }
  if (w->mode == WINDOW_SOLID && ! w->a.override_redirect) {
    return SHADOW_FULL;
  }
  if (w->mode != WINDOW_SOLID && w->a.override_redirect) {
    // GTK4 may may render popups and menus using non-solid override_redirect windows.
    // Avoid Double-shadow and only draw shadows in case of WINTYPE_NOTIFY. Alternatively,
    // or additionally, we could use _NET_WM_OPAQUE_REGION, s. also
    // https://gitlab.gnome.org/GNOME/gtk/-/issues/3879
    switch(w->window_type){
    case WINTYPE_NOTIFY: return SHADOW_FULL;
    default: return SHADOW_NO;
    }
  }

  // A non-solid (e.g. ARGB) XOR override_redirect window. Render a shadow for those only
  // at the borders but not directly below the window. Examples include:
  // * transparent terminals (ARGB): a center-shadow would make the font less readable
  // * zoom's screen-share (override_redirect): a center-shadow darkens the whole desktop.
  //   Note: currently, we do not track whether a window makes use of the XShape extension.
  //         zoom does, so in the future we may use that instead.
  return SHADOW_NOCENTER;

}

static bool shadow_should_render(shadowtype t){
  switch (t) {
  case SHADOW_UNKNOWN: return false;
  case SHADOW_FULL: return true;
  case SHADOW_NOCENTER: return true;
  case SHADOW_NO: return false;
  }
  assert(false);
  return false;
}

static bool
win_needs_second_pass(const win *w) {
  return shadow_should_render(w->shadow_type)
      || w->mode != WINDOW_SOLID || HAS_FRAME_OPACITY(w);
}

static XserverRegion
win_extents(Display *dpy, win *w) {
  XRectangle r;

  r.x = w->a.x;
  r.y = w->a.y;
  r.width = w->a.width + w->a.border_width * 2;
  r.height = w->a.height + w->a.border_width * 2;

  if(unlikely(w->shadow_type == SHADOW_UNKNOWN)){
    w->shadow_type = shadow_find_type(w);
  }

  if (shadow_should_render(w->shadow_type)) {
    XRectangle sr;
    int gsize = gaussian_map->size;
    int win_w = w->a.width + w->a.border_width * 2;
    int win_h = w->a.height + w->a.border_width * 2;

    double opacity = shadow_opacity;
    if (w->mode != WINDOW_SOLID) {
      opacity = opacity * ((double)w->opacity) / ((double)OPAQUE);
    }
    if (HAS_FRAME_OPACITY(w)) {
      opacity = opacity * frame_opacity;
    }

    w->shadow_dx = shadow_offset_x;
    w->shadow_dy = shadow_offset_y;

    // 9-patch fast path: a full (opaque-center) shadow on a window bigger than
    // the blur kernel is just corners+edges+solid-center, all size-independent.
    // Composite it from shared tiles so a resize never rebuilds an O(w*h) mask.
    // SHADOW_NOCENTER keeps the per-window path (its cleared center depends on
    // window size/offset), as do windows too small to tile.
    if (w->shadow_type == SHADOW_FULL && gsize > 0 && win_w > gsize && win_h > gsize) {
      int op = (int)(opacity * 25);
      if (op < 0) op = 0;
      if (op > 25) op = 25;
      ensure_shadow_tiles(dpy, op);
      if (w->shadow) {
        XRenderFreePicture(dpy, w->shadow);
        w->shadow = None;
      }
      w->shadow_9patch = True;
      w->shadow_opacity_int = op;
      w->shadow_width = win_w + gsize;
      w->shadow_height = win_h + gsize;
    } else {
      w->shadow_9patch = False;
      if (!w->shadow) {
        w->shadow = shadow_picture(
          dpy, opacity, w->shadow_type,
          win_w, win_h,
          &w->shadow_width, &w->shadow_height);
      }
    }

    sr.x = w->a.x + w->shadow_dx;
    sr.y = w->a.y + w->shadow_dy;
    sr.width = w->shadow_width;
    sr.height = w->shadow_height;

    if (sr.x < r.x) {
      r.width = (r.x + r.width) - sr.x;
      r.x = sr.x;
    }

    if (sr.y < r.y) {
      r.height = (r.y + r.height) - sr.y;
      r.y = sr.y;
    }

    if (sr.x + sr.width > r.x + r.width) {
      r.width = sr.x + sr.width - r.x;
    }

    if (sr.y + sr.height > r.y + r.height) {
      r.height = sr.y + sr.height - r.y;
    }
  }
  if(! w->extents){
    w->extents = XFixesCreateRegion(dpy, &r, 1);
  } else {
    XFixesSetRegion(dpy, w->extents, &r, 1);
  }
  return w->extents;

}

/* Shadow geometry and opacity depend on the current rendering mode.  The
 * shared 9-patch path has no per-window Picture, so invalidating only
 * w->shadow leaves stale tiles after opacity/focus changes. */
static void
invalidate_shadow(Display *dpy, win *w) {
  if (w->shadow) {
    XRenderFreePicture(dpy, w->shadow);
    w->shadow = None;
  }
  w->shadow_9patch = False;
  w->shadow_opacity_int = 0;
  w->shadow_type = SHADOW_UNKNOWN;

  if (w->extents) {
    add_damage(dpy, w->extents);
    add_damage(dpy, win_extents(dpy, w));
  }
}

static XserverRegion
border_size(Display *dpy, win *w) {
  XserverRegion border;

  /*
   * if window doesn't exist anymore,  this will generate an error
   * as well as not generate a region.  Perhaps a better XFixes
   * architecture would be to have a request that copies instead
   * of creates, that way you'd just end up with an empty region
   * instead of an invalid XID.
   */

  set_ignore(dpy, NextRequest(dpy));
  border = XFixesCreateRegionFromWindow(
    dpy, w->id, WindowRegionBounding);

  /* translate this */
  set_ignore(dpy, NextRequest(dpy));
  XFixesTranslateRegion(dpy, border,
    w->a.x + w->a.border_width,
    w->a.y + w->a.border_width);

  return border;
}

static Window
find_client_win(Display *dpy, Window win) {
  Window root, parent;
  Window *children;
  unsigned int nchildren;
  unsigned int i;
  Window client = 0;

  if(win_is_client(win)){
    return win;
  }

  if (!XQueryTree(dpy, win, &root,
      &parent, &children, &nchildren)) {
    return 0;
  }

  for (i = 0; i < nchildren; i++) {
    client = find_client_win(dpy, children[i]);
    if (client) break;
  }

  if (children) XFree((char *)children);

  return client;
}

static void
get_frame_extents(win* w,
                  unsigned int *left,
                  unsigned int *right,
                  unsigned int *top,
                  unsigned int *bottom) {
  unsigned long *extents;
  Atom type;
  int format;
  unsigned long nitems, after;
  unsigned char *data = NULL;
  int result;
  Window client_window = 0;

  *left = 0;
  *right = 0;
  *top = 0;
  *bottom = 0;

  client_window = find_client_win(dpy, w->id);
  if (!client_window) {
    w->hidden_type = win_state_is_hidden( w->id) ? HIDDEN_YES : HIDDEN_NO;
    return;
  }

  if(w->id != client_window){
    // i3 in tabbed mode changes the _NET_WM_STATE attribute from
    // _NET_WM_STATE_FOCUSED to _NET_WM_STATE_HIDDEN when switching tabs. Further, i3
    // spans a "container" window around the client, while only the client has the
    // _NET_WM_STATE attribute set. Thus, although fastcompmgr normally only listens to
    // events of "container" windows, in this case, we also have to register for
    // "Property" changes of the client window:
    win_register_client_events(client_window);
  }
  w->hidden_type = win_state_is_hidden(client_window) ? HIDDEN_YES : HIDDEN_NO;

  // FIXME: determine the active window on fastcompmgr startup and set opacity accordingly
  // if(win_has_focus(client_window)){
  //   fprintf(stderr, "YES, HAS FOCUS: 0x%lx\n", client_window);
  // }

  result = XGetWindowProperty(
    dpy, client_window, atom_net_frame_extents,
    0L, 4L, False, XA_CARDINAL,
    &type, &format, &nitems, &after,
    (unsigned char **)&data);

  if (result == Success) {
    if (data && type == XA_CARDINAL && format == 32 && nitems == 4 && after == 0) {
      extents = (unsigned long *)data;
      *left =
        (unsigned int)extents[0];
      *right =
        (unsigned int)extents[1];
      *top =
        (unsigned int)extents[2];
      *bottom =
        (unsigned int)extents[3];
    }
    XFree(data);
  }
}

static Bool
win_paint_needed(win* w, CompRect* ignore_reg){
  // if invisible, ignore it
    if (unlikely(w->a.x + w->a.width < 1 || w->a.y + w->a.height < 1
        || w->a.x >= root_width || w->a.y >= root_height)) {
      return False;
    }

    switch (w->hidden_type) {
    case HIDDEN_UNKNOWN: {
      fprintf(stderr, "fastcompmgr warning: hidden state still unknown in "
                      "win_paint_needed: 0x%lx\n", w->id);
      Window client_window = find_client_win(dpy, w->id);
      if (!client_window) {
        // We already tried to find a client on add_win - give up for now.
        w->hidden_type = HIDDEN_IGNORE;
        break;
      }

      win_register_client_events(client_window);
      if(win_state_is_hidden(client_window)){
        w->hidden_type = HIDDEN_YES;
        return false;
      } else {
        w->hidden_type = HIDDEN_NO;
      }
      break;
    }
    case HIDDEN_YES: return false;
    case HIDDEN_NO: break;
    case HIDDEN_IGNORE: break;
    }

    // Unmapped, destroyed or translucent windows must not contribute to the ignore region.
    // Same applies to override_redirect windows, which some screenshooter apps employ
    // (s. e.g. xfce4-screenshooter
    // screenshooter-capture.c::get_rectangle_screenshot_composited )
    if (w->a.map_state != IsViewable || w->destroyed || w->opacity != OPAQUE ||
        w->a.override_redirect){
      return True;
    }
    CompRect w_rect = {.x1 = w->a.x, .y1 = w->a.y,
                   .x2 = w->a.x + w->a.width, .y2 = w->a.y + w->a.height,
                   .w = w->a.width , .h = w->a.height };
    return rect_paint_needed(ignore_reg, &w_rect);
}

static void
paint_all(Display *dpy, XserverRegion region) {
  win *w;
  win *t = 0;
  Bool ignore_region_is_dirty = g_paint_ignore_region_is_dirty;
  g_paint_ignore_region_is_dirty = False;

#if MONITOR_REPAINT
  root_buffer = root_picture;
#else
  if (unlikely(!root_buffer)) {
    Pixmap rootPixmap = XCreatePixmap(
      dpy, root, root_width, root_height,
      DefaultDepth(dpy, g_screen));

    root_buffer = XRenderCreatePicture(dpy, rootPixmap,
      XRenderFindVisualFormat(dpy, DefaultVisual(dpy, g_screen)),
      0, 0);

    XFreePixmap(dpy, rootPixmap);
  }
#endif

  XFixesSetPictureClipRegion(dpy, root_picture, 0, 0, region);
  XFixesCopyRegion(dpy, screen_damage, region);

#if MONITOR_REPAINT
  XRenderComposite(
    dpy, PictOpSrc, black_picture, None,
    root_picture, 0, 0, 0, 0, 0, 0,
    root_width, root_height);
#endif

#if DEBUG_REPAINT
  printf("paint:");
#endif

  CompRect ignore_reg = {0};
  for (w = list; w; w = w->next) {
    // Don't do this here, otherwise we get artifacts after move.
    // if (w->need_configure){
    //   do_configure_win(dpy, w);
    // }

#if CAN_DO_USABLE
    if (!w->usable) continue;
#endif

    /* never painted, ignore it */
    if (likely(!w->damaged)) continue;

    // Note that undamaged windows should not contribute to the ignore
    // region. Otherwise VBoxManager makes other windows disappear during startup.
    if(unlikely(ignore_region_is_dirty || clip_changed)){
      // maybe_todo: pass only clipped rects, actually visible on screen.
      // Now we may choose the ignore region from a big window which
      // resides largely outside the screen.
      w->paint_needed = win_paint_needed(w, &ignore_reg);
    }
    if(!w->paint_needed) continue;

    if (!w->picture) {
      XRenderPictureAttributes pa;
      XRenderPictFormat *format;
      Drawable draw = w->id;

#if HAS_NAME_WINDOW_PIXMAP
      if (has_name_pixmap && !w->pixmap) {
        set_ignore(dpy, NextRequest(dpy));
        w->pixmap = XCompositeNameWindowPixmap(dpy, w->id);
      }
      if (w->pixmap) draw = w->pixmap;
#endif

      format = XRenderFindVisualFormat(dpy, w->a.visual);
      pa.subwindow_mode = IncludeInferiors;
      w->picture = XRenderCreatePicture(
        dpy, draw, format, CPSubwindowMode, &pa);
    }

#if DEBUG_REPAINT
    printf(" 0x%x", w->id);
#endif

    if (!w->border_size) {
      w->border_size = border_size (dpy, w);
    }

    if (unlikely(!w->extents)) {
      win_extents(dpy, w);
    }

    if (w->mode == WINDOW_SOLID && !HAS_FRAME_OPACITY(w)) {
      int x, y, wid, hei;

#if HAS_NAME_WINDOW_PIXMAP
      x = w->a.x;
      y = w->a.y;
      wid = w->a.width + w->a.border_width * 2;
      hei = w->a.height + w->a.border_width * 2;
#else
      x = w->a.x + w->a.border_width;
      y = w->a.y + w->a.border_width;
      wid = w->a.width;
      hei = w->a.height;
#endif

      set_ignore(dpy, NextRequest(dpy));
      XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, region);

      set_ignore(dpy, NextRequest(dpy));
      XFixesSubtractRegion(dpy, region, region, w->border_size);

      XRenderComposite(
        dpy, PictOpSrc, w->picture,
        None, root_buffer, 0, 0, 0, 0,
        x, y, wid, hei);
    }

    if (win_needs_second_pass(w)) {
      if (!w->border_clip) {
        w->border_clip = XFixesCreateRegion(dpy, 0, 0);
      }
      XFixesCopyRegion(dpy, w->border_clip, region);
      w->prev_trans = t;
      t = w;
    }
  }

#if DEBUG_REPAINT
  printf("\n");
  fflush(stdout);
#endif

  XFixesSetPictureClipRegion(dpy,
    root_buffer, 0, 0, region);
  paint_root(dpy);

  for (w = t; w; w = w->prev_trans) {
    XFixesSetPictureClipRegion(dpy,
      root_buffer, 0, 0, w->border_clip);

    if(shadow_should_render(w->shadow_type)) {
      if (w->shadow_9patch) {
        render_shadow_9patch(dpy, w);
      } else if (w->shadow) {
        XRenderComposite(
          dpy, PictOpOver, cshadow_picture, w->shadow,
          root_buffer, 0, 0, 0, 0,
          w->a.x + w->shadow_dx, w->a.y + w->shadow_dy,
          w->shadow_width, w->shadow_height);
      }
    }

    if (w->opacity != OPAQUE && !w->alpha_pict) {
      w->alpha_pict = solid_picture(
        dpy, False, (double)w->opacity / OPAQUE, 0, 0, 0);
    }
    if (HAS_FRAME_OPACITY(w) && !w->alpha_border_pict) {
      w->alpha_border_pict = solid_picture(
        dpy, False, frame_opacity, 0, 0, 0);
    }

    if (w->mode != WINDOW_SOLID || HAS_FRAME_OPACITY(w)) {
      int x, y, wid, hei;
      // 2024-11-26: Without the next two lines, the Microsoft-Teams screen-share
      // window has a broken frame instead of a shadow, with a "startup-frozen"
      // picture. Inspired by xcompmgr's commit 5a7d139f (2012-08-11).
      XFixesIntersectRegion(dpy, w->border_clip, w->border_clip, w->border_size);
      XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, w->border_clip);

#if HAS_NAME_WINDOW_PIXMAP
      x = w->a.x;
      y = w->a.y;
      wid = w->a.width + w->a.border_width * 2;
      hei = w->a.height + w->a.border_width * 2;
#else
      x = w->a.x + w->a.border_width;
      y = w->a.y + w->a.border_width;
      wid = w->a.width;
      hei = w->a.height;
#endif

      set_ignore(dpy, NextRequest(dpy));

      if (!HAS_FRAME_OPACITY(w)) {
        XRenderComposite(
          dpy, PictOpOver, w->picture, w->alpha_pict,
          root_buffer, 0, 0, 0, 0, x, y, wid, hei);
      } else {
        /* TODO - clean me */
        unsigned int t = w->top_width;
        unsigned int l = w->left_width;
        unsigned int b = w->bottom_width;
        unsigned int r = w->right_width;

        if (l > (unsigned int)wid) l = wid;
        if (r > (unsigned int)wid - l) r = (unsigned int)wid - l;
        if (t > (unsigned int)hei) t = hei;
        if (b > (unsigned int)hei - t) b = (unsigned int)hei - t;

        /* top */
        XRenderComposite(
          dpy, PictOpOver, w->picture, w->alpha_border_pict, root_buffer,
          0, 0, 0, 0, x, y, wid, t);

        /* left */
        XRenderComposite(
          dpy, PictOpOver, w->picture, w->alpha_border_pict, root_buffer,
          0, t, 0, t, x, y + t, l, hei - t);

        /* bottom */
        XRenderComposite(
          dpy, PictOpOver, w->picture, w->alpha_border_pict, root_buffer,
          l, hei - b, l, hei - b, x + l, y + hei - b, wid - l - r, b);

        /* right */
        XRenderComposite(
          dpy, PictOpOver, w->picture, w->alpha_border_pict, root_buffer,
          wid - r, t, wid - r, t, x + wid - r, y + t, r, hei - t);

        /* body */
        XRenderComposite(
          dpy, PictOpOver, w->picture, w->alpha_pict, root_buffer,
          l, t, l, t, x + l, y + t, wid - l - r, hei - t - b);
      }
    }
  }

#if ! MONITOR_REPAINT
    XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, screen_damage);
    XRenderComposite(
      dpy, PictOpSrc, root_buffer, None,
      root_picture, 0, 0, 0, 0,
      0, 0, root_width, root_height);
#endif // ! MONITOR_REPAINT
}

static void
add_damage(Display *dpy, XserverRegion damage) {
  if (all_damage_is_dirty) {
    XFixesUnionRegion(dpy, all_damage, all_damage, damage);
  } else {
    XFixesCopyRegion(dpy, all_damage, damage);
    all_damage_is_dirty = True;
  }
}


static void
add_damage_if_hidden_changed(Window window, bool is_reparent_event) {
  win *w = find_win_any_parent(window);
  if(unlikely(!w)){
    return;
  }
  if(is_reparent_event){
    win_register_client_events(window);
  }
  hiddentype hidden_type = win_state_is_hidden(window) ? HIDDEN_YES : HIDDEN_NO;
  // _NET_WM_STATE may change without altering _NET_WM_STATE_HIDDEN, so
  // check, if there's need for action.
  if(w->hidden_type == hidden_type){
    return;
  }
  w->hidden_type = hidden_type;
  if(w->extents){
    add_damage(dpy, w->extents);
  }
  clip_changed = True;
  set_paint_ignore_region_dirty();
}

static void
repair_win(Display *dpy, win *w) {
  XserverRegion parts;

  if (!w->damaged) {
    /* The first DamageNotify after a map/unmap represents a new window
     * image. Repaint its complete extents, as the original implementation
     * did, rather than deferring a possibly incomplete damage region. */
    parts = win_extents(dpy, w);
    set_ignore(dpy, NextRequest(dpy));
    XDamageSubtract(dpy, w->damage, None, None);
  } else {
    parts = g_xregion_tmp;
    set_ignore(dpy, NextRequest(dpy));
    XDamageSubtract(dpy, w->damage, None, parts);
    XFixesTranslateRegion(dpy, parts,
      w->a.x + w->a.border_width,
      w->a.y + w->a.border_width);
  }

  add_damage(dpy, parts);
  w->damaged = 1;
}

#if 0
static const char*
wintype_name(wintype type) {
  const char *t;

  switch (type) {
    case WINTYPE_DESKTOP:
      t = "desktop";
      break;
    case WINTYPE_DOCK:
      t = "dock";
      break;
    case WINTYPE_TOOLBAR:
      t = "toolbar";
      break;
    case WINTYPE_MENU:
      t = "menu";
      break;
    case WINTYPE_UTILITY:
      t = "utility";
      break;
    case WINTYPE_SPLASH:
      t = "slash";
      break;
    case WINTYPE_DIALOG:
      t = "dialog";
      break;
    case WINTYPE_NORMAL:
      t = "normal";
      break;
    case WINTYPE_DROPDOWN_MENU:
      t = "dropdown";
      break;
    case WINTYPE_POPUP_MENU:
      t = "popup";
      break;
    case WINTYPE_TOOLTIP:
      t = "tooltip";
      break;
    case WINTYPE_NOTIFY:
      t = "notification";
      break;
    case WINTYPE_COMBO:
      t = "combo";
      break;
    case WINTYPE_DND:
      t = "dnd";
      break;
    default:
      t = "unknown";
      break;
  }

  return t;
}
#endif

static wintype
get_wintype_prop(Display * dpy, Window w) {
  Atom actual;
  int format;
  unsigned long n, left, off;
  unsigned char *data;

  off = 0;

  do {
    set_ignore(dpy, NextRequest(dpy));

    int result = XGetWindowProperty(
      dpy, w, atom_win_type, off, 1L, False, XA_ATOM,
      &actual, &format, &n, &left, &data);

    if (unlikely(result != Success)) break;

    if (likely(data != None && actual == XA_ATOM && format == 32 && n == 1)) {
      int i;
      Atom a = ((Atom *)data)[0];
      for (i = 1; i < NUM_WINTYPES; ++i) {
        if (a == win_type[i]) {
          /* known type */
          XFree((void *) data);
          return i;
        }
      }
      XFree((void *) data);
    }
    else if (data != None) {
      XFree((void *) data);
    }
    ++off;
  } while (left >= 4);

  return WINTYPE_UNKNOWN;
}

static wintype
determine_wintype(Display *dpy, Window w, Window top) {
  Window root_return, parent_return;
  Window *children = NULL;
  unsigned int nchildren, i;
  wintype type;

  type = get_wintype_prop(dpy, w);
  if (type != WINTYPE_UNKNOWN) return type;

  set_ignore(dpy, NextRequest(dpy));
  if (unlikely(!XQueryTree(dpy, w, &root_return, &parent_return,
                  &children, &nchildren))) {
    goto free_out;
  }

  for (i = 0; i < nchildren; i++) {
    type = determine_wintype(dpy, children[i], top);
    if (type != WINTYPE_UNKNOWN) goto free_out;
  }

  if (w != top) {
    type = WINTYPE_UNKNOWN;
  } else {
    type = WINTYPE_NORMAL;
  }

free_out:
  if (children) XFree((void *)children);
  return type;
}

static unsigned int
get_opacity_prop(Display *dpy, win *w, unsigned int def);
static uint
win_suggest_opacity(win *w, bool *is_userdefined);

static void
handle_ConfigureNotify(Display *dpy, XConfigureEvent *ce);

static void
map_win(Display *dpy, Window id,
        unsigned long sequence, Bool fade) {
  win *w = find_win(id);

  if (unlikely(!w)) return;

  w->a.map_state = IsViewable;
  w->window_type = determine_wintype(dpy, w->id, w->id);

#if 0
  printf("window 0x%x type %s\n",
    w->id, wintype_name(w->window_type));
#endif

  /* select before reading the property
     so that no property changes are lost */
  XSelectInput(dpy, id, PropertyChangeMask | FocusChangeMask);

  w->opacity = win_suggest_opacity(w, &w->userdefined_opacity);

  determine_mode(dpy, w);

#if CAN_DO_USABLE
  w->damage_bounds.x = w->damage_bounds.y = 0;
  w->damage_bounds.width = w->damage_bounds.height = 0;
#endif
  w->damaged = 0;
  w->paint_needed = True;

  if (fade && win_type_fade[w->window_type]) {
    set_fade(
      dpy, w, 0, get_opacity_percent(dpy, w),
      fade_in_step, 0, True, True);
  }

  set_paint_ignore_region_dirty();

  /* if any configure events happened while
     the window was unmapped, then configure
     the window to its correct place */
  // if (w->need_configure) {
  //   handle_ConfigureNotify(dpy, &w->queue_configure);
  // }
}

static void
finish_unmap_win(Display *dpy, win *w) {
  w->damaged = 0;
#if CAN_DO_USABLE
  w->usable = False;
#endif

  if (w->extents != None) {
    add_damage(dpy, w->extents);
  }

#if HAS_NAME_WINDOW_PIXMAP
  if (w->pixmap) {
    XFreePixmap(dpy, w->pixmap);
    w->pixmap = None;
  }
#endif

  if (w->picture) {
    set_ignore(dpy, NextRequest(dpy));
    XRenderFreePicture(dpy, w->picture);
    w->picture = None;
  }

  if (w->border_size) {
    set_ignore(dpy, NextRequest(dpy));
      XFixesDestroyRegion(dpy, w->border_size);
      w->border_size = None;
  }

  if (w->shadow) {
    XRenderFreePicture(dpy, w->shadow);
    w->shadow = None;
  }

  clip_changed = True;
}

#if HAS_NAME_WINDOW_PIXMAP
static void
unmap_callback(Display *dpy, win *w) {
  finish_unmap_win(dpy, w);
}
#endif

static void
unmap_win(Display *dpy, Window id, Bool fade) {
  win *w = find_win(id);

  if (!w) return;

    /* don't care about properties anymore */
  set_ignore(dpy, NextRequest(dpy));
  XSelectInput(dpy, w->id, 0);

  w->a.map_state = IsUnmapped;
  set_paint_ignore_region_dirty();


#if HAS_NAME_WINDOW_PIXMAP
  if (w->pixmap && fade && win_type_fade[w->window_type]) {
    set_fade(dpy, w, w->opacity * 1.0 / OPAQUE, 0.0,
             fade_out_step, unmap_callback, False, True);
  } else
#endif
    finish_unmap_win(dpy, w);
}

static bool is_gtk_frame_extent(Display *dpy, Window w){
  Atom type;
  int format;
  unsigned long nitems, after;
  unsigned char *data = NULL;
  int result;

  result = XGetWindowProperty(dpy, w, atom_gtk_frame_extents, 0, LONG_MAX,
    false, XA_CARDINAL, &type, &format, &nitems, &after, (unsigned char **)&data);
  if (result == Success && data != NULL) {
    bool valid = type == XA_CARDINAL && format == 32 && nitems == 4 && after == 0;
    XFree((void *)data);
    return valid;
  }
  return false;
}

/* Get the opacity prop from window
   not found: default
   otherwise the value
 */
static bool
get_opacity_prop_for_window(Display *dpy, Window window, unsigned int *opacity) {
  Atom actual;
  int format;
  unsigned long n, left;

  unsigned char *data = NULL;
  int result = XGetWindowProperty(
    dpy, window, atom_opacity, 0L, 1L, False,
    XA_CARDINAL, &actual, &format, &n, &left, &data);

  if (result == Success && data != NULL && actual == XA_CARDINAL
      && format == 32 && n == 1 && left == 0) {
    unsigned long value = ((unsigned long *)data)[0];
    XFree((void *)data);
    if (value <= UINT_MAX) {
      *opacity = (unsigned int)value;
      return true;
    }
    return false;
  }
  if (data) XFree((void *)data);
  return false;
}

static unsigned int
get_opacity_prop(Display *dpy, win *w, unsigned int def) {
  unsigned int opacity;
  if (get_opacity_prop_for_window(dpy, w->id, &opacity)) return opacity;

  Window client = find_client_win(dpy, w->id);
  if (client && client != w->id
      && get_opacity_prop_for_window(dpy, client, &opacity)) return opacity;
  return def;
}

/*
 * Get the opacity property from the window in a percent format
 * not found: default
 * otherwise: the value
*/

static double
get_opacity_percent(Display *dpy, win *w) {
  double def = win_type_opacity[w->window_type];
  unsigned int opacity =
    get_opacity_prop(dpy, w, (unsigned int)(OPAQUE * def));

  return opacity * 1.0 / OPAQUE;
}

static void
determine_mode(Display *dpy, win *w) {
  int mode;
  XRenderPictFormat *format;

  /* if trans prop == -1 fall back on  previous tests*/

  if (w->alpha_pict) {
    XRenderFreePicture(dpy, w->alpha_pict);
    w->alpha_pict = None;
  }

  if (w->alpha_border_pict) {
    XRenderFreePicture(dpy, w->alpha_border_pict);
    w->alpha_border_pict = None;
  }

  if (w->shadow_pict) {
    XRenderFreePicture(dpy, w->shadow_pict);
    w->shadow_pict = None;
  }

  if (w->a.class == InputOnly) {
    format = 0;
  } else {
    format = XRenderFindVisualFormat(dpy, w->a.visual);
  }

  if (format && format->type == PictTypeDirect
      && format->direct.alphaMask) {
    mode = WINDOW_ARGB;
  } else if (w->opacity != OPAQUE) {
    mode = WINDOW_TRANS;
  } else {
    mode = WINDOW_SOLID;
  }

  w->mode = mode;

  if (w->extents) {
    add_damage(dpy, w->extents);
  }
}

static void
set_opacity(Display *dpy, win *w, unsigned long opacity) {
  if (fade_trans) {
    double old_opacity = (double)w->opacity / OPAQUE;
    w->opacity = opacity;
    set_fade(dpy, w, old_opacity,
      (double)w->opacity / OPAQUE,
      fade_out_step, 0, True, False);
  } else {
    w->opacity = opacity;
    determine_mode(dpy, w);
    invalidate_shadow(dpy, w);
  }
  set_paint_ignore_region_dirty();
}


static uint
win_suggest_opacity(win* w, bool* is_userdefined){
  uint default_op = (uint)(win_type_opacity[w->window_type]*OPAQUE);
  uint actual_opacity = get_opacity_prop(g_dpy, w, default_op);
  *is_userdefined = actual_opacity != default_op;
  return actual_opacity;
}


static void
add_win(Display *dpy, Window id, Window prev) {
  win *new = calloc(1, sizeof(win));
  win **p;

  if (unlikely(!new)) return;

  if (prev) {
    for (p = &list; *p; p = &(*p)->next) {
      if ((*p)->id == prev && !(*p)->destroyed)
        break;
    }
  } else {
    p = &list;
  }

  new->id = id;
  set_ignore(dpy, NextRequest(dpy));

  if (unlikely(!XGetWindowAttributes(dpy, id, &new->a))) {
    free(new);
    return;
  }

#if HAS_NAME_WINDOW_PIXMAP
  new->pixmap = None;
#endif
  new->picture = None;

  if (new->a.class == InputOnly) {
    // we used calloc, so no need to set zeroes
    // new->damage_sequence = 0;
    new->damage = None;
  } else {
    new->damage_sequence = NextRequest(dpy);
    set_ignore(dpy, NextRequest(dpy));
    new->damage = XDamageCreate(dpy, id, XDamageReportNonEmpty);
  }

  new->alpha_pict = None;
  new->alpha_border_pict = None;
  new->shadow_pict = None;
  new->border_size = None;
  new->extents = None;
  new->shadow = None;
  new->shadow_9patch = False;

  // we used calloc, so no need to set zeroes
  // new->shadow_dx = 0;
  // new->shadow_dy = 0;
  // new->shadow_width = 0;
  // new->shadow_height = 0;
  // new->window_type = WINTYPE_UNKNOWN;
  // new->prev_trans = 0;
  // new->left_width = 0;
  // new->right_width = 0;
  // new->top_width = 0;
  // new->bottom_width = 0;
  // new->need_configure = False;
  // new->destroyed = False;
  // new->damaged = 0;
// #if CAN_DO_USABLE
  // new->usable = False;
// #endif

  new->opacity = OPAQUE;

  new->border_clip = None;
  get_frame_extents(new,
    &new->left_width, &new->right_width,
    &new->top_width, &new->bottom_width);

  new->next = *p;
  *p = new;

  if (new->a.map_state == IsViewable) {
    new->window_type = determine_wintype(dpy, id, id);
    new->opacity = win_suggest_opacity(new, &new->userdefined_opacity);
    map_win(dpy, id, new->damage_sequence - 1, True);
  }
}

static void
set_paint_ignore_region_dirty(void){
  g_paint_ignore_region_is_dirty = True;
}

void
restack_win(Display *dpy, win *w, Window new_above) {
  Window old_above;

  if (w->next) {
    old_above = w->next->id;
  } else {
    old_above = None;
  }

  if (old_above != new_above) {
    win **prev;

    /* unhook */
    for (prev = &list; *prev; prev = &(*prev)->next) {
      if ((*prev) == w) break;
    }

    *prev = w->next;

    /* rehook */
    for (prev = &list; *prev; prev = &(*prev)->next) {
      if ((*prev)->id == new_above && !(*prev)->destroyed)
        break;
    }

    w->next = *prev;
    *prev = w;
  }
}

static void
do_configure_win(Display *dpy, win* w){
  XConfigureEvent* ce = &w->queue_configure;
  int dx = ce->x - w->a.x;
  int dy = ce->y - w->a.y;
  bool border_changed = w->a.border_width != ce->border_width;

  w->need_configure = False;
  w->a.x = ce->x;
  w->a.y = ce->y;
  if (w->configure_size_changed || border_changed) {

#if HAS_NAME_WINDOW_PIXMAP
    if (w->pixmap) {
      XFreePixmap(dpy, w->pixmap);
      w->pixmap = None;
      if (w->picture) {
        XRenderFreePicture(dpy, w->picture);
        w->picture = None;
      }
    }
#endif

    if (w->shadow) {
      XRenderFreePicture(dpy, w->shadow);
      w->shadow = None;
    }
  }

  w->a.width = ce->width;
  w->a.height = ce->height;
  w->a.border_width = ce->border_width;
  bool override_changed = w->a.override_redirect != ce->override_redirect;
  w->a.override_redirect = ce->override_redirect;
  if (override_changed) invalidate_shadow(dpy, w);

  // border_size is this window's bounding region in screen coordinates, so it
  // only changes when this window itself moves/resizes - invalidate it here so
  // paint_all rebuilds it. Previously paint_all destroyed+recreated border_size
  // (and extents) for *every* painted window on *every* clip_changed, needlessly
  // churning regions of non-moving windows during a drag.
  // ponytail: reshapes (XShape) without a configure are not tracked here, same
  // as the rest of the code; the next configure refreshes it.
  if (w->border_size && (w->configure_size_changed || border_changed)) {
    XFixesDestroyRegion(dpy, w->border_size);
    w->border_size = None;
  } else if (w->border_size && (dx || dy)) {
    XFixesTranslateRegion(dpy, w->border_size, dx, dy);
  }

  if (w->a.map_state != IsUnmapped
#if CAN_DO_USABLE
    && w->usable
#endif
      ) {
    // both, the old and new window position/size are damaged.
    if (likely(w->extents != None)) {
      add_damage(dpy, w->extents);
    }
    add_damage(dpy, win_extents(dpy, w));
  }

  clip_changed = True;
  w->configure_size_changed = false;
  set_paint_ignore_region_dirty();
}

Bool g_configure_needed = False;

static void
handle_ConfigureNotify(Display *dpy, XConfigureEvent *ce) {
  win *w = find_win(ce->window);

  if (unlikely(!w)) {
    if (ce->window == root) {
      if (root_buffer) {
        XRenderFreePicture(dpy, root_buffer);
        root_buffer = None;
      }
      root_width = ce->width;
      root_height = ce->height;
    }
    return;
  }
  // save the configure event for later. While on the one hand, we're only
  // interested in the final position and size (after timeout), a change in size
  // invalidates the pixmap, so remember any resize event.
  g_configure_needed = True;
  w->need_configure = True;
  if (w->a.width != ce->width || w->a.height != ce->height
      || w->a.border_width != ce->border_width) {
    w->configure_size_changed = true;
  }

  w->queue_configure = *ce;

  // w->a.override_redirect = ce->override_redirect;
  restack_win(dpy, w, ce->above);
}

static void
circulate_win(Display *dpy, XCirculateEvent *ce) {
  win *w = find_win(ce->window);
  Window new_above;

  if (!w) return;

  if (ce->place == PlaceOnTop) {
    new_above = list->id;
  } else {
    new_above = None;
  }

  restack_win(dpy, w, new_above);
  clip_changed = True;
}

static void
finish_destroy_win(Display *dpy, Window id) {
  win **prev, *w;

  for (prev = &list; (w = *prev); prev = &w->next) {
    if (w->id == id && w->destroyed) {
      finish_unmap_win(dpy, w);
      *prev = w->next;

      if (w->alpha_pict) {
        XRenderFreePicture(dpy, w->alpha_pict);
        w->alpha_pict = None;
      }

      if (w->alpha_border_pict) {
        XRenderFreePicture(dpy, w->alpha_border_pict);
        w->alpha_border_pict = None;
      }

      if (w->shadow_pict) {
        XRenderFreePicture(dpy, w->shadow_pict);
        w->shadow_pict = None;
      }

      /* fix leak, from freedesktop repo */
      if (w->shadow) {
        XRenderFreePicture (dpy, w->shadow);
        w->shadow = None;
      }

      if (w->damage != None) {
        set_ignore(dpy, NextRequest(dpy));
        XDamageDestroy(dpy, w->damage);
        w->damage = None;
      }

      cleanup_fade(dpy, w);

      if (w->border_clip) {
        XFixesDestroyRegion(dpy, w->border_clip);
        w->border_clip = None;
      }
      if(w->extents){
        XFixesDestroyRegion(dpy, w->extents);
        w->extents = None;
      }
      free(w);
      break;
    }
  }
}

#if HAS_NAME_WINDOW_PIXMAP
static void
destroy_callback(Display *dpy, win *w) {
  finish_destroy_win(dpy, w->id);
}
#endif

static void
destroy_win(Display *dpy, Window id, Bool fade) {
  win *w = find_win(id);

  if (w) w->destroyed = True;

  set_paint_ignore_region_dirty();

#if HAS_NAME_WINDOW_PIXMAP
  if (w && w->pixmap && fade && win_type_fade[w->window_type]) {
    set_fade(dpy, w, w->opacity * 1.0 / OPAQUE,
      0.0, fade_out_step, destroy_callback,
      False, True);
  } else
#endif
  {
    finish_destroy_win(dpy, id);
  }
}

#if 0
static void
dump_win(win *w) {
  printf("\t%08lx: %d x %d + %d + %d(%d)\n", w->id,
    w->a.width, w->a.height,
    w->a.x, w->a.y, w->a.border_width);
}


static void
dump_wins(void) {
  win *w;

  printf("windows:\n");
  for (w = list; w; w = w->next) {
    dump_win(w);
  }
}
#endif

static void
damage_win(Display *dpy, XDamageNotifyEvent *de) {
  win *w = find_win(de->drawable);

  if (unlikely(!w)) return;

#if CAN_DO_USABLE
  if (!w->usable) {
    if (w->damage_bounds.width == 0 || w->damage_bounds.height == 0) {
      w->damage_bounds = de->area;
    } else {
      if (de->area.x < w->damage_bounds.x) {
        w->damage_bounds.width += (w->damage_bounds.x - de->area.x);
        w->damage_bounds.x = de->area.x;
      }
      if (de->area.y < w->damage_bounds.y) {
        w->damage_bounds.height += (w->damage_bounds.y - de->area.y);
        w->damage_bounds.y = de->area.y;
      }
      if (de->area.x + de->area.width
          > w->damage_bounds.x + w->damage_bounds.width) {
        w->damage_bounds.width =
          de->area.x + de->area.width - w->damage_bounds.x;
      }
      if (de->area.y + de->area.height
          > w->damage_bounds.y + w->damage_bounds.height) {
        w->damage_bounds.height =
          de->area.y + de->area.height - w->damage_bounds.y;
      }
    }

#if 0
    printf("unusable damage %d, %d: %d x %d bounds %d, %d: %d x %d\n",
      de->area.x,
      de->area.y,
      de->area.width,
      de->area.height,
      w->damage_bounds.x,
      w->damage_bounds.y,
      w->damage_bounds.width,
      w->damage_bounds.height);
#endif

    if (w->damage_bounds.x <= 0
        && w->damage_bounds.y <= 0
        && w->a.width <= w->damage_bounds.x + w->damage_bounds.width
        && w->a.height <= w->damage_bounds.y + w->damage_bounds.height) {
      clip_changed = True;
      if (win_type_fade[w->window_type]) {
        set_fade(dpy, w, 0, get_opacity_percent(dpy, w),
                 fade_in_step, 0, True, True);
      }
      w->usable = True;
    }
  }

  if (w->usable)
#endif
  repair_win(dpy, w);
}

static int
error(Display *dpy, XErrorEvent *ev) {
  int o;
  const char *name = "Unknown";

  if (should_ignore(dpy, ev->serial)) {
    return 0;
  }

  if (ev->request_code == composite_opcode
      && ev->minor_code == X_CompositeRedirectSubwindows) {
    fprintf(stderr, "Another composite manager is already running\n");
    exit(1);
  }

  o = ev->error_code - xfixes_error;
  switch (o) {
    case BadRegion:
      name = "BadRegion";
      break;
    default:
      break;
  }

  o = ev->error_code - damage_error;
  switch (o) {
    case BadDamage:
      name = "BadDamage";
      break;
    default:
      break;
  }

  o = ev->error_code - render_error;
  switch (o) {
    case BadPictFormat:
      name ="BadPictFormat";
      break;
    case BadPicture:
      name ="BadPicture";
      break;
    case BadPictOp:
      name ="BadPictOp";
      break;
    case BadGlyphSet:
      name ="BadGlyphSet";
      break;
    case BadGlyph:
      name ="BadGlyph";
      break;
    default:
      break;
  }

  fprintf(stderr, "error %d (%s) request %d minor %d serial %lu\n",
      ev->error_code, name, ev->request_code,
      ev->minor_code, ev->serial);

/*  abort();      this is just annoying to most people */
  return 0;
}

static void
expose_root(Display *dpy, Window root, XRectangle *rects, int nrects) {
  XFixesSetRegion(dpy, g_xregion_tmp, rects, nrects);
  add_damage(dpy, g_xregion_tmp);
}

#if DEBUG_EVENTS
static int
ev_serial(XEvent *ev) {
  if ((ev->type & 0x7f) != KeymapNotify) {
    return ev->xany.serial;
  }
  return NextRequest(ev->xany.display);
}

static char *
ev_name(XEvent *ev) {
  static char buf[128];
  // what?? switch (ev->type & 0x7f) {
  switch (ev->type) {
    case FocusIn:
      return "FocusIn";
    case FocusOut:
      return "FocusOut";
    case CreateNotify:
      return "CreateNotify";
    case ConfigureNotify:
      return "ConfigureNotify";
    case DestroyNotify:
      return "DestroyNotify";
    case Expose:
      return "Expose";
    case MapNotify:
      return "Map";
    case UnmapNotify:
      return "Unmap";
    case ReparentNotify:
      return "Reparent";
    case CirculateNotify:
      return "Circulate";
    case PropertyNotify:
      return "PropertyNotify";
    default:
      if (ev->type == damage_event + XDamageNotify) {
        return "Damage";
      }
      sprintf(buf, "Event %d", ev->type);
      return buf;
  }
}

static Window
ev_window(XEvent *ev) {
  switch (ev->type) {
    case Expose:
      return ev->xexpose.window;
    case MapNotify:
      return ev->xmap.window;
    case UnmapNotify:
      return ev->xunmap.window;
    case ReparentNotify:
      return ev->xreparent.window;
    case CirculateNotify:
      return ev->xcirculate.window;
    default:
      if (ev->type == damage_event + XDamageNotify) {
        return ((XDamageNotifyEvent *)ev)->drawable;
      }
      return 0;
  }
}
#endif

static bool
parse_int_option(const char *text, int min, int max, int *value) {
  char *end;
  long parsed;

  errno = 0;
  parsed = strtol(text, &end, 10);
  if (errno || end == text || *end != '\0' || parsed < min || parsed > max) {
    return false;
  }
  *value = (int)parsed;
  return true;
}

static bool
parse_double_option(const char *text, double min, double max, double *value) {
  char *end;
  double parsed;

  errno = 0;
  parsed = strtod(text, &end);
  if (errno || end == text || *end != '\0' || !isfinite(parsed)
      || parsed < min || parsed > max) {
    return false;
  }
  *value = parsed;
  return true;
}

static void
invalid_option(const char *option, const char *value) {
  fprintf(stderr, "Invalid value for %s: %s\n", option, value);
  exit(2);
}

void
usage(char *program, int exitcode) {
  fprintf(stderr, "%s v0.6.1\n", program);
  fprintf(stderr, "usage: %s [options]\n", program);

  fprintf(stderr,
      R"SOMERANDOMTEXT(Options
    -d display
    Which display should be managed
    -r radius
    The blur radius for shadows. (default 12)
    -o opacity
    The translucency for shadows. (default .75)
    -l left-offset
    The left offset for shadows. (default -15)
    -t top-offset
    The top offset for shadows. (default -15)
    -I fade-in-step
    Opacity change between steps while fading in. (default 0.028)
    -O fade-out-step
    Opacity change between steps while fading out. (default 0.03)
    -D fade-delta-time
    The time between steps in a fade in milliseconds. (default 10)
    -m opacity
    The opacity for menu, dropdown-menu, popup-menu, and combo windows. (default 1.0)
    -c
    Enable client-side shadows on windows.
    -C
    Avoid drawing shadows on dock/panel windows.
    -f
    Fade windows in/out when opening/closing.
    -F
    Fade windows during opacity changes.
    -i opacity
    Opacity of inactive windows. (0.0 - 1.0)
    -e opacity
    Opacity of window titlebars and borders. (0.0 - 1.0)
    -S
    Enable synchronous operation (for debugging).
    -h, --help
    Show this help text.
    --shadow-red value
    Red color value of shadow (0.0 - 1.0, defaults to 0).
    --shadow-green value
    Green color value of shadow (0.0 - 1.0, defaults to 0).
    --shadow-blue value
    Blue color value of shadow (0.0 - 1.0, defaults to 0).
    --refresh-rate N
    Override display refresh rate in Hz (0 = autodetect via XRandR, default).)SOMERANDOMTEXT"
  );
  fprintf(stderr, "\n");

  exit(exitcode);
}

static Bool
register_cm (Display *dpy)
{
  Window w;
  Atom a;
  static char net_wm_cm[] = "_NET_WM_CM_Sxx";

  snprintf (net_wm_cm, sizeof (net_wm_cm), "_NET_WM_CM_S%d", g_screen);
  a = XInternAtom (dpy, net_wm_cm, False);
  w = XGetSelectionOwner (dpy, a);
  if (w != None) {
    XTextProperty tp;
    char **strs;
    int count;
    Atom winNameAtom = XInternAtom (dpy, "_NET_WM_NAME", False);

    if (!XGetTextProperty (dpy, w, &tp, winNameAtom) &&
        !XGetTextProperty (dpy, w, &tp, XA_WM_NAME))
    {
      fprintf (stderr,
         "Another composite manager is already running (0x%lx)\n",
         (unsigned long) w);
      return False;
    }
    if (XmbTextPropertyToTextList (dpy, &tp, &strs, &count) == Success)
    {
      fprintf (stderr,
         "Another composite manager is already running (%s)\n", strs[0]);
      XFreeStringList (strs);
    }
    XFree (tp.value);
    return False;
  }

  w = XCreateSimpleWindow (dpy, RootWindow (dpy, g_screen), 0, 0, 1, 1, 0, None,
          None);

  Xutf8SetWMProperties (dpy, w, "fastcompmgr", "fastcompmgr", NULL, 0, NULL, NULL,
      NULL);

  XSetSelectionOwner (dpy, a, w, 0);
  return True;
}

static void run_configures(Display *dpy){
  win *w;
  for (w = list; w; w = w->next) {
    if (w->need_configure && !w->destroyed){
      do_configure_win(dpy, w);
    }
  }
}

static void
do_paint(Display *dpy){
   if (!all_damage_is_dirty) return;
   paint_all(dpy, all_damage);
   XSync(dpy, False);
   g_last_paint_ms = get_time_in_milliseconds();
   all_damage_is_dirty = False;
   clip_changed = False;
}

static void
check_paint(Display *dpy){
  if (fade_timeout() == 0) run_fades(dpy);

  if (g_configure_needed || all_damage_is_dirty) {
    int64_t now = get_time_in_milliseconds();
    if (now - g_last_paint_ms >= g_paint_interval_ms) {
      if (g_configure_needed) {
        g_configure_needed = False;
        run_configures(dpy);
      }
      do_paint(dpy);
    }
  }
}

static int
paint_timeout(void) {
  int timeout = fade_timeout();

  if (g_configure_needed || all_damage_is_dirty) {
    int64_t remaining = g_paint_interval_ms
      - (get_time_in_milliseconds() - g_last_paint_ms);
    int paint_wait = remaining <= 0 ? 0
      : remaining > INT_MAX ? INT_MAX : (int)remaining;
    if (timeout < 0 || paint_wait < timeout) timeout = paint_wait;
  }
  return timeout;
}


int
main(int argc, char **argv) {
  const static struct option longopt[] = {
    { "shadow-red", required_argument, NULL, 0 },
    { "shadow-green", required_argument, NULL, 0 },
    { "shadow-blue", required_argument, NULL, 0 },
    { "help", no_argument, NULL, 0 },
    { "refresh-rate", required_argument, NULL, 0 },
    { 0, 0, 0, 0 },
  };

  XEvent ev;
  Window root_return, parent_return;
  Window *children;
  unsigned int nchildren;
  int i;
  XRectangle *expose_rects = 0;
  int size_expose = 0;
  int n_expose = 0;
  struct pollfd ufd;
  int p;
  int composite_major, composite_minor;
  double shadow_red = 0.0;
  double shadow_green = 0.0;
  double shadow_blue = 0.0;
  int g_override_refresh_rate = 0;
  char *display = 0;
  int o;
  int longopt_idx;
  Bool no_dock_shadow = False;
  if(!event_init()){
    exit(1);
  }

  for (i = 0; i < NUM_WINTYPES; ++i) {
    win_type_fade[i] = False;
    win_type_shadow[i] = False;
    win_type_opacity[i] = 1.0;
  }

  while ((o = getopt_long(argc, argv, "D:I:O:d:r:o:m:l:t:i:e:schnfFCaS",
                          longopt, &longopt_idx)) != -1) {
    switch (o) {
       // Long options
      case 0:
        switch (longopt_idx) {
          case 0: if (!parse_double_option(optarg, 0, 1, &shadow_red)) invalid_option("--shadow-red", optarg); break;
          case 1: if (!parse_double_option(optarg, 0, 1, &shadow_green)) invalid_option("--shadow-green", optarg); break;
          case 2: if (!parse_double_option(optarg, 0, 1, &shadow_blue)) invalid_option("--shadow-blue", optarg); break;
          case 3: usage(argv[0], 0); break;
          case 4: {
            if (!parse_int_option(optarg, 0, 1000, &g_override_refresh_rate))
              invalid_option("--refresh-rate", optarg);
            break;
          }
          default:
            fprintf(stderr, "Bug, unhandeled longopt_idx %d\n", longopt_idx);
            exit(2);
        }
        break;
      // Short options
      case 'd':
        display = optarg;
        break;
      case 'D':
        if (!parse_int_option(optarg, 1, 10000, &fade_delta)) invalid_option("-D", optarg);
        break;
      case 'I':
        if (!parse_double_option(optarg, 0.0001, 1.0, &fade_in_step)) invalid_option("-I", optarg);
        break;
      case 'O':
        if (!parse_double_option(optarg, 0.0001, 1.0, &fade_out_step)) invalid_option("-O", optarg);
        break;
      case 'c':
        for (i = 1; i < NUM_WINTYPES; ++i) {
          win_type_shadow[i] = True;
        }
        win_type_shadow[WINTYPE_DESKTOP] = False;
        // ponytail: menus/popups/tooltips are override_redirect and often
        // ARGB/CSD, so we only know their (oversized) window rect, not the
        // visible shape. A rect shadow then renders as a transparent box
        // around the popup. Skip shadows for these transient types (same as
        // the typical picom shadow-exclude). Upgrade path: honor
        // _NET_WM_OPAQUE_REGION / XShape to draw shape-correct shadows.
        win_type_shadow[WINTYPE_MENU] = False;
        win_type_shadow[WINTYPE_DROPDOWN_MENU] = False;
        win_type_shadow[WINTYPE_POPUP_MENU] = False;
        win_type_shadow[WINTYPE_TOOLTIP] = False;
        win_type_shadow[WINTYPE_COMBO] = False;
        break;
      case 'h': usage(argv[0], 0); break;
      case 'C':
        no_dock_shadow = True;
        break;
      case 'm':
        if (!parse_double_option(optarg, 0, 1, &win_type_opacity[WINTYPE_MENU])) invalid_option("-m", optarg);
        win_type_opacity[WINTYPE_DROPDOWN_MENU] = win_type_opacity[WINTYPE_MENU];
        win_type_opacity[WINTYPE_POPUP_MENU] = win_type_opacity[WINTYPE_MENU];
        win_type_opacity[WINTYPE_COMBO] = win_type_opacity[WINTYPE_MENU];
        break;
      case 'f':
        for (i = 1; i < NUM_WINTYPES; ++i) {
          win_type_fade[i] = True;
        }
        break;
      case 'F':
        fade_trans = True;
        break;
      case 'S':
        synchronize = True;
        break;
      case 'r':
        if (!parse_int_option(optarg, 1, 256, &shadow_radius)) invalid_option("-r", optarg);
        break;
      case 'o':
        if (!parse_double_option(optarg, 0, 1, &shadow_opacity)) invalid_option("-o", optarg);
        break;
      case 'l':
        if (!parse_int_option(optarg, -65535, 65535, &shadow_offset_x)) invalid_option("-l", optarg);
        break;
      case 't':
        if (!parse_int_option(optarg, -65535, 65535, &shadow_offset_y)) invalid_option("-t", optarg);
        break;
      case 'i':
        if (!parse_double_option(optarg, 0, 1, &inactive_opacity)) invalid_option("-i", optarg);
        break;
      case 'e':
        if (!parse_double_option(optarg, 0, 1, &frame_opacity)) invalid_option("-e", optarg);
        break;
      case 'n':
      case 'a':
      case 's':
        fprintf(stderr, "Warning: "
          "-n, -a, and -s have been removed.\n");
        break;
      default:
        usage(argv[0], 1);
        break;
    }
  }

  if (optind != argc) {
    fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
    usage(argv[0], 1);
  }

  if (no_dock_shadow) {
    win_type_shadow[WINTYPE_DOCK] = False;
  }

  dpy = XOpenDisplay(display);
  if (!dpy) {
    fprintf(stderr, "Can't open display\n");
    exit(1);
  }
  g_dpy = dpy;

  XSetErrorHandler(error);
  if (synchronize) {
    XSynchronize(dpy, 1);
  }

  g_screen = DefaultScreen(dpy);
  root = RootWindow(dpy, g_screen);

  if (!XRenderQueryExtension(dpy, &render_event, &render_error)) {
    fprintf(stderr, "No render extension\n");
    exit(1);
  }

  if (!XQueryExtension(dpy, COMPOSITE_NAME, &composite_opcode,
                       &composite_event, &composite_error)) {
    fprintf(stderr, "No composite extension\n");
    exit(1);
  }

  XCompositeQueryVersion(dpy, &composite_major, &composite_minor);

#if HAS_NAME_WINDOW_PIXMAP
  if (composite_major > 0 || composite_minor >= 2) {
    has_name_pixmap = True;
  }
#endif

  if (!XDamageQueryExtension(dpy, &damage_event, &damage_error)) {
    fprintf(stderr, "No damage extension\n");
    exit(1);
  }

  if (!XFixesQueryExtension(dpy, &xfixes_event, &xfixes_error)) {
    fprintf(stderr, "No XFixes extension\n");
    exit(1);
  }

  if(! register_cm(dpy))
    exit(1);

  /* get atoms */
  atom_opacity = XInternAtom(dpy,
    "_NET_WM_WINDOW_OPACITY", False);
  atom_win_type = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE", False);
  atom_pixmap = XInternAtom(dpy,
    "PIXMAP", False);
  atom_wm_state = XInternAtom(dpy,
    "WM_STATE", False);
  atom_net_frame_extents = XInternAtom(dpy,
    "_NET_FRAME_EXTENTS", False);
  atom_gtk_frame_extents = XInternAtom(dpy,
    "_GTK_FRAME_EXTENTS", False);
  atom_net_wm_state = XInternAtom(dpy,
    "_NET_WM_STATE", False);
  atom_net_wm_state_hidden = XInternAtom (dpy,
    "_NET_WM_STATE_HIDDEN", False);
  atom_net_wm_state_focused = XInternAtom (dpy,
    "_NET_WM_STATE_FOCUSED", False);
  atom_net_active_window = XInternAtom (dpy,
    "_NET_ACTIVE_WINDOW", False);
  win_type[WINTYPE_DESKTOP] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DESKTOP", False);
  win_type[WINTYPE_DOCK] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DOCK", False);
  win_type[WINTYPE_TOOLBAR] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
  win_type[WINTYPE_MENU] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_MENU", False);
  win_type[WINTYPE_UTILITY] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_UTILITY", False);
  win_type[WINTYPE_SPLASH] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_SPLASH", False);
  win_type[WINTYPE_DIALOG] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DIALOG", False);
  win_type[WINTYPE_NORMAL] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_NORMAL", False);
  win_type[WINTYPE_DROPDOWN_MENU] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
  win_type[WINTYPE_POPUP_MENU] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
  win_type[WINTYPE_TOOLTIP] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
  win_type[WINTYPE_NOTIFY] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
  win_type[WINTYPE_COMBO] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_COMBO", False);
  win_type[WINTYPE_DND] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DND", False);

  gaussian_map = make_gaussian_map(dpy, shadow_radius);
  if (!gaussian_map) {
    fprintf(stderr, "Unable to allocate the shadow blur map.\n");
    exit(1);
  }
  presum_gaussian(gaussian_map);

  if(!root_init()){
    exit(1);
  }

  {
    // ponytail: XRRConfigCurrentRate is legacy/per-screen; one global cap. Per-output refresh only if mixed-rate multi-monitor matters.
    int randr_event_base, randr_error_base;
    XRRScreenConfiguration *cfg = XRRQueryExtension(dpy, &randr_event_base,
        &randr_error_base) ? XRRGetScreenInfo(dpy, root) : NULL;
    short rate = cfg ? XRRConfigCurrentRate(cfg) : 0;
    if (cfg) XRRFreeScreenConfigInfo(cfg);
    g_paint_interval_ms = (rate > 0) ? ((1000 + rate - 1) / rate) : 16;
    if (g_override_refresh_rate > 0)
      g_paint_interval_ms = (1000 + g_override_refresh_rate - 1) / g_override_refresh_rate;
    if (g_paint_interval_ms < 1) g_paint_interval_ms = 1;  // ponytail: floor; interval 0 would disable throttle + spin
  }

  black_picture = solid_picture(dpy, True, 1, 0, 0, 0);

  // Allow for user-defined shadow color:
  if (!shadow_red && !shadow_green && !shadow_blue)
    cshadow_picture = black_picture;
  else
    cshadow_picture = solid_picture(dpy, True, 1,
        shadow_red, shadow_green, shadow_blue);

  all_damage = XFixesCreateRegion(dpy, 0, 0);
  all_damage_is_dirty = False;
  g_xregion_tmp = XFixesCreateRegion(dpy, 0, 0);
  screen_damage = XFixesCreateRegion(dpy, 0, 0);

  clip_changed = True;
  XGrabServer(dpy);

  XCompositeRedirectSubwindows(
    dpy, root, CompositeRedirectManual);

  XSelectInput(dpy, root,
    SubstructureNotifyMask
    | ExposureMask
    | StructureNotifyMask
    | PropertyChangeMask);

  XQueryTree(dpy, root, &root_return,
    &parent_return, &children, &nchildren);

  for (i = 0; i < nchildren; i++) {
    add_win(dpy, children[i], i ? children[i-1] : None);
  }

  XFree(children);

  XUngrabServer(dpy);

  ufd.fd = ConnectionNumber(dpy);
  ufd.events = POLLIN;

  {
    XRectangle root_rect = { .x=0, .y=0,
                             .width=root_width , .height=root_height };
    XFixesSetRegion(dpy, g_xregion_tmp, &root_rect, 1);
    paint_all(dpy, g_xregion_tmp);
    XSync(dpy, False);
    g_last_paint_ms = get_time_in_milliseconds();
  }

  for (;;) {
    /*    dump_wins(); */
    do {
      if (!QLength(dpy)) {
        XFlush(dpy);
        int poll_result = poll(&ufd, 1, paint_timeout());
        if (unlikely(poll_result == 0)) {
          check_paint(dpy);
          break;
        }
        if (unlikely(poll_result < 0 && errno == EINTR)) continue;
      }

      XNextEvent(dpy, &ev);

      if (likely((ev.type & 0x7f) != KeymapNotify)) {
        discard_ignore(dpy, ev.xany.serial);
      }

#if DEBUG_EVENTS
      fprintf(stderr, "event %s serial 0x%08x window 0x%08lx\n",
          ev_name(&ev), ev_serial(&ev), ev_window(&ev));
#endif

      switch (ev.type) {
        case FocusIn: {
          if (!inactive_opacity) break;

          // stop focusing windows the cursor is over.
          // with this, windows dont focus right after being
          // deiconified, this needs to be fixed by blocking
          // the right kind of FocusOut event
          if (ev.xfocus.detail == NotifyPointer) break;

          win *fw = find_win(ev.xfocus.window);
          if (IS_NORMAL_WIN(fw) && ! fw->userdefined_opacity) {
            set_opacity(dpy, fw, OPAQUE);
          }
          break;
        }
        case FocusOut: {
          if (!inactive_opacity) break;

          // this fixes deiconify refocus
          // need != notifygrab here otherwise windows wont
          // lower opacity when grabbed for dragging
          if (ev.xfocus.mode != NotifyGrab
              && ev.xfocus.detail == NotifyVirtual) break;

          win *fw = find_win(ev.xfocus.window);
          if (IS_NORMAL_WIN(fw) && ! fw->userdefined_opacity) {
            set_opacity(dpy, fw, INACTIVE_OPACITY);
          }
          break;
        }
        case CreateNotify:
          //if (ev.xcreatewindow.override_redirect) break;
          add_win(dpy, ev.xcreatewindow.window, 0);
          break;
        case ConfigureNotify:
          handle_ConfigureNotify(dpy, &ev.xconfigure);
          break;
        case DestroyNotify:
          destroy_win(dpy, ev.xdestroywindow.window, True);
          break;
        case MapNotify:
          map_win(dpy, ev.xmap.window, ev.xmap.serial, True);
          break;
        case UnmapNotify:
          unmap_win(dpy, ev.xunmap.window, True);
          break;
        case ReparentNotify:
          // Reparent for instance occurs, when the window manager restarts. In the
          // process, events we registered for previously are lost. Events for toplevel
          // windows, as well as PropertyChange events for non-toplevel clients are
          // re-registered in add_win. However, *afterwards* the client may be reparented
          // *again*, from root to its corresponding toplevel window. Thus, we have to
          // register client events again!
          // Note that currently we do NOT check for "stale" window states. Possibly,
          // a hidden window where the client is reparented may remain hidden.
          // I did not see this in pracice though, since we take the _NET_WM_STATE_HIDDEN
          // from the client window and the client *should* be tied to its toplevel window.
          if (ev.xreparent.parent == root) {
            add_win(dpy, ev.xreparent.window, 0);
          } else {
            add_damage_if_hidden_changed(ev.xreparent.window, true);
            // FIXME: we only manage toplevel windows, so does this EVER fire?
            destroy_win(dpy, ev.xreparent.window, True);
          }
          break;
        case CirculateNotify:
          circulate_win(dpy, &ev.xcirculate);
          break;
        case Expose:
          if (ev.xexpose.window == root) {
            int more = ev.xexpose.count + 1;
            if (n_expose == size_expose) {
              if (expose_rects) {
                expose_rects = realloc(expose_rects,
                  (size_expose + more) * sizeof(XRectangle));
                size_expose += more;
              } else {
                expose_rects = malloc(more * sizeof(XRectangle));
                size_expose = more;
              }
            }
            expose_rects[n_expose].x = ev.xexpose.x;
            expose_rects[n_expose].y = ev.xexpose.y;
            expose_rects[n_expose].width = ev.xexpose.width;
            expose_rects[n_expose].height = ev.xexpose.height;
            n_expose++;
            if (ev.xexpose.count == 0) {
              expose_root(dpy, root, expose_rects, n_expose);
              n_expose = 0;
            }
          }
          break;
        case PropertyNotify:
          if (ev.xproperty.window == root) {
            for (p = 0; root_background_props[p]; p++) {
              if (ev.xproperty.atom ==
                  XInternAtom(dpy, root_background_props[p], False)) {
                if (root_tile) {
                  XClearArea(dpy, root, 0, 0, 0, 0, True);
                  XRenderFreePicture(dpy, root_tile);
                  root_tile = None;
                  break;
                }
              }
            }
          }
          // if (ev.xproperty.atom == atom_net_active_window) {
          //   fprintf(stderr, "active win changed\n");
          // }

          /* check if Trans property was changed */
          if (ev.xproperty.atom == atom_opacity) {
            /* reset mode and redraw window */
            win *w = find_win_any_parent(ev.xproperty.window);
            if (w) {
              uint opacity = win_suggest_opacity(w, &w->userdefined_opacity);
              set_opacity(dpy, w, opacity);
            }
          } else if (ev.xproperty.atom == atom_net_wm_state) {
            add_damage_if_hidden_changed(ev.xproperty.window, false);
          } else if (ev.xproperty.atom == atom_net_frame_extents
                     || ev.xproperty.atom == atom_gtk_frame_extents) {
            win *w = find_win_any_parent(ev.xproperty.window);
            if (w) {
              get_frame_extents(w, &w->left_width, &w->right_width,
                &w->top_width, &w->bottom_width);
              if (w->border_size) {
                XFixesDestroyRegion(dpy, w->border_size);
                w->border_size = None;
              }
              invalidate_shadow(dpy, w);
            }
          } else if (ev.xproperty.atom == atom_win_type) {
            win *w = find_win_any_parent(ev.xproperty.window);
            if (w) {
              w->window_type = determine_wintype(dpy, w->id, w->id);
              invalidate_shadow(dpy, w);
            }
          }
          break;
        case SelectionClear:
          fprintf(stderr, "Another composite manager started and took the _NET_WM_CM_Sn "
	          "selection. Bye.\n");
          exit(0);
          break;
        default:
          if (likely(ev.type == damage_event + XDamageNotify)) {
            damage_win(dpy, (XDamageNotifyEvent *)&ev);
          }
          break;
      }
    } while (QLength(dpy));

    check_paint(dpy);
  }
}
