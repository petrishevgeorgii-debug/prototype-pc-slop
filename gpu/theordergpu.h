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
/* PROTOTYPE: each save's own name and detail (its subtitle, e.g. "MISSION( 3 )", and the difficulty)
 * and its folder (the game's files, for the mission's picture) for the game's save screen, before
 * theordergpu_choice_begin; the items are the saves only. */
void theordergpu_choice_rows(const char *const *names, const char *const *details, const char *const *folders, int count);
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
    int menu_back; /* the right button held over the game's menus (the mouse in the menus): back */
    int press_options; /* the port presses Options (the trainer leaves the pause menu: ExitPause) */
    int press_pause;   /* the port clicks the touchpad's right side (the trainer opens the pause menu) */
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
enum { THEORDER_MENU_VIDEO = 0, THEORDER_MENU_CONTROLLER = 1, THEORDER_MENU_DISPLAY = 2, THEORDER_MENU_TRAINER = 3,
       THEORDER_MENU_PAGES = 4 };
/* PROTOTYPE's trainer (page 3, its own screen in the pause menu, F1 or holding the touchpad;
 * game/p1/trainer.cpp does what the rows say): two switches and two one-shot actions. */
enum { THEORDER_TRAINER_GOD = 0, THEORDER_TRAINER_CRITICAL_MASS = 1, THEORDER_TRAINER_EP = 2,
       THEORDER_TRAINER_UNLOCK = 3, THEORDER_TRAINER_ROWS = 4 };
/* A request to open (or close) the trainer: F1 in the window, or the touchpad held (runtime_pad.c). */
void theordergpu_trainer_request(void);
/* The mouse as a pointer in the game's menus (gpu/shim/port_pointer.cpp, game/p1/menu_mouse.cpp):
 * active while the menus are up and the Mouse in menus setting is on; x, y: the place in the
 * game's frame (0..1); inside: over the game's picture; buttons: 1 left, 2 right, 4 middle;
 * wheel: notches since the last take (up positive). */
typedef struct {
    int active, inside;
    float x, y;
    unsigned buttons;
    int wheel;
} TheOrderPointer;
/* mode 1: the game's menus are up, 0: playing, -1: unchanged; returns 1 while the cursor shows. */
int theordergpu_pointer_mode(int mode);
void theordergpu_pointer_take(TheOrderPointer *out);
/* Left clicks in the window since the last call (in game, in the menus, in a movie): the game
 * layer skips a playing movie with one, as the PC version did (game/p1/menu_mouse.cpp). */
int theordergpu_pointer_clicks(void);
/* A save list for the game's own save screen (PROTOTYPE's storage.gfx, game/p1/save_menu.cpp):
 * the saves the system dialog was asked to list (src/runtime_services.c). */
typedef struct TheOrderSaveRow {
    char name[160], detail[160];
    char folder[512]; /* the save's folder on this computer, or "" */
    int year, month, day, hour, minute; /* hour < 0: no date */
} TheOrderSaveRow;
typedef struct TheOrderSaveMenu {
    int mode;    /* 1 save, 2 load */
    int has_new; /* the list offers a new save (the dialog's first item) */
    int count;   /* saves in rows */
    TheOrderSaveRow rows[32];
} TheOrderSaveMenu;

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
    /* The mouse in the game's menus (theordergpu_pointer_mode, theordergpu_pointer_take). */
    int (*pointer_mode)(int mode);
    void (*pointer_take)(TheOrderPointer *out);
    int (*pointer_clicks)(void);
    /* The trainer: requests to open it since the last call (F1, the touchpad held); the one-shot
     * actions chosen since the last call (1 << THEORDER_TRAINER_EP, 1 << ..._UNLOCK); a press of
     * Options (in the pause menu: back to the game) and a click of the touchpad's right side (in
     * the game: the pause menu, which opens on its map) for the game. The switches are get(3, row). */
    int (*trainer_open)(void);
    unsigned (*trainer_actions)(void);
    void (*press_options)(void);
    void (*press_pause)(void);
    /* The Lua console (the overlay's, the key under Esc; gpu/shim/port_console.cpp): the next typed
     * command into `out` (0 when none), and what one printed or returned (kind 1 output, 2 an
     * error, 3 a note). game/p1/console.cpp runs them in the game's Lua. */
    int (*console_take)(char *out, int size);
    void (*console_print)(const char *text, int kind);
    /* Hide HUD: presses of its key (F3) since the last call; the game layer flips the game's own
     * Hide HUD switch (game/p1/hud.cpp). */
    int (*hud_toggle)(void);
    /* The save list on the game's own save screen: the game layer says the screen works
     * (save_menu_ready), takes a list to show (save_menu_take: 0 when none) and gives the choice
     * (save_menu_done: the dialog's item, -2 cancelled). */
    void (*save_menu_ready)(void);
    int (*save_menu_take)(TheOrderSaveMenu *out);
    void (*save_menu_done)(int choice);
} TheOrderGameMenu;
const TheOrderGameMenu *theordergpu_game_menu(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned theordergpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
