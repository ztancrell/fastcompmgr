
#include <stdio.h>
#include <stdbool.h>

#include "cm-root.h"
#include "cm-global.h"

Window root;
Picture root_picture;
Picture root_buffer;
int root_width;
int root_height;

const char *root_background_props[] = {
  "_XROOTPMAP_ID",
  "_XSETROOT_ID",
  0
};


static inline unsigned int
_get_valid_pixmap_depth(Pixmap pxmap) {
  if (!pxmap) return 0;

  Window rroot = None;
  int rx = 0, ry = 0;
  unsigned rwid = 0, rhei = 0, rborder = 0, rdepth = 0;
  // In some window managers without managed desktops or also in some versions of
  // xfce (4.18), the found pixmap is invalid having a size of zero.
  bool is_valid =  XGetGeometry(g_dpy, pxmap, &rroot, &rx, &ry,
        &rwid, &rhei, &rborder, &rdepth) && rwid && rhei;
  if(is_valid){
    return rdepth;
  }
  return 0;
}


// XRenderFind(Standard)Format() is a roundtrip, so cache the results
static XRenderPictFormat* renderformats[33] = {NULL};

static Picture _create_background_pict(Pixmap pix, unsigned int depth)
{
  XRenderPictureAttributes pa;
  XRenderPictFormat *format = NULL;

  if (depth < sizeof(renderformats) / sizeof(renderformats[0])) {
    format = renderformats[depth];
  }
  if (format == NULL) {
    switch(depth){
      case 0:
          break;
      case 1:
          format = XRenderFindStandardFormat(g_dpy, PictStandardA1);
          break;
      case 8:
          format = XRenderFindStandardFormat(g_dpy, PictStandardA8);
          break;
      case 24:
          format = XRenderFindStandardFormat(g_dpy, PictStandardRGB24);
          break;
      case 32:
          format = XRenderFindStandardFormat(g_dpy, PictStandardARGB32);
          break;
      default: {
          fprintf(stderr, "Unhandled root background depth %u - please report!\n", depth);
          break;
      }
    }
    if (format != NULL && depth < sizeof(renderformats) / sizeof(renderformats[0])) {
      renderformats[depth] = format;
    }
  }
  if (format == NULL) {
    // Stay safe, and do not cache the fallback render format without further research.
    format = XRenderFindVisualFormat(g_dpy, DefaultVisual(g_dpy, g_screen));
  }
  if (format == NULL) {
    fprintf(stderr, "Unable to find an X Render format for the root background.\n");
    return None;
  }

  pa.repeat = True;
  return XRenderCreatePicture(g_dpy, pix, format, CPRepeat, &pa);
}

bool root_init(){
  XRenderPictureAttributes pa;
  root_width = DisplayWidth(g_dpy, g_screen);
  root_height = DisplayHeight(g_dpy, g_screen);

  pa.subwindow_mode = IncludeInferiors;
  root_picture = XRenderCreatePicture(g_dpy, root,
    XRenderFindVisualFormat(g_dpy, DefaultVisual(g_dpy, g_screen)),
    CPSubwindowMode, &pa);
  return true;
}

/// Create the root background picture. First check, if the root window already
/// has a valid corresponding pixmap. If so, do not overwrite it, such that e.g.
/// openbox's root background image is preserved. Create the picture using the
/// same depth, otherwise we're flooded with errors like
/// "error 143 (BadPicture) request 139 minor 8 serial 78698". If no valid
/// background pixmap is found, we create one ourselves using DefaultVisual()
/// and set a fixed solid background color.
Picture root_create_tile() {
  Picture picture = None;
  Atom actual_type;
  Pixmap pixmap = None;
  int actual_format;
  unsigned long nitems;
  unsigned long bytes_after;
  unsigned char *prop = NULL;
  unsigned pict_depth = 0;
  bool owns_pixmap = false;
  int p;
  int res;
  const char* valid_pix_str;

  for (p=0; root_background_props[p]; p++) {
    Pixmap candidate = None;

    prop = NULL;
    res = XGetWindowProperty(g_dpy, root,
          XInternAtom(g_dpy, root_background_props[p], False),
          0, 1, False, atom_pixmap, &actual_type,
          &actual_format, &nitems, &bytes_after, &prop);
    if (res == Success && prop != NULL
          && actual_type == atom_pixmap && actual_format == 32
          && nitems == 1 && bytes_after == 0) {
      candidate = (Pixmap)((unsigned long *)prop)[0];
    }
    if (prop != NULL) {
      XFree(prop);
    }

    pict_depth = _get_valid_pixmap_depth(candidate);
    if(pict_depth){
      pixmap = candidate;
      break;
    }
  }

  if(pixmap == None){
    valid_pix_str = "invalid";
    pixmap = XCreatePixmap(g_dpy, root, 1, 1, DefaultDepth(g_dpy, g_screen));
    owns_pixmap = true;
  } else {
    valid_pix_str = "valid";
  }
  fprintf(stderr, "info: root background pixmap is %s.\n", valid_pix_str);
  picture = _create_background_pict(pixmap, pict_depth);

  if (owns_pixmap && picture != None) {
    XRenderColor  c;
    c.red = c.green = c.blue = 0x8080;
    c.alpha = 0xffff;
    XRenderFillRectangle(
      g_dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);
  }
  if (owns_pixmap) {
    XFreePixmap(g_dpy, pixmap);
  }
  return picture;
}
