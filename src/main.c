#define _POSIX_C_SOURCE 200809L

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define XDND_VERSION 5
#define WINDOW_WIDTH 480
#define LINE_HEIGHT 20
#define PADDING 10
#define EXTRA_LINE_SPACING 4

typedef struct {
    Atom aware;
    Atom enter;
    Atom position;
    Atom status;
    Atom leave;
    Atom drop;
    Atom finished;
    Atom selection;

    Atom uri_list;
    Atom plain;
    Atom targets;
    Atom action_copy;
} Atoms;

typedef struct {
    char **paths;
    int count;

    char *uri_data;
    size_t uri_length;

    char *plain_data;
    size_t plain_length;
} FileList;

typedef struct {
    Display *display;
    int screen;
    Window root;
    Window window;

    Atom wm_delete_window;

    GC gc;
    XFontStruct *font;

    Atoms atoms;
    FileList files;

    Window current_target;
    int target_accepts;
    int dragging;
    int drop_sent;
} App;

/* ---------- General helpers ---------- */

static void fatal(const char *message)
{
    fprintf(stderr, "xdragon: %s\n", message);
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t size)
{
    void *p = malloc(size);

    if (!p) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    return p;
}

static void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size);

    if (!p) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }

    return p;
}

static char *xstrdup(const char *s)
{
    char *copy = strdup(s);

    if (!copy) {
        perror("strdup");
        exit(EXIT_FAILURE);
    }

    return copy;
}

static void append_bytes(
    char **buffer,
    size_t *length,
    size_t *capacity,
    const char *data,
    size_t data_length
) {
    if (*length + data_length + 1 > *capacity) {
        size_t new_capacity = *capacity ? *capacity : 128;

        while (new_capacity < *length + data_length + 1)
            new_capacity *= 2;

        *buffer = xrealloc(*buffer, new_capacity);
        *capacity = new_capacity;
    }

    memcpy(*buffer + *length, data, data_length);
    *length += data_length;
    (*buffer)[*length] = '\0';
}

static void append_string(
    char **buffer,
    size_t *length,
    size_t *capacity,
    const char *string
) {
    append_bytes(buffer, length, capacity, string, strlen(string));
}

/* ---------- URI encoding ---------- */

static int uri_character_is_safe(unsigned char c)
{
    return isalnum(c) ||
           c == '-' ||
           c == '_' ||
           c == '.' ||
           c == '~' ||
           c == '/';
}

static void append_uri_path(
    char **buffer,
    size_t *length,
    size_t *capacity,
    const char *path
) {
    static const char hex[] = "0123456789ABCDEF";

    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        unsigned char c = *p;

        if (uri_character_is_safe(c)) {
            append_bytes(buffer, length, capacity, (const char *)&c, 1);
        } else {
            char encoded[3];

            encoded[0] = '%';
            encoded[1] = hex[c >> 4];
            encoded[2] = hex[c & 15];

            append_bytes(buffer, length, capacity, encoded, 3);
        }
    }
}

/* ---------- File data ---------- */

static char *make_uri_list(char **paths, int count, size_t *out_length)
{
    char *result = NULL;
    size_t length = 0;
    size_t capacity = 0;

    for (int i = 0; i < count; i++) {
        char absolute_path[PATH_MAX];

        if (realpath(paths[i], absolute_path) == NULL) {
            fprintf(
                stderr,
                "xdragon: cannot resolve '%s': %s\n",
                paths[i],
                strerror(errno)
            );

            /*
             * Use the supplied path anyway. This allows paths that may
             * disappear after the program starts.
             */
            strncpy(absolute_path, paths[i], sizeof(absolute_path) - 1);
            absolute_path[sizeof(absolute_path) - 1] = '\0';
        }

        append_string(
            &result,
            &length,
            &capacity,
            "file://"
        );

        append_uri_path(
            &result,
            &length,
            &capacity,
            absolute_path
        );

        append_string(
            &result,
            &length,
            &capacity,
            "\r\n"
        );
    }

    if (!result) {
        result = xstrdup("");
        length = 0;
    }

    *out_length = length;
    return result;
}

static char *make_plain_list(char **paths, int count, size_t *out_length)
{
    char *result = NULL;
    size_t length = 0;
    size_t capacity = 0;

    for (int i = 0; i < count; i++) {
        append_string(
            &result,
            &length,
            &capacity,
            paths[i]
        );

        append_string(
            &result,
            &length,
            &capacity,
            i + 1 == count ? "\n" : "\n"
        );
    }

    if (!result) {
        result = xstrdup("");
        length = 0;
    }

    *out_length = length;
    return result;
}

static FileList load_files(int argc, char **argv)
{
    FileList files;

    memset(&files, 0, sizeof(files));

    files.count = argc - 1;
    files.paths = xmalloc(sizeof(char *) * files.count);

    for (int i = 0; i < files.count; i++)
        files.paths[i] = xstrdup(argv[i + 1]);

    files.uri_data = make_uri_list(
        files.paths,
        files.count,
        &files.uri_length
    );

    files.plain_data = make_plain_list(
        files.paths,
        files.count,
        &files.plain_length
    );

    return files;
}

static void free_files(FileList *files)
{
    for (int i = 0; i < files->count; i++)
        free(files->paths[i]);

    free(files->paths);
    free(files->uri_data);
    free(files->plain_data);

    memset(files, 0, sizeof(*files));
}

/* ---------- X11 atoms ---------- */

static void initialize_atoms(Display *display, Atoms *atoms)
{
    atoms->aware = XInternAtom(display, "XdndAware", False);
    atoms->enter = XInternAtom(display, "XdndEnter", False);
    atoms->position = XInternAtom(display, "XdndPosition", False);
    atoms->status = XInternAtom(display, "XdndStatus", False);
    atoms->leave = XInternAtom(display, "XdndLeave", False);
    atoms->drop = XInternAtom(display, "XdndDrop", False);
    atoms->finished = XInternAtom(display, "XdndFinished", False);
    atoms->selection = XInternAtom(display, "XdndSelection", False);

    atoms->uri_list = XInternAtom(display, "text/uri-list", False);
    atoms->plain = XInternAtom(display, "text/plain", False);
    atoms->targets = XInternAtom(display, "TARGETS", False);
    atoms->action_copy = XInternAtom(display, "XdndActionCopy", False);
}

/* ---------- Window drawing ---------- */

static void draw_window(App *app)
{
    Display *display = app->display;
    Window window = app->window;

    XClearWindow(display, window);

    int y = PADDING + app->font->ascent;

    for (int i = 0; i < app->files.count; i++) {
        XDrawString(
            display,
            window,
            app->gc,
            PADDING,
            y,
            app->files.paths[i],
            (int)strlen(app->files.paths[i])
        );

        y += LINE_HEIGHT;
    }

    XFlush(display);
}

static void set_window_title(App *app, const char *title)
{
    XStoreName(app->display, app->window, title);
}

/* ---------- XDND target discovery ---------- */

static int window_supports_xdnd(
    Display *display,
    Window window,
    Atom aware_atom
) {
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        display,
        window,
        aware_atom,
        0,
        1,
        False,
        AnyPropertyType,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &data
    );

    if (data)
        XFree(data);

    return status == Success && actual_type != None;
}

/*
 * XQueryPointer returns the immediate child under the pointer. Many
 * applications use nested child windows, so walk upward until a window
 * advertises XdndAware.
 */
static Window find_xdnd_target(
    Display *display,
    Window starting_window,
    Atom aware_atom
) {
    Window current = starting_window;

    while (current != None) {
        if (current != DefaultRootWindow(display) &&
            window_supports_xdnd(display, current, aware_atom)) {
            return current;
        }

        Window root;
        Window parent;
        Window *children = NULL;
        unsigned int child_count = 0;

        if (!XQueryTree(
                display,
                current,
                &root,
                &parent,
                &children,
                &child_count)) {
            return None;
        }

        if (children)
            XFree(children);

        if (parent == None || parent == root)
            return None;

        current = parent;
    }

    return None;
}

/* ---------- XDND messages ---------- */

static void send_client_message(
    App *app,
    Window target,
    Atom message_type,
    long data0,
    long data1,
    long data2,
    long data3,
    long data4
) {
    XClientMessageEvent event;

    memset(&event, 0, sizeof(event));

    event.type = ClientMessage;
    event.display = app->display;
    event.window = target;
    event.message_type = message_type;
    event.format = 32;

    event.data.l[0] = data0;
    event.data.l[1] = data1;
    event.data.l[2] = data2;
    event.data.l[3] = data3;
    event.data.l[4] = data4;

    XSendEvent(
        app->display,
        target,
        False,
        NoEventMask,
        (XEvent *)&event
    );

    XFlush(app->display);
}

static void send_xdnd_enter(App *app, Window target)
{
    /*
     * Bits 24-31 contain the XDND version. Since we offer only two types,
     * they fit directly into data.l[2] and data.l[3].
     */
    send_client_message(
        app,
        target,
        app->atoms.enter,
        (long)app->window,
        (long)(XDND_VERSION << 24),
        (long)app->atoms.uri_list,
        (long)app->atoms.plain,
        0
    );
}

static void send_xdnd_leave(App *app, Window target)
{
    send_client_message(
        app,
        target,
        app->atoms.leave,
        (long)app->window,
        0,
        0,
        0,
        0
    );
}

static void send_xdnd_position(
    App *app,
    Window target,
    int root_x,
    int root_y
) {
    /*
     * XdndPosition packs root coordinates into one 32-bit value.
     */
    unsigned long packed_coordinates =
        ((unsigned long)(root_x & 0xffff) << 16) |
        ((unsigned long)(root_y & 0xffff));

    send_client_message(
        app,
        target,
        app->atoms.position,
        (long)app->window,
        0,
        (long)packed_coordinates,
        CurrentTime,
        (long)app->atoms.action_copy
    );
}

static void send_xdnd_drop(App *app, Window target)
{
    send_client_message(
        app,
        target,
        app->atoms.drop,
        (long)app->window,
        0,
        CurrentTime,
        0,
        0
    );
}

/* ---------- Selection handling ---------- */

static void send_selection_notify(
    App *app,
    XSelectionRequestEvent *request,
    Atom property
) {
    XSelectionEvent notify;

    memset(&notify, 0, sizeof(notify));

    notify.type = SelectionNotify;
    notify.display = request->display;
    notify.requestor = request->requestor;
    notify.selection = request->selection;
    notify.target = request->target;
    notify.time = request->time;
    notify.property = property;

    XSendEvent(
        app->display,
        request->requestor,
        False,
        0,
        (XEvent *)&notify
    );

    XFlush(app->display);
}

static void handle_selection_request(
    App *app,
    XSelectionRequestEvent *request
) {
    Atom property = request->property;

    /*
     * ICCCM allows a requestor to set property=None. In that case use the
     * requested target atom as the property name.
     */
    if (property == None)
        property = request->target;

    if (request->target == app->atoms.targets) {
        Atom offered_targets[] = {
            app->atoms.targets,
            app->atoms.uri_list,
            app->atoms.plain
        };

        XChangeProperty(
            app->display,
            request->requestor,
            property,
            XA_ATOM,
            32,
            PropModeReplace,
            (unsigned char *)offered_targets,
            sizeof(offered_targets) / sizeof(offered_targets[0])
        );

        send_selection_notify(app, request, property);
        return;
    }

    if (request->target == app->atoms.uri_list) {
        XChangeProperty(
            app->display,
            request->requestor,
            property,
            app->atoms.uri_list,
            8,
            PropModeReplace,
            (unsigned char *)app->files.uri_data,
            (int)app->files.uri_length
        );

        send_selection_notify(app, request, property);
        return;
    }

    if (request->target == app->atoms.plain) {
        XChangeProperty(
            app->display,
            request->requestor,
            property,
            app->atoms.plain,
            8,
            PropModeReplace,
            (unsigned char *)app->files.plain_data,
            (int)app->files.plain_length
        );

        send_selection_notify(app, request, property);
        return;
    }

    /*
     * Unsupported target.
     */
    send_selection_notify(app, request, None);
}

/* ---------- Pointer and drag handling ---------- */

static Window pointer_target(App *app, int *root_x, int *root_y)
{
    Window root_return;
    Window child_return;

    int win_x;
    int win_y;
    unsigned int mask;

    if (!XQueryPointer(
            app->display,
            app->root,
            &root_return,
            &child_return,
            root_x,
            root_y,
            &win_x,
            &win_y,
            &mask)) {
        return None;
    }

    if (child_return == app->window)
        return None;

    return find_xdnd_target(
        app->display,
        child_return,
        app->atoms.aware
    );
}

static void update_drag_target(App *app)
{
    int root_x;
    int root_y;

    Window target = pointer_target(app, &root_x, &root_y);

    if (target != app->current_target) {
        if (app->current_target != None)
            send_xdnd_leave(app, app->current_target);

        app->current_target = target;
        app->target_accepts = 0;

        if (target != None) {
            send_xdnd_enter(app, target);
            send_xdnd_position(app, target, root_x, root_y);
        }
    } else if (target != None) {
        send_xdnd_position(app, target, root_x, root_y);
    }
}

static void begin_drag(App *app)
{
    app->dragging = 1;
    app->drop_sent = 0;
    app->current_target = None;
    app->target_accepts = 0;

    XSetSelectionOwner(
        app->display,
        app->atoms.selection,
        app->window,
        CurrentTime
    );

    XGrabPointer(
        app->display,
        app->window,
        False,
        PointerMotionMask |
        ButtonReleaseMask |
        ButtonPressMask,
        GrabModeAsync,
        GrabModeAsync,
        None,
        None,
        CurrentTime
    );

    set_window_title(app, "xdragon - dragging");
    update_drag_target(app);
}

static void finish_drag(App *app)
{
    if (app->current_target != None) {
        if (app->target_accepts) {
            send_xdnd_drop(app, app->current_target);
            app->drop_sent = 1;
        } else {
            send_xdnd_leave(app, app->current_target);
        }
    }

    XUngrabPointer(app->display, CurrentTime);

    app->dragging = 0;

    if (!app->drop_sent)
        set_window_title(app, "xdragon - rejected");
    else
        set_window_title(app, "xdragon - waiting");
}

/* ---------- X event handling ---------- */

static void handle_client_message(App *app, XClientMessageEvent *event)
{
    if (event->message_type == app->atoms.status) {
        /*
         * XdndStatus:
         * data.l[0] = target window
         * data.l[1] bit 0 = target accepts the offered operation
         */
        if ((Window)event->data.l[0] == app->current_target)
            app->target_accepts = (event->data.l[1] & 1) != 0;

        return;
    }

    if (event->message_type == app->atoms.finished) {
        /*
         * XdndFinished:
         * data.l[0] = target window
         * data.l[1] bit 0 = drop was accepted
         */
        if (app->drop_sent) {
            if (event->data.l[1] & 1)
                set_window_title(app, "xdragon - drop complete");
            else
                set_window_title(app, "xdragon - drop rejected");
        }

        return;
    }
}

static void handle_event(App *app, XEvent *event)
{
    switch (event->type) {
    case Expose:
        if (event->xexpose.count == 0)
            draw_window(app);
        break;

    case ConfigureNotify:
        draw_window(app);
        break;

    case ButtonPress:
        if (event->xbutton.button == Button1 && !app->dragging)
            begin_drag(app);
        break;

    case MotionNotify:
        if (app->dragging)
            update_drag_target(app);
        break;

    case ButtonRelease:
        if (event->xbutton.button == Button1 && app->dragging)
            finish_drag(app);
        break;

    case SelectionRequest:
        handle_selection_request(
            app,
            &event->xselectionrequest
        );
        break;

    case ClientMessage:
        handle_client_message(
            app,
            &event->xclient
        );
        break;

	case KeyPress: {
	   KeySym key = XLookupKeysym(
	   &event->xkey,
	   0
	   );

	   if (key == XK_q || key == XK_Escape)
		   exit(EXIT_SUCCESS);
		break;
	   }

    case DestroyNotify:
        exit(EXIT_SUCCESS);
        break;


    default:
        break;
    }
}

/* ---------- Program startup ---------- */

static void initialize_window(App *app)
{
    Display *display = app->display;

    int width = WINDOW_WIDTH;
    int height = PADDING * 2 +
                 app->files.count * LINE_HEIGHT;

    if (height < 60)
        height = 60;

    if (height > 400)
       height = 400;

    app->window = XCreateSimpleWindow(
        display,
        app->root,
        100,
        100,
        (unsigned int)width,
        (unsigned int)height,
        1,
		WhitePixel(display, app->screen),
        BlackPixel(display, app->screen)
    );

	/* Tell the window manager this is a utility window. */
	Atom wm_window_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
	Atom wm_window_type_dialog = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False); //prevents maximization
	Atom wm_state_stays_on_top = XInternAtom(display, "_NET_WM_STATE_STAYS_ON_TOP", False);

	XChangeProperty(
			display,
			app->window,
			wm_window_type,
			XA_ATOM,
			32,
			PropModeReplace,
			(unsigned char *)&wm_window_type_dialog,
			1);

	XChangeProperty(
			display,
			app->window,
			wm_state,
			XA_ATOM,
			32,
			PropModeReplace,
			(unsigned char *)&wm_state_stays_on_top,
			1);


    XSelectInput(
        display,
        app->window,
        ExposureMask |
	    KeyPressMask |
        ButtonPressMask |
        ButtonReleaseMask |
        PointerMotionMask |
        StructureNotifyMask
    );

    app->font = XLoadQueryFont(display, "fixed");

    if (!app->font)
        app->font = XQueryFont(
            display,
            XGContextFromGC(
                DefaultGC(display, app->screen)
            )
        );

    app->gc = XCreateGC(
        display,
        app->window,
        0,
        NULL
    );

    XSetForeground(
        display,
        app->gc,
        WhitePixel(display, app->screen)
    );

    XStoreName(display, app->window, "xdragon");

    XMapRaised(display, app->window);

    /*
     * Advertise XDND version 5.
     */
    unsigned long version = XDND_VERSION;

    XChangeProperty(
        display,
        app->window,
        app->atoms.aware,
        XA_ATOM,
        32,
        PropModeReplace,
        (unsigned char *)&version,
        1
    );

    XFlush(display);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(
            stderr,
            "Usage: %s FILE...\n",
            argv[0]
        );
        return EXIT_FAILURE;
    }

    App app;

    memset(&app, 0, sizeof(app));

    app.files = load_files(argc, argv);

    app.display = XOpenDisplay(NULL);

    if (!app.display)
        fatal("cannot open X display");

    app.screen = DefaultScreen(app.display);
    app.root = RootWindow(app.display, app.screen);

    initialize_atoms(app.display, &app.atoms);
    initialize_window(&app);

    draw_window(&app);

    for (;;) {
        XEvent event;

        XNextEvent(app.display, &event);
        handle_event(&app, &event);
    }

    free_files(&app.files);
    XCloseDisplay(app.display);

    return EXIT_SUCCESS;
}

