/*
 * Helper functions for the back-end driver for the Nuklear GUI. Currently, GDI+
 * (for Windows) and GLFW with OpenGL (for Linux) are supported.
 *
 * Copyright 2019-2022 CompuPhase
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#if defined _WIN32
# define WIN32_LEAN_AND_MEAN
# define WINVER       0x0500 /* to enable RegisterDeviceNotification() */
# define _WIN32_WINNT 0x0501 /* for DEVICE_NOTIFY_ALL_INTERFACE_CLASSES */
# include <windows.h>
# include <dbt.h>
#elif defined __linux__
# include <unistd.h>
# include <libusb-1.0/libusb.h>
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "guidriver.h"
#include "nuklear_mousepointer.h"

#if defined _WIN32
# include "nuklear_gdip.h"
#elif defined __linux__ || defined __unix__
# include "findfont.h"
# include "lodepng.h"
# include "nuklear_glfw_gl2.h"
#endif

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if defined _WIN32

static int fontType = 0;
static GdipFont *fontStd = NULL;
static GdipFont *fontMono = NULL;
static GdipFont *fontHeading1 = NULL;
static GdipFont *fontHeading2 = NULL;
static GdipFont *fontSmall = NULL;
static HWND hwndApp = NULL;
static volatile int UsbEvent = 0;
static unsigned short UsbVid = 0, UsbPid = 0;

static LRESULT CALLBACK WindowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  switch (msg) {
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  case WM_DEVICECHANGE:
    if (wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE) {
      DEV_BROADCAST_DEVICEINTERFACE *hdr = (DEV_BROADCAST_DEVICEINTERFACE*)lparam;
      if (hdr != NULL && hdr->dbcc_size >= sizeof(DEV_BROADCAST_DEVICEINTERFACE)) {
        char name[256];
        int i;
        for (i = 0; i < sizeof(name) - 1 && ((short*)(hdr->dbcc_name))[i] != 0; i++)
          name[i] = ((short*)(hdr->dbcc_name))[i];
        name[i] = '\0';
        const char *vid_p=strstr(name,"VID_");
        const char *pid_p = strstr(name, "PID_");
        if (vid_p != NULL && pid_p != NULL) {
          int vid = (int)strtol(vid_p + 4, NULL, 16);
          int pid = (int)strtol(pid_p + 4, NULL, 16);
          if (vid == UsbVid && pid == UsbPid)
            UsbEvent = (wparam == DBT_DEVICEARRIVAL) ? DEVICE_INSERT : DEVICE_REMOVE;
        }
      }
    }
    break;
  }
  if (nk_gdip_handle_event(wnd, msg, wparam, lparam))
    return 0;
  return DefWindowProcW(wnd, msg, wparam, lparam);
}

/** guidriver_init() creates the application window.
 *
 *  \param width      The width of the client area.
 *  \param height     The height of the client area.
 *  \param flags      A combination of options.
 *  \param fontstd    The name of the preferred standard font (may be NULL to
 *                    use the default).
 *  \param fontmono   The name of the preferred monospaced font (may be NULL to
 *                    use the default).
 *  \param fontsize   The size of the main font, in pixels (may be a partial
 *                    number).
 *
 *  \note In Microsoft Windows, the application icon (in the frame window) is
 *        set to the icon with the name "appicon" the application's resources.
 *        In Linux, the icon must be a PNG image that is converted to a C array
 *        with 'xd' or 'xxd'; the array must be called appicon_data, and the
 *        size variable must be appicon_datasize.
 */
struct nk_context* guidriver_init(const char *caption, int width, int height, int flags,
                                  const char *fontsystem, const char *fontmono, float fontsize)
{
  struct nk_context *ctx;
  WNDCLASSW wc;
  DWORD style, exstyle;
  RECT rect;
  RECT rcDesktop;
  wchar_t wcapt[128];
  int i, j, x, y, len;

  SetRect(&rect, 0, 0, width, height);
  if (flags & GUIDRV_RESIZEABLE) {
    style = WS_OVERLAPPEDWINDOW | WS_SIZEBOX;
    exstyle = 0;
  } else {
    style = WS_POPUPWINDOW | WS_CAPTION;
    exstyle = WS_EX_APPWINDOW;
  }
  memset(&wc, 0, sizeof(wc));
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandleW(0);
  wc.hIcon = LoadIcon(wc.hInstance, "appicon");;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = GetStockObject(DKGRAY_BRUSH);
  wc.lpszClassName = L"NuklearWindowClass";
  RegisterClassW(&wc);

  /* convert string to Unicode */
  len = strlen(caption);
  for (i = j = 0; i < len; ) {
    char leader = caption[i];
    int tch;
    if ((leader & 0x80) == 0) {
      tch = caption[i];
      i += 1;
    } else if ((leader & 0xE0) == 0xC0) {
      tch = (caption[i] & 0x1F) << 6;
      tch |= (caption[i+1] & 0x3F);
      i += 2;
    } else if ((leader & 0xF0) == 0xE0) {
      tch = (caption[i] & 0xF) << 12;
      tch |= (caption[i+1] & 0x3F) << 6;
      tch |= (caption[i+2] & 0x3F);
      i += 3;
    } else if ((leader & 0xF8) == 0xF0) {
      tch = (caption[i] & 0x7) << 18;
      tch |= (caption[i+1] & 0x3F) << 12;
      tch |= (caption[i+2] & 0x3F) << 6;
      tch |= (caption[i+3] & 0x3F);
      i += 4;
    } else if ((leader & 0xFC) == 0xF8) {
      tch = (caption[i] & 0x3) << 24;
      tch |= (caption[i] & 0x3F) << 18;
      tch |= (caption[i] & 0x3F) << 12;
      tch |= (caption[i] & 0x3F) << 6;
      tch |= (caption[i] & 0x3F);
      i += 5;
    } else if ((leader & 0xFE) == 0xFC) {
      tch = (caption[i] & 0x1) << 30;
      tch |= (caption[i] & 0x3F) << 24;
      tch |= (caption[i] & 0x3F) << 18;
      tch |= (caption[i] & 0x3F) << 12;
      tch |= (caption[i] & 0x3F) << 6;
      tch |= (caption[i] & 0x3F);
      i += 6;
    } else {
      assert(0);
      tch = 0;  /* to avoid a compiler warning about a potentionally uninitialized variable */
    }
    wcapt[j++] = (wchar_t)tch;
  }
  wcapt[j] = 0;

  /* get size of primary monitor, to center the utility on in */
  GetWindowRect(GetDesktopWindow(), &rcDesktop);
  AdjustWindowRectEx(&rect, style, FALSE, exstyle);
  if (flags & GUIDRV_CENTER) {
    x = (rcDesktop.right - rect.right) / 2;
    y = (rcDesktop.bottom - rect.bottom) / 2;
  } else {
    x = y = CW_USEDEFAULT;
  }

  hwndApp = CreateWindowExW(exstyle, wc.lpszClassName, wcapt,
                            style | WS_MINIMIZEBOX | WS_VISIBLE,
                            x, y, rect.right - rect.left, rect.bottom - rect.top,
                            NULL, NULL, wc.hInstance, NULL);
  if (hwndApp == NULL)
    return NULL;

  if (flags & GUIDRV_TIMER)
    SetTimer(hwndApp, 1, 100, NULL);

  ctx = nk_gdip_init(hwndApp, width, height);

  fontStd = fontHeading1 = fontHeading2 = fontSmall = NULL;
  if (fontsystem != NULL && strlen(fontsystem) > 0)
    fontStd = nk_gdipfont_create(fontsystem, fontsize, NK_FONTREGULAR);
  if (fontStd == NULL) {
    fontsystem = "Segoe UI";
    fontStd = nk_gdipfont_create(fontsystem, fontsize, NK_FONTREGULAR);
  }
  if (fontStd == NULL) {
    fontsystem = "Tahoma";
    fontStd = nk_gdipfont_create(fontsystem, fontsize, NK_FONTREGULAR);
  }
  if (fontStd == NULL) {
    fontsystem = "Microsoft Sans Serif";
    fontStd = nk_gdipfont_create(fontsystem, fontsize, NK_FONTREGULAR);
  }
  if (fontStd != NULL) {
    fontHeading1 = nk_gdipfont_create(fontsystem, 1.4*fontsize, NK_FONTBOLD);
    fontHeading2 = nk_gdipfont_create(fontsystem, 1.2*fontsize, NK_FONTBOLDITALIC);
    fontSmall = nk_gdipfont_create(fontsystem, 0.75*fontsize, NK_FONTREGULAR);
  }

  fontMono = NULL;
  if (fontmono != NULL && strlen(fontmono) > 0)
    fontMono = nk_gdipfont_create(fontmono, fontsize, NK_FONTREGULAR);
  if (fontMono == NULL)
    fontMono = nk_gdipfont_create("Hack", fontsize, NK_FONTREGULAR);
  if (fontMono == NULL)
    fontMono = nk_gdipfont_create("DejaVu Sans Mono", fontsize, NK_FONTREGULAR);
  if (fontMono == NULL)
    fontMono = nk_gdipfont_create("Consolas", fontsize, NK_FONTREGULAR);
  if (fontMono == NULL)
    fontMono = nk_gdipfont_create("Courier New", fontsize, NK_FONTREGULAR);

  assert(fontStd != NULL);
  nk_gdipfont_set_voffset(fontStd, (-fontsize*0.2-0.5));
  nk_gdip_set_font(fontStd);

  pointer_init((void*)hwndApp);

  return ctx;
}

void guidriver_close(void)
{
  pointer_cleanup();
  if (fontStd != NULL)
    nk_gdipfont_del(fontStd);
  if (fontMono != NULL)
    nk_gdipfont_del(fontMono);
  if (fontHeading1 != NULL)
    nk_gdipfont_del(fontHeading1);
  if (fontHeading2 != NULL)
    nk_gdipfont_del(fontHeading2);
  if (fontSmall != NULL)
    nk_gdipfont_del(fontSmall);
  nk_gdip_shutdown();
  // UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

/** guidriver_setfont() switches font between standard (proportional) and
 *  monospaced.
 *  \param ctx    The Nuklear context.
 *  \param type   Either FONT_STD, FONT_MONO or FONT_HEADINGx.
 *  \return The previous type.
 */
int guidriver_setfont(struct nk_context *ctx, int type)
{
  int prev = fontType;
  (void)ctx;
  switch (type) {
  case FONT_STD:
    if (fontStd != NULL) {
      nk_gdipfont_set_voffset(fontStd, -3);
      nk_gdip_set_font(fontStd);
      fontType = type;
    }
    break;
  case FONT_MONO:
    if (fontMono != NULL) {
      nk_gdipfont_set_voffset(fontMono, 0);
      nk_gdip_set_font(fontMono);
      fontType = type;
    }
    break;
  case FONT_HEADING1:
    if (fontHeading1 != NULL) {
      nk_gdipfont_set_voffset(fontHeading1, 0);
      nk_gdip_set_font(fontHeading1);
      fontType = type;
    }
    break;
  case FONT_HEADING2:
    if (fontHeading2 != NULL) {
      nk_gdipfont_set_voffset(fontHeading2, 0);
      nk_gdip_set_font(fontHeading2);
      fontType = type;
    }
    break;
  case FONT_SMALL:
    if (fontSmall != NULL) {
      nk_gdipfont_set_voffset(fontSmall, -2);
      nk_gdip_set_font(fontSmall);
      fontType = type;
    }
    break;
  }
  return prev;
}

/** guidriver_appsize() returns the size of the client area of the
 *  application window. A program can use this to resize Nuklear windows to
 *  fit into the application window.
 */
bool guidriver_appsize(int *width, int *height)
{
  if (IsWindow(hwndApp)) {
    RECT rc;
    GetClientRect(hwndApp, &rc);
    assert(width != NULL && height != NULL);
    *width = rc.right - rc.left;
    *height = rc.bottom - rc. top;
    return true;
  }
  return false;
}

void guidriver_render(struct nk_color clear)
{
  nk_gdip_render(NK_ANTI_ALIASING_ON, clear);
}

bool guidriver_poll(bool waitidle)
{
  MSG msg;

  if (waitidle) {
    /* wait for an event, to avoid taking CPU load without anything to do */
    if (GetMessageW(&msg, NULL, 0, 0) <= 0)
      return false;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  /* so there was at least one event, handle all outstanding events */
  while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT)
      return false;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return true;
}

int guidriver_monitor_usb(unsigned short vid, unsigned short pid)
{
  if (UsbVid != vid || UsbPid != pid) {
    /* one-time initialization: register this window for insertion/removal messages */
    DEV_BROADCAST_DEVICEINTERFACE filter;
    memset(&filter, 0, sizeof filter);
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    RegisterDeviceNotification(hwndApp, &filter, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
    UsbVid = vid;
    UsbPid = pid;
    UsbEvent = 0;
  }

  /* return & clear the flag */
  int ret = UsbEvent;
  UsbEvent = 0;
  return ret;
}

void *guidriver_apphandle(void)
{
  return &hwndApp;
}

struct nk_image guidriver_image_from_memory(const unsigned char *data, unsigned size)
{
  return nk_gdip_load_image_from_memory(data, size);
}

#elif defined __linux__

static GLFWwindow *winApp;
static int fontType = 0;
static struct nk_font *fontStd = NULL;
static struct nk_font *fontMono = NULL;
static struct nk_font *fontHeading1 = NULL;
static struct nk_font *fontHeading2 = NULL;
static struct nk_font *fontSmall = NULL;
static volatile int UsbEvent = 0;
static unsigned short UsbVid = 0, UsbPid = 0;

static void error_callback(int e, const char *d)
{
  fprintf(stderr, "Error %d: %s\n", e, d);
}

struct nk_context* guidriver_init(const char *caption, int width, int height, int flags,
                                  const char *fontsystem, const char *fontmono, float fontsize)
{
  extern const unsigned char appicon_data[];
  extern const unsigned int appicon_datasize;
  struct nk_context *ctx;
  struct nk_font_config fontconfig;
  char path[256];
  const char *fontname;
  GLFWimage icons[1];

  /* GLFW */
  glfwSetErrorCallback(error_callback);
  if (!glfwInit()) {
    // fprintf(stderr, "[GFLW] failed to init!\n");
    return NULL;
  }
  glfwWindowHint(GLFW_RESIZABLE, (flags & GUIDRV_RESIZEABLE) != 0);
  winApp = glfwCreateWindow(width, height, caption, NULL, NULL);
  glfwMakeContextCurrent(winApp);

  /* add window icon */
# if GLFW_VERSION_MAJOR >= 3 && GLFW_VERSION_MINOR >= 2
    memset(icons, 0, sizeof icons);
    unsigned error = lodepng_decode32(&icons[0].pixels, (unsigned*)&icons[0].width, (unsigned*)&icons[0].height, appicon_data, appicon_datasize);
    if (!error)
      glfwSetWindowIcon(winApp, 1, icons);
    free(icons[0].pixels);
# endif

  ctx = nk_glfw3_init(winApp, NK_GLFW3_INSTALL_CALLBACKS);
  fontconfig = nk_font_config(fontsize);
  fontconfig.pixel_snap = 1;    /* align characters to pixel boundary, to increase sharpness */
  fontconfig.oversample_h = 1;  /* disable horizontal oversampling, as recommended for pixel_snap */

  fontname = NULL;
  if (fontsystem != NULL && strlen(fontsystem) > 0 && font_locate(path, sizeof path, fontsystem, ""))
    fontname = fontsystem;
  else if (font_locate(path, sizeof path, "DejaVu Sans", ""))
    fontname = "DejaVu Sans";
  else if (font_locate(path, sizeof path, "Ubuntu", ""))
    fontname = "Ubuntu";
  else if (font_locate(path, sizeof path, "FreeSans", ""))
    fontname = "FreeSans";
  else if (font_locate(path, sizeof path, "Liberation Sans", ""))
    fontname = "Liberation Sans";

  if (fontname != NULL) {
    struct nk_font_atlas *atlas;

    font_locate(path, sizeof path, fontname, "");
    nk_glfw3_font_stash_begin(&atlas);
    fontStd = nk_font_atlas_add_from_file(atlas, path, fontsize, &fontconfig);
    nk_glfw3_font_stash_end();

    nk_glfw3_font_stash_begin(&atlas);
    fontSmall = nk_font_atlas_add_from_file(atlas, path, 0.75*fontsize, &fontconfig);
    nk_glfw3_font_stash_end();

    font_locate(path, sizeof path, fontname, "Bold");
    nk_glfw3_font_stash_begin(&atlas);
    fontHeading1 = nk_font_atlas_add_from_file(atlas, path, 1.4*fontsize, &fontconfig);
    nk_glfw3_font_stash_end();

    font_locate(path, sizeof path, fontname, "Bold Italic");
    nk_glfw3_font_stash_begin(&atlas);
    fontHeading2 = nk_font_atlas_add_from_file(atlas, path, 1.2*fontsize, &fontconfig);
    nk_glfw3_font_stash_end();

    if (fontStd != NULL)
      nk_style_set_font(ctx, &fontStd->handle);

    /* Load Cursor: if you uncomment cursor loading please hide the cursor */
    /*nk_style_load_all_cursors(ctx, atlas->cursors);*/
  }

  if ((fontmono != NULL && strlen(fontmono) > 0 && font_locate(path, sizeof path, fontmono, ""))
      || font_locate(path, sizeof path, "Hack", "")
      || font_locate(path, sizeof path, "Andale Mono", "")
      || font_locate(path, sizeof path, "FreeMono", "")
      || font_locate(path, sizeof path, "Liberation Mono", ""))
  {
    struct nk_font_atlas *atlas;
    nk_glfw3_font_stash_begin(&atlas);
    fontMono = nk_font_atlas_add_from_file(atlas, path, fontsize, &fontconfig);
    nk_glfw3_font_stash_end();
  }

  pointer_init(winApp);

  return ctx;
}

void guidriver_close(void)
{
  pointer_cleanup();
  nk_glfw3_shutdown();
  glfwTerminate();
  if (UsbVid != 0)
    libusb_exit(NULL);
}

/** guidriver_setfont() switches font between standard (proportional) and
 *  monospaced.
 *  \param type   Either FONT_STD, FONT_MONO or FONT_HEADINGx.
 *  \return The previous type.
 */
int guidriver_setfont(struct nk_context *ctx, int type)
{
  int prev = fontType;
  switch (type) {
  case FONT_STD:
    if (fontStd != NULL) {
      nk_style_set_font(ctx, &fontStd->handle);
      fontType = type;
    }
    break;
  case FONT_MONO:
    if (fontMono != NULL) {
      nk_style_set_font(ctx, &fontMono->handle);
      fontType = type;
    }
    break;
  case FONT_HEADING1:
    if (fontHeading1 != NULL) {
      nk_style_set_font(ctx, &fontHeading1->handle);
      fontType = type;
    }
    break;
  case FONT_HEADING2:
    if (fontHeading2 != NULL) {
      nk_style_set_font(ctx, &fontHeading2->handle);
      fontType = type;
    }
    break;
  case FONT_SMALL:
    if (fontSmall != NULL) {
      nk_style_set_font(ctx, &fontSmall->handle);
      fontType = type;
    }
    break;
  }
  return prev;
}

bool guidriver_appsize(int *width, int *height)
{
  glfwGetWindowSize(winApp, width, height);
  return true;
}

void guidriver_render(struct nk_color clear)
{
  int width = 0, height = 0;

  glfwGetWindowSize(winApp, &width, &height);
  glViewport(0, 0, width, height);
  glClear(GL_COLOR_BUFFER_BIT);
  glClearColor(clear.r/255.0, clear.g/255.0, clear.b/255.0, clear.a/255.0);
  /* IMPORTANT: `nk_glfw_render` modifies some global OpenGL state
   * with blending, scissor, face culling and depth test and defaults everything
   * back into a default state. Make sure to either save and restore or
   * reset your own state after drawing rendering the UI. */
  nk_glfw3_render(NK_ANTI_ALIASING_ON);
  glfwSwapBuffers(winApp);
}

bool guidriver_poll(bool waitidle)
{
  (void)waitidle;
  if (glfwWindowShouldClose(winApp))
    return false;
  glfwPollEvents();
  nk_glfw3_new_frame();
  return true;
}

static int hotplug_callback(libusb_context *ctx, libusb_device *device,
                            libusb_hotplug_event event, void *user_data)
{
  (void)ctx;
  (void)device;
  (void)user_data;
  UsbEvent = (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) ? DEVICE_INSERT : DEVICE_REMOVE;
  return 0;
}

int guidriver_monitor_usb(unsigned short vid, unsigned short pid)
{
  if (UsbVid != vid || UsbPid != pid) {
    /* one-time initialization: register this window for insertion/removal messages */
    libusb_init(NULL);
    if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
      libusb_hotplug_callback_handle callback_handle;
      libusb_hotplug_register_callback(NULL,
                                       LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                       0, vid, pid, LIBUSB_HOTPLUG_MATCH_ANY,
                                       hotplug_callback, NULL, &callback_handle);
    }
    UsbVid = vid;
    UsbPid = pid;
    UsbEvent = 0;
  }

  return UsbEvent;
}

void *guidriver_apphandle(void)
{
  return winApp;
}

#if !defined(GL_GENERATE_MIPMAP)
# define GL_GENERATE_MIPMAP 0x8191 /* from GLEW.h, OpenGL 1.4 only! */
#endif

struct nk_image guidriver_image_from_memory(const unsigned char *data, unsigned size)
{
  unsigned w, h;
  unsigned char *pixels;
  GLuint tex;

  if (lodepng_decode32(&pixels, &w, &h, data, size) != 0)
    return nk_image_id(0);

  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_NEAREST);
# if defined(_USE_OPENGL) && (_USE_OPENGL > 2)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
# else
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
# endif

  return nk_image_id((int)tex);
}

#endif

