/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef THEORDERGPU_H
#define THEORDERGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} TheOrderGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void theordergpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int theordergpu_init(const TheOrderGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t theordergpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int theordergpu_handle_fault(void *ucontext, void *address);
/* THEORDER_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void theordergpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int theordergpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int theordergpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int theordergpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int theordergpu_choice_poll(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int theordergpu_overlay_captures_input(void);
/* The game's field of view constants in the loaded image (writable): the in-game menu scales them. */
void theordergpu_set_fov_sites(double *const *sites, int count);
/* Mouse input since the last call (window thread, gpu/shim/window.cpp): motion in pixels,
 * buttons as SDL_BUTTON_MASK bits, wheel in notches (up positive); all zero unless captured
 * (mouse look on, window focused, menu and text entry closed). sensitivity: the menu's,
 * 1.0 = 100%. */
typedef struct {
    float dx, dy, sensitivity;
    uint32_t buttons;
    int captured;
    int wheel;
} TheOrderMouse;
void theordergpu_mouse_take(TheOrderMouse *out);
/* Gamepad options (theorder.ini, the port menu, the game's Controller screen, the launcher), read
 * live by runtime_pad.c: vibration strength in percent (0 off), light_bar 1: the game's colours,
 * menu_vibration: a tap in the game's menus, 0 off, 1 on pressing, 2 also on moving;
 * extra_vibration 1: the port's own effects in the game (consuming, the whip fist...);
 * haptics 1: a DualSense on USB plays everything on its HD haptics (runtime_haptics.c);
 * haptics_sound 0..3: how much of the game's sound (impacts, explosions) they also play. */
typedef struct {
    int vibration, light_bar, menu_vibration, extra_vibration, haptics, haptics_sound;
} TheOrderPadOptions;
void theordergpu_pad_options(TheOrderPadOptions *out);
/* PROTOTYPE: the view axis (unit vector, world space) of the last frame's camera, a counter that
 * grows with every new camera and its time (steady clock, ns); 0 before the first camera (it is
 * found while the camera is tracked, THEORDER_UPSCALER not "none"). For mouse look (runtime_pad.c). */
int theordergpu_camera_forward(float *out, uint64_t *serial, uint64_t *ns);
/* PROTOTYPE: the port's settings as rows of the game's own options screens, page 0 Options > Video,
 * page 1 Options > Controller and page 2 the Display screen the port adds (scripts/proto_menu.py adds the rows,
 * game/p1/options_menu.cpp answers the screens' calls). Every row is a choice: `choices` texts,
 * the current one `get`, `set` applies one (live) and `reset` the default; `save` writes the
 * settings file. Texts are in the game's language. Called on the game's thread. */
enum { THEORDER_MENU_VIDEO = 0, THEORDER_MENU_CONTROLLER = 1, THEORDER_MENU_DISPLAY = 2, THEORDER_MENU_PAGES = 3 };
typedef struct TheOrderGameMenu {
    int (*count)(int page);
    const char *(*label)(int page, int row);
    int (*choices)(int page, int row);
    const char *(*choice)(int page, int row, int index);
    int (*get)(int page, int row);
    void (*set)(int page, int row, int index);
    void (*reset)(int page, int row);
    void (*save)(void);
    /* The game's own database setting (an int) the row also switches, or NULL: kept at 1 while
     * the row's choice is not 0 and 0 when it is (Vibration: the game's own rumble switch). */
    const char *(*linked)(int page, int row);
    /* Quit to the desktop (the game's own Quit buttons, fe_ExitGame): saves the settings and
     * closes the window as its close button does. Returns at once; the process ends shortly. */
    void (*quit)(void);
} TheOrderGameMenu;
const TheOrderGameMenu *theordergpu_game_menu(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned theordergpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
