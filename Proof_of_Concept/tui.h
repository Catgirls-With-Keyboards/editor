#include "libtmt/tmt.h"

#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#ifndef tui_PANIC
#define tui_PANIC() exit(1);
#endif

#define MAX_COMPONENTS 64
#define MAX_CHILDREN 64

typedef struct {
  uint16_t new_width, new_height;
} ResizeEvent;

typedef struct {
  uint16_t mouse_x, mouse_y;
  enum {
    MOUSE_BUTTON_1,
    MOUSE_BUTTON_2,
    MOUSE_BUTTON_3,
    MOUSE_MOVE,
  } mouse_action;
} MouseEvent;

typedef struct {
  wchar_t key;
} KeyEvent;

typedef enum { END, RESIZE, MOUSE, KEY } EventKind;

typedef struct {
  bool handled;
  EventKind kind;
  union {
    ResizeEvent resizeEvent;
    MouseEvent mouseEvent;
    KeyEvent keyEvent;
  };
} Event;

struct Component;
typedef struct Component Component;

typedef struct {
  TMT *screen;

  uint16_t window_width;
  uint16_t window_height;

  bool _needs_resize;
  bool _exiting;

  // Things towards the back of the list
  // are on top of the ones at the front.
  uint16_t num_components;
  Component *componentList[MAX_COMPONENTS];
  Component *rootComponent;
} GlobalContext;

typedef struct {
  uint16_t x, y;
  uint16_t width, height;
} Position;

struct Component {
  GlobalContext *context;
  Component *parent;

  Position pos;

  uint16_t num_children;
  Component *children[MAX_CHILDREN];

  int (*onClick)(MouseEvent event);  // Returns if the event was handled by one
                                     // of its children or not.
  int (*onKeypress)(KeyEvent event); // Returns if the event was handled by one
                                     // of its children or not.
  void (*render)(TMT *screen);       // Calls the render() of any subcomponents.
  void (*resize)(Position new_pos);  // Calls the resize() of any subcomponents.

  enum {
    Kind1,
    Kind2,
    // For each kind of component
  } kind;
  union {
    struct {
      // ...
    } ComponentKind1Data;
    struct {
      // ...
    } ComponentKind2Data;
    // For all the stuff each kind of component has to store
  };
};

typedef void (*tuisighandler_t)(int);

//////////////////////
// GLOBAL VARIABLES //
//////////////////////
// "Public"
static GlobalContext tui_globalcontext;
// "Private"
static int _tui_active = 0;
static struct termios _old_tio;
static tuisighandler_t _old_sigwinch;
static tuisighandler_t _old_sigterm;
static tuisighandler_t _old_sigint;
static char *_old_locale;

// Helper fucnctions

static inline void tui_error(const char *message);

static inline bool inComponent(Component *c, uint16_t x, uint16_t y) {
  Position p = c->pos;
  return (x >= p.x) & (x <= p.x + p.width) & (y >= p.y) & (y <= p.y + p.height);
}

// Move component to the front of the list
static void raiseComponent(Component *c) {
  uint16_t nc = tui_globalcontext.num_components;
  Component **components = tui_globalcontext.componentList;

  if (nc <= 1)
    return;
  Component *tmp = components[0];
  if (tmp == c)
    return;
  components[0] = c;

  for (size_t i = 1; i < nc; i++) {
    Component *despot = components[i];
    if (despot == c) {
      components[i] = tmp;
      break;
    } else {
      components[i] = tmp;
      tmp = despot;
    }
  }
}

// TODO: have TMT resize itself to match.
static inline void updateSize(void) {
  struct winsize ws;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
#if (USHRT_MAX > UINT16_MAX)
  if ((ws.ws_row > UINT16_MAX) | (ws.ws_col > UINT16_MAX)) {
    const char errmsg[] = "Tui can't handle a terminal this big.\n";
    fprintf(stderr, errmsg);
    tui_PANIC();
  }
#endif
  tui_globalcontext.window_width = (uint16_t)ws.ws_col;
  tui_globalcontext.window_height = (uint16_t)ws.ws_row;
}

static void sigwinch_handler(int sig) {
  (void)sig;
  tui_globalcontext._needs_resize = true;
}

static void sigint_sigterm_handler(int sig) {
  (void)sig;
  tui_globalcontext._exiting = true;
}

static inline void tui_init(void) {
  // Install signal handlers, save what they used to be.
  // Get term size
  // Initialize global context
  // Initialize memory for screenbuffer
  // Initialize memory allocator

  // Disable echo, disable canonical mode (line buffering).
  // Record the old terminal attributes.
  tcgetattr(1, &_old_tio);
  struct termios _tio_new = _old_tio;
  _tio_new.c_lflag &= (~ECHO & ~ICANON);
  tcsetattr(1, TCSANOW, &_tio_new);

  // Initialize the terminal
  char init_term[] = "\x1b?1049h"   // Alt buffer
                     "\x1b[2J"      // Clear screen
                     "\x1b[H"       // Cursor to home position
                     "\x1b[?25l"    // Hide cursor
                     "\x1b[?1000l"; // Enable mouse events
  write(1, init_term, sizeof(init_term));

  // Copy the current locale, set new one
  if (!(_old_locale = setlocale(LC_CTYPE, NULL)))
    tui_error("Could not query the locale.");

  if (!setlocale(LC_ALL, "C.UTF-8"))
    if (!setlocale(LC_ALL, "en_US.UTF-8"))
      tui_error("Could not set locale to utf8.");

  // Initialize the global context
  tui_globalcontext._exiting = 0;
  tui_globalcontext._needs_resize = 0;

  // Init component list
  tui_globalcontext.rootComponent = NULL;
  tui_globalcontext.num_components = 0;
  for (size_t i = 0; i < MAX_COMPONENTS; i++)
    tui_globalcontext.componentList[i] = NULL;

  // Install signal handlers, back up old ones
  _old_sigint = signal(SIGINT, sigint_sigterm_handler);
  _old_sigterm = signal(SIGTERM, sigint_sigterm_handler);
  _old_sigwinch = signal(SIGWINCH, sigwinch_handler);

  // Get the terminal size, init tmt
  updateSize(); // Sets window_height and window_width
  tui_globalcontext.screen =
      tmt_open(tui_globalcontext.window_height, tui_globalcontext.window_width,
               NULL, NULL, NULL);

  _tui_active = 1;
}

static inline void tui_deinit(void) {
  if (!_tui_active)
    return;
  _tui_active = 0;

  // Swap in old signal hadnlers
  signal(SIGINT, _old_sigint);
  signal(SIGTERM, _old_sigterm);
  signal(SIGWINCH, _old_sigwinch);

  // Restore old terminal attributes
  tcsetattr(1, TCSANOW, &_old_tio);

  // Reset locale
  setlocale(LC_ALL, _old_locale);

  // Return the terminal to normal
  char restore_term[] =
      "\x1b[?1049h"  // Return to alt buffer if we somehow escaped
      "\x1b[?1000l"  // No mouse events
      "\x1b[2J"      // Clear screen
      "\x1b[?25h"    // Show cursor
      "\x1b[?1049l"; // Return to main buffer
  write(1, restore_term, sizeof(restore_term));
}

static inline void tui_error(const char *message) {
  tui_deinit();
  fprintf(stderr, "%s\n", message);
  tui_PANIC();
}

static inline Event handle_event(void) {
  if (!_tui_active)
    tui_error("Root component not initialized.");
  if (tui_globalcontext.rootComponent)
    tui_error("Tui is not active.");

  Event e;
  if (tui_globalcontext._exiting) {
    tui_deinit();
    return (e.kind = END, e.handled = 1), e;
  }

  Component **components = tui_globalcontext.componentList;

  // Resize in response to SIGWINCH
  if (tui_globalcontext._needs_resize) {
    tui_globalcontext._needs_resize = 0;

    // Ask the OS for the new size
    updateSize();

    // Bubble resizes down
    tui_globalcontext.rootComponent->resize((Position){
        0, 0, tui_globalcontext.window_width, tui_globalcontext.window_height});

    // return resize info
    e.kind = RESIZE;
    e.handled = 1;
    e.resizeEvent.new_height = tui_globalcontext.window_height;
    e.resizeEvent.new_width = tui_globalcontext.window_width;
    return e;
  }

  // Get event from some dependency

  // Handle the event (This depends on the kind of event.)

  // For KEY and MOUSE events, find the topmost component that it applies
  // to (overlaps the (x,y) coordinate where the event happened, and has
  // the applicable event handler).
  // That event handler will give the event to its parent if it doesn't
  // want to handle, it, then it to its parent, etc. At the end, we mark
  // it as handled or not.

  for (uint16_t i = 0; i < tui_globalcontext.num_components; i++) {
    if (e.kind == KEY) {
      if (components[i]->onKeypress) {
        e.handled = components[i]->onKeypress(e.keyEvent);
        if (e.handled)
          break;
      }
    } else if (e.kind == MOUSE) {
      if (components[i]->onClick) {
        if (inComponent(components[i], e.mouseEvent.mouse_x,
                        e.mouseEvent.mouse_y)) {
          e.handled = components[i]->onClick(e.mouseEvent);
          if (e.handled)
            break;
        }
      }
    }
  }

  return e;
}

// First: Are the attrs equal?
// If yes, return _unused1 = true immediately.
// Second: Are there any bits set in b1 that are not set in b2?
// If yes, return _unused2 = true immediately.
// Third: What bits are present in b2 that are not present in b1?
// Return that.
static inline TMTATTRS subtract_attr_bits(TMTATTRS b1, TMTATTRS b2) {

  b1._unused1 = 0;
  b1._unused2 = 0;
  b2._unused1 = 0;
  b2._unused2 = 0;

  uint8_t b1attrs, b2attrs;
  memcpy(&b1attrs, (char *)&b1 + offsetof(TMTATTRS, attrs), sizeof(uint8_t));
  memcpy(&b2attrs, (char *)&b2 + offsetof(TMTATTRS, attrs), sizeof(uint8_t));

  return b1;
}

static inline void writescreen(TMT *tmt) {
  // For every dirty line in the screen, render the line.
  const TMTSCREEN *screen = tmt_screen(tui_globalcontext.screen);
  size_t nline = screen->nline, ncol = screen->ncol;
  for (size_t lnum = 0; lnum < nline; lnum++) {
    TMTLINE *line = screen->lines[lnum];
    if (!line->dirty)
      continue;

    TMTATTRS last;
    last.fg = (tmt_color_t){0, 0, 0, TMT_ANSI_COLOR_DEFAULT};
    last.bg = (tmt_color_t){0, 0, 0, TMT_ANSI_COLOR_DEFAULT};
    last.attrs = 0;
    for (size_t cnum = 0; cnum < ncol; cnum++) {
      wchar_t c = line->chars[cnum].c;
      TMTATTRS attrs = line->chars[cnum].a;
      if (attrs.reverse) {
        tmt_color_t tmp = attrs.bg;
        attrs.bg = attrs.fg;
        attrs.fg = tmp;
        attrs.reverse = 0;
      }

      size_t cbuflen = 0;
      char cbuffer[64];

      // Apply colors and attributes

      // Finally append the character and write.
      // If wctomb doesn't know about that character,
      // use the unicode replacement character.
      int wcret = wctomb(cbuffer, c);
      if (wcret == -1) {
        const char replacement_char[] = "\uFFFD";
        wcret = sprintf(cbuffer, replacement_char);
      }
      cbuflen += wcret;
      tmt_write(tmt, cbuffer, cbuflen);
    }
  }
}

static inline void render_window(void) {
  tui_globalcontext.rootComponent->render(tui_globalcontext.screen);
  writescreen(tui_globalcontext.screen);
}

static inline int example_main(void) {
  // Render loop
  tui_init();
  atexit(tui_deinit);

  // Build out component tree
  for (;;) {
    Event e = handle_event();
    if (e.kind == END)
      exit(0);

    // Do whatever we want based on events
    // and if they've already been handled
    // by callbacks or not. Generally speaking,
    // components should be in charge of changing
    // themselves.

    render_window();
  }

  return 0;
}
