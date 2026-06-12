// x11_block_rightclick_target_window.c
//
// Intercept right-clicks on X11 using XInput2 and selectively block them
// when the pointer is over a window whose title or WM_CLASS matches a
// configured denylist.
//
// This version differs from the original in two important ways:
//  1. The denylist is no longer compiled into the binary.
//  2. The source is fully documented to make maintenance easier.
//
// Configuration is loaded from an X resource style file via Xrm.
//
// Example config file:
//   x11BlockRightclick.denyTitle: LXQt Panel
//   x11BlockRightclick.denyTitle.1: pcmanfm-desktop0
//   x11BlockRightclick.denyTitle.2: pcmanfm-desktop1
//   x11BlockRightclick.denyTitle.3: pcmanfm-desktop2
//   x11BlockRightclick.denyTitle.4: pcmanfm-desktop3
//   x11BlockRightclick.denyClass: pcmanfm
//   x11BlockRightclick.denyClass.1: lxqt-panel
//
// Default config path resolution:
//   1. $X11_BLOCK_RIGHTCLICK_CONFIG
//   2. $HOME/.config/x11-block-rightclick.conf
//
// Build example:
//   cc -O2 -Wall -Wextra -o x11-block-rightclick \
//      x11_block_rightclick_target_window.c \
//      -lX11 -lXi
//
// Notes:
// - This program is for X11, not Wayland.
// - It uses a passive XIGrabButton on Button3 (right click).
// - Allowed clicks are replayed to the original client.
// - Blocked clicks are consumed by not replaying the event.

#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <syslog.h>
#include <limits.h>
#include <unistd.h>

/*
 * Maximum string sizes used for window metadata capture.
 * These are intentionally conservative and sufficient for most X11 titles.
 */
#define TITLE_BUFSZ 512
#define CLASS_BUFSZ 256

/*
 * The logical application name used in the X resource database.
 * Resource keys will look like:
 *   x11BlockRightclick.denyTitle
 *   x11BlockRightclick.denyTitle.1
 *   x11BlockRightclick.denyClass
 *   x11BlockRightclick.denyClass.1
 */
#define APP_RES_NAME "x11BlockRightclick"

/*
 * Small dynamic string list used for denylist entries loaded from config.
 */
struct string_list {
    char **items;
    size_t len;
    size_t cap;
};

/*
 * Global XInput opcode returned by XQueryExtension().
 * This lets us recognize XInput2 GenericEvent cookies in the event loop.
 */
static int xi_opcode = -1;

/*
 * Global Display pointer is kept so cleanup() can safely ungrab and close
 * the X connection on termination.
 */
static Display *g_dpy = NULL;

/*
 * Termination flag set from signal handlers.
 * sig_atomic_t is used because it is signal-safe for simple stores/loads.
 */
static volatile sig_atomic_t g_stop = 0;

/*
 * Runtime-loaded denylist data.
 * These replace the old compile-time hardcoded arrays.
 */
static struct string_list g_deny_titles = {0};
static struct string_list g_deny_classes = {0};

/*
 * Free every string owned by a string_list and reset it to empty.
 */
static void string_list_free(struct string_list *list) {
    if (!list) return;

    for (size_t i = 0; i < list->len; ++i) {
        free(list->items[i]);
    }

    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

/*
 * Append a copy of 's' to the dynamic string list.
 *
 * Returns:
 *   1 on success
 *   0 on allocation failure or invalid input
 */
static int string_list_push(struct string_list *list, const char *s) {
    if (!list || !s || !*s) return 0;

    if (list->len == list->cap) {
        size_t new_cap = list->cap ? (list->cap * 2) : 8;
        char **new_items = realloc(list->items, new_cap * sizeof(*new_items));
        if (!new_items) return 0;
        list->items = new_items;
        list->cap = new_cap;
    }

    char *copy = strdup(s);
    if (!copy) return 0;

    list->items[list->len++] = copy;
    return 1;
}

/*
 * Remove leading and trailing ASCII whitespace from a mutable string.
 * The trimming is done in place and the resulting pointer may point
 * into the middle of the original buffer.
 */
static char *trim_whitespace(char *s) {
    if (!s) return s;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v')
        ++s;

    if (!*s) return s;

    char *end = s + strlen(s) - 1;
    while (end > s &&
           (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r' || *end == '\f' || *end == '\v')) {
        *end-- = '\0';
    }

    return s;
}

/*
 * Test whether 'haystack' contains any configured denylist token.
 *
 * Matching semantics intentionally mirror the original code:
 * a denylist entry is considered matched if it appears as a substring.
 *
 * Returns:
 *   1 if a match is found
 *   0 otherwise
 */
static int list_matches_substring(const struct string_list *list, const char *haystack) {
    if (!list || !haystack || !*haystack) return 0;

    for (size_t i = 0; i < list->len; ++i) {
        if (list->items[i] && *list->items[i] && strstr(haystack, list->items[i]) != NULL)
            return 1;
    }

    return 0;
}

/*
 * Resolve the configuration file path.
 *
 * Order:
 *   1. X11_BLOCK_RIGHTCLICK_CONFIG environment variable
 *   2. $HOME/.config/x11-block-rightclick.conf
 *
 * The resolved path is written to 'buf'.
 *
 * Returns:
 *   1 on success
 *   0 if no path can be formed
 */
static int get_config_path(char *buf, size_t bufsz) {
    if (!buf || bufsz == 0) return 0;

    const char *env = getenv("X11_BLOCK_RIGHTCLICK_CONFIG");
    if (env && *env) {
        snprintf(buf, bufsz, "%s", env);
        return 1;
    }

    const char *home = getenv("HOME");
    if (!home || !*home) return 0;

    snprintf(buf, bufsz, "%s/.config/x11-block-rightclick.conf", home);
    return 1;
}

/*
 * Lookup one X resource string from the database.
 *
 * Example:
 *   name = "x11BlockRightclick.denyTitle"
 *   class = "X11BlockRightclick.DenyTitle"
 *
 * XrmGetResource() returns a string value if present.
 * The returned pointer is managed by Xrm; copy it before storing long term.
 *
 * Returns:
 *   1 if the resource exists and was copied into 'buf'
 *   0 otherwise
 */
static int xrm_get_string(XrmDatabase db,
                          const char *name,
                          const char *class_name,
                          char *buf,
                          size_t bufsz) {
    if (!db || !name || !class_name || !buf || bufsz == 0) return 0;

    char *type = NULL;
    XrmValue value;

    if (!XrmGetResource(db, name, class_name, &type, &value))
        return 0;

    if (!value.addr || value.size == 0)
        return 0;

    size_t len = (size_t)value.size;
    if (len >= bufsz) len = bufsz - 1;

    memcpy(buf, value.addr, len);
    buf[len] = '\0';

    char *trimmed = trim_whitespace(buf);
    if (trimmed != buf)
        memmove(buf, trimmed, strlen(trimmed) + 1);

    return buf[0] != '\0';
}

/*
 * Load a numbered series of X resources into a string list.
 *
 * Supported key layout:
 *   <base_name>           first item
 *   <base_name>.1         second item
 *   <base_name>.2         third item
 *   ...
 *
 * This style is convenient for human-edited config files.
 *
 * Returns:
 *   count of successfully loaded entries
 */
static size_t xrm_load_series(XrmDatabase db,
                              const char *base_name,
                              const char *base_class,
                              struct string_list *out_list) {
    if (!db || !base_name || !base_class || !out_list) return 0;

    char name[256];
    char class_name[256];
    char value[1024];
    size_t count = 0;

    /*
     * First try the unnumbered base key.
     */
    snprintf(name, sizeof(name), "%s", base_name);
    snprintf(class_name, sizeof(class_name), "%s", base_class);

    if (xrm_get_string(db, name, class_name, value, sizeof(value))) {
        if (string_list_push(out_list, value))
            ++count;
    }

    /*
     * Then try numbered suffixes until the first missing key.
     * This simple convention is easy to document and debug.
     */
    for (unsigned int i = 1; i < 10000; ++i) {
        snprintf(name, sizeof(name), "%s.%u", base_name, i);
        snprintf(class_name, sizeof(class_name), "%s.%u", base_class, i);

        if (!xrm_get_string(db, name, class_name, value, sizeof(value)))
            break;

        if (string_list_push(out_list, value))
            ++count;
    }

    return count;
}

/*
 * Load denylist entries from a config file using the X resource manager.
 *
 * Expected resource keys:
 *   x11BlockRightclick.denyTitle
 *   x11BlockRightclick.denyTitle.1
 *   ...
 *   x11BlockRightclick.denyClass
 *   x11BlockRightclick.denyClass.1
 *   ...
 *
 * Returns:
 *   1 on success (even if one list is empty)
 *   0 on failure to open/load the config database
 */
static int load_denylist_from_config(const char *path) {
    if (!path || !*path) return 0;

    XrmInitialize();

    XrmDatabase db = XrmGetFileDatabase(path);
    if (!db) {
        syslog(LOG_ERR, "Failed to load config file: %s", path);
        return 0;
    }

    string_list_free(&g_deny_titles);
    string_list_free(&g_deny_classes);

    size_t ntitles = xrm_load_series(
        db,
        APP_RES_NAME ".denyTitle",
        "X11BlockRightclick.DenyTitle",
        &g_deny_titles
    );

    size_t nclasses = xrm_load_series(
        db,
        APP_RES_NAME ".denyClass",
        "X11BlockRightclick.DenyClass",
        &g_deny_classes
    );

    syslog(LOG_INFO,
           "Loaded config: %s (titles=%lu classes=%lu)",
           path,
           (unsigned long)ntitles,
           (unsigned long)nclasses);

    XrmDestroyDatabase(db);
    return 1;
}

/*
 * Convenience wrapper for title denylist checks.
 */
static int title_matches_denylist(const char *title) {
    return list_matches_substring(&g_deny_titles, title);
}

/*
 * Convenience wrapper for class denylist checks.
 */
static int class_matches_denylist(const char *class_) {
    return list_matches_substring(&g_deny_classes, class_);
}

/*
 * Check whether a window has a given property.
 *
 * We only read a single item because the content is not important here;
 * we only care whether the property exists and has a real type.
 *
 * Returns:
 *   1 if property exists
 *   0 otherwise
 */
static int has_property(Display *dpy, Window win, Atom prop) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int ok = XGetWindowProperty(dpy, win, prop, 0, 1, False, AnyPropertyType,
                                &actual_type, &actual_format, &nitems, &bytes_after,
                                &data) == Success;
    if (data) XFree(data);
    return ok && actual_type != None;
}

/*
 * Try to read the modern EWMH title property: _NET_WM_NAME encoded as UTF8_STRING.
 *
 * Returns:
 *   1 if a title was read successfully
 *   0 otherwise
 */
static int get_window_title_net_wm_name(Display *dpy, Window win, char *buf, size_t bufsz) {
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, win, net_wm_name, 0, 1024, False, utf8,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           &data) != Success || !data) {
        return 0;
    }

    if (actual_type == utf8 && actual_format == 8 && nitems > 0) {
        size_t len = nitems;
        if (len >= bufsz) len = bufsz - 1;
        memcpy(buf, data, len);
        buf[len] = '\0';
        XFree(data);
        return 1;
    }

    XFree(data);
    return 0;
}

/*
 * Fallback title resolution using classic ICCCM mechanisms.
 *
 * Strategy:
 *   1. XGetWMName()
 *   2. XFetchName()
 *
 * Returns:
 *   1 if a title was read
 *   0 otherwise
 */
static int get_window_title_wm_name(Display *dpy, Window win, char *buf, size_t bufsz) {
    XTextProperty prop;

    if (XGetWMName(dpy, win, &prop) && prop.value && prop.encoding != None) {
        Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        Atom string_atom = XInternAtom(dpy, "STRING", False);

        if (prop.encoding == utf8 || prop.encoding == string_atom || prop.format == 8) {
            size_t len = strlen((char *)prop.value);
            if (len >= bufsz) len = bufsz - 1;
            memcpy(buf, prop.value, len);
            buf[len] = '\0';
            XFree(prop.value);
            return 1;
        }
        XFree(prop.value);
    }

    char *name = NULL;
    if (XFetchName(dpy, win, &name) && name) {
        size_t len = strlen(name);
        if (len >= bufsz) len = bufsz - 1;
        memcpy(buf, name, len);
        buf[len] = '\0';
        XFree(name);
        return 1;
    }

    return 0;
}

/*
 * Unified title getter:
 *   prefer _NET_WM_NAME, then fall back to WM_NAME / XFetchName.
 */
static int get_window_title(Display *dpy, Window win, char *buf, size_t bufsz) {
    if (get_window_title_net_wm_name(dpy, win, buf, bufsz))
        return 1;
    if (get_window_title_wm_name(dpy, win, buf, bufsz))
        return 1;
    return 0;
}

/*
 * Read WM_CLASS using XGetClassHint() and format it as:
 *   res_name/res_class
 *
 * Example:
 *   "pcmanfm/pcmanfm"
 *
 * This is convenient for substring matching because either component
 * can be targeted with one config token.
 *
 * Returns:
 *   1 if class data was available
 *   0 otherwise
 */
static int get_window_class(Display *dpy, Window win, char *buf, size_t bufsz) {
    XClassHint hint;
    memset(&hint, 0, sizeof(hint));

    if (!XGetClassHint(dpy, win, &hint))
        return 0;

    const char *res_name = hint.res_name ? hint.res_name : "";
    const char *res_class = hint.res_class ? hint.res_class : "";

    snprintf(buf, bufsz, "%s%s%s",
             res_name,
             (*res_name && *res_class) ? "/" : "",
             res_class);

    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
    return 1;
}

/*
 * Determine whether a root-coordinate point lies inside 'win'.
 *
 * We first ensure the window is viewable and then translate the root
 * coordinates into the window's coordinate system.
 *
 * Returns:
 *   1 if the point is inside the window
 *   0 otherwise
 */
static int point_in_window(Display *dpy, Window win, int root_x, int root_y) {
    XWindowAttributes attr;
    Window child = None;
    int win_x = 0, win_y = 0;

    if (!XGetWindowAttributes(dpy, win, &attr))
        return 0;
    if (attr.map_state != IsViewable)
        return 0;

    if (!XTranslateCoordinates(dpy, DefaultRootWindow(dpy), win,
                               root_x, root_y, &win_x, &win_y, &child)) {
        return 0;
    }

    return (win_x >= 0 && win_y >= 0 &&
            win_x < attr.width && win_y < attr.height);
}

/*
 * Starting from a window, walk downward through children to find the
 * deepest mapped child that still contains the pointer coordinate.
 *
 * Children are scanned from top to bottom in stacking order by iterating
 * the returned child list in reverse.
 */
static Window deepest_window_at_point(Display *dpy, Window start, int root_x, int root_y) {
    Window current = start;

    for (;;) {
        Window root_ret, parent_ret, *children = NULL;
        unsigned int nchildren = 0;
        Window next = None;

        if (!XQueryTree(dpy, current, &root_ret, &parent_ret, &children, &nchildren))
            break;

        for (int i = (int)nchildren - 1; i >= 0; --i) {
            if (point_in_window(dpy, children[i], root_x, root_y)) {
                next = children[i];
                break;
            }
        }

        if (children) XFree(children);
        if (next == None) break;
        current = next;
    }

    return current;
}

/*
 * Walk a subtree looking for a descendant with the WM_STATE property.
 *
 * In reparenting window manager environments, the actual client window is
 * often not the immediate child under the pointer. WM_STATE is a common
 * marker used to identify the client window managed by the WM.
 *
 * Returns:
 *   matching window, or None if not found
 */
static Window find_wm_state_descendant(Display *dpy, Window win) {
    Atom wm_state = XInternAtom(dpy, "WM_STATE", False);

    if (has_property(dpy, win, wm_state))
        return win;

    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren = 0;
    Window found = None;

    if (!XQueryTree(dpy, win, &root_ret, &parent_ret, &children, &nchildren))
        return None;

    for (unsigned int i = 0; i < nchildren; ++i) {
        found = find_wm_state_descendant(dpy, children[i]);
        if (found != None)
            break;
    }

    if (children) XFree(children);
    return found;
}

/*
 * Resolve the actual target client window under the pointer.
 *
 * Strategy:
 *   1. Find the deepest child at the pointer position.
 *   2. Search that subtree for a WM_STATE window.
 *   3. Fallback to searching the original root child subtree.
 *   4. If no WM_STATE window is found, return the deepest child itself.
 */
static Window resolve_client_window(Display *dpy, Window root_child, int root_x, int root_y) {
    if (root_child == None)
        return None;

    Window deep = deepest_window_at_point(dpy, root_child, root_x, root_y);
    Window client = find_wm_state_descendant(dpy, deep);
    if (client != None)
        return client;

    client = find_wm_state_descendant(dpy, root_child);
    if (client != None)
        return client;

    return deep;
}

/*
 * Query the current pointer target from the root window and resolve
 * the most meaningful target window for filtering.
 *
 * out_root_child receives the direct child of root under the pointer.
 *
 * Returns:
 *   resolved target window, or None on failure
 */
static Window get_pointer_target_window(Display *dpy, int root_x, int root_y, Window *out_root_child) {
    Window root = DefaultRootWindow(dpy);
    Window root_ret = None, child_ret = None;
    int win_x = 0, win_y = 0;
    unsigned int mask = 0;

    if (!XQueryPointer(dpy, root, &root_ret, &child_ret,
                       &root_x, &root_y, &win_x, &win_y, &mask)) {
        return None;
    }

    if (out_root_child)
        *out_root_child = child_ret;

    if (child_ret == None)
        return root_ret;

    return resolve_client_window(dpy, child_ret, root_x, root_y);
}

/*
 * Collect the currently hovered target window and optionally its title/class.
 *
 * root_x and root_y come from the XInput2 button event and represent pointer
 * location in root coordinates at the time of the click.
 *
 * Returns:
 *   1 on success
 *   0 on failure
 */
static int get_target_window_info(Display *dpy,
                                  Window *out_win,
                                  char *title_buf, size_t title_bufsz,
                                  char *class_buf, size_t class_bufsz,
                                  int root_x, int root_y) {
    Window root_child = None;
    Window w = get_pointer_target_window(dpy, root_x, root_y, &root_child);
    if (w == None)
        return 0;

    if (out_win) *out_win = w;

    if (title_buf && title_bufsz > 0) {
        title_buf[0] = '\0';
        get_window_title(dpy, w, title_buf, title_bufsz);
    }

    if (class_buf && class_bufsz > 0) {
        class_buf[0] = '\0';
        get_window_class(dpy, w, class_buf, class_bufsz);
    }

    return 1;
}

/*
 * Subscribe the root window to XI_ButtonPress events from all master devices.
 *
 * This lets us receive right-click events globally after the passive grab.
 */
static void xi_select_events(Display *dpy, Window root) {
    XIEventMask mask;
    unsigned char bits[(XI_LASTEVENT + 7) / 8];
    memset(bits, 0, sizeof(bits));

    mask.deviceid = XIAllMasterDevices;
    mask.mask_len = sizeof(bits);
    mask.mask = bits;

    XISetMask(bits, XI_ButtonPress);
    XISelectEvents(dpy, root, &mask, 1);
    XFlush(dpy);
}

/*
 * Passively grab mouse button 3 (right click) on the root window.
 *
 * Why GrabModeSync for pointer_mode?
 *   Because we want the pointer stream to pause until we decide whether
 *   to replay the event (allow) or consume it (block).
 *
 * Why GrabModeAsync for keyboard_mode?
 *   Keyboard events are unrelated to this filter and should continue normally.
 *
 * Returns:
 *   XInput status code from XIGrabButton()
 */
static int xi_grab_right_button(Display *dpy, Window root) {
    XIEventMask mask;
    unsigned char bits[(XI_LASTEVENT + 7) / 8];
    XIGrabModifiers mods[1];

    memset(bits, 0, sizeof(bits));
    memset(mods, 0, sizeof(mods));

    mask.deviceid = XIAllMasterDevices;
    mask.mask_len = sizeof(bits);
    mask.mask = bits;

    XISetMask(bits, XI_ButtonPress);
    mods[0].modifiers = XIAnyModifier;

    int rc = XIGrabButton(
        dpy,
        XIAllMasterDevices,
        3,
        root,
        None,
        GrabModeSync,
        GrabModeAsync,
        False,
        &mask,
        1,
        mods
    );

    XFlush(dpy);
    return rc;
}

/*
 * Allow the intercepted click to continue to the original client.
 *
 * ReplayPointer reprocesses the event after releasing the synchronous grab,
 * which is the classic X11/XInput way to "let the click through".
 */
static void allow_event(Display *dpy, Time t) {
    XAllowEvents(dpy, ReplayPointer, t);
    XFlush(dpy);
}

/*
 * Consume the intercepted click instead of replaying it.
 *
 * In this program's design, not replaying the ButtonPress is what makes
 * the original target window not receive the right-click as intended.
 */
static void block_event(Display *dpy, Time t) {
    (void)t;
    XAllowEvents(dpy, AsyncPointer, CurrentTime);
    XFlush(dpy);
}

/*
 * Signal handler used for SIGINT and SIGTERM.
 * Only sets the stop flag; no non-signal-safe work is done here.
 */
static void request_stop(int sig) {
    (void)sig;
    g_stop = 1;
}

/*
 * Release runtime resources.
 *
 * Important steps:
 *   1. Ungrab Button3 from the root window.
 *   2. Close the X display.
 *   3. Free loaded denylist strings.
 *   4. Close syslog.
 */
static void cleanup(void) {
    if (g_dpy) {
        XIGrabModifiers mod = { .modifiers = XIAnyModifier };
        XIUngrabButton(g_dpy, XIAllMasterDevices, 3,
                       DefaultRootWindow(g_dpy), 1, &mod);
        XFlush(g_dpy);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
    }

    string_list_free(&g_deny_titles);
    string_list_free(&g_deny_classes);
    closelog();
}

/*
 * Program entry point.
 *
 * High-level flow:
 *   1. Open syslog and the X display.
 *   2. Load configuration from file.
 *   3. Verify XInput2 availability.
 *   4. Subscribe to XI_ButtonPress and grab Button3 globally.
 *   5. For each right-click:
 *        - resolve the target window under the pointer
 *        - read title and WM_CLASS
 *        - compare against loaded denylist
 *        - replay or block the event accordingly
 *   6. Clean up on termination.
 */
int main(void) {
    openlog("x11-block-rightclick", LOG_PID | LOG_NDELAY, LOG_USER);
    setlogmask(LOG_UPTO(LOG_INFO));

    /*
     * Open the default X display from the environment.
     */
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        syslog(LOG_ERR, "Cannot open X display");
        return 1;
    }
    g_dpy = dpy;

    /*
     * Install simple termination handlers.
     */
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    /*
     * Load the denylist config before the event loop starts.
     * If no config can be loaded, fail fast because the program would
     * otherwise run with an unintuitive empty policy.
     */
    char config_path[PATH_MAX];
    if (!get_config_path(config_path, sizeof(config_path))) {
        syslog(LOG_ERR, "Cannot resolve config path");
        cleanup();
        return 1;
    }

    if (access(config_path, R_OK) != 0) {
        syslog(LOG_ERR, "Config file not readable: %s", config_path);
        cleanup();
        return 1;
    }

    if (!load_denylist_from_config(config_path)) {
        cleanup();
        return 1;
    }

    /*
     * Discover the XInput extension opcode used in GenericEvent cookies.
     */
    int event, error;
    if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &event, &error)) {
        syslog(LOG_ERR, "X Input extension not available");
        cleanup();
        return 1;
    }

    /*
     * Require XInput2.
     * Version 2.2 is what your original code requested and is kept here.
     */
    int major = 2, minor = 2;
    if (XIQueryVersion(dpy, &major, &minor) != Success) {
        syslog(LOG_ERR, "XInput2 not available");
        cleanup();
        return 1;
    }

    /*
     * Listen on the root window and grab right-clicks globally.
     */
    Window root = DefaultRootWindow(dpy);
    xi_select_events(dpy, root);

    int grab_rc = xi_grab_right_button(dpy, root);
    if (grab_rc != Success) {
        syslog(LOG_ERR, "XIGrabButton failed: %d", grab_rc);
        cleanup();
        return 1;
    }

    syslog(LOG_INFO, "Started on DISPLAY=%s config=%s", XDisplayString(dpy), config_path);

    /*
     * Main event loop.
     */
    while (!g_stop) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        /*
         * Ignore non-XInput generic events.
         */
        if (ev.xcookie.type != GenericEvent || ev.xcookie.extension != xi_opcode)
            continue;

        if (!XGetEventData(dpy, &ev.xcookie))
            continue;

        if (ev.xcookie.evtype == XI_ButtonPress) {
            XIDeviceEvent *xiev = (XIDeviceEvent *)ev.xcookie.data;

            /*
             * detail == 3 means Button3, i.e. the usual right mouse button.
             */
            if (xiev->detail == 3) {
                int root_x = (int)xiev->root_x;
                int root_y = (int)xiev->root_y;

                Window target = None;
                char title[TITLE_BUFSZ] = {0};
                char klass[CLASS_BUFSZ] = {0};

                get_target_window_info(dpy, &target,
                                       title, sizeof(title),
                                       klass, sizeof(klass),
                                       root_x, root_y);

                int deny = title_matches_denylist(title) || class_matches_denylist(klass);

                if (deny) {
                    // syslog(LOG_INFO,
                    //        "Blocked Button3 win=0x%lx title=\"%s\" class=\"%s\"",
                    //        (unsigned long)target,
                    //        title[0] ? title : "<unknown>",
                    //        klass[0] ? klass : "<unknown>");
                    block_event(dpy, xiev->time);
                } else {
                    allow_event(dpy, xiev->time);
                }
            }
        }

        XFreeEventData(dpy, &ev.xcookie);
    }

    syslog(LOG_INFO, "Stopping");
    cleanup();
    return 0;
}