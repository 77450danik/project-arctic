/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * win32k: what the parts of USER give one another (Wine's server/user.h).
 */
#ifndef WIN32K_USER_H
#define WIN32K_USER_H

#include "compat.h"

struct atom_table;
struct region;
struct window;
struct msg_queue;
struct hook_table;
struct window_class;
struct clipboard;
struct thread_input;
struct monitor_info;
struct timeout_user;

/* the USER body of a window station: its name, handles and access stay
 * with the object wineserver keeps (docs/win32k.md) */
struct winstation
{
    struct object      obj;                /* object header */
    object_id_t        id;                 /* of wineserver's object */
    unsigned int       flags;              /* winstation flags */
    struct list        entry;              /* entry in global winstation list */
    struct list        desktops;           /* list of desktops of this winstation */
    struct desktop    *input_desktop;      /* desktop receiving user input */
    struct clipboard  *clipboard;          /* clipboard information */
    struct atom_table *atom_table;         /* global atom table */
    unsigned int       monitor_count;      /* number of monitors */
    struct monitor_info *monitors;         /* window station monitors */
    unsigned long long monitor_serial;     /* winstation monitor update counter */
};

struct key_repeat
{
    int                  enable;           /* enable auto-repeat */
    timeout_t            delay;            /* auto-repeat delay */
    timeout_t            period;           /* auto-repeat period */
    union hw_input       input;            /* the input to repeat */
    user_handle_t        win;              /* target window for input event */
    struct timeout_user *timeout;          /* timeout for repeat */
};

/* the USER body of a desktop, as above */
struct desktop
{
    struct object        obj;              /* object header */
    object_id_t          id;               /* of wineserver's object */
    struct winstation   *winstation;       /* winstation this desktop belongs to */
    timeout_t            input_time;       /* last time this desktop had the input */
    struct list          entry;            /* entry in winstation list of desktops */
    struct list          threads;          /* list of threads connected to this desktop */
    struct window       *top_window;       /* desktop window for this desktop */
    struct window       *msg_window;       /* HWND_MESSAGE top window */
    struct window       *shell_window;     /* shell window for this desktop */
    struct window       *shell_listview;   /* shell list view window for this desktop */
    struct window       *progman_window;   /* progman window for this desktop */
    struct window       *taskman_window;   /* taskman window for this desktop */
    struct hook_table   *global_hooks;     /* table of global hooks on this desktop */
    struct list          hotkeys;          /* list of registered hotkeys */
    struct list          shell_hooks;      /* list of windows that get shell hook messages */
    struct list          pointers;         /* list of active pointers */
    struct timeout_user *close_timeout;    /* timeout before closing the desktop */
    struct thread_input *foreground_input; /* thread input of foreground thread */
    process_id_t         foreground_pid;   /* id of the foreground process */
    process_id_t         foreground_allowed; /* AllowSetForegroundWindow: it may take the foreground, -1 any, 0 none */
    unsigned int         users;            /* processes and threads using this desktop */
    unsigned char        alt_pressed;      /* last key press was Alt (used to determine msg on release) */
    struct key_repeat    key_repeat;       /* key auto-repeat */
    unsigned int         clip_flags;       /* last cursor clip flags */
    user_handle_t        cursor_win;       /* window that contains the cursor */
    user_handle_t        snap_window;      /* window dragged to a screen edge, for the compositor */
    struct rectangle     snap_rect;        /* where it goes if it is let go there */
    desktop_shm_t       *shared;           /* desktop session shared memory */
};

/* clipboard functions */

extern void cleanup_clipboard_window( struct desktop *desktop, user_handle_t window );
extern void cleanup_clipboard_thread( struct w32_thread *thread );

/* hook functions */

extern void remove_thread_hooks( struct w32_thread *thread );
extern unsigned int get_active_hooks(void);
extern struct w32_thread *get_first_global_hook( struct desktop *desktop, int id );
extern void add_desktop_hook_count( struct desktop *desktop, struct w32_thread *thread, int count );

/* queue functions */

extern void free_msg_queue( struct w32_thread *thread );
extern struct hook_table *get_queue_hooks( struct w32_thread *thread );
extern void set_queue_hooks( struct w32_thread *thread, struct hook_table *hooks );
extern void add_queue_hook_count( struct w32_thread *thread, unsigned int index, int count );
extern void inc_queue_paint_count( struct w32_thread *thread, int incr );
extern void queue_cleanup_window( struct w32_thread *thread, user_handle_t win );
extern int init_thread_queue( struct w32_thread *thread );
extern void attach_thread_input( struct msg_queue *queue_from, struct msg_queue *queue_to );
extern void detach_thread_input( struct msg_queue *queue_from, struct msg_queue *queue_to, struct desktop *desktop );
extern int is_desktop_cursor_hidden( struct desktop *desktop );
extern void set_clip_rectangle( struct desktop *desktop, const struct rectangle *rect,
                                unsigned int flags, int reset );
extern void update_cursor_pos( struct desktop *desktop );
extern void shell_hook_notify( struct desktop *desktop, unsigned int code, user_handle_t window );
extern void free_shell_hooks( struct desktop *desktop );
extern void post_message( user_handle_t win, unsigned int message,
                          lparam_t wparam, lparam_t lparam );
extern void send_notify_message( user_handle_t win, unsigned int message,
                                 lparam_t wparam, lparam_t lparam );
extern void post_win_event( struct w32_thread *thread, unsigned int event,
                            user_handle_t win, unsigned int object_id,
                            unsigned int child_id, client_ptr_t proc,
                            const WCHAR *module, data_size_t module_size,
                            user_handle_t handle );
extern void free_hotkeys( struct desktop *desktop, user_handle_t window );
extern void free_pointers( struct desktop *desktop );
extern void set_rawinput_process( struct w32_process *process, int enable );

/* window functions */

extern struct w32_process *get_top_window_owner( struct desktop *desktop );
extern void get_virtual_screen_rect( struct desktop *desktop, struct rectangle *rect, int is_raw );
extern void update_windows_monitor_dpi( struct winstation *winstation );
extern void post_desktop_message( struct desktop *desktop, unsigned int message,
                                  lparam_t wparam, lparam_t lparam );
extern void free_window_handle( struct window *win );
extern void destroy_thread_windows( struct w32_thread *thread );
extern int is_child_window( user_handle_t parent, user_handle_t child );
extern struct w32_thread *make_window_foreground( struct desktop *desktop, user_handle_t window,
                                                  int *is_desktop, int *set_foreground );
extern int is_window_visible( user_handle_t window );
extern int is_window_transparent( user_handle_t window );
extern int make_window_active( user_handle_t window );
extern struct w32_thread *get_window_thread( user_handle_t handle );
extern user_handle_t shallow_window_from_point( struct desktop *desktop, int x, int y );
extern struct w32_thread *window_thread_from_point( user_handle_t scope, int x, int y );
extern user_handle_t find_window_to_repaint( user_handle_t parent, struct w32_thread *thread );
extern void set_window_rect_visible( user_handle_t window, struct rectangle rect );

/* windows station functions (the USER bodies, winstation.c) */

extern struct winstation *get_visible_winstation(void);
extern struct desktop *get_input_desktop( struct winstation *winstation );
extern int set_input_desktop( struct winstation *winstation, struct desktop *new_desktop );
extern struct winstation *get_process_winstation( struct w32_process *process, unsigned int access );
extern struct desktop *get_thread_desktop( struct w32_thread *thread, unsigned int access );
/* a desktop of a request: ntdll gives win32k's number of the desktop in the
 * field the handle was in, after wineserver checked the handle's access */
extern struct desktop *get_desktop_obj( struct w32_process *process, obj_handle_t id, unsigned int access );
extern void set_process_default_desktop( struct w32_process *process, struct desktop *desktop );
extern void release_thread_desktop( struct w32_thread *thread, int close );
extern void close_process_desktop( struct w32_process *process );

/* session.c */
extern session_shm_t *shared_session;
int win32k_session_init(void);
volatile void *alloc_shared_object(mem_size_t shm_size);
void free_shared_object(volatile void *object_shm);
void invalidate_shared_object(volatile void *object_shm);
struct obj_locator get_shared_object_locator(const volatile void *object_shm);

/* atom.c */
int win32k_atoms_init(void);
struct atom_table *get_global_atom_table(void);
struct atom_table *get_user_atom_table(void);
atom_t add_atom(struct atom_table *table, struct unicode_str str);
atom_t find_atom(struct atom_table *table, struct unicode_str str);
atom_t grab_atom(struct atom_table *table, atom_t atom);
void release_atom(struct atom_table *table, atom_t atom);

/* class.c */
struct window_class;
void destroy_process_classes(struct w32_process *process);
struct window_class *grab_class(struct w32_process *process, atom_t atom, mod_handle_t instance,
				struct obj_locator *locator);
void release_class(struct window_class *class);
int is_desktop_class(struct window_class *class);
int is_message_class(struct window_class *class);
int get_class_style(struct window_class *class);
atom_t get_class_atom(struct window_class *class);
unsigned int get_class_fnid(struct window_class *class, data_size_t *extra_size, data_size_t *private_size);
client_ptr_t get_class_wndproc(struct window_class *class, bool *is_ansi);
client_ptr_t get_class_client_ptr(struct window_class *class);

/* window.c */
struct window_class *get_window_class(user_handle_t window);

/* handles.c (the server's user.c) */
extern user_handle_t alloc_user_handle( void *ptr, volatile void *shared, unsigned short type );
extern void *get_user_object( user_handle_t handle, unsigned short type );
extern void *get_user_object_handle( user_handle_t *handle, unsigned short type );
extern user_handle_t get_user_full_handle( user_handle_t handle );
extern void *free_user_handle( user_handle_t handle );
extern void *next_user_handle( user_handle_t *handle, unsigned short type );
extern void free_process_user_handles( struct w32_process *process );

/* validate a window handle and return the full handle */
static inline user_handle_t get_valid_window_handle( user_handle_t win )
{
    if (get_user_object_handle( &win, NTUSER_OBJ_WINDOW )) return win;
    set_win32_error( ERROR_INVALID_WINDOW_HANDLE );
    return 0;
}

/* region.c */
struct region;
extern struct region *create_empty_region(void);
extern struct region *create_region_from_req_data( const void *data, data_size_t size );
extern void free_region( struct region *region );
extern void set_region_rect( struct region *region, const struct rectangle *rect );
extern struct rectangle *get_region_data( const struct region *region, data_size_t max_size,
                                          data_size_t *total_size );
extern struct rectangle *get_region_data_and_free( struct region *region, data_size_t max_size,
                                                   data_size_t *total_size );
extern int is_region_empty( const struct region *region );
extern int is_region_equal( const struct region *region1, const struct region *region2 );
extern void get_region_extents( const struct region *region, struct rectangle *rect );
extern void offset_region( struct region *region, int x, int y );
extern void mirror_region( const struct rectangle *client_rect, struct region *region );
extern void scale_region( struct region *region, struct ratio dpi_from, struct ratio dpi_to );
extern struct region *copy_region( struct region *dst, const struct region *src );
extern struct region *intersect_region( struct region *dst, const struct region *src1,
                                        const struct region *src2 );
extern struct region *subtract_region( struct region *dst, const struct region *src1,
                                       const struct region *src2 );
extern struct region *union_region( struct region *dst, const struct region *src1,
                                    const struct region *src2 );
extern struct region *xor_region( struct region *dst, const struct region *src1,
                                  const struct region *src2 );
extern int point_in_region( struct region *region, int x, int y );
extern int rect_in_region( struct region *region, const struct rectangle *rect );

/* rectangles (Wine's server/user.h) */

/* checks if two rectangles are identical */
static inline int is_rect_equal( const struct rectangle *rect1, const struct rectangle *rect2 )
{
    return (rect1->left == rect2->left && rect1->right == rect2->right &&
            rect1->top == rect2->top && rect1->bottom == rect2->bottom);
}

static inline int is_rect_empty( const struct rectangle *rect )
{
    return (rect->left >= rect->right || rect->top >= rect->bottom);
}

static inline int point_in_rect( const struct rectangle *rect, int x, int y )
{
    return (x >= rect->left && x < rect->right && y >= rect->top && y < rect->bottom);
}

static inline int scale_dpi( int val, struct ratio dpi_from, struct ratio dpi_to )
{
    /* Arctic: signed, or a negative coordinate turns into a huge positive one */
    long long num = dpi_to.num * dpi_from.den, den = dpi_from.num * dpi_to.den;
    if (val >= 0) return (int)((val * num + (den / 2)) / den);
    return (int)((val * num - (den / 2)) / den);
}

static inline void scale_dpi_rect( struct rectangle *rect, struct ratio dpi_from, struct ratio dpi_to )
{
    rect->left   = scale_dpi( rect->left, dpi_from, dpi_to );
    rect->top    = scale_dpi( rect->top, dpi_from, dpi_to );
    rect->right  = scale_dpi( rect->right, dpi_from, dpi_to );
    rect->bottom = scale_dpi( rect->bottom, dpi_from, dpi_to );
}

/* offset the coordinates of a rectangle */
static inline void offset_rect( struct rectangle *rect, int offset_x, int offset_y )
{
    rect->left   += offset_x;
    rect->top    += offset_y;
    rect->right  += offset_x;
    rect->bottom += offset_y;
}

/* mirror a rectangle respective to the window client area */
static inline void mirror_rect( const struct rectangle *client_rect, struct rectangle *rect )
{
    int width = client_rect->right - client_rect->left;
    int tmp = rect->left;
    rect->left = width - rect->right;
    rect->right = width - tmp;
}

/* compute the intersection of two rectangles; return 0 if the result is empty */
static inline int intersect_rect( struct rectangle *dst, const struct rectangle *src1, const struct rectangle *src2 )
{
    dst->left   = max( src1->left, src2->left );
    dst->top    = max( src1->top, src2->top );
    dst->right  = min( src1->right, src2->right );
    dst->bottom = min( src1->bottom, src2->bottom );
    return !is_rect_empty( dst );
}

static inline void reset_bounds( struct rectangle *bounds )
{
    bounds->left = bounds->top = INT_MAX;
    bounds->right = bounds->bottom = INT_MIN;
}

static inline void union_rect( struct rectangle *dest, const struct rectangle *src1, const struct rectangle *src2 )
{
    if (is_rect_empty( src1 ))
    {
        if (is_rect_empty( src2 ))
        {
            reset_bounds( dest );
            return;
        }
        else *dest = *src2;
    }
    else
    {
        if (is_rect_empty( src2 )) *dest = *src1;
        else
        {
            dest->left   = min( src1->left, src2->left );
            dest->right  = max( src1->right, src2->right );
            dest->top    = min( src1->top, src2->top );
            dest->bottom = max( src1->bottom, src2->bottom );
        }
    }
}

#endif
