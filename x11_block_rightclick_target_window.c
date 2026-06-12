// x11_block_rightclick_target_window.c
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <syslog.h>

static const char *denylist_titles[] = {
    "LXQt Panel",
    "pcmanfm-desktop0",
    "pcmanfm-desktop1",
    "pcmanfm-desktop2",
    "pcmanfm-desktop3",
    NULL
};

static const char *denylist_classes[] = {
    "pcmanfm",
    "lxqt-panel",
    NULL
};

static int xi_opcode = -1;
static Display *g_dpy = NULL;
static volatile sig_atomic_t g_stop = 0;

static int title_matches_denylist(const char *title) {
    if (!title || !*title) return 0;
    for (int i = 0; denylist_titles[i]; ++i) {
        if (strstr(title, denylist_titles[i]) != NULL)
            return 1;
    }
    return 0;
}

static int class_matches_denylist(const char *class_) {
    if (!class_ || !*class_) return 0;
    for (int i = 0; denylist_classes[i]; ++i) {
        if (strstr(class_, denylist_classes[i]) != NULL)
            return 1;
    }
    return 0;
}

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

static int get_window_title(Display *dpy, Window win, char *buf, size_t bufsz) {
    if (get_window_title_net_wm_name(dpy, win, buf, bufsz))
        return 1;
    if (get_window_title_wm_name(dpy, win, buf, bufsz))
        return 1;
    return 0;
}

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

static void allow_event(Display *dpy, Time t) {
    XAllowEvents(dpy, ReplayPointer, t);
    XFlush(dpy);
}

static void block_event(Display *dpy, Time t) {
    XAllowEvents(dpy, AsyncPointer, t);
    XFlush(dpy);
}

static void request_stop(int sig) {
    (void)sig;
    g_stop = 1;
}

static void cleanup(void) {
    if (g_dpy) {
        XIUngrabButton(g_dpy, XIAllMasterDevices, 3,
                       DefaultRootWindow(g_dpy), 1, &(XIGrabModifiers){ .modifiers = XIAnyModifier });
        XFlush(g_dpy);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
    }
    closelog();
}

int main(void) {
    openlog("x11-block-rightclick", LOG_PID | LOG_NDELAY, LOG_USER);
    setlogmask(LOG_UPTO(LOG_INFO));

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        syslog(LOG_ERR, "Cannot open X display");
        return 1;
    }
    g_dpy = dpy;

    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    int event, error;
    if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &event, &error)) {
        syslog(LOG_ERR, "X Input extension not available");
        cleanup();
        return 1;
    }

    int major = 2, minor = 2;
    if (XIQueryVersion(dpy, &major, &minor) != Success) {
        syslog(LOG_ERR, "XInput2 not available");
        cleanup();
        return 1;
    }

    Window root = DefaultRootWindow(dpy);
    xi_select_events(dpy, root);

    int grab_rc = xi_grab_right_button(dpy, root);
    if (grab_rc != Success) {
        syslog(LOG_ERR, "XIGrabButton failed: %d", grab_rc);
        cleanup();
        return 1;
    }

    syslog(LOG_INFO, "Started on DISPLAY=%s", XDisplayString(dpy));

    while (!g_stop) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        if (ev.xcookie.type != GenericEvent || ev.xcookie.extension != xi_opcode)
            continue;

        if (!XGetEventData(dpy, &ev.xcookie))
            continue;

        if (ev.xcookie.evtype == XI_ButtonPress) {
            XIDeviceEvent *xiev = (XIDeviceEvent *)ev.xcookie.data;

            if (xiev->detail == 3) {
                int root_x = (int)xiev->root_x;
                int root_y = (int)xiev->root_y;

                Window target = None;
                char title[512] = {0};
                char klass[256] = {0};

                get_target_window_info(dpy, &target,
                                       title, sizeof(title),
                                       klass, sizeof(klass),
                                       root_x, root_y);

                int deny = title_matches_denylist(title) || class_matches_denylist(klass);

                if (deny) {
                    syslog(LOG_INFO, "Blocked Button3 win=0x%lx title=\"%s\" class=\"%s\"",
                           (unsigned long)target,
                           title[0] ? title : "<unknown>",
                           klass[0] ? klass : "<unknown>");
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
