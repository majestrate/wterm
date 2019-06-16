/* See LICENSE for license details. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
/* for BTN_* definitions */
#include "wld/wayland.h"
#include "wld/wld.h"
#include <fontconfig/fontconfig.h>
#include <libgen.h>
#include <linux/input.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wchar.h>
#include <xkbcommon/xkbcommon.h>

#include "arg.h"
#include "xdg-shell-unstable-v5-client-protocol.h"
#include "xdg-shell-unstable-v6-client-protocol.h"

char *argv0;

#if defined(__linux)
#include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#endif

/* Arbitrary sizes */
#define UTF_INVALID 0xFFFD
#define UTF_SIZ 4
#define ESC_BUF_SIZ (128 * UTF_SIZ)
#define ESC_ARG_SIZ 16
#define STR_BUF_SIZ ESC_BUF_SIZ
#define STR_ARG_SIZ ESC_ARG_SIZ
#define DRAW_BUF_SIZ 20 * 1024
#define XK_ANY_MOD UINT_MAX
#define XK_NO_MOD 0
#define XK_SWITCH_MOD (1 << 13)

#define MOD_MASK_ANY UINT_MAX
#define MOD_MASK_NONE 0
#define MOD_MASK_CTRL (1 << 0)
#define MOD_MASK_ALT (1 << 1)
#define MOD_MASK_SHIFT (1 << 2)
#define MOD_MASK_LOGO (1 << 3)

#define AXIS_VERTICAL WL_POINTER_AXIS_VERTICAL_SCROLL
#define AXIS_HORIZONTAL WL_POINTER_AXIS_HORIZONTAL_SCROLL

/* macros */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define LEN(a) (sizeof(a) / sizeof(a)[0])
#define DEFAULT(a, b) (a) = (a) ? (a) : (b)
#define BETWEEN(x, a, b) ((a) <= (x) && (x) <= (b))
#define ISCONTROLC0(c) (BETWEEN(c, 0, 0x1f) || (c) == '\177')
#define ISCONTROLC1(c) (BETWEEN(c, 0x80, 0x9f))
#define ISCONTROL(c) (ISCONTROLC0(c) || ISCONTROLC1(c))
#define ISDELIM(u) (utf8strchr(worddelimiters, u) != NULL)
#define LIMIT(x, a, b) (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b)                                                          \
  ((a).mode != (b).mode || (a).fg != (b).fg || (a).bg != (b).bg)
#define IS_SET(flag) ((term.mode & (flag)) != 0)
#define TIMEDIFF(t1, t2)                                                       \
  ((t1.tv_sec - t2.tv_sec) * 1000 + (t1.tv_nsec - t2.tv_nsec) / 1E6)
#define MODBIT(x, set, bit) ((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

#define TRUECOLOR(r, g, b) (1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x) (1 << 24 & (x))
#define TRUERED(x) (((x)&0xff0000) >> 8)
#define TRUEGREEN(x) (((x)&0xff00))
#define TRUEBLUE(x) (((x)&0xff) << 8)

enum glyph_attribute {
  ATTR_NULL = 0,
  ATTR_BOLD = 1 << 0,
  ATTR_FAINT = 1 << 1,
  ATTR_ITALIC = 1 << 2,
  ATTR_UNDERLINE = 1 << 3,
  ATTR_BLINK = 1 << 4,
  ATTR_REVERSE = 1 << 5,
  ATTR_INVISIBLE = 1 << 6,
  ATTR_STRUCK = 1 << 7,
  ATTR_WRAP = 1 << 8,
  ATTR_WIDE = 1 << 9,
  ATTR_WDUMMY = 1 << 10,
  ATTR_BOLD_FAINT = ATTR_BOLD | ATTR_FAINT,
};

enum cursor_movement { CURSOR_SAVE, CURSOR_LOAD };

enum cursor_state {
  CURSOR_DEFAULT = 0,
  CURSOR_WRAPNEXT = 1,
  CURSOR_ORIGIN = 2
};

enum term_mode {
  MODE_WRAP = 1 << 0,
  MODE_INSERT = 1 << 1,
  MODE_APPKEYPAD = 1 << 2,
  MODE_ALTSCREEN = 1 << 3,
  MODE_CRLF = 1 << 4,
  MODE_MOUSEBTN = 1 << 5,
  MODE_MOUSEMOTION = 1 << 6,
  MODE_REVERSE = 1 << 7,
  MODE_KBDLOCK = 1 << 8,
  MODE_HIDE = 1 << 9,
  MODE_ECHO = 1 << 10,
  MODE_APPCURSOR = 1 << 11,
  MODE_MOUSESGR = 1 << 12,
  MODE_8BIT = 1 << 13,
  MODE_BLINK = 1 << 14,
  MODE_FBLINK = 1 << 15,
  MODE_FOCUS = 1 << 16,
  MODE_MOUSEX10 = 1 << 17,
  MODE_MOUSEMANY = 1 << 18,
  MODE_BRCKTPASTE = 1 << 19,
  MODE_PRINT = 1 << 20,
  MODE_MOUSE =
      MODE_MOUSEBTN | MODE_MOUSEMOTION | MODE_MOUSEX10 | MODE_MOUSEMANY,
};

enum charset {
  CS_GRAPHIC0,
  CS_GRAPHIC1,
  CS_UK,
  CS_USA,
  CS_MULTI,
  CS_GER,
  CS_FIN
};

enum escape_state {
  ESC_START = 1,
  ESC_CSI = 2,
  ESC_STR = 4, /* DCS, OSC, PM, APC */
  ESC_ALTCHARSET = 8,
  ESC_STR_END = 16, /* a final string was encountered */
  ESC_TEST = 32,    /* Enter in test mode */
};

enum window_state { WIN_VISIBLE = 1, WIN_FOCUSED = 2 };

enum selection_mode { SEL_IDLE = 0, SEL_EMPTY = 1, SEL_READY = 2 };

enum selection_type { SEL_REGULAR = 1, SEL_RECTANGULAR = 2 };

enum selection_snap { SNAP_WORD = 1, SNAP_LINE = 2 };

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

typedef uint_least32_t Rune;

typedef struct {
  Rune u;      /* character code */
  ushort mode; /* attribute flags */
  uint32_t fg; /* foreground  */
  uint32_t bg; /* background  */
} Glyph;

typedef Glyph *Line;

typedef struct {
  Glyph attr; /* current char attributes */
  int x;
  int y;
  char state;
} TCursor;

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode> [<mode>]] */
typedef struct {
  char buf[ESC_BUF_SIZ]; /* raw string */
  int len;               /* raw string length */
  char priv;
  int arg[ESC_ARG_SIZ];
  int narg; /* nb of args */
  char mode[2];
} CSIEscape;

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct {
  char type;             /* ESC type ... */
  char buf[STR_BUF_SIZ]; /* raw string */
  int len;               /* raw string length */
  char *args[STR_ARG_SIZ];
  int narg; /* nb of args */
} STREscape;

/* Internal representation of the screen */
typedef struct {
  int row;         /* nb row */
  int col;         /* nb col */
  Line *line;      /* screen */
  Line *alt;       /* alternate screen */
  int *dirty;      /* dirtyness of lines */
  TCursor c;       /* cursor */
  int top;         /* top    scroll limit */
  int bot;         /* bottom scroll limit */
  int mode;        /* terminal mode flags */
  int esc;         /* escape state flags */
  char trantbl[4]; /* charset table translation */
  int charset;     /* current charset */
  int icharset;    /* selected charset for sequence */
  int numlock;     /* lock numbers in keyboard */
  int *tabs;
} Term;

typedef struct {
  struct xkb_context *ctx;
  struct xkb_keymap *keymap;
  struct xkb_state *state;
  xkb_mod_index_t ctrl, alt, shift, logo;
  unsigned int mods;
} XKB;

typedef struct {
  struct wl_display *dpy;
  struct wl_compositor *cmp;
  struct wl_shm *shm;
  struct wl_seat *seat;
  struct wl_keyboard *keyboard;
  struct wl_pointer *pointer;
  struct wl_data_device_manager *datadevmanager;
  struct wl_data_device *datadev;
  struct wl_data_offer *seloffer;
  struct wl_surface *surface;
  struct wl_buffer *buffer;
  struct zxdg_shell_v6 *xdgshell_v6;
  struct wl_shell *shell;
  struct wl_shell_surface *shellsurf;
  struct xdg_shell *xdgshell;
  struct xdg_surface *xdgsurface;
  struct wl_surface *popupsurface;
  struct zxdg_surface_v6 *xdgsurface_v6;
  struct zxdg_toplevel_v6 *xdgtoplevel;
  XKB xkb;
  bool configured;
  int px, py; /* pointer x and y */
  int tw, th; /* tty width and height */
  int w, h;   /* window width and height */
  int ch;     /* char height */
  int cw;     /* char width  */
  int vis;
  char state; /* focus, redraw, visible */
  int cursor; /* cursor style */
  struct wl_callback *framecb;
} Wayland;

typedef struct {
  struct wld_context *ctx;
  struct wld_font_context *fontctx;
  struct wld_renderer *renderer;
  struct wld_buffer *buffer, *oldbuffer;
} WLD;

typedef struct {
  struct wl_cursor_theme *theme;
  struct wl_cursor *cursor;
  struct wl_surface *surface;
} Cursor;

typedef struct {
  uint b;
  uint mask;
  char *s;
} Mousekey;

typedef struct {
  int axis;
  int dir;
  uint mask;
  char s[ESC_BUF_SIZ];
} Axiskey;

typedef struct {
  xkb_keysym_t k;
  uint mask;
  char *s;
  /* three valued logic variables: 0 indifferent, 1 on, -1 off */
  signed char appkey;    /* application keypad */
  signed char appcursor; /* application cursor */
  signed char crlf;      /* crlf mode          */
} Key;

typedef struct {
  int mode;
  int type;
  int snap;
  /*
   * Selection variables:
   * nb – normalized coordinates of the beginning of the selection
   * ne – normalized coordinates of the end of the selection
   * ob – original coordinates of the beginning of the selection
   * oe – original coordinates of the end of the selection
   */
  struct {
    int x, y;
  } nb, ne, ob, oe;

  char *primary;
  struct wl_data_source *source;
  int alt;
  uint32_t tclick1, tclick2;
} Selection;

typedef union {
  int i;
  uint ui;
  float f;
  const void *v;
} Arg;

typedef struct {
  uint mod;
  xkb_keysym_t keysym;
  void (*func)(const Arg *);
  const Arg arg;
} Shortcut;

typedef struct {
  char str[32];
  uint32_t key;
  int len;
  bool started;
  struct timespec last;
} Repeat;

/* function definitions used in config.h */
static void numlock(const Arg *);
static void selpaste(const Arg *);
static void wlzoom(const Arg *);
static void wlzoomabs(const Arg *);
static void wlzoomreset(const Arg *);
static void printsel(const Arg *);
static void printscreen(const Arg *);
static void toggleprinter(const Arg *);

/* Config.h for applying patches and the configuration. */
#include "config.h"

/* Font structure */
typedef struct {
  int height;
  int width;
  int ascent;
  int descent;
  short lbearing;
  short rbearing;
  struct wld_font *match;
  FcFontSet *set;
  FcPattern *pattern;
} Font;

/* Drawing Context */
typedef struct {
  uint32_t col[MAX(LEN(colorname), 256)];
  Font font, bfont, ifont, ibfont;
} DC;

static void die(const char *, ...);
static void draw(void);
static void redraw(void);
static void drawregion(int, int, int, int);
static void execsh(void);
static void stty(void);
static void sigchld(int);
static void run(void);
static void cresize(int, int);

static void csidump(void);
static void csihandle(void);
static void csiparse(void);
static void csireset(void);
static int eschandle(uchar);
static void strdump(void);
static void strhandle(void);
static void strparse(void);
static void strreset(void);

static int tattrset(int);
static void tprinter(char *, size_t);
static void tdumpsel(void);
static void tdumpline(int);
static void tdump(void);
static void tclearregion(int, int, int, int);
static void tcursor(int);
static void tdeletechar(int);
static void tdeleteline(int);
static void tinsertblank(int);
static void tinsertblankline(int);
static int tlinelen(int);
static void tmoveto(int, int);
static void tmoveato(int, int);
static void tnew(int, int);
static void tnewline(int);
static void tputtab(int);
static void tputc(Rune);
static void treset(void);
static void tresize(int, int);
static void tscrollup(int, int);
static void tscrolldown(int, int);
static void tsetattr(int *, int);
static void tsetchar(Rune, Glyph *, int, int);
static void tsetscroll(int, int);
static void tswapscreen(void);
static void tsetdirt(int, int);
static void tsetdirtattr(int);
static void tsetmode(int, int, int *, int);
static void tfulldirt(void);
static void techo(Rune);
static void tcontrolcode(uchar);
static void tdectest(char);
static uint32_t tdefcolor(int *, int *, int);
static void tdeftran(char);
static inline int match(uint, uint);
static void ttynew(void);
static void ttyread(void);
static void ttyresize(void);
static void ttysend(char *, size_t);
static void ttywrite(const char *, size_t);
static void tstrsequence(uchar);

static inline uchar sixd_to_8bit(int);
static void wldraws(char *, Glyph, int, int, int, int);
static void wldrawglyph(Glyph, int, int);
static void wlclear(int, int, int, int);
static void wldrawcursor(void);
static void wlinit(void);
static void wlloadcols(void);
static int wlsetcolorname(int, const char *);
static void wlloadcursor(void);
static int wlloadfont(Font *, FcPattern *);
static void wlloadfonts(char *, double);
static void wlsettitle(char *);
static void wlresettitle(void);
static void wlseturgency(int);
static void wlsetsel(char *, uint32_t);
static void wltermclear(int, int, int, int);
static void wlunloadfont(Font *f);
static void wlunloadfonts(void);
static void wlresize(int, int);

static void regglobal(void *, struct wl_registry *, uint32_t, const char *,
                      uint32_t);
static void regglobalremove(void *, struct wl_registry *, uint32_t);
static void surfenter(void *, struct wl_surface *, struct wl_output *);
static void surfleave(void *, struct wl_surface *, struct wl_output *);
static void framedone(void *, struct wl_callback *, uint32_t);
static void kbdkeymap(void *, struct wl_keyboard *, uint32_t, int32_t,
                      uint32_t);
static void kbdenter(void *, struct wl_keyboard *, uint32_t,
                     struct wl_surface *, struct wl_array *);
static void kbdleave(void *, struct wl_keyboard *, uint32_t,
                     struct wl_surface *);
static void kbdkey(void *, struct wl_keyboard *, uint32_t, uint32_t, uint32_t,
                   uint32_t);
static void kbdmodifiers(void *, struct wl_keyboard *, uint32_t, uint32_t,
                         uint32_t, uint32_t, uint32_t);
static void kbdrepeatinfo(void *, struct wl_keyboard *, int32_t, int32_t);
static void ptrenter(void *, struct wl_pointer *, uint32_t, struct wl_surface *,
                     wl_fixed_t, wl_fixed_t);
static void ptrleave(void *, struct wl_pointer *, uint32_t,
                     struct wl_surface *);
static void ptrmotion(void *, struct wl_pointer *, uint32_t, wl_fixed_t,
                      wl_fixed_t);
static void ptrbutton(void *, struct wl_pointer *, uint32_t, uint32_t, uint32_t,
                      uint32_t);
static void ptraxis(void *, struct wl_pointer *, uint32_t, uint32_t,
                    wl_fixed_t);

static void shellsurfping(void *, struct wl_shell_surface *, uint32_t);
static void shellsurfconfigure(void *, struct wl_shell_surface *, uint32_t,
                               int32_t, int32_t);
static void shellsurfpopupdone(void *, struct wl_shell_surface *);

static void xdgshellv6ping(void *, struct zxdg_shell_v6 *, uint32_t);
static void xdgsurfv6configure(void *, struct zxdg_surface_v6 *, uint32_t);
static void xdgsurfconfigure(void *, struct xdg_surface *, int32_t, int32_t,
                             struct wl_array *, uint32_t);
static void xdgsurfclose(void *, struct xdg_surface *);
static void xdgtopconfigure(void *, struct zxdg_toplevel_v6 *, int32_t, int32_t,
                            struct wl_array *);
static void xdgtopclose(void *, struct zxdg_toplevel_v6 *);
static void xdgshellping(void *, struct xdg_shell *, uint32_t);
static void datadevoffer(void *, struct wl_data_device *,
                         struct wl_data_offer *);
static void datadeventer(void *, struct wl_data_device *, uint32_t,
                         struct wl_surface *, wl_fixed_t, wl_fixed_t,
                         struct wl_data_offer *);
static void datadevleave(void *, struct wl_data_device *);
static void datadevmotion(void *, struct wl_data_device *, uint32_t,
                          wl_fixed_t x, wl_fixed_t y);
static void datadevdrop(void *, struct wl_data_device *);
static void datadevselection(void *, struct wl_data_device *,
                             struct wl_data_offer *);
static void dataofferoffer(void *, struct wl_data_offer *, const char *);
static void datasrctarget(void *, struct wl_data_source *, const char *);
static void datasrcsend(void *, struct wl_data_source *, const char *, int32_t);
static void datasrccancelled(void *, struct wl_data_source *);

static void selinit(void);
static void selnormalize(void);
static inline int selected(int, int);
static char *getsel(void);
static void selcopy(uint32_t);
static void selscroll(int, int);
static void selsnap(int *, int *, int);
static int x2col(int);
static int y2row(int);

static size_t utf8decode(char *, Rune *, size_t);
static Rune utf8decodebyte(char, size_t *);
static size_t utf8encode(Rune, char *);
static char utf8encodebyte(Rune, size_t);
static char *utf8strchr(char *s, Rune u);
static size_t utf8validate(Rune *, size_t);

static ssize_t xwrite(int, const char *, size_t);
static void *xmalloc(size_t);
static void *xrealloc(void *, size_t);
static char *xstrdup(char *);

static void usage(void);

static struct wl_registry_listener reglistener = {regglobal, regglobalremove};
static struct wl_surface_listener surflistener = {surfenter, surfleave};
static struct wl_callback_listener framelistener = {framedone};
static struct wl_keyboard_listener kbdlistener = {
    kbdkeymap, kbdenter, kbdleave, kbdkey, kbdmodifiers, kbdrepeatinfo};
static struct wl_pointer_listener ptrlistener = {ptrenter, ptrleave, ptrmotion,
                                                 ptrbutton, ptraxis};
static struct zxdg_shell_v6_listener shell_v6_listener = {xdgshellv6ping};
static struct zxdg_surface_v6_listener surf_v6_listener = {xdgsurfv6configure};
static struct xdg_shell_listener shell_listener = {xdgshellping};
static struct wl_shell_surface_listener shellsurf_listener = {
    shellsurfping, shellsurfconfigure, shellsurfpopupdone};
static struct xdg_surface_listener xdgsurflistener = {xdgsurfconfigure,
                                                      xdgsurfclose};
static struct zxdg_toplevel_v6_listener xdgtoplevellistener = {xdgtopconfigure,
                                                               xdgtopclose};
static struct wl_data_device_listener datadevlistener = {
    datadevoffer,  datadeventer, datadevleave,
    datadevmotion, datadevdrop,  datadevselection};
static struct wl_data_offer_listener dataofferlistener = {dataofferoffer};
static struct wl_data_source_listener datasrclistener = {
    datasrctarget, datasrcsend, datasrccancelled};

/* Globals */
static DC dc;
static Wayland wl;
static WLD wld;
static Cursor cursor;
static Term term;
static CSIEscape csiescseq;
static STREscape strescseq;
static int cmdfd;
static pid_t pid;
static Selection sel;
static Repeat repeat;
static bool needdraw = true;
static int iofd = 1;
static char **opt_cmd = NULL;
static char *opt_io = NULL;
static char *opt_title = NULL;
static char *opt_class = NULL;
static char *opt_font = NULL;
static char *opt_line = NULL;
static int oldbutton = 3; /* button event on startup: 3 = release */
static int oldx, oldy;
static char *usedfont = NULL;
static double usedfontsize = 0;
static double defaultfontsize = 0;

static uchar utfbyte[UTF_SIZ + 1] = {0x80, 0, 0xC0, 0xE0, 0xF0};
static uchar utfmask[UTF_SIZ + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static Rune utfmin[UTF_SIZ + 1] = {0, 0, 0x80, 0x800, 0x10000};
static Rune utfmax[UTF_SIZ + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

/* Font Ring Cache */
enum { FRC_NORMAL, FRC_ITALIC, FRC_BOLD, FRC_ITALICBOLD };

typedef struct {
  struct wld_font *font;
  int flags;
  Rune unicodep;
} Fontcache;

/* Fontcache is an array now. A new font will be appended to the array. */
static Fontcache frc[16];
static int frclen = 0;

ssize_t xwrite(int fd, const char *s, size_t len) {
  size_t aux = len;
  ssize_t r;

  while (len > 0) {
    r = write(fd, s, len);
    if (r < 0)
      return r;
    len -= r;
    s += r;
  }

  return aux;
}

void *xmalloc(size_t len) {
  void *p = malloc(len);

  if (!p)
    die("Out of memory\n");

  return p;
}

void *xrealloc(void *p, size_t len) {
  if ((p = realloc(p, len)) == NULL)
    die("Out of memory\n");

  return p;
}

char *xstrdup(char *s) {
  if ((s = strdup(s)) == NULL)
    die("Out of memory\n");

  return s;
}

size_t utf8decode(char *c, Rune *u, size_t clen) {
  size_t i, j, len, type;
  Rune udecoded;

  *u = UTF_INVALID;
  if (!clen)
    return 0;
  udecoded = utf8decodebyte(c[0], &len);
  if (!BETWEEN(len, 1, UTF_SIZ))
    return 1;
  for (i = 1, j = 1; i < clen && j < len; ++i, ++j) {
    udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
    if (type != 0)
      return j;
  }
  if (j < len)
    return 0;
  *u = udecoded;
  utf8validate(u, len);

  return len;
}

Rune utf8decodebyte(char c, size_t *i) {
  for (*i = 0; *i < LEN(utfmask); ++(*i))
    if (((uchar)c & utfmask[*i]) == utfbyte[*i])
      return (uchar)c & ~utfmask[*i];

  return 0;
}

size_t utf8encode(Rune u, char *c) {
  size_t len, i;

  len = utf8validate(&u, 0);
  if (len > UTF_SIZ)
    return 0;

  for (i = len - 1; i != 0; --i) {
    c[i] = utf8encodebyte(u, 0);
    u >>= 6;
  }
  c[0] = utf8encodebyte(u, len);

  return len;
}

char utf8encodebyte(Rune u, size_t i) { return utfbyte[i] | (u & ~utfmask[i]); }

char *utf8strchr(char *s, Rune u) {
  Rune r;
  size_t i, j, len;

  len = strlen(s);
  for (i = 0, j = 0; i < len; i += j) {
    if (!(j = utf8decode(&s[i], &r, len - i)))
      break;
    if (r == u)
      return &(s[i]);
  }

  return NULL;
}

size_t utf8validate(Rune *u, size_t i) {
  if (!BETWEEN(*u, utfmin[i], utfmax[i]) || BETWEEN(*u, 0xD800, 0xDFFF))
    *u = UTF_INVALID;
  for (i = 1; *u > utfmax[i]; ++i)
    ;

  return i;
}

void selinit(void) {
  sel.tclick1 = 0;
  sel.tclick2 = 0;
  sel.mode = SEL_IDLE;
  sel.ob.x = -1;
  sel.primary = NULL;
  sel.source = NULL;
}

int x2col(int x) {
  x -= borderpx;
  x /= wl.cw;

  return LIMIT(x, 0, term.col - 1);
}

int y2row(int y) {
  y -= borderpx;
  y /= wl.ch;

  return LIMIT(y, 0, term.row - 1);
}

int tlinelen(int y) {
  int i = term.col;

  if (term.line[y][i - 1].mode & ATTR_WRAP)
    return i;

  while (i > 0 && term.line[y][i - 1].u == ' ')
    --i;

  return i;
}

void selnormalize(void) {
  int i;

  if (sel.type == SEL_REGULAR && sel.ob.y != sel.oe.y) {
    sel.nb.x = sel.ob.y < sel.oe.y ? sel.ob.x : sel.oe.x;
    sel.ne.x = sel.ob.y < sel.oe.y ? sel.oe.x : sel.ob.x;
  } else {
    sel.nb.x = MIN(sel.ob.x, sel.oe.x);
    sel.ne.x = MAX(sel.ob.x, sel.oe.x);
  }
  sel.nb.y = MIN(sel.ob.y, sel.oe.y);
  sel.ne.y = MAX(sel.ob.y, sel.oe.y);

  selsnap(&sel.nb.x, &sel.nb.y, -1);
  selsnap(&sel.ne.x, &sel.ne.y, +1);

  /* expand selection over line breaks */
  if (sel.type == SEL_RECTANGULAR)
    return;
  i = tlinelen(sel.nb.y);
  if (i < sel.nb.x)
    sel.nb.x = i;
  if (tlinelen(sel.ne.y) <= sel.ne.x)
    sel.ne.x = term.col - 1;
}

int selected(int x, int y) {
  if (sel.mode == SEL_EMPTY)
    return 0;

  if (sel.type == SEL_RECTANGULAR)
    return BETWEEN(y, sel.nb.y, sel.ne.y) && BETWEEN(x, sel.nb.x, sel.ne.x);

  return BETWEEN(y, sel.nb.y, sel.ne.y) && (y != sel.nb.y || x >= sel.nb.x) &&
         (y != sel.ne.y || x <= sel.ne.x);
}

void selsnap(int *x, int *y, int direction) {
  int newx, newy, xt, yt;
  int delim, prevdelim;
  Glyph *gp, *prevgp;

  switch (sel.snap) {
  case SNAP_WORD:
    /*
     * Snap around if the word wraps around at the end or
     * beginning of a line.
     */
    prevgp = &term.line[*y][*x];
    prevdelim = ISDELIM(prevgp->u);
    for (;;) {
      newx = *x + direction;
      newy = *y;
      if (!BETWEEN(newx, 0, term.col - 1)) {
        newy += direction;
        newx = (newx + term.col) % term.col;
        if (!BETWEEN(newy, 0, term.row - 1))
          break;

        if (direction > 0)
          yt = *y, xt = *x;
        else
          yt = newy, xt = newx;
        if (!(term.line[yt][xt].mode & ATTR_WRAP))
          break;
      }

      if (newx >= tlinelen(newy))
        break;

      gp = &term.line[newy][newx];
      delim = ISDELIM(gp->u);
      if (!(gp->mode & ATTR_WDUMMY) &&
          (delim != prevdelim || (delim && gp->u != prevgp->u)))
        break;

      *x = newx;
      *y = newy;
      prevgp = gp;
      prevdelim = delim;
    }
    break;
  case SNAP_LINE:
    /*
     * Snap around if the the previous line or the current one
     * has set ATTR_WRAP at its end. Then the whole next or
     * previous line will be selected.
     */
    *x = (direction < 0) ? 0 : term.col - 1;
    if (direction < 0) {
      for (; *y > 0; *y += direction) {
        if (!(term.line[*y - 1][term.col - 1].mode & ATTR_WRAP)) {
          break;
        }
      }
    } else if (direction > 0) {
      for (; *y < term.row - 1; *y += direction) {
        if (!(term.line[*y][term.col - 1].mode & ATTR_WRAP)) {
          break;
        }
      }
    }
    break;
  }
}

void getbuttoninfo(void) {
  int type;
  uint state = wl.xkb.mods & ~forceselmod;

  sel.alt = IS_SET(MODE_ALTSCREEN);

  sel.oe.x = x2col(wl.px);
  sel.oe.y = y2row(wl.py);
  selnormalize();

  sel.type = SEL_REGULAR;
  for (type = 1; type < LEN(selmasks); ++type) {
    if (match(selmasks[type], state)) {
      sel.type = type;
      break;
    }
  }
}

void wlmousereport(int button, bool release, int x, int y) {
  int len;
  char buf[40];

  if (!IS_SET(MODE_MOUSEX10)) {
    button += ((wl.xkb.mods & MOD_MASK_SHIFT) ? 4 : 0) +
              ((wl.xkb.mods & MOD_MASK_LOGO) ? 8 : 0) +
              ((wl.xkb.mods & MOD_MASK_CTRL) ? 16 : 0);
  }

  if (IS_SET(MODE_MOUSESGR)) {
    len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c", button, x + 1, y + 1,
                   release ? 'm' : 'M');
  } else if (x < 223 && y < 223) {
    len = snprintf(buf, sizeof(buf), "\033[M%c%c%c", 32 + button, 32 + x + 1,
                   32 + y + 1);
  } else {
    return;
  }

  ttywrite(buf, len);
}

void wlmousereportbutton(uint32_t button, uint32_t state) {
  bool release = state == WL_POINTER_BUTTON_STATE_RELEASED;

  if (!IS_SET(MODE_MOUSESGR) && release) {
    button = 3;
  } else {
    switch (button) {
    case BTN_LEFT:
      button = 0;
      break;
    case BTN_MIDDLE:
      button = 1;
      break;
    case BTN_RIGHT:
      button = 2;
      break;
    }
  }

  oldbutton = release ? 3 : button;

  /* don't report release events when in X10 mode */
  if (IS_SET(MODE_MOUSEX10) && release) {
    return;
  }

  wlmousereport(button, release, oldx, oldy);
}

void wlmousereportmotion(wl_fixed_t fx, wl_fixed_t fy) {
  int x = x2col(wl_fixed_to_int(fx)), y = y2row(wl_fixed_to_int(fy));

  if (x == oldx && y == oldy)
    return;
  if (!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY))
    return;
  /* MOUSE_MOTION: no reporting if no button is pressed */
  if (IS_SET(MODE_MOUSEMOTION) && oldbutton == 3)
    return;

  oldx = x;
  oldy = y;
  wlmousereport(oldbutton + 32, false, x, y);
}

void wlmousereportaxis(uint32_t axis, wl_fixed_t amount) {
  wlmousereport(64 + (axis == AXIS_VERTICAL ? 4 : 6) + (amount > 0 ? 1 : 0),
                false, oldx, oldy);
}

char *getsel(void) {
  char *str, *ptr;
  int y, bufsize, lastx, linelen;
  Glyph *gp, *last;

  if (sel.ob.x == -1)
    return NULL;

  bufsize = (term.col + 1) * (sel.ne.y - sel.nb.y + 1) * UTF_SIZ;
  ptr = str = xmalloc(bufsize);

  /* append every set & selected glyph to the selection */
  for (y = sel.nb.y; y <= sel.ne.y; y++) {
    linelen = tlinelen(y);

    if (sel.type == SEL_RECTANGULAR) {
      gp = &term.line[y][sel.nb.x];
      lastx = sel.ne.x;
    } else {
      gp = &term.line[y][sel.nb.y == y ? sel.nb.x : 0];
      lastx = (sel.ne.y == y) ? sel.ne.x : term.col - 1;
    }
    last = &term.line[y][MIN(lastx, linelen - 1)];
    while (last >= gp && last->u == ' ')
      --last;

    for (; gp <= last; ++gp) {
      if (gp->mode & ATTR_WDUMMY)
        continue;

      ptr += utf8encode(gp->u, ptr);
    }

    /*
     * Copy and pasting of line endings is inconsistent
     * in the inconsistent terminal and GUI world.
     * The best solution seems like to produce '\n' when
     * something is copied from wterm and convert '\n' to
     * '\r', when something to be pasted is received by
     * wterm.
     * FIXME: Fix the computer world.
     */
    if ((y < sel.ne.y || lastx >= linelen) && !(last->mode & ATTR_WRAP))
      *ptr++ = '\n';
  }
  *ptr = 0;
  return str;
}

void selcopy(uint32_t serial) { wlsetsel(getsel(), serial); }

static inline void selwritebuf(char *buf, int len) {
  char *repl = buf;

  /*
   * As seen in getsel:
   * Line endings are inconsistent in the terminal and GUI world
   * copy and pasting. When receiving some selection data,
   * replace all '\n' with '\r'.
   * FIXME: Fix the computer world.
   */
  while ((repl = memchr(repl, '\n', len))) {
    *repl++ = '\r';
  }

  if (IS_SET(MODE_BRCKTPASTE))
    ttywrite("\033[200~", 6);
  ttysend(buf, len);
  if (IS_SET(MODE_BRCKTPASTE))
    ttywrite("\033[201~", 6);
}

void selpaste(const Arg *dummy) {
  int fds[2], len, left;
  char buf[BUFSIZ], *str;

  if (wl.seloffer) {
    /* check if we are pasting from ourselves */
    if (sel.source) {
      str = sel.primary;
      left = strlen(sel.primary);
      while (left > 0) {
        len = MIN(sizeof buf, left);
        memcpy(buf, str, len);
        selwritebuf(buf, len);
        left -= len;
        str += len;
      }
    } else {
      pipe(fds);
      wl_data_offer_receive(wl.seloffer, "text/plain", fds[1]);
      wl_display_flush(wl.dpy);
      close(fds[1]);
      while ((len = read(fds[0], buf, sizeof buf)) > 0) {
        selwritebuf(buf, len);
      }
      close(fds[0]);
    }
  }
}

void selclear(void) {
  if (sel.ob.x == -1)
    return;
  sel.mode = SEL_IDLE;
  sel.ob.x = -1;
  tsetdirt(sel.nb.y, sel.ne.y);
}

void wlsetsel(char *str, uint32_t serial) {
  free(sel.primary);
  sel.primary = str;

  if (str) {
    sel.source = wl_data_device_manager_create_data_source(wl.datadevmanager);
    wl_data_source_add_listener(sel.source, &datasrclistener, NULL);
    wl_data_source_offer(sel.source, "text/plain; charset=utf-8");
  } else {
    sel.source = NULL;
  }
  wl_data_device_set_selection(wl.datadev, sel.source, serial);
}

void die(const char *errstr, ...) {
  va_list ap;

  va_start(ap, errstr);
  vfprintf(stdout, errstr, ap);
  va_end(ap);
  exit(1);
}

void execsh(void) {
  char **args, *sh, *prog;
  const struct passwd *pw;

  errno = 0;
  if ((pw = getpwuid(getuid())) == NULL) {
    if (errno)
      die("getpwuid:%s\n", strerror(errno));
    else
      die("who are you?\n");
  }

  if ((sh = getenv("SHELL")) == NULL)
    sh = (pw->pw_shell[0]) ? pw->pw_shell : shell;

  if (opt_cmd)
    prog = opt_cmd[0];
  else if (utmp)
    prog = utmp;
  else
    prog = sh;
  args = (opt_cmd) ? opt_cmd : (char *[]){prog, NULL};

  unsetenv("COLUMNS");
  unsetenv("LINES");
  unsetenv("TERMCAP");
  setenv("LOGNAME", pw->pw_name, 1);
  setenv("USER", pw->pw_name, 1);
  setenv("SHELL", sh, 1);
  setenv("HOME", pw->pw_dir, 1);
  setenv("TERM", termname, 1);

  signal(SIGCHLD, SIG_DFL);
  signal(SIGHUP, SIG_DFL);
  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);
  signal(SIGTERM, SIG_DFL);
  signal(SIGALRM, SIG_DFL);

  execvp(prog, args);
  _exit(1);
}

void sigchld(int a) {
  int stat;
  pid_t p;

  if ((p = waitpid(pid, &stat, WNOHANG)) < 0)
    die("Waiting for pid %hd failed: %s\n", pid, strerror(errno));

  if (pid != p)
    return;

  if (!WIFEXITED(stat) || WEXITSTATUS(stat))
    die("child finished with error '%d'\n", stat);
  exit(0);
}

void stty(void) {
  char cmd[_POSIX_ARG_MAX], **p, *q, *s;
  size_t n, siz;

  if ((n = strlen(stty_args)) > sizeof(cmd) - 1)
    die("incorrect stty parameters\n");
  memcpy(cmd, stty_args, n);
  q = cmd + n;
  siz = sizeof(cmd) - n;
  for (p = opt_cmd; p && (s = *p); ++p) {
    if ((n = strlen(s)) > siz - 1)
      die("stty parameter length too long\n");
    *q++ = ' ';
    q = memcpy(q, s, n);
    q += n;
    siz -= n + 1;
  }
  *q = '\0';
  if (system(cmd) != 0)
    perror("Couldn't call stty");
}

void ttynew(void) {
  int m, s;
  struct winsize w = {term.row, term.col, 0, 0};

  if (opt_io) {
    term.mode |= MODE_PRINT;
    iofd = (!strcmp(opt_io, "-")) ? 1 : open(opt_io, O_WRONLY | O_CREAT, 0666);
    if (iofd < 0) {
      fprintf(stderr, "Error opening %s:%s\n", opt_io, strerror(errno));
    }
  }

  if (opt_line) {
    if ((cmdfd = open(opt_line, O_RDWR)) < 0)
      die("open line failed: %s\n", strerror(errno));
    close(0);
    dup(cmdfd);
    stty();
    return;
  }

  /* seems to work fine on linux, openbsd and freebsd */
  if (openpty(&m, &s, NULL, NULL, &w) < 0)
    die("openpty failed: %s\n", strerror(errno));

  switch (pid = fork()) {
  case -1:
    die("fork failed\n");
    break;
  case 0:
    close(iofd);
    setsid(); /* create a new process group */
    dup2(s, 0);
    dup2(s, 1);
    dup2(s, 2);
    if (ioctl(s, TIOCSCTTY, NULL) < 0)
      die("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
    close(s);
    close(m);
    execsh();
    break;
  default:
    close(s);
    cmdfd = m;
    signal(SIGCHLD, sigchld);
    break;
  }
}

void ttyread(void) {
  static char buf[BUFSIZ];
  static int buflen = 0;
  char *ptr;
  int charsize; /* size of utf8 char in bytes */
  Rune unicodep;
  int ret;

  /* append read bytes to unprocessed bytes */
  if ((ret = read(cmdfd, buf + buflen, LEN(buf) - buflen)) < 0)
    die("Couldn't read from shell: %s\n", strerror(errno));

  /* process every complete utf8 char */
  buflen += ret;
  ptr = buf;
  while ((charsize = utf8decode(ptr, &unicodep, buflen))) {
    tputc(unicodep);
    ptr += charsize;
    buflen -= charsize;
  }

  /* keep any uncomplete utf8 char for the next call */
  memmove(buf, ptr, buflen);
  needdraw = true;
}

void ttywrite(const char *s, size_t n) {
  fd_set wfd;
  struct timespec tv;
  ssize_t r;

  /*
   * Remember that we are using a pty, which might be a modem line.
   * Writing too much will clog the line. That's why we are doing this
   * dance.
   * FIXME: Migrate the world to Plan 9.
   */
  while (n > 0) {
    FD_ZERO(&wfd);
    FD_SET(cmdfd, &wfd);
    tv.tv_sec = 0;
    tv.tv_nsec = 0;

    /* Check if we can write. */
    if (pselect(cmdfd + 1, NULL, &wfd, NULL, &tv, NULL) < 0) {
      if (errno == EINTR)
        continue;
      die("select failed: %s\n", strerror(errno));
    }
    if (!FD_ISSET(cmdfd, &wfd)) {
      /* No, then free some buffer space. */
      ttyread();
    } else {
      /*
       * Only write 256 bytes at maximum. This seems to be a
       * reasonable value for a serial line. Bigger values
       * might clog the I/O.
       */
      r = write(cmdfd, s, (n < 256) ? n : 256);
      if (r < 0) {
        die("write error on tty: %s\n", strerror(errno));
      }
      if (r < n) {
        /*
         * We weren't able to write out everything.
         * This means the buffer is getting full
         * again. Empty it.
         */
        ttyread();
        n -= r;
        s += r;
      } else {
        /* All bytes have been written. */
        break;
      }
    }
  }
}

void ttysend(char *s, size_t n) {
  int len;
  Rune u;

  ttywrite(s, n);
  if (IS_SET(MODE_ECHO))
    while ((len = utf8decode(s, &u, n)) > 0) {
      techo(u);
      n -= len;
      s += len;
    }
}

void ttyresize(void) {
  struct winsize w;

  w.ws_row = term.row;
  w.ws_col = term.col;
  w.ws_xpixel = wl.tw;
  w.ws_ypixel = wl.th;
  if (ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
    fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

int tattrset(int attr) {
  int i, j;

  for (i = 0; i < term.row - 1; i++) {
    for (j = 0; j < term.col - 1; j++) {
      if (term.line[i][j].mode & attr)
        return 1;
    }
  }

  return 0;
}

void tsetdirt(int top, int bot) {
  int i;

  LIMIT(top, 0, term.row - 1);
  LIMIT(bot, 0, term.row - 1);

  for (i = top; i <= bot; i++)
    term.dirty[i] = 1;

  needdraw = true;
}

void tsetdirtattr(int attr) {
  int i, j;

  for (i = 0; i < term.row - 1; i++) {
    for (j = 0; j < term.col - 1; j++) {
      if (term.line[i][j].mode & attr) {
        tsetdirt(i, i);
        break;
      }
    }
  }
}

void tfulldirt(void) { tsetdirt(0, term.row - 1); }

void tcursor(int mode) {
  static TCursor c[2];
  int alt = IS_SET(MODE_ALTSCREEN);

  if (mode == CURSOR_SAVE) {
    c[alt] = term.c;
  } else if (mode == CURSOR_LOAD) {
    term.c = c[alt];
    tmoveto(c[alt].x, c[alt].y);
  }
}

void treset(void) {
  uint i;

  term.c = (TCursor){{.mode = ATTR_NULL, .fg = defaultfg, .bg = defaultbg},
                     .x = 0,
                     .y = 0,
                     .state = CURSOR_DEFAULT};

  memset(term.tabs, 0, term.col * sizeof(*term.tabs));
  for (i = tabspaces; i < term.col; i += tabspaces)
    term.tabs[i] = 1;
  term.top = 0;
  term.bot = term.row - 1;
  term.mode = MODE_WRAP;
  memset(term.trantbl, CS_USA, sizeof(term.trantbl));
  term.charset = 0;

  for (i = 0; i < 2; i++) {
    tmoveto(0, 0);
    tcursor(CURSOR_SAVE);
    tclearregion(0, 0, term.col - 1, term.row - 1);
    tswapscreen();
  }
}

void tnew(int col, int row) {
  term = (Term){.c = {.attr = {.fg = defaultfg, .bg = defaultbg}}};
  tresize(col, row);
  term.numlock = 1;

  treset();
}

void tswapscreen(void) {
  Line *tmp = term.line;

  term.line = term.alt;
  term.alt = tmp;
  term.mode ^= MODE_ALTSCREEN;
  tfulldirt();
}

void tscrolldown(int orig, int n) {
  int i;
  Line temp;

  LIMIT(n, 0, term.bot - orig + 1);

  tsetdirt(orig, term.bot - n);
  tclearregion(0, term.bot - n + 1, term.col - 1, term.bot);

  for (i = term.bot; i >= orig + n; i--) {
    temp = term.line[i];
    term.line[i] = term.line[i - n];
    term.line[i - n] = temp;
  }

  selscroll(orig, n);
}

void tscrollup(int orig, int n) {
  int i;
  Line temp;

  LIMIT(n, 0, term.bot - orig + 1);

  tclearregion(0, orig, term.col - 1, orig + n - 1);
  tsetdirt(orig + n, term.bot);

  for (i = orig; i <= term.bot - n; i++) {
    temp = term.line[i];
    term.line[i] = term.line[i + n];
    term.line[i + n] = temp;
  }

  selscroll(orig, -n);
}

void selscroll(int orig, int n) {
  if (sel.ob.x == -1)
    return;

  if (BETWEEN(sel.ob.y, orig, term.bot) || BETWEEN(sel.oe.y, orig, term.bot)) {
    if ((sel.ob.y += n) > term.bot || (sel.oe.y += n) < term.top) {
      selclear();
      return;
    }
    if (sel.type == SEL_RECTANGULAR) {
      if (sel.ob.y < term.top)
        sel.ob.y = term.top;
      if (sel.oe.y > term.bot)
        sel.oe.y = term.bot;
    } else {
      if (sel.ob.y < term.top) {
        sel.ob.y = term.top;
        sel.ob.x = 0;
      }
      if (sel.oe.y > term.bot) {
        sel.oe.y = term.bot;
        sel.oe.x = term.col;
      }
    }
    selnormalize();
  }
}

void tnewline(int first_col) {
  int y = term.c.y;

  if (y == term.bot) {
    tscrollup(term.top, 1);
  } else {
    y++;
  }
  tmoveto(first_col ? 0 : term.c.x, y);
}

void csiparse(void) {
  char *p = csiescseq.buf, *np;
  long int v;

  csiescseq.narg = 0;
  if (*p == '?') {
    csiescseq.priv = 1;
    p++;
  }

  csiescseq.buf[csiescseq.len] = '\0';
  while (p < csiescseq.buf + csiescseq.len) {
    np = NULL;
    v = strtol(p, &np, 10);
    if (np == p)
      v = 0;
    if (v == LONG_MAX || v == LONG_MIN)
      v = -1;
    csiescseq.arg[csiescseq.narg++] = v;
    p = np;
    if (*p != ';' || csiescseq.narg == ESC_ARG_SIZ)
      break;
    p++;
  }
  csiescseq.mode[0] = *p++;
  csiescseq.mode[1] = (p < csiescseq.buf + csiescseq.len) ? *p : '\0';
}

/* for absolute user moves, when decom is set */
void tmoveato(int x, int y) {
  tmoveto(x, y + ((term.c.state & CURSOR_ORIGIN) ? term.top : 0));
}

void tmoveto(int x, int y) {
  int miny, maxy;

  if (term.c.state & CURSOR_ORIGIN) {
    miny = term.top;
    maxy = term.bot;
  } else {
    miny = 0;
    maxy = term.row - 1;
  }
  term.c.state &= ~CURSOR_WRAPNEXT;
  term.c.x = LIMIT(x, 0, term.col - 1);
  term.c.y = LIMIT(y, miny, maxy);
}

void tsetchar(Rune u, Glyph *attr, int x, int y) {
  static char *vt100_0[62] = {
      /* 0x41 - 0x7e */
      "↑", "↓", "→", "←", "█", "▚", "☃",      /* A - G */
      0,   0,   0,   0,   0,   0,   0,   0,   /* H - O */
      0,   0,   0,   0,   0,   0,   0,   0,   /* P - W */
      0,   0,   0,   0,   0,   0,   0,   " ", /* X - _ */
      "◆", "▒", "␉", "␌", "␍", "␊", "°", "±", /* ` - g */
      "␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺", /* h - o */
      "⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", /* p - w */
      "│", "≤", "≥", "π", "≠", "£", "·",      /* x - ~ */
  };

  /*
   * The table is proudly stolen from rxvt.
   */
  if (term.trantbl[term.charset] == CS_GRAPHIC0 && BETWEEN(u, 0x41, 0x7e) &&
      vt100_0[u - 0x41])
    utf8decode(vt100_0[u - 0x41], &u, UTF_SIZ);

  if (term.line[y][x].mode & ATTR_WIDE) {
    if (x + 1 < term.col) {
      term.line[y][x + 1].u = ' ';
      term.line[y][x + 1].mode &= ~ATTR_WDUMMY;
    }
  } else if (term.line[y][x].mode & ATTR_WDUMMY) {
    term.line[y][x - 1].u = ' ';
    term.line[y][x - 1].mode &= ~ATTR_WIDE;
  }

  term.dirty[y] = 1;
  term.line[y][x] = *attr;
  term.line[y][x].u = u;
}

void tclearregion(int x1, int y1, int x2, int y2) {
  int x, y, temp;
  Glyph *gp;

  if (x1 > x2)
    temp = x1, x1 = x2, x2 = temp;
  if (y1 > y2)
    temp = y1, y1 = y2, y2 = temp;

  LIMIT(x1, 0, term.col - 1);
  LIMIT(x2, 0, term.col - 1);
  LIMIT(y1, 0, term.row - 1);
  LIMIT(y2, 0, term.row - 1);

  for (y = y1; y <= y2; y++) {
    term.dirty[y] = 1;
    for (x = x1; x <= x2; x++) {
      gp = &term.line[y][x];
      if (selected(x, y))
        selclear();
      gp->fg = term.c.attr.fg;
      gp->bg = term.c.attr.bg;
      gp->mode = 0;
      gp->u = ' ';
    }
  }
}

void tdeletechar(int n) {
  int dst, src, size;
  Glyph *line;

  LIMIT(n, 0, term.col - term.c.x);

  dst = term.c.x;
  src = term.c.x + n;
  size = term.col - src;
  line = term.line[term.c.y];

  memmove(&line[dst], &line[src], size * sizeof(Glyph));
  tclearregion(term.col - n, term.c.y, term.col - 1, term.c.y);
}

void tinsertblank(int n) {
  int dst, src, size;
  Glyph *line;

  LIMIT(n, 0, term.col - term.c.x);

  dst = term.c.x + n;
  src = term.c.x;
  size = term.col - dst;
  line = term.line[term.c.y];

  memmove(&line[dst], &line[src], size * sizeof(Glyph));
  tclearregion(src, term.c.y, dst - 1, term.c.y);
}

void tinsertblankline(int n) {
  if (BETWEEN(term.c.y, term.top, term.bot))
    tscrolldown(term.c.y, n);
}

void tdeleteline(int n) {
  if (BETWEEN(term.c.y, term.top, term.bot))
    tscrollup(term.c.y, n);
}

uint32_t tdefcolor(int *attr, int *npar, int l) {
  int32_t idx = -1;
  uint r, g, b;

  switch (attr[*npar + 1]) {
  case 2: /* direct color in RGB space */
    if (*npar + 4 >= l) {
      fprintf(stderr, "erresc(38): Incorrect number of parameters (%d)\n",
              *npar);
      break;
    }
    r = attr[*npar + 2];
    g = attr[*npar + 3];
    b = attr[*npar + 4];
    *npar += 4;
    if (!BETWEEN(r, 0, 255) || !BETWEEN(g, 0, 255) || !BETWEEN(b, 0, 255))
      fprintf(stderr, "erresc: bad rgb color (%u,%u,%u)\n", r, g, b);
    else
      idx = TRUECOLOR(r, g, b);
    break;
  case 5: /* indexed color */
    if (*npar + 2 >= l) {
      fprintf(stderr, "erresc(38): Incorrect number of parameters (%d)\n",
              *npar);
      break;
    }
    *npar += 2;
    if (!BETWEEN(attr[*npar], 0, 255))
      fprintf(stderr, "erresc: bad fgcolor %d\n", attr[*npar]);
    else
      idx = attr[*npar];
    break;
  case 0: /* implemented defined (only foreground) */
  case 1: /* transparent */
  case 3: /* direct color in CMY space */
  case 4: /* direct color in CMYK space */
  default:
    fprintf(stderr, "erresc(38): gfx attr %d unknown\n", attr[*npar]);
    break;
  }

  return idx;
}

void tsetattr(int *attr, int l) {
  int i;
  uint32_t idx;

  for (i = 0; i < l; i++) {
    switch (attr[i]) {
    case 0:
      term.c.attr.mode &=
          ~(ATTR_BOLD | ATTR_FAINT | ATTR_ITALIC | ATTR_UNDERLINE | ATTR_BLINK |
            ATTR_REVERSE | ATTR_INVISIBLE | ATTR_STRUCK);
      term.c.attr.fg = defaultfg;
      term.c.attr.bg = defaultbg;
      break;
    case 1:
      term.c.attr.mode |= ATTR_BOLD;
      break;
    case 2:
      term.c.attr.mode |= ATTR_FAINT;
      break;
    case 3:
      term.c.attr.mode |= ATTR_ITALIC;
      break;
    case 4:
      term.c.attr.mode |= ATTR_UNDERLINE;
      break;
    case 5: /* slow blink */
            /* FALLTHROUGH */
    case 6: /* rapid blink */
      term.c.attr.mode |= ATTR_BLINK;
      break;
    case 7:
      term.c.attr.mode |= ATTR_REVERSE;
      break;
    case 8:
      term.c.attr.mode |= ATTR_INVISIBLE;
      break;
    case 9:
      term.c.attr.mode |= ATTR_STRUCK;
      break;
    case 22:
      term.c.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT);
      break;
    case 23:
      term.c.attr.mode &= ~ATTR_ITALIC;
      break;
    case 24:
      term.c.attr.mode &= ~ATTR_UNDERLINE;
      break;
    case 25:
      term.c.attr.mode &= ~ATTR_BLINK;
      break;
    case 27:
      term.c.attr.mode &= ~ATTR_REVERSE;
      break;
    case 28:
      term.c.attr.mode &= ~ATTR_INVISIBLE;
      break;
    case 29:
      term.c.attr.mode &= ~ATTR_STRUCK;
      break;
    case 38:
      if ((idx = tdefcolor(attr, &i, l)) >= 0)
        term.c.attr.fg = idx;
      break;
    case 39:
      term.c.attr.fg = defaultfg;
      break;
    case 48:
      if ((idx = tdefcolor(attr, &i, l)) >= 0)
        term.c.attr.bg = idx;
      break;
    case 49:
      term.c.attr.bg = defaultbg;
      break;
    default:
      if (BETWEEN(attr[i], 30, 37)) {
        term.c.attr.fg = attr[i] - 30;
      } else if (BETWEEN(attr[i], 40, 47)) {
        term.c.attr.bg = attr[i] - 40;
      } else if (BETWEEN(attr[i], 90, 97)) {
        term.c.attr.fg = attr[i] - 90 + 8;
      } else if (BETWEEN(attr[i], 100, 107)) {
        term.c.attr.bg = attr[i] - 100 + 8;
      } else {
        fprintf(stderr, "erresc(default): gfx attr %d unknown\n", attr[i]),
            csidump();
      }
      break;
    }
  }
}

void tsetscroll(int t, int b) {
  int temp;

  LIMIT(t, 0, term.row - 1);
  LIMIT(b, 0, term.row - 1);
  if (t > b) {
    temp = t;
    t = b;
    b = temp;
  }
  term.top = t;
  term.bot = b;
}

void tsetmode(int priv, int set, int *args, int narg) {
  int *lim, mode;
  int alt;

  for (lim = args + narg; args < lim; ++args) {
    if (priv) {
      switch (*args) {
      case 1: /* DECCKM -- Cursor key */
        MODBIT(term.mode, set, MODE_APPCURSOR);
        break;
      case 5: /* DECSCNM -- Reverse video */
        mode = term.mode;
        MODBIT(term.mode, set, MODE_REVERSE);
        if (mode != term.mode)
          redraw();
        break;
      case 6: /* DECOM -- Origin */
        MODBIT(term.c.state, set, CURSOR_ORIGIN);
        tmoveato(0, 0);
        break;
      case 7: /* DECAWM -- Auto wrap */
        MODBIT(term.mode, set, MODE_WRAP);
        break;
      case 0:  /* Error (IGNORED) */
      case 2:  /* DECANM -- ANSI/VT52 (IGNORED) */
      case 3:  /* DECCOLM -- Column  (IGNORED) */
      case 4:  /* DECSCLM -- Scroll (IGNORED) */
      case 8:  /* DECARM -- Auto repeat (IGNORED) */
      case 18: /* DECPFF -- Printer feed (IGNORED) */
      case 19: /* DECPEX -- Printer extent (IGNORED) */
      case 42: /* DECNRCM -- National characters (IGNORED) */
      case 12: /* att610 -- Start blinking cursor (IGNORED) */
        break;
      case 25: /* DECTCEM -- Text Cursor Enable Mode */
        MODBIT(term.mode, !set, MODE_HIDE);
        break;
      case 9: /* X10 mouse compatibility mode */
        MODBIT(term.mode, 0, MODE_MOUSE);
        MODBIT(term.mode, set, MODE_MOUSEX10);
        break;
      case 1000: /* 1000: report button press */
        MODBIT(term.mode, 0, MODE_MOUSE);
        MODBIT(term.mode, set, MODE_MOUSEBTN);
        break;
      case 1002: /* 1002: report motion on button press */
        MODBIT(term.mode, 0, MODE_MOUSE);
        MODBIT(term.mode, set, MODE_MOUSEMOTION);
        break;
      case 1003: /* 1003: enable all mouse motions */
        MODBIT(term.mode, 0, MODE_MOUSE);
        MODBIT(term.mode, set, MODE_MOUSEMANY);
        break;
      case 1004: /* 1004: send focus events to tty */
        MODBIT(term.mode, set, MODE_FOCUS);
        break;
      case 1006: /* 1006: extended reporting mode */
        MODBIT(term.mode, set, MODE_MOUSESGR);
        break;
      case 1034:
        MODBIT(term.mode, set, MODE_8BIT);
        break;
      case 1049: /* swap screen & set/restore cursor as xterm */
        if (!allowaltscreen)
          break;
        tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
        /* FALLTHROUGH */
      case 47: /* swap screen */
      case 1047:
        if (!allowaltscreen)
          break;
        alt = IS_SET(MODE_ALTSCREEN);
        if (alt) {
          tclearregion(0, 0, term.col - 1, term.row - 1);
        }
        if (set ^ alt) /* set is always 1 or 0 */
          tswapscreen();
        if (*args != 1049)
          break;
        /* FALLTHROUGH */
      case 1048:
        tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
        break;
      case 2004: /* 2004: bracketed paste mode */
        MODBIT(term.mode, set, MODE_BRCKTPASTE);
        break;
      /* Not implemented mouse modes. See comments there. */
      case 1001: /* mouse highlight mode; can hang the
              terminal by design when implemented. */
      case 1005: /* UTF-8 mouse mode; will confuse
              applications not supporting UTF-8
              and luit. */
      case 1015: /* urxvt mangled mouse mode; incompatible
              and can be mistaken for other control
              codes. */
      default:
        fprintf(stderr, "erresc: unknown private set/reset mode %d\n", *args);
        break;
      }
    } else {
      switch (*args) {
      case 0: /* Error (IGNORED) */
        break;
      case 2: /* KAM -- keyboard action */
        MODBIT(term.mode, set, MODE_KBDLOCK);
        break;
      case 4: /* IRM -- Insertion-replacement */
        MODBIT(term.mode, set, MODE_INSERT);
        break;
      case 12: /* SRM -- Send/Receive */
        MODBIT(term.mode, !set, MODE_ECHO);
        break;
      case 20: /* LNM -- Linefeed/new line */
        MODBIT(term.mode, set, MODE_CRLF);
        break;
      default:
        fprintf(stderr, "erresc: unknown set/reset mode %d\n", *args);
        break;
      }
    }
  }
}

void csihandle(void) {
  char buf[40];
  int len;

  switch (csiescseq.mode[0]) {
  default:
  unknown:
    fprintf(stderr, "erresc: unknown csi ");
    csidump();
    /* die(""); */
    break;
  case '@': /* ICH -- Insert <n> blank char */
    DEFAULT(csiescseq.arg[0], 1);
    tinsertblank(csiescseq.arg[0]);
    break;
  case 'A': /* CUU -- Cursor <n> Up */
    DEFAULT(csiescseq.arg[0], 1);
    tmoveto(term.c.x, term.c.y - csiescseq.arg[0]);
    break;
  case 'B': /* CUD -- Cursor <n> Down */
  case 'e': /* VPR --Cursor <n> Down */
    DEFAULT(csiescseq.arg[0], 1);
    tmoveto(term.c.x, term.c.y + csiescseq.arg[0]);
    break;
  case 'i': /* MC -- Media Copy */
    switch (csiescseq.arg[0]) {
    case 0:
      tdump();
      break;
    case 1:
      tdumpline(term.c.y);
      break;
    case 2:
      tdumpsel();
      break;
    case 4:
      term.mode &= ~MODE_PRINT;
      break;
    case 5:
      term.mode |= MODE_PRINT;
      break;
    }
    break;
  case 'c': /* DA -- Device Attributes */
    if (csiescseq.arg[0] == 0)
      ttywrite(vtiden, sizeof(vtiden) - 1);
    break;
  case 'C': /* CUF -- Cursor <n> Forward */
  case 'a': /* HPR -- Cursor <n> Forward */
    DEFAULT(csiescseq.arg[0], 1);
    tmoveto(term.c.x + csiescseq.arg[0], term.c.y);
    break;
  case 'D': /* CUB -- Cursor <n> Backward */
    DEFAULT(csiescseq.arg[0], 1);
    tmoveto(term.c.x - csiescseq.arg[0], term.c.y);
    break;
  case 'E': /* CNL -- Cursor <n> Down and first col */
    DEFAULT(csiescseq.arg[0], 1);
    tmoveto(0, term.c.y + csiescseq.arg[0]);
    break;
  case 'F': /* CPL -- Cursor <n> Up and first col */
    DEFAULT(csiescseq.arg[0], 1);
    tmoveto(0, term.c.y - csiescseq.arg[0]);
    break;
  case 'g': /* TBC -- Tabulation clear */
    switch (csiescseq.arg[0]) {
    case 0: /* clear current tab stop */
      term.tabs[term.c.x] = 0;
      break;
    case 3: /* clear all the tabs */
      memset(term.tabs, 0, term.col * sizeof(*term.tabs));
      break;
    default:
      goto unknown;
    }
    break;
  case 'G': /* CHA -- Move to <col> */
  case '`': /* HPA */
    DEFAULT(csiescseq.arg[0], 1);
    tmoveto(csiescseq.arg[0] - 1, term.c.y);
    break;
  case 'H': /* CUP -- Move to <row> <col> */
  case 'f': /* HVP */
    DEFAULT(csiescseq.arg[0], 1);
    DEFAULT(csiescseq.arg[1], 1);
    tmoveato(csiescseq.arg[1] - 1, csiescseq.arg[0] - 1);
    break;
  case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
    DEFAULT(csiescseq.arg[0], 1);
    tputtab(csiescseq.arg[0]);
    break;
  case 'J': /* ED -- Clear screen */
    selclear();
    switch (csiescseq.arg[0]) {
    case 0: /* below */
      tclearregion(term.c.x, term.c.y, term.col - 1, term.c.y);
      if (term.c.y < term.row - 1) {
        tclearregion(0, term.c.y + 1, term.col - 1, term.row - 1);
      }
      break;
    case 1: /* above */
      if (term.c.y > 1)
        tclearregion(0, 0, term.col - 1, term.c.y - 1);
      tclearregion(0, term.c.y, term.c.x, term.c.y);
      break;
    case 2: /* all */
      tclearregion(0, 0, term.col - 1, term.row - 1);
      break;
    default:
      goto unknown;
    }
    break;
  case 'K': /* EL -- Clear line */
    switch (csiescseq.arg[0]) {
    case 0: /* right */
      tclearregion(term.c.x, term.c.y, term.col - 1, term.c.y);
      break;
    case 1: /* left */
      tclearregion(0, term.c.y, term.c.x, term.c.y);
      break;
    case 2: /* all */
      tclearregion(0, term.c.y, term.col - 1, term.c.y);
      break;
    }
    break;
  case 'S': /* SU -- Scroll <n> line up */
    DEFAULT(csiescseq.arg[0], 1);
    tscrollup(term.top, csiescseq.arg[0]);
    break;
  case 'T': /* SD -- Scroll <n> line down */
    DEFAULT(csiescseq.arg[0], 1);
    tscrolldown(term.top, csiescseq.arg[0]);
    break;
  case 'L': /* IL -- Insert <n> blank lines */
    DEFAULT(csiescseq.arg[0], 1);
    tinsertblankline(csiescseq.arg[0]);
    break;
  case 'l': /* RM -- Reset Mode */
    tsetmode(csiescseq.priv, 0, csiescseq.arg, csiescseq.narg);
    break;
  case 'M': /* DL -- Delete <n> lines */
    DEFAULT(csiescseq.arg[0], 1);
    tdeleteline(csiescseq.arg[0]);
    break;
  case 'X': /* ECH -- Erase <n> char */
    DEFAULT(csiescseq.arg[0], 1);
    tclearregion(term.c.x, term.c.y, term.c.x + csiescseq.arg[0] - 1, term.c.y);
    break;
  case 'P': /* DCH -- Delete <n> char */
    DEFAULT(csiescseq.arg[0], 1);
    tdeletechar(csiescseq.arg[0]);
    break;
  case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
    DEFAULT(csiescseq.arg[0], 1);
    tputtab(-csiescseq.arg[0]);
    break;
  case 'd': /* VPA -- Move to <row> */
    DEFAULT(csiescseq.arg[0], 1);
    tmoveato(term.c.x, csiescseq.arg[0] - 1);
    break;
  case 'h': /* SM -- Set terminal mode */
    tsetmode(csiescseq.priv, 1, csiescseq.arg, csiescseq.narg);
    break;
  case 'm': /* SGR -- Terminal attribute (color) */
    tsetattr(csiescseq.arg, csiescseq.narg);
    break;
  case 'n': /* DSR – Device Status Report (cursor position) */
    if (csiescseq.arg[0] == 6) {
      len =
          snprintf(buf, sizeof(buf), "\033[%i;%iR", term.c.y + 1, term.c.x + 1);
      ttywrite(buf, len);
    }
    break;
  case 'r': /* DECSTBM -- Set Scrolling Region */
    if (csiescseq.priv) {
      goto unknown;
    } else {
      DEFAULT(csiescseq.arg[0], 1);
      DEFAULT(csiescseq.arg[1], term.row);
      tsetscroll(csiescseq.arg[0] - 1, csiescseq.arg[1] - 1);
      tmoveato(0, 0);
    }
    break;
  case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
    tcursor(CURSOR_SAVE);
    break;
  case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
    tcursor(CURSOR_LOAD);
    break;
  case ' ':
    switch (csiescseq.mode[1]) {
    case 'q': /* DECSCUSR -- Set Cursor Style */
      DEFAULT(csiescseq.arg[0], 1);
      if (!BETWEEN(csiescseq.arg[0], 0, 6)) {
        goto unknown;
      }
      wl.cursor = csiescseq.arg[0];
      break;
    default:
      goto unknown;
    }
    break;
  }
}

void csidump(void) {
  int i;
  uint c;

  printf("ESC[");
  for (i = 0; i < csiescseq.len; i++) {
    c = csiescseq.buf[i] & 0xff;
    if (isprint(c)) {
      putchar(c);
    } else if (c == '\n') {
      printf("(\\n)");
    } else if (c == '\r') {
      printf("(\\r)");
    } else if (c == 0x1b) {
      printf("(\\e)");
    } else {
      printf("(%02x)", c);
    }
  }
  putchar('\n');
}

void csireset(void) { memset(&csiescseq, 0, sizeof(csiescseq)); }

void strhandle(void) {
  char *p = NULL;
  int j, narg, par;

  term.esc &= ~(ESC_STR_END | ESC_STR);
  strparse();
  par = (narg = strescseq.narg) ? atoi(strescseq.args[0]) : 0;

  switch (strescseq.type) {
  case ']': /* OSC -- Operating System Command */
    switch (par) {
    case 0:
    case 1:
    case 2:
      if (narg > 1)
        wlsettitle(strescseq.args[1]);
      return;
    case 4: /* color set */
      if (narg < 3)
        break;
      p = strescseq.args[2];
      /* FALLTHROUGH */
    case 104: /* color reset, here p = NULL */
      j = (narg > 1) ? atoi(strescseq.args[1]) : -1;
      if (wlsetcolorname(j, p)) {
        fprintf(stderr, "erresc: invalid color %s\n", p);
      } else {
        /*
         * TODO if defaultbg color is changed, borders
         * are dirty
         */
        redraw();
      }
      return;
    }
    break;
  case 'k': /* old title set compatibility */
    wlsettitle(strescseq.args[0]);
    return;
  case 'P': /* DCS -- Device Control String */
  case '_': /* APC -- Application Program Command */
  case '^': /* PM -- Privacy Message */
    return;
  }

  fprintf(stderr, "erresc: unknown str ");
  strdump();
}

void strparse(void) {
  int c;
  char *p = strescseq.buf;

  strescseq.narg = 0;
  strescseq.buf[strescseq.len] = '\0';

  if (*p == '\0')
    return;

  while (strescseq.narg < STR_ARG_SIZ) {
    strescseq.args[strescseq.narg++] = p;
    while ((c = *p) != ';' && c != '\0')
      ++p;
    if (c == '\0')
      return;
    *p++ = '\0';
  }
}

void strdump(void) {
  int i;
  uint c;

  printf("ESC%c", strescseq.type);
  for (i = 0; i < strescseq.len; i++) {
    c = strescseq.buf[i] & 0xff;
    if (c == '\0') {
      return;
    } else if (isprint(c)) {
      putchar(c);
    } else if (c == '\n') {
      printf("(\\n)");
    } else if (c == '\r') {
      printf("(\\r)");
    } else if (c == 0x1b) {
      printf("(\\e)");
    } else {
      printf("(%02x)", c);
    }
  }
  printf("ESC\\\n");
}

void strreset(void) { memset(&strescseq, 0, sizeof(strescseq)); }

void tprinter(char *s, size_t len) {
  if (iofd != -1 && xwrite(iofd, s, len) < 0) {
    fprintf(stderr, "Error writing in %s:%s\n", opt_io, strerror(errno));
    close(iofd);
    iofd = -1;
  }
}

void toggleprinter(const Arg *arg) { term.mode ^= MODE_PRINT; }

void printscreen(const Arg *arg) { tdump(); }

void printsel(const Arg *arg) { tdumpsel(); }

void tdumpsel(void) {
  char *ptr;

  if ((ptr = getsel())) {
    tprinter(ptr, strlen(ptr));
    free(ptr);
  }
}

void tdumpline(int n) {
  char buf[UTF_SIZ];
  Glyph *bp, *end;

  bp = &term.line[n][0];
  end = &bp[MIN(tlinelen(n), term.col) - 1];
  if (bp != end || bp->u != ' ') {
    for (; bp <= end; ++bp)
      tprinter(buf, utf8encode(bp->u, buf));
  }
  tprinter("\n", 1);
}

void tdump(void) {
  int i;

  for (i = 0; i < term.row; ++i)
    tdumpline(i);
}

void tputtab(int n) {
  uint x = term.c.x;

  if (n > 0) {
    while (x < term.col && n--)
      for (++x; x < term.col && !term.tabs[x]; ++x)
        /* nothing */;
  } else if (n < 0) {
    while (x > 0 && n++)
      for (--x; x > 0 && !term.tabs[x]; --x)
        /* nothing */;
  }
  term.c.x = LIMIT(x, 0, term.col - 1);
}

void techo(Rune u) {
  if (ISCONTROL(u)) { /* control code */
    if (u & 0x80) {
      u &= 0x7f;
      tputc('^');
      tputc('[');
    } else if (u != '\n' && u != '\r' && u != '\t') {
      u ^= 0x40;
      tputc('^');
    }
  }
  tputc(u);
  needdraw = true;
}

void tdeftran(char ascii) {
  static char cs[] = "0B";
  static int vcs[] = {CS_GRAPHIC0, CS_USA};
  char *p;

  if ((p = strchr(cs, ascii)) == NULL) {
    fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
  } else {
    term.trantbl[term.icharset] = vcs[p - cs];
  }
}

void tdectest(char c) {
  int x, y;

  if (c == '8') { /* DEC screen alignment test. */
    for (x = 0; x < term.col; ++x) {
      for (y = 0; y < term.row; ++y)
        tsetchar('E', &term.c.attr, x, y);
    }
  }
}

void tstrsequence(uchar c) {
  switch (c) {
  case 0x90: /* DCS -- Device Control String */
    c = 'P';
    break;
  case 0x9f: /* APC -- Application Program Command */
    c = '_';
    break;
  case 0x9e: /* PM -- Privacy Message */
    c = '^';
    break;
  case 0x9d: /* OSC -- Operating System Command */
    c = ']';
    break;
  }
  strreset();
  strescseq.type = c;
  term.esc |= ESC_STR;
}

void tcontrolcode(uchar ascii) {
  switch (ascii) {
  case '\t': /* HT */
    tputtab(1);
    return;
  case '\b': /* BS */
    tmoveto(term.c.x - 1, term.c.y);
    return;
  case '\r': /* CR */
    tmoveto(0, term.c.y);
    return;
  case '\f': /* LF */
  case '\v': /* VT */
  case '\n': /* LF */
    /* go to first col if the mode is set */
    tnewline(IS_SET(MODE_CRLF));
    return;
  case '\a': /* BEL */
    if (term.esc & ESC_STR_END) {
      /* backwards compatibility to xterm */
      strhandle();
    } else {
      if (!(wl.state & WIN_FOCUSED))
        wlseturgency(1);
      /* XXX: No bell on wayland
       * if (bellvolume)
       *     XkbBell(xw.dpy, xw.win, bellvolume, (Atom)NULL);
       */
    }
    break;
  case '\033': /* ESC */
    csireset();
    term.esc &= ~(ESC_CSI | ESC_ALTCHARSET | ESC_TEST);
    term.esc |= ESC_START;
    return;
  case '\016': /* SO (LS1 -- Locking shift 1) */
  case '\017': /* SI (LS0 -- Locking shift 0) */
    term.charset = 1 - (ascii - '\016');
    return;
  case '\032': /* SUB */
    tsetchar('?', &term.c.attr, term.c.x, term.c.y);
  case '\030': /* CAN */
    csireset();
    break;
  case '\005': /* ENQ (IGNORED) */
  case '\000': /* NUL (IGNORED) */
  case '\021': /* XON (IGNORED) */
  case '\023': /* XOFF (IGNORED) */
  case 0177:   /* DEL (IGNORED) */
    return;
  case 0x84: /* TODO: IND */
    break;
  case 0x85:     /* NEL -- Next line */
    tnewline(1); /* always go to first col */
    break;
  case 0x88: /* HTS -- Horizontal tab stop */
    term.tabs[term.c.x] = 1;
    break;
  case 0x8d: /* TODO: RI */
  case 0x8e: /* TODO: SS2 */
  case 0x8f: /* TODO: SS3 */
  case 0x98: /* TODO: SOS */
    break;
  case 0x9a: /* DECID -- Identify Terminal */
    ttywrite(vtiden, sizeof(vtiden) - 1);
    break;
  case 0x9b: /* TODO: CSI */
  case 0x9c: /* TODO: ST */
    break;
  case 0x90: /* DCS -- Device Control String */
  case 0x9f: /* APC -- Application Program Command */
  case 0x9e: /* PM -- Privacy Message */
  case 0x9d: /* OSC -- Operating System Command */
    tstrsequence(ascii);
    return;
  }
  /* only CAN, SUB, \a and C1 chars interrupt a sequence */
  term.esc &= ~(ESC_STR_END | ESC_STR);
}

/*
 * returns 1 when the sequence is finished and it hasn't to read
 * more characters for this sequence, otherwise 0
 */
int eschandle(uchar ascii) {
  switch (ascii) {
  case '[':
    term.esc |= ESC_CSI;
    return 0;
  case '#':
    term.esc |= ESC_TEST;
    return 0;
  case 'P': /* DCS -- Device Control String */
  case '_': /* APC -- Application Program Command */
  case '^': /* PM -- Privacy Message */
  case ']': /* OSC -- Operating System Command */
  case 'k': /* old title set compatibility */
    tstrsequence(ascii);
    return 0;
  case 'n': /* LS2 -- Locking shift 2 */
  case 'o': /* LS3 -- Locking shift 3 */
    term.charset = 2 + (ascii - 'n');
    break;
  case '(': /* GZD4 -- set primary charset G0 */
  case ')': /* G1D4 -- set secondary charset G1 */
  case '*': /* G2D4 -- set tertiary charset G2 */
  case '+': /* G3D4 -- set quaternary charset G3 */
    term.icharset = ascii - '(';
    term.esc |= ESC_ALTCHARSET;
    return 0;
  case 'D': /* IND -- Linefeed */
    if (term.c.y == term.bot) {
      tscrollup(term.top, 1);
    } else {
      tmoveto(term.c.x, term.c.y + 1);
    }
    break;
  case 'E':      /* NEL -- Next line */
    tnewline(1); /* always go to first col */
    break;
  case 'H': /* HTS -- Horizontal tab stop */
    term.tabs[term.c.x] = 1;
    break;
  case 'M': /* RI -- Reverse index */
    if (term.c.y == term.top) {
      tscrolldown(term.top, 1);
    } else {
      tmoveto(term.c.x, term.c.y - 1);
    }
    break;
  case 'Z': /* DECID -- Identify Terminal */
    ttywrite(vtiden, sizeof(vtiden) - 1);
    break;
  case 'c': /* RIS -- Reset to inital state */
    treset();
    wlresettitle();
    wlloadcols();
    break;
  case '=': /* DECPAM -- Application keypad */
    term.mode |= MODE_APPKEYPAD;
    break;
  case '>': /* DECPNM -- Normal keypad */
    term.mode &= ~MODE_APPKEYPAD;
    break;
  case '7': /* DECSC -- Save Cursor */
    tcursor(CURSOR_SAVE);
    break;
  case '8': /* DECRC -- Restore Cursor */
    tcursor(CURSOR_LOAD);
    break;
  case '\\': /* ST -- String Terminator */
    if (term.esc & ESC_STR_END)
      strhandle();
    break;
  default:
    fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n", (uchar)ascii,
            isprint(ascii) ? ascii : '.');
    break;
  }
  return 1;
}

void tputc(Rune u) {
  char c[UTF_SIZ];
  int control;
  int width, len;
  Glyph *gp;

  control = ISCONTROL(u);
  len = utf8encode(u, c);
  if (!control && (width = wcwidth(u)) == -1) {
    memcpy(c, "\357\277\275", 4); /* UTF_INVALID */
    width = 1;
  }

  if (IS_SET(MODE_PRINT))
    tprinter(c, len);

  /*
   * STR sequence must be checked before anything else
   * because it uses all following characters until it
   * receives a ESC, a SUB, a ST or any other C1 control
   * character.
   */
  if (term.esc & ESC_STR) {
    if (u == '\a' || u == 030 || u == 032 || u == 033 || ISCONTROLC1(u)) {
      term.esc &= ~(ESC_START | ESC_STR);
      term.esc |= ESC_STR_END;
    } else if (strescseq.len + len < sizeof(strescseq.buf) - 1) {
      memmove(&strescseq.buf[strescseq.len], c, len);
      strescseq.len += len;
      return;
    } else {
      /*
       * Here is a bug in terminals. If the user never sends
       * some code to stop the str or esc command, then wterm
       * will stop responding. But this is better than
       * silently failing with unknown characters. At least
       * then users will report back.
       *
       * In the case users ever get fixed, here is the code:
       */
      /*
       * term.esc = 0;
       * strhandle();
       */
      return;
    }
  }

  /*
   * Actions of control codes must be performed as soon they arrive
   * because they can be embedded inside a control sequence, and
   * they must not cause conflicts with sequences.
   */
  if (control) {
    tcontrolcode(u);
    /*
     * control codes are not shown ever
     */
    return;
  } else if (term.esc & ESC_START) {
    if (term.esc & ESC_CSI) {
      csiescseq.buf[csiescseq.len++] = u;
      if (BETWEEN(u, 0x40, 0x7E) ||
          csiescseq.len >= sizeof(csiescseq.buf) - 1) {
        term.esc = 0;
        csiparse();
        csihandle();
      }
      return;
    } else if (term.esc & ESC_ALTCHARSET) {
      tdeftran(u);
    } else if (term.esc & ESC_TEST) {
      tdectest(u);
    } else {
      if (!eschandle(u))
        return;
      /* sequence already finished */
    }
    term.esc = 0;
    /*
     * All characters which form part of a sequence are not
     * printed
     */
    return;
  }
  if (sel.ob.x != -1 && BETWEEN(term.c.y, sel.ob.y, sel.oe.y))
    selclear();

  gp = &term.line[term.c.y][term.c.x];
  if (IS_SET(MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT)) {
    gp->mode |= ATTR_WRAP;
    tnewline(1);
    gp = &term.line[term.c.y][term.c.x];
  }

  if (IS_SET(MODE_INSERT) && term.c.x + width < term.col)
    memmove(gp + width, gp, (term.col - term.c.x - width) * sizeof(Glyph));

  if (term.c.x + width > term.col) {
    tnewline(1);
    gp = &term.line[term.c.y][term.c.x];
  }

  tsetchar(u, &term.c.attr, term.c.x, term.c.y);

  if (width == 2) {
    gp->mode |= ATTR_WIDE;
    if (term.c.x + 1 < term.col) {
      gp[1].u = '\0';
      gp[1].mode = ATTR_WDUMMY;
    }
  }
  if (term.c.x + width < term.col) {
    tmoveto(term.c.x + width, term.c.y);
  } else {
    term.c.state |= CURSOR_WRAPNEXT;
  }
}

void tresize(int col, int row) {
  int i;
  int minrow = MIN(row, term.row);
  int mincol = MIN(col, term.col);
  int *bp;
  TCursor c;

  if (col < 1 || row < 1) {
    fprintf(stderr, "tresize: error resizing to %dx%d\n", col, row);
    return;
  }

  /*
   * slide screen to keep cursor where we expect it -
   * tscrollup would work here, but we can optimize to
   * memmove because we're freeing the earlier lines
   */
  for (i = 0; i <= term.c.y - row; i++) {
    free(term.line[i]);
    free(term.alt[i]);
  }
  /* ensure that both src and dst are not NULL */
  if (i > 0) {
    memmove(term.line, term.line + i, row * sizeof(Line));
    memmove(term.alt, term.alt + i, row * sizeof(Line));
  }
  for (i += row; i < term.row; i++) {
    free(term.line[i]);
    free(term.alt[i]);
  }

  /* resize to new height */
  term.line = xrealloc(term.line, row * sizeof(Line));
  term.alt = xrealloc(term.alt, row * sizeof(Line));
  term.dirty = xrealloc(term.dirty, row * sizeof(*term.dirty));
  term.tabs = xrealloc(term.tabs, col * sizeof(*term.tabs));

  /* resize each row to new width, zero-pad if needed */
  for (i = 0; i < minrow; i++) {
    term.line[i] = xrealloc(term.line[i], col * sizeof(Glyph));
    term.alt[i] = xrealloc(term.alt[i], col * sizeof(Glyph));
  }

  /* allocate any new rows */
  for (/* i == minrow */; i < row; i++) {
    term.line[i] = xmalloc(col * sizeof(Glyph));
    term.alt[i] = xmalloc(col * sizeof(Glyph));
  }
  if (col > term.col) {
    bp = term.tabs + term.col;

    memset(bp, 0, sizeof(*term.tabs) * (col - term.col));
    while (--bp > term.tabs && !*bp)
      /* nothing */;
    for (bp += tabspaces; bp < term.tabs + col; bp += tabspaces)
      *bp = 1;
  }
  /* update terminal size */
  term.col = col;
  term.row = row;
  /* reset scrolling region */
  tsetscroll(0, row - 1);
  /* make use of the LIMIT in tmoveto */
  tmoveto(term.c.x, term.c.y);
  /* Clearing both screens (it makes dirty all lines) */
  c = term.c;
  for (i = 0; i < 2; i++) {
    if (mincol < col && 0 < minrow) {
      tclearregion(mincol, 0, col - 1, minrow - 1);
    }
    if (0 < col && minrow < row) {
      tclearregion(0, minrow, col - 1, row - 1);
    }
    tswapscreen();
    tcursor(CURSOR_LOAD);
  }
  term.c = c;
}

void wlresize(int col, int row) {
  union wld_object object;

  wl.tw = MAX(1, col * wl.cw);
  wl.th = MAX(1, row * wl.ch);

  wld.oldbuffer = wld.buffer;
  wld.buffer = wld_create_buffer(wld.ctx, wl.w, wl.h, WLD_FORMAT_ARGB8888, 0);

  if (!wld.buffer)
    die("failed to create buffer");
  wld_export(wld.buffer, WLD_WAYLAND_OBJECT_BUFFER, &object);
  wl.buffer = object.ptr;
  if (wld.oldbuffer) {
    wld_buffer_unreference(wld.oldbuffer);
    wld.oldbuffer = 0;
  }
}

uchar sixd_to_8bit(int x) { return x == 0 ? 0 : 0x37 + 0x28 * x; }

int wlloadcolor(int i, const char *name, uint32_t *color) {
  if (!name) {
    if (BETWEEN(i, 16, 255)) {  /* 256 color */
      if (i < 6 * 6 * 6 + 16) { /* same colors as xterm */
        *color = 0xff << 24 | sixd_to_8bit(((i - 16) / 36) % 6) << 16 |
                 sixd_to_8bit(((i - 16) / 6) % 6) << 8 |
                 sixd_to_8bit(((i - 16) / 1) % 6);
      } else { /* greyscale */
        *color = 0xff << 24 | (0x8 + 0xa * (i - (6 * 6 * 6 + 16))) * 0x10101;
      }
      return true;
    } else
      name = colorname[i];
  }

  return wld_lookup_named_color(name, color);
}

void wlloadcols(void) {
  int i;

  for (i = 0; i < LEN(dc.col); i++)
    if (!wlloadcolor(i, NULL, &dc.col[i])) {
      if (colorname[i])
        die("Could not allocate color '%s'\n", colorname[i]);
      else
        die("Could not allocate color %d\n", i);
    }
}

int wlsetcolorname(int x, const char *name) {
  uint32_t color;

  if (!BETWEEN(x, 0, LEN(dc.col)))
    return 1;

  if (!wlloadcolor(x, name, &color))
    return 1;

  dc.col[x] = color;

  return 0;
}

static void wlloadcursor(void) {
  char *names[] = {mouseshape, "xterm", "ibeam", "text"};
  int i;

  cursor.theme = wl_cursor_theme_load(NULL, 32, wl.shm);

  for (i = 0; !cursor.cursor && i < LEN(names); i++)
    cursor.cursor = wl_cursor_theme_get_cursor(cursor.theme, names[i]);

  cursor.surface = wl_compositor_create_surface(wl.cmp);
}

void wltermclear(int col1, int row1, int col2, int row2) {
  uint32_t color = dc.col[IS_SET(MODE_REVERSE) ? defaultfg : defaultbg];
  color = (color & term_alpha << 24) | (color & 0x00FFFFFF);
  wld_fill_rectangle(wld.renderer, color, borderpx + col1 * wl.cw,
                     borderpx + row1 * wl.ch, (col2 - col1 + 1) * wl.cw,
                     (row2 - row1 + 1) * wl.ch);
}

/*
 * Absolute coordinates.
 */
void wlclear(int x1, int y1, int x2, int y2) {
  uint32_t color = dc.col[IS_SET(MODE_REVERSE) ? defaultfg : defaultbg];
  color = (color & term_alpha << 24) | (color & 0x00FFFFFF);
  wld_fill_rectangle(wld.renderer, color, x1, y1, x2 - x1, y2 - y1);
}

int wlloadfont(Font *f, FcPattern *pattern) {
  FcPattern *match;
  FcResult result;

  match = FcFontMatch(NULL, pattern, &result);
  if (!match)
    return 1;

  if (!(f->match = wld_font_open_pattern(wld.fontctx, match))) {
    FcPatternDestroy(match);
    return 1;
  }

  f->set = NULL;
  f->pattern = FcPatternDuplicate(pattern);

  f->ascent = f->match->ascent;
  f->descent = f->match->descent;
  f->lbearing = 0;
  f->rbearing = f->match->max_advance;

  f->height = f->ascent + f->descent;
  f->width = f->lbearing + f->rbearing;

  return 0;
}

void wlloadfonts(char *fontstr, double fontsize) {
  FcPattern *pattern;
  double fontval;
  float ceilf(float);

  if (fontstr[0] == '-') {
    /* XXX: need XftXlfdParse equivalent */
    pattern = NULL;
  } else {
    pattern = FcNameParse((FcChar8 *)fontstr);
  }

  if (!pattern)
    die("%s: can't open font %s\n", argv0, fontstr);

  if (fontsize > 1) {
    FcPatternDel(pattern, FC_PIXEL_SIZE);
    FcPatternDel(pattern, FC_SIZE);
    FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)fontsize);
    usedfontsize = fontsize;
  } else {
    if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval) ==
        FcResultMatch) {
      usedfontsize = fontval;
    } else if (FcPatternGetDouble(pattern, FC_SIZE, 0, &fontval) ==
               FcResultMatch) {
      usedfontsize = -1;
    } else {
      /*
       * Default font size is 12, if none given. This is to
       * have a known usedfontsize value.
       */
      FcPatternAddDouble(pattern, FC_PIXEL_SIZE, 12);
      usedfontsize = 12;
    }
    defaultfontsize = usedfontsize;
  }

  FcConfigSubstitute(0, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);

  if (wlloadfont(&dc.font, pattern))
    die("%s: can't open font %s\n", argv0, fontstr);

  if (usedfontsize < 0) {
    FcPatternGetDouble(dc.font.pattern, FC_PIXEL_SIZE, 0, &fontval);
    usedfontsize = fontval;
    if (fontsize == 0)
      defaultfontsize = fontval;
  }

  /* Setting character width and height. */
  wl.cw = ceilf(dc.font.width * cwscale);
  wl.ch = ceilf(dc.font.height * chscale);

  FcPatternDel(pattern, FC_SLANT);
  FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
  if (wlloadfont(&dc.ifont, pattern))
    die("%s: can't open font %s\n", argv0, fontstr);

  FcPatternDel(pattern, FC_WEIGHT);
  FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
  if (wlloadfont(&dc.ibfont, pattern))
    die("%s: can't open font %s\n", argv0, fontstr);

  FcPatternDel(pattern, FC_SLANT);
  FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
  if (wlloadfont(&dc.bfont, pattern))
    die("%s: can't open font %s\n", argv0, fontstr);

  FcPatternDestroy(pattern);
}

void wlunloadfont(Font *f) {
  wld_font_close(f->match);
  FcPatternDestroy(f->pattern);
  if (f->set)
    FcFontSetDestroy(f->set);
}

void wlunloadfonts(void) {
  /* Free the loaded fonts in the font cache.  */
  while (frclen > 0)
    wld_font_close(frc[--frclen].font);

  wlunloadfont(&dc.font);
  wlunloadfont(&dc.bfont);
  wlunloadfont(&dc.ifont);
  wlunloadfont(&dc.ibfont);
}

void wlzoom(const Arg *arg) {
  Arg larg;

  larg.f = usedfontsize + arg->f;
  wlzoomabs(&larg);
}

void wlzoomabs(const Arg *arg) {
  wlunloadfonts();
  wlloadfonts(usedfont, arg->f);
  cresize(0, 0);
  redraw();
  /* XXX: Should the window size be updated here because wayland doesn't
   * have a notion of hints?
   * xhints();
   */
}

void wlzoomreset(const Arg *arg) {
  Arg larg;

  if (defaultfontsize > 0) {
    larg.f = defaultfontsize;
    wlzoomabs(&larg);
  }
}

void wlinit(void) {
  struct wl_registry *registry;

  if (!(wl.dpy = wl_display_connect(NULL)))
    die("Can't open display\n");

  registry = wl_display_get_registry(wl.dpy);
  wl_registry_add_listener(registry, &reglistener, NULL);

  wl_display_dispatch(wl.dpy);
  wl_display_roundtrip(wl.dpy);

  wld.ctx = wld_wayland_create_context(wl.dpy, WLD_ANY);
  if (!wld.ctx)
    die("Can't create wayland context\n");
  wld.renderer = wld_create_renderer(wld.ctx);
  if (!wld.ctx)
    die("Can't create renderer\n");
  if (!wl.shm)
    die("Display has no SHM\n");
  if (!wl.seat)
    die("Display has no seat\n");
  if (!wl.datadevmanager)
    die("Display has no data device manager\n");

  wl.xkb.ctx = xkb_context_new(0);

  wl_display_roundtrip(wl.dpy);
  wl.keyboard = wl_seat_get_keyboard(wl.seat);
  if (!wl.keyboard)
    die("Display has no keyboard\n");
  wl_keyboard_add_listener(wl.keyboard, &kbdlistener, NULL);
  wl.pointer = wl_seat_get_pointer(wl.seat);
  wl_pointer_add_listener(wl.pointer, &ptrlistener, NULL);
  wl.datadev =
      wl_data_device_manager_get_data_device(wl.datadevmanager, wl.seat);
  wl_data_device_add_listener(wl.datadev, &datadevlistener, NULL);

  /* font */
  if (!FcInit())
    die("Could not init fontconfig.\n");

  usedfont = (opt_font == NULL) ? font : opt_font;
  wld.fontctx = wld_font_create_context();
  wlloadfonts(usedfont, 0);

  wlloadcols();
  wlloadcursor();
  wl.vis = 0;
  wl.h = 2 * borderpx + term.row * wl.ch;
  wl.w = 2 * borderpx + term.col * wl.cw;

  wl.surface = wl_compositor_create_surface(wl.cmp);
  wl_surface_add_listener(wl.surface, &surflistener, NULL);
  if (wl.xdgshell) {
    xdg_shell_use_unstable_version(wl.xdgshell, XDG_SHELL_VERSION_CURRENT);
    wl.xdgsurface = xdg_shell_get_xdg_surface(wl.xdgshell, wl.surface);
    if (wl.xdgsurface) {
      xdg_shell_add_listener(wl.xdgshell, &shell_listener, NULL);
      xdg_surface_add_listener(wl.xdgsurface, &xdgsurflistener, NULL);
    } else
      die("failed to get xdgsurface");
  } else if (wl.xdgshell_v6) {
    wl.xdgsurface_v6 =
        zxdg_shell_v6_get_xdg_surface(wl.xdgshell_v6, wl.surface);
    zxdg_surface_v6_add_listener(wl.xdgsurface_v6, &surf_v6_listener, NULL);
    wl.xdgtoplevel = zxdg_surface_v6_get_toplevel(wl.xdgsurface_v6);
    zxdg_toplevel_v6_add_listener(wl.xdgtoplevel, &xdgtoplevellistener, NULL);
  } else
    die("no wayland shell");
  wl_surface_commit(wl.surface);
  wlresettitle();
}

/*
 * TODO: Implement something like XftDrawGlyphFontSpec in wld, and then apply a
 * similar patch to ae1923d27533ff46400d93765e971558201ca1ee
 */

void wldraws(char *s, Glyph base, int x, int y, int charlen, int bytelen) {
  int winx = borderpx + x * wl.cw, winy = borderpx + y * wl.ch,
      width = charlen * wl.cw, xp, i;
  int frcflags, charexists;
  int u8fl, u8fblen, u8cblen, doesexist;
  char *u8c, *u8fs;
  Rune unicodep;
  Font *font = &dc.font;
  FcResult fcres;
  FcPattern *fcpattern, *fontpattern;
  FcFontSet *fcsets[] = {NULL};
  FcCharSet *fccharset;
  uint32_t fg, bg, temp;
  int oneatatime;

  frcflags = FRC_NORMAL;

  if (base.mode & ATTR_ITALIC) {
    if (base.fg == defaultfg)
      base.fg = defaultitalic;
    font = &dc.ifont;
    frcflags = FRC_ITALIC;
  } else if ((base.mode & ATTR_ITALIC) && (base.mode & ATTR_BOLD)) {
    if (base.fg == defaultfg)
      base.fg = defaultitalic;
    font = &dc.ibfont;
    frcflags = FRC_ITALICBOLD;
  } else if (base.mode & ATTR_UNDERLINE) {
    if (base.fg == defaultfg)
      base.fg = defaultunderline;
  }

  if (IS_TRUECOL(base.fg)) {
    fg = base.fg;
  } else {
    fg = dc.col[base.fg];
  }

  if (IS_TRUECOL(base.bg)) {
    bg = base.bg | 0xff000000;
  } else {
    bg = dc.col[base.bg];
  }

  if (base.mode & ATTR_BOLD) {
    /*
     * change basic system colors [0-7]
     * to bright system colors [8-15]
     */
    if (BETWEEN(base.fg, 0, 7) && !(base.mode & ATTR_FAINT))
      fg = dc.col[base.fg + 8];

    if (base.mode & ATTR_ITALIC) {
      font = &dc.ibfont;
      frcflags = FRC_ITALICBOLD;
    } else {
      font = &dc.bfont;
      frcflags = FRC_BOLD;
    }
  }

  if (IS_SET(MODE_REVERSE)) {
    if (fg == dc.col[defaultfg]) {
      fg = dc.col[defaultbg];
    } else {
      fg = ~(fg & 0xffffff);
    }

    if (bg == dc.col[defaultbg]) {
      bg = dc.col[defaultfg];
    } else {
      bg = ~(bg & 0xffffff);
    }
  }

  if (base.mode & ATTR_REVERSE) {
    temp = fg;
    fg = bg;
    bg = temp;
  }

  if (base.mode & ATTR_FAINT && !(base.mode & ATTR_BOLD)) {
    fg = (fg & (0xff << 24)) | ((((fg >> 16) & 0xff) / 2) << 16) |
         ((((fg >> 8) & 0xff) / 2) << 8) | ((fg & 0xff) / 2);
  }

  if (base.mode & ATTR_BLINK && term.mode & MODE_BLINK)
    fg = bg;

  if (base.mode & ATTR_INVISIBLE)
    fg = bg;

  /* Intelligent cleaning up of the borders. */
  if (x == 0) {
    wlclear(0, (y == 0) ? 0 : winy, borderpx,
            ((y >= term.row - 1) ? wl.h : (winy + wl.ch)));
  }
  if (x + charlen >= term.col) {
    wlclear(winx + width, (y == 0) ? 0 : winy, wl.w,
            ((y >= term.row - 1) ? wl.h : (winy + wl.ch)));
  }
  if (y == 0)
    wlclear(winx, 0, winx + width, borderpx);
  if (y == term.row - 1)
    wlclear(winx, winy + wl.ch, winx + width, wl.h);

  /* Clean up the region we want to draw to. */
  wld_fill_rectangle(wld.renderer,
                     (bg & (term_alpha << 24)) | (bg & 0x00FFFFFF), winx, winy,
                     width, wl.ch);
  for (xp = winx; bytelen > 0;) {
    /*
     * Search for the range in the to be printed string of glyphs
     * that are in the main font. Then print that range. If
     * some glyph is found that is not in the font, do the
     * fallback dance.
     */
    u8fs = s;
    u8fblen = 0;
    u8fl = 0;
    oneatatime = font->width != wl.cw;
    for (;;) {
      u8c = s;
      u8cblen = utf8decode(s, &unicodep, UTF_SIZ);
      s += u8cblen;
      bytelen -= u8cblen;

      doesexist = wld_font_ensure_char(font->match, unicodep);
      if (doesexist) {
        u8fl++;
        u8fblen += u8cblen;
        if (!oneatatime && bytelen > 0)
          continue;
      }

      if (u8fl > 0) {
        wld_draw_text(wld.renderer, font->match, fg, xp, winy + font->ascent,
                      u8fs, u8fblen, NULL);
        xp += wl.cw * u8fl;
      }
      break;
    }
    if (doesexist) {
      if (oneatatime)
        continue;
      break;
    }

    /* Search the font cache. */
    for (i = 0; i < frclen; i++) {
      charexists = wld_font_ensure_char(frc[i].font, unicodep);
      /* Everything correct. */
      if (charexists && frc[i].flags == frcflags)
        break;
      /* We got a default font for a not found glyph. */
      if (!charexists && frc[i].flags == frcflags &&
          frc[i].unicodep == unicodep) {
        break;
      }
    }

    /* Nothing was found. */
    if (i >= frclen) {
      if (!font->set)
        font->set = FcFontSort(0, font->pattern, 1, 0, &fcres);
      fcsets[0] = font->set;

      /*
       * Nothing was found in the cache. Now use
       * some dozen of Fontconfig calls to get the
       * font for one single character.
       *
       * Xft and fontconfig are design failures.
       */
      fcpattern = FcPatternDuplicate(font->pattern);
      fccharset = FcCharSetCreate();

      FcCharSetAddChar(fccharset, unicodep);
      FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
      FcPatternAddBool(fcpattern, FC_SCALABLE, 1);

      FcConfigSubstitute(0, fcpattern, FcMatchPattern);
      FcDefaultSubstitute(fcpattern);

      fontpattern = FcFontSetMatch(0, fcsets, 1, fcpattern, &fcres);

      /*
       * Overwrite or create the new cache entry.
       */
      if (frclen >= LEN(frc)) {
        frclen = LEN(frc) - 1;
        wld_font_close(frc[frclen].font);
        frc[frclen].unicodep = 0;
      }

      frc[frclen].font = wld_font_open_pattern(wld.fontctx, fontpattern);
      frc[frclen].flags = frcflags;
      frc[frclen].unicodep = unicodep;

      i = frclen;
      frclen++;

      FcPatternDestroy(fcpattern);
      FcCharSetDestroy(fccharset);
    }

    wld_draw_text(wld.renderer, frc[i].font, fg, xp, winy + frc[i].font->ascent,
                  u8c, u8cblen, NULL);

    xp += wl.cw * wcwidth(unicodep);
  }

  if (base.mode & ATTR_UNDERLINE) {
    wld_fill_rectangle(wld.renderer, fg, winx, winy + font->ascent + 1, width,
                       1);
  }

  if (base.mode & ATTR_STRUCK) {
    wld_fill_rectangle(wld.renderer, fg, winx, winy + 2 * font->ascent / 3,
                       width, 1);
  }
}

void wldrawglyph(Glyph g, int x, int y) {
  static char buf[UTF_SIZ];
  size_t len = utf8encode(g.u, buf);
  int width = g.mode & ATTR_WIDE ? 2 : 1;

  wldraws(buf, g, x, y, width, len);
}

void wldrawcursor(void) {
  static int oldx = 0, oldy = 0;
  int curx;
  Glyph g = {' ', ATTR_NULL, defaultbg, defaultcs};

  LIMIT(oldx, 0, term.col - 1);
  LIMIT(oldy, 0, term.row - 1);

  curx = term.c.x;

  /* adjust position if in dummy */
  if (term.line[oldy][oldx].mode & ATTR_WDUMMY)
    oldx--;
  if (term.line[term.c.y][curx].mode & ATTR_WDUMMY)
    curx--;

  g.u = term.line[term.c.y][term.c.x].u;

  /* remove the old cursor */
  wldrawglyph(term.line[oldy][oldx], oldx, oldy);
  if (oldx != curx || oldy != term.c.y) {
    wl_surface_damage(wl.surface, borderpx + oldx * wl.cw,
                      borderpx + oldy * wl.ch, wl.cw, wl.ch);
  }

  if (IS_SET(MODE_HIDE))
    return;
  uint32_t cs = dc.col[defaultcs] & (term_alpha << 24);
  /* draw the new one */
  if (wl.state & WIN_FOCUSED) {
    switch (wl.cursor) {
    case 0: /* Blinking Block */
    case 1: /* Blinking Block (Default) */
    case 2: /* Steady Block */
      if (IS_SET(MODE_REVERSE)) {
        g.mode |= ATTR_REVERSE;
        g.fg = defaultcs;
        g.bg = defaultfg;
      }

      g.mode |= term.line[term.c.y][curx].mode & ATTR_WIDE;
      wldrawglyph(g, term.c.x, term.c.y);
      break;
    case 3: /* Blinking Underline */
    case 4: /* Steady Underline */
      wld_fill_rectangle(wld.renderer, cs, borderpx + curx * wl.cw,
                         borderpx + (term.c.y + 1) * wl.ch - cursorthickness,
                         wl.cw, cursorthickness);
      break;
    case 5: /* Blinking bar */
    case 6: /* Steady bar */
      wld_fill_rectangle(wld.renderer, cs, borderpx + curx * wl.cw,
                         borderpx + term.c.y * wl.ch, cursorthickness, wl.ch);
      break;
    }
  } else {
    wld_fill_rectangle(wld.renderer, cs, borderpx + curx * wl.cw,
                       borderpx + term.c.y * wl.ch, wl.cw - 1, 1);
    wld_fill_rectangle(wld.renderer, cs, borderpx + curx * wl.cw,
                       borderpx + term.c.y * wl.ch, 1, wl.ch - 1);
    wld_fill_rectangle(wld.renderer, cs, borderpx + (curx + 1) * wl.cw - 1,
                       borderpx + term.c.y * wl.ch, 1, wl.ch - 1);
    wld_fill_rectangle(wld.renderer, cs, borderpx + curx * wl.cw,
                       borderpx + (term.c.y + 1) * wl.ch - 1, wl.cw, 1);
  }
  wl_surface_damage(wl.surface, borderpx + curx * wl.cw,
                    borderpx + term.c.y * wl.ch, wl.cw, wl.ch);
  oldx = curx, oldy = term.c.y;
}

void wlsettitle(char *title) {
  if (wl.xdgsurface)
    xdg_surface_set_title(wl.xdgsurface, title);
  else if (wl.xdgtoplevel)
    zxdg_toplevel_v6_set_title(wl.xdgtoplevel, title);
  else if (wl.shellsurf)
    wl_shell_surface_set_title(wl.shellsurf, title);
}

void wlresettitle(void) { wlsettitle(opt_title ? opt_title : "wterm"); }

void redraw(void) { tfulldirt(); }

void draw(void) {
  int y, y0;

  for (y = 0; y <= term.bot; ++y) {
    if (!term.dirty[y])
      continue;
    for (y0 = y; y <= term.bot && term.dirty[y]; ++y)
      ;
    wl_surface_damage(wl.surface, 0, borderpx + y0 * wl.ch, wl.w,
                      (y - y0) * wl.ch);
  }

  wld_set_target_buffer(wld.renderer, wld.buffer);
  drawregion(0, 0, term.col, term.row);
  wl.framecb = wl_surface_frame(wl.surface);
  wl_callback_add_listener(wl.framecb, &framelistener, NULL);
  wld_flush(wld.renderer);
  wl_surface_attach(wl.surface, wl.buffer, 0, 0);
  wl_surface_commit(wl.surface);
  /* need to wait to destroy the old buffer until we commit the new
   * buffer */
  if (wld.oldbuffer) {
    wld_buffer_unreference(wld.oldbuffer);
    wld.oldbuffer = 0;
  }
  needdraw = false;
}

void drawregion(int x1, int y1, int x2, int y2) {
  int ic, ib, x, y, ox;
  Glyph base, new;
  char buf[DRAW_BUF_SIZ];
  int ena_sel = sel.ob.x != -1 && sel.alt == IS_SET(MODE_ALTSCREEN);

  for (y = y1; y < y2; y++) {
    if (!term.dirty[y])
      continue;

    wltermclear(0, y, term.col, y);
    term.dirty[y] = 0;
    base = term.line[y][0];
    ic = ib = ox = 0;
    for (x = x1; x < x2; x++) {
      new = term.line[y][x];
      if (new.mode == ATTR_WDUMMY)
        continue;
      if (ena_sel && selected(x, y))
        new.mode ^= ATTR_REVERSE;
      if (ib > 0 && (ATTRCMP(base, new) || ib >= DRAW_BUF_SIZ - UTF_SIZ)) {
        wldraws(buf, base, ox, y, ic, ib);
        ic = ib = 0;
      }
      if (ib == 0) {
        ox = x;
        base = new;
      }

      ib += utf8encode(new.u, buf + ib);
      ic += (new.mode &ATTR_WIDE) ? 2 : 1;
    }
    if (ib > 0)
      wldraws(buf, base, ox, y, ic, ib);
  }
  wldrawcursor();
}

void wlseturgency(int add) { /* XXX: no urgency equivalent yet in wayland */
}

int match(uint mask, uint state) {
  return mask == MOD_MASK_ANY || mask == (state & ~(ignoremod));
}

void numlock(const Arg *dummy) { term.numlock ^= 1; }

char *kmap(xkb_keysym_t k, uint state) {
  Key *kp;
  int i;

  /* Check for mapped keys out of X11 function keys. */
  for (i = 0; i < LEN(mappedkeys); i++) {
    if (mappedkeys[i] == k)
      break;
  }
  if (i == LEN(mappedkeys)) {
    if ((k & 0xFFFF) < 0xFD00)
      return NULL;
  }

  for (kp = key; kp < key + LEN(key); kp++) {
    if (kp->k != k)
      continue;

    if (!match(kp->mask, state))
      continue;

    if (IS_SET(MODE_APPKEYPAD) ? kp->appkey < 0 : kp->appkey > 0)
      continue;
    if (term.numlock && kp->appkey == 2)
      continue;

    if (IS_SET(MODE_APPCURSOR) ? kp->appcursor < 0 : kp->appcursor > 0)
      continue;

    if (IS_SET(MODE_CRLF) ? kp->crlf < 0 : kp->crlf > 0)
      continue;

    return kp->s;
  }

  return NULL;
}

void cresize(int width, int height) {
  int col, row;

  if (width != 0)
    wl.w = width;
  if (height != 0)
    wl.h = height;

  col = (wl.w - 2 * borderpx) / wl.cw;
  row = (wl.h - 2 * borderpx) / wl.ch;

  tresize(col, row);
  wlresize(col, row);
  ttyresize();
}

void regglobal(void *data, struct wl_registry *registry, uint32_t name,
               const char *interface, uint32_t version) {
  if (getenv("WTERM_DEBUG"))
    printf("interface %s\n", interface);

  if (strcmp(interface, "wl_compositor") == 0) {
    wl.cmp = wl_registry_bind(registry, name, &wl_compositor_interface, 3);
  } else if (strcmp(interface, "zxdg_shell_v6") == 0) {
    // printf("init zxdg_shell_v6\n");
    wl.xdgshell_v6 =
        wl_registry_bind(registry, name, &zxdg_shell_v6_interface, 1);
    zxdg_shell_v6_add_listener(wl.xdgshell_v6, &shell_v6_listener, NULL);
  } else if (strcmp(interface, "wl_shm") == 0) {
    wl.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, "wl_seat") == 0) {
    wl.seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
  } else if (strcmp(interface, "wl_data_device_manager") == 0) {
    wl.datadevmanager =
        wl_registry_bind(registry, name, &wl_data_device_manager_interface, 1);
  } else if (strcmp(interface, "wl_output") == 0) {
    /* bind to outputs so we can get surface enter events */
    wl_registry_bind(registry, name, &wl_output_interface, 2);
  } else if (strcmp(interface, "xdg_shell") == 0) {
    wl.xdgshell = wl_registry_bind(registry, name, &xdg_shell_interface, 1);
  }
}

void regglobalremove(void *data, struct wl_registry *registry, uint32_t name) {}

void surfenter(void *data, struct wl_surface *surface,
               struct wl_output *output) {
  wl.vis++;
  if (!(wl.state & WIN_VISIBLE))
    wl.state |= WIN_VISIBLE;
}

void surfleave(void *data, struct wl_surface *surface,
               struct wl_output *output) {
  if (--wl.vis == 0)
    wl.state &= ~WIN_VISIBLE;
}

void framedone(void *data, struct wl_callback *callback, uint32_t msecs) {
  wl_callback_destroy(callback);
  wl.framecb = NULL;
  if (needdraw && wl.state & WIN_VISIBLE) {
    draw();
  }
}

void kbdkeymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
               int32_t fd, uint32_t size) {
  char *string;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    close(fd);
    return;
  }

  string = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);

  if (string == MAP_FAILED) {
    close(fd);
    return;
  }
  wl.xkb.keymap = xkb_keymap_new_from_string(wl.xkb.ctx, string,
                                             XKB_KEYMAP_FORMAT_TEXT_V1, 0);
  munmap(string, size);
  close(fd);
  wl.xkb.state = xkb_state_new(wl.xkb.keymap);

  wl.xkb.ctrl = xkb_keymap_mod_get_index(wl.xkb.keymap, XKB_MOD_NAME_CTRL);
  wl.xkb.alt = xkb_keymap_mod_get_index(wl.xkb.keymap, XKB_MOD_NAME_ALT);
  wl.xkb.shift = xkb_keymap_mod_get_index(wl.xkb.keymap, XKB_MOD_NAME_SHIFT);
  wl.xkb.logo = xkb_keymap_mod_get_index(wl.xkb.keymap, XKB_MOD_NAME_LOGO);

  wl.xkb.mods = 0;
}

void kbdenter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
              struct wl_surface *surface, struct wl_array *keys) {
  wl.state |= WIN_FOCUSED;
  if (IS_SET(MODE_FOCUS))
    ttywrite("\033[I", 3);
  /* need to redraw the cursor */
  needdraw = true;
}

void kbdleave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
              struct wl_surface *surface) {
  /* selection offers are invalidated when we lose keyboard focus */
  wl.seloffer = NULL;
  wl.state &= ~WIN_FOCUSED;
  if (IS_SET(MODE_FOCUS))
    ttywrite("\033[O", 3);
  /* need to redraw the cursor */
  needdraw = true;
  /* disable key repeat */
  repeat.len = 0;
}

void kbdkey(void *data, struct wl_keyboard *keyboard, uint32_t serial,
            uint32_t time, uint32_t key, uint32_t state) {
  xkb_keysym_t ksym;
  char buf[32], *str;
  int len;
  Rune c;
  Shortcut *bp;

  if (IS_SET(MODE_KBDLOCK))
    return;

  if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
    if (repeat.key == key)
      repeat.len = 0;
    return;
  }

  ksym = xkb_state_key_get_one_sym(wl.xkb.state, key + 8);
  len = xkb_keysym_to_utf8(ksym, buf, sizeof buf);
  if (len > 0)
    --len;

  /* 1. shortcuts */
  for (bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
    if (ksym == bp->keysym && match(bp->mod, wl.xkb.mods)) {
      bp->func(&(bp->arg));
      return;
    }
  }

  /* 2. custom keys from config.h */
  if ((str = kmap(ksym, wl.xkb.mods))) {
    len = strlen(str);
    goto send;
  }

  /* 3. composed string from input method */
  if (len == 0)
    return;
  if (len == 1 && wl.xkb.mods & MOD_MASK_ALT) {
    if (IS_SET(MODE_8BIT)) {
      if (*buf < 0177) {
        c = *buf | 0x80;
        len = utf8encode(c, buf);
      }
    } else {
      buf[1] = buf[0];
      buf[0] = '\033';
      len = 2;
    }
  }
  /* convert character to control character */
  else if (len == 1 && wl.xkb.mods & MOD_MASK_CTRL) {
    if ((*buf >= '@' && *buf < '\177') || *buf == ' ')
      *buf &= 0x1F;
    else if (*buf == '2')
      *buf = '\000';
    else if (*buf >= '3' && *buf <= '7')
      *buf -= ('3' - '\033');
    else if (*buf == '8')
      *buf = '\177';
    else if (*buf == '/')
      *buf = '_' & 0x1F;
  }

  str = buf;

send:
  memcpy(repeat.str, str, len);
  repeat.key = key;
  repeat.len = len;
  repeat.started = false;
  clock_gettime(CLOCK_MONOTONIC, &repeat.last);
  ttysend(str, len);
}

void kbdmodifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                  uint32_t dep, uint32_t lat, uint32_t lck, uint32_t group) {
  if (!wl.xkb.state)
    return;
  xkb_mod_mask_t mod_mask;
  xkb_state_update_mask(wl.xkb.state, dep, lat, lck, group, 0, 0);

  mod_mask = xkb_state_serialize_mods(wl.xkb.state, XKB_STATE_MODS_EFFECTIVE);
  wl.xkb.mods = 0;

  if (mod_mask & (1 << wl.xkb.ctrl))
    wl.xkb.mods |= MOD_MASK_CTRL;
  if (mod_mask & (1 << wl.xkb.alt))
    wl.xkb.mods |= MOD_MASK_ALT;
  if (mod_mask & (1 << wl.xkb.shift))
    wl.xkb.mods |= MOD_MASK_SHIFT;
  if (mod_mask & (1 << wl.xkb.logo))
    wl.xkb.mods |= MOD_MASK_LOGO;
}

void kbdrepeatinfo(void *data, struct wl_keyboard *keyboard, int32_t rate,
                   int32_t delay) {
  keyrepeatdelay = delay;
  keyrepeatinterval = 1000 / rate;
}

void ptrenter(void *data, struct wl_pointer *pointer, uint32_t serial,
              struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
  struct wl_cursor_image *img = cursor.cursor->images[0];
  struct wl_buffer *buffer;

  wl_pointer_set_cursor(pointer, serial, cursor.surface, img->hotspot_x,
                        img->hotspot_y);
  buffer = wl_cursor_image_get_buffer(img);
  wl_surface_attach(cursor.surface, buffer, 0, 0);
  wl_surface_damage(cursor.surface, 0, 0, img->width, img->height);
  wl_surface_commit(cursor.surface);
}

void ptrleave(void *data, struct wl_pointer *pointer, uint32_t serial,
              struct wl_surface *surface) {}

void ptrmotion(void *data, struct wl_pointer *pointer, uint32_t serial,
               wl_fixed_t x, wl_fixed_t y) {
  int oldey, oldex, oldsby, oldsey;

  if (IS_SET(MODE_MOUSE)) {
    wlmousereportmotion(x, y);
    return;
  }

  wl.px = wl_fixed_to_int(x);
  wl.py = wl_fixed_to_int(y);

  if (!sel.mode)
    return;

  sel.mode = SEL_READY;
  oldey = sel.oe.y;
  oldex = sel.oe.x;
  oldsby = sel.nb.y;
  oldsey = sel.ne.y;
  getbuttoninfo();

  if (oldey != sel.oe.y || oldex != sel.oe.x)
    tsetdirt(MIN(sel.nb.y, oldsby), MAX(sel.ne.y, oldsey));
}

void ptrbutton(void *data, struct wl_pointer *pointer, uint32_t serial,
               uint32_t time, uint32_t button, uint32_t state) {
  Mousekey *mk;

  if (IS_SET(MODE_MOUSE) && !(wl.xkb.mods & forceselmod)) {
    wlmousereportbutton(button, state);
    return;
  }

  switch (state) {
  case WL_POINTER_BUTTON_STATE_RELEASED:
    if (button == BTN_MIDDLE) {
      selpaste(NULL);
    } else if (button == BTN_LEFT) {
      if (sel.mode == SEL_READY) {
        getbuttoninfo();
        selcopy(serial);
      } else
        selclear();
      sel.mode = SEL_IDLE;
      tsetdirt(sel.nb.y, sel.ne.y);
    }
    break;

  case WL_POINTER_BUTTON_STATE_PRESSED:
    for (mk = mshortcuts; mk < mshortcuts + LEN(mshortcuts); mk++) {
      if (button == mk->b && match(mk->mask, wl.xkb.mods)) {
        ttysend(mk->s, strlen(mk->s));
        return;
      }
    }

    if (button == BTN_LEFT) {
      /* Clear previous selection, logically and visually. */
      selclear();
      sel.mode = SEL_EMPTY;
      sel.type = SEL_REGULAR;
      sel.oe.x = sel.ob.x = x2col(wl.px);
      sel.oe.y = sel.ob.y = y2row(wl.py);

      /*
       * If the user clicks below predefined timeouts
       * specific snapping behaviour is exposed.
       */
      if (time - sel.tclick2 <= tripleclicktimeout) {
        sel.snap = SNAP_LINE;
      } else if (time - sel.tclick1 <= doubleclicktimeout) {
        sel.snap = SNAP_WORD;
      } else {
        sel.snap = 0;
      }
      selnormalize();

      if (sel.snap != 0)
        sel.mode = SEL_READY;
      tsetdirt(sel.nb.y, sel.ne.y);
      sel.tclick2 = sel.tclick1;
      sel.tclick1 = time;
    }
    break;
  }
}

void ptraxis(void *data, struct wl_pointer *pointer, uint32_t time,
             uint32_t axis, wl_fixed_t value) {
  Axiskey *ak;
  int dir = value > 0 ? +1 : -1;

  if (IS_SET(MODE_MOUSE) && !(wl.xkb.mods & forceselmod)) {
    wlmousereportaxis(axis, value);
    return;
  }

  for (ak = ashortcuts; ak < ashortcuts + LEN(ashortcuts); ak++) {
    if (axis == ak->axis && dir == ak->dir && match(ak->mask, wl.xkb.mods)) {
      ttysend(ak->s, strlen(ak->s));
      return;
    }
  }
}

void shellsurfping(void *user, struct wl_shell_surface *surface,
                   uint32_t serial) {
  wl_shell_surface_pong(surface, serial);
  if (getenv("WTERM_DEBUG"))
    printf("shellsurf ping serial=%d\n", serial);
}
void shellsurfconfigure(void *user, struct wl_shell_surface *surface,
                        uint32_t edges, int32_t w, int32_t h) {
  if (getenv("WTERM_DEBUG"))
    printf("shell surface configured\n");
}

void shellsurfpopupdone(void *user, struct wl_shell_surface *surface) {}

void xdgshellv6ping(void *data, struct zxdg_shell_v6 *shell, uint32_t serial) {
  zxdg_shell_v6_pong(shell, serial);
}

void xdgsurfv6configure(void *data, struct zxdg_surface_v6 *surf,
                        uint32_t serial) {
  zxdg_surface_v6_ack_configure(surf, serial);
}

void xdgshellping(void *data, struct xdg_shell *shell, uint32_t serial) {
  xdg_shell_pong(shell, serial);
  if (getenv("WTERM_DEBUG"))
    printf("serial=%d\n", serial);
}

void xdgsurfconfigure(void *data, struct xdg_surface *surf, int32_t w,
                      int32_t h, struct wl_array *states, uint32_t serial) {
  xdg_surface_ack_configure(surf, serial);
  xdg_surface_set_app_id(surf, opt_class ? opt_class : termname);
  wl.configured = true;
  if (wl.h == h && wl.w == w)
    return;
  cresize(w, h);
}

void xdgtopconfigure(void *data, struct zxdg_toplevel_v6 *top, int32_t w,
                     int32_t h, struct wl_array *states) {
  zxdg_toplevel_v6_set_app_id(top, opt_class ? opt_class : termname);
  wl.configured = true;
  if (wl.w == w && wl.h == h)
    return;
  cresize(w, h);
}

static void close_shell_and_exit() {
  kill(pid, SIGHUP);
  exit(0);
}

void xdgtopclose(void *data, struct zxdg_toplevel_v6 *top) {
  close_shell_and_exit();
}

void xdgsurfclose(void *data, struct xdg_surface *surf) {
  close_shell_and_exit();
}

void datadevoffer(void *data, struct wl_data_device *datadev,
                  struct wl_data_offer *offer) {
  wl_data_offer_add_listener(offer, &dataofferlistener, NULL);
}

void datadeventer(void *data, struct wl_data_device *datadev, uint32_t serial,
                  struct wl_surface *surf, wl_fixed_t x, wl_fixed_t y,
                  struct wl_data_offer *offer) {}

void datadevleave(void *data, struct wl_data_device *datadev) {}

void datadevmotion(void *data, struct wl_data_device *datadev, uint32_t time,
                   wl_fixed_t x, wl_fixed_t y) {}

void datadevdrop(void *data, struct wl_data_device *datadev) {}

void datadevselection(void *data, struct wl_data_device *datadev,
                      struct wl_data_offer *offer) {
  if (offer && (uintptr_t)wl_data_offer_get_user_data(offer) == 1)
    wl.seloffer = offer;
  else
    wl.seloffer = NULL;
}

void dataofferoffer(void *data, struct wl_data_offer *offer,
                    const char *mimetype) {
  /* mark the offer as usable if it supports plain text */
  if (strncmp(mimetype, "text/plain", 10) == 0)
    wl_data_offer_set_user_data(offer, (void *)(uintptr_t)1);
}

void datasrctarget(void *data, struct wl_data_source *source,
                   const char *mimetype) {}

void datasrcsend(void *data, struct wl_data_source *source,
                 const char *mimetype, int32_t fd) {
  char *buf = sel.primary;
  int len = strlen(sel.primary);
  ssize_t ret;
  while ((ret = write(fd, buf, MIN(len, BUFSIZ))) > 0) {
    len -= ret;
    buf += ret;
  }
  close(fd);
}

void datasrccancelled(void *data, struct wl_data_source *source) {
  if (sel.source == source) {
    sel.source = NULL;
    selclear();
  }
  wl_data_source_destroy(source);
}

void run(void) {
  fd_set rfd;
  int wlfd = wl_display_get_fd(wl.dpy), blinkset = 0;
  struct timespec drawtimeout, *tv = NULL, now, last, lastblink;
  ulong msecs;

  ttynew();
  /* Look for initial configure. */
  wl_display_roundtrip(wl.dpy);
  if (!wl.configured) {
    cresize(wl.w, wl.h);
  }
  draw();

  clock_gettime(CLOCK_MONOTONIC, &last);
  lastblink = last;

  for (;;) {
    FD_ZERO(&rfd);
    FD_SET(cmdfd, &rfd);
    FD_SET(wlfd, &rfd);

    if (pselect(MAX(wlfd, cmdfd) + 1, &rfd, NULL, NULL, tv, NULL) < 0) {
      if (errno == EINTR)
        continue;
      die("select failed: %s\n", strerror(errno));
    }

    if (FD_ISSET(cmdfd, &rfd)) {
      ttyread();
      if (blinktimeout) {
        blinkset = tattrset(ATTR_BLINK);
        if (!blinkset)
          MODBIT(term.mode, 0, MODE_BLINK);
      }
    }

    if (FD_ISSET(wlfd, &rfd)) {

      if (wl_display_dispatch(wl.dpy) == -1)
        die("Connection error\n");
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    msecs = -1;

    if (blinkset && blinktimeout) {
      if (TIMEDIFF(now, lastblink) >= blinktimeout) {
        tsetdirtattr(ATTR_BLINK);
        term.mode ^= MODE_BLINK;
        lastblink = now;
      } else {
        msecs = MIN(msecs, blinktimeout - TIMEDIFF(now, lastblink));
      }
    }
    if (repeat.len > 0) {
      if (TIMEDIFF(now, repeat.last) >=
          (repeat.started ? keyrepeatinterval : keyrepeatdelay)) {
        repeat.started = true;
        repeat.last = now;
        ttysend(repeat.str, repeat.len);
      } else {
        msecs =
            MIN(msecs, (repeat.started ? keyrepeatinterval : keyrepeatdelay) -
                           TIMEDIFF(now, repeat.last));
      }
    }

    if (needdraw) {
      if (!wl.framecb) {
        draw();
      }
    }

    if (msecs == -1) {
      tv = NULL;
    } else {
      drawtimeout.tv_nsec = 1E6 * msecs;
      drawtimeout.tv_sec = 0;
      tv = &drawtimeout;
    }

    wl_display_dispatch_pending(wl.dpy);
    wl_display_flush(wl.dpy);
  }
}

void usage(void) {
  die("%1$s " VERSION " (c) 2010-2015 st engineers, 2015-2019 wterm engineers\n"
      "usage: %1$s [-a] [-v] [-c class] [-f font] [-o file]\n"
      "          [-t title] [-T title] [-e command ...]"
      " [command ...]\n"
      "       %1$s [-a] [-v] [-c class] [-f font] [-o file]\n"
      "          [-t title] [-T title] [-l line]"
      " [stty_args ...]\n",
      argv0);
}

int main(int argc, char *argv[]) {
  ARGBEGIN {
  case 'a':
    allowaltscreen = 0;
    break;
  case 'c':
    opt_class = EARGF(usage());
    break;
  case 'e':
    if (argc > 0)
      --argc, ++argv;
    goto run;
  case 'f':
    opt_font = EARGF(usage());
    break;
  case 'o':
    opt_io = EARGF(usage());
    break;
  case 'l':
    opt_line = EARGF(usage());
    break;
  case 't':
  case 'T':
    opt_title = EARGF(usage());
    break;
  case 'v':
  default:
    usage();
  }
  ARGEND;

run:
  if (argc > 0) {
    /* eat all remaining arguments */
    opt_cmd = argv;
    if (!opt_title && !opt_line)
      opt_title = basename(xstrdup(argv[0]));
  }
  setlocale(LC_CTYPE, "");
  tnew(80, 24);
  wlinit();
  selinit();
  run();

  return 0;
}
