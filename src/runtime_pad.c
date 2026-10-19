/* libScePad on SDL3 gamepads, with a keyboard fallback. SDL events are pumped
 * by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Default keyboard layout (The Order; THEORDER_KEY_MAP changes it, see apply_button_map, and
 * the PROTOTYPE launcher sends its own, scripts/proto_controls.py):
 *   WASD left stick, arrow keys right stick, Space Cross, LShift Circle,
 *   E Square, Q Triangle, 1 L1, 3 R1, R L2, F R2, Z L3, C R3,
 *   Enter Options, Tab left touchpad (photo mode), Backspace right touchpad (pause menu),
 *   IJKL d-pad (I up, K down, J left, L right); mouse: left R2, right L2, middle R3, side L1/R1. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/theordergpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
    /* Host-only sources, never shown to the game: a click on one side of the touchpad from a
     * button or key without a touch surface (play_click turns them into a touch and a click). */
    CLICK_LEFT=0x40000000, CLICK_RIGHT=0x80000000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}

/* A click without a touch surface (Back/View, Tab, Backspace, the pad file) is played like a
 * finger: the touch is reported CLICK_LEAD_US before the button, the button stays at least
 * CLICK_HOLD_US, and the touch lingers CLICK_TAIL_US after the release: games read a click
 * on its release with the finger still down, and ignore a touch that comes and goes with the
 * button. The side is chosen when the click starts and kept: the right source with L1 held is
 * a left click (for pads without a touchpad). */
enum { CLICK_LEAD_US=50000, CLICK_HOLD_US=50000, CLICK_TAIL_US=100000 };
static struct { int side; uint64_t pressed, released; } click={-1,0,0};
static void play_click(PadData *d) {
    const uint64_t now=d->timestamp;
    uint32_t sources=d->buttons & (CLICK_LEFT|CLICK_RIGHT);
    d->buttons&=~(CLICK_LEFT|CLICK_RIGHT);
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) { /* a replay, or a pad without fingers */
        d->buttons&=~BTN_TOUCHPAD;
        sources|=CLICK_LEFT;
    }
    const int side=(sources & CLICK_LEFT) ? 0 : (sources & CLICK_RIGHT) ? !(d->buttons & BTN_L1) : -1;
    if (side>=0 && (click.side<0 || click.released)) { click.side=side; click.pressed=now; click.released=0; }
    else if (side<0 && click.side>=0 && !click.released) click.released=now;
    if (click.side<0) return;
    const uint64_t down=click.pressed+CLICK_LEAD_US;
    uint64_t up=down+CLICK_HOLD_US;
    if (!click.released) up=UINT64_MAX;
    else if (click.released>up) up=click.released;
    if (now>=up && now-up>=CLICK_TAIL_US) { click.side=-1; return; }
    if (d->touch_count<2) {
        d->touches[d->touch_count]=(PadTouch){.x=click.side ? 1440 : 480,.y=471,.id=d->touch_count};
        ++d->touch_count;
    }
    if (now>=down && now<up) d->buttons|=BTN_TOUCHPAD;
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* Button sources: a host button or key and the PS4 button it presses. The defaults below can be
 * changed per PS4 button with THEORDER_PAD_MAP (gamepad) and THEORDER_KEY_MAP (keyboard and mouse),
 * e.g. THEORDER_KEY_MAP="cross=Q,square=Left Shift" or THEORDER_PAD_MAP="cross=b,circle=a":
 * comma-separated <ps4 button>=<SDL key or gamepad button name>. The first entry for a PS4 button
 * drops its default sources, further entries add to it. A leading "!" drops every default (the
 * PROTOTYPE launcher sends its whole layout). "l1+r1=Tab" presses several buttons at once.
 * Keyboard map sources are also "Mouse Left", "Mouse Right", "Mouse Middle", "Mouse X1",
 * "Mouse X2", "Wheel Up" and "Wheel Down" (a short press per notch), and its targets also
 * lstick_up/down/left/right and rstick_up/down/left/right (full deflection while held).
 * touchpad_left (alias touchpad) and touchpad_right are the two sides of the touchpad click:
 * photo mode and the pause menu. Back/View (Share on a DualSense) is the right click, and with
 * L1 held the left one. Gamepad sticks, triggers, the real touchpad click and the overlay
 * shortcuts are not remapped. */
enum {
    LS_UP=0x200000, LS_DOWN=0x400000, LS_LEFT=0x800000, LS_RIGHT=0x1000000,
    RS_UP=0x2000000, RS_DOWN=0x4000000, RS_LEFT=0x8000000, RS_RIGHT=0x10000000,
    STICK_BITS=LS_UP|LS_DOWN|LS_LEFT|LS_RIGHT|RS_UP|RS_DOWN|RS_LEFT|RS_RIGHT,
};
static const struct { const char *name; uint32_t ps; } ps_names[]={
    {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
    {"options",BTN_OPTIONS}, {"touchpad",CLICK_LEFT}, {"touchpad_left",CLICK_LEFT}, {"touchpad_right",CLICK_RIGHT},
    {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    {"lstick_up",LS_UP}, {"lstick_down",LS_DOWN}, {"lstick_left",LS_LEFT}, {"lstick_right",LS_RIGHT},
    {"rstick_up",RS_UP}, {"rstick_down",RS_DOWN}, {"rstick_left",RS_LEFT}, {"rstick_right",RS_RIGHT},
};
/* Keyboard map sources past the scancodes: mouse buttons (SDL_BUTTON_LEFT..X2) and wheel notches. */
enum { SRC_MOUSE=0x1000, SRC_WHEEL_UP=0x1100, SRC_WHEEL_DOWN=0x1101 };
static const struct { const char *name; int source; } mouse_names[]={
    {"mouse left",SRC_MOUSE+SDL_BUTTON_LEFT}, {"mouse right",SRC_MOUSE+SDL_BUTTON_RIGHT},
    {"mouse middle",SRC_MOUSE+SDL_BUTTON_MIDDLE}, {"mouse x1",SRC_MOUSE+SDL_BUTTON_X1},
    {"mouse x2",SRC_MOUSE+SDL_BUTTON_X2}, {"wheel up",SRC_WHEEL_UP}, {"wheel down",SRC_WHEEL_DOWN},
};
typedef struct { int source; uint32_t ps; } ButtonSource;
typedef struct { ButtonSource entries[96]; size_t count; } ButtonMap;
static ButtonMap pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,CLICK_RIGHT},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
/* The Order's layout (THEORDER_KEY_MAP replaces it; PROTOTYPE's comes from the launcher). */
static ButtonMap key_map={{
    {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_RETURN,BTN_CROSS},
    {SDL_SCANCODE_LSHIFT,BTN_CIRCLE}, {SDL_SCANCODE_ESCAPE,BTN_CIRCLE},
    {SDL_SCANCODE_E,BTN_SQUARE}, {SDL_SCANCODE_Q,BTN_TRIANGLE},
    {SDL_SCANCODE_1,BTN_L1}, {SDL_SCANCODE_3,BTN_R1},
    {SDL_SCANCODE_R,BTN_L2}, {SDL_SCANCODE_F,BTN_R2},
    {SDL_SCANCODE_Z,BTN_L3}, {SDL_SCANCODE_C,BTN_R3},
    {SDL_SCANCODE_TAB,CLICK_LEFT}, {SDL_SCANCODE_BACKSPACE,CLICK_RIGHT},
    {SDL_SCANCODE_I,BTN_UP}, {SDL_SCANCODE_K,BTN_DOWN},
    {SDL_SCANCODE_J,BTN_LEFT}, {SDL_SCANCODE_L,BTN_RIGHT},
    {SDL_SCANCODE_UP,BTN_UP}, {SDL_SCANCODE_DOWN,BTN_DOWN},
    {SDL_SCANCODE_LEFT,BTN_LEFT}, {SDL_SCANCODE_RIGHT,BTN_RIGHT},
    {SDL_SCANCODE_W,LS_UP}, {SDL_SCANCODE_S,LS_DOWN}, {SDL_SCANCODE_A,LS_LEFT}, {SDL_SCANCODE_D,LS_RIGHT},
    {SDL_SCANCODE_UP,RS_UP}, {SDL_SCANCODE_DOWN,RS_DOWN}, {SDL_SCANCODE_LEFT,RS_LEFT}, {SDL_SCANCODE_RIGHT,RS_RIGHT},
    {SRC_MOUSE+SDL_BUTTON_LEFT,BTN_R2}, {SRC_MOUSE+SDL_BUTTON_RIGHT,BTN_L2}, {SRC_MOUSE+SDL_BUTTON_MIDDLE,BTN_R3},
    {SRC_MOUSE+SDL_BUTTON_X1,BTN_L1}, {SRC_MOUSE+SDL_BUTTON_X2,BTN_R1},
},35};

static char *trim(char *text) {
    while (*text==' ' || *text=='\t') ++text;
    char *end=text+strlen(text);
    while (end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) *--end=0;
    return text;
}
static uint32_t ps_from_name(char *names) {
    uint32_t ps=0;
    for (char *part=names, *next; part; part=next) {
        if ((next=strchr(part,'+'))) *next++=0;
        part=trim(part);
        uint32_t one=0;
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!SDL_strcasecmp(part,ps_names[i].name)) one=ps_names[i].ps;
        if (!one) return 0;
        ps|=one;
    }
    return ps;
}
static int key_source(const char *name) {
    for (size_t i=0;i<sizeof(mouse_names)/sizeof(*mouse_names);++i)
        if (!SDL_strcasecmp(name,mouse_names[i].name)) return mouse_names[i].source;
    const SDL_Scancode code=SDL_GetScancodeFromName(name);
    return code>SDL_SCANCODE_UNKNOWN ? (int)code : -1;
}

/* Applies a THEORDER_PAD_MAP/THEORDER_KEY_MAP value; returns how many entries were used. Bad entries are
 * logged and skipped, so their PS4 button keeps its default. */
static int apply_button_map(ButtonMap *map, const char *spec, bool keyboard) {
    if (!spec || !*spec) return 0;
    static char copy[8192];
    snprintf(copy,sizeof(copy),"%s",spec);
    int used=0;
    char *entry=trim(copy);
    if (*entry=='!') { map->count=0; ++entry; }
    uint32_t cleared=0;
    while (entry) {
        char *next=strchr(entry,',');
        if (next) *next++=0;
        char *eq=strchr(entry,'=');
        if (!eq) {
            if (*trim(entry)) printf("Runtime: pad map: ignored '%s' (expected button=name)\n",trim(entry));
            entry=next;
            continue;
        }
        *eq=0;
        char *button=trim(entry);
        const char *name=trim(eq+1);
        entry=next;
        char label[64];
        snprintf(label,sizeof(label),"%s",button);
        const uint32_t ps=ps_from_name(button);
        const int source=!*name ? -1 : keyboard ? key_source(name) : (int)SDL_GetGamepadButtonFromString(name);
        const bool bad_source=keyboard ? source<0 : source<=(int)SDL_GAMEPAD_BUTTON_INVALID;
        if (!ps || bad_source || (!keyboard && (ps & STICK_BITS))) {
            printf("Runtime: pad map: ignored '%s=%s'\n",label,name);
            continue;
        }
        if (ps & ~cleared) { /* the first entry for a button replaces its defaults */
            size_t kept=0;
            for (size_t i=0;i<map->count;++i) if (!(map->entries[i].ps & ps & ~cleared)) map->entries[kept++]=map->entries[i];
            map->count=kept;
            cleared|=ps;
        }
        if (map->count<sizeof(map->entries)/sizeof(*map->entries)) map->entries[map->count++]=(ButtonSource){source,ps};
        ++used;
    }
    return used;
}

static void load_button_maps(void) {
    static bool loaded;
    if (loaded) return;
    loaded=true;
    const int pad=apply_button_map(&pad_map,getenv("THEORDER_PAD_MAP"),false);
    const int keys=apply_button_map(&key_map,getenv("THEORDER_KEY_MAP"),true);
    if (pad || keys) printf("Runtime: pad map: %d gamepad and %d keyboard/mouse entries\n",pad,keys);
}

/* Mouse look: the motion turns the right stick, with the bloodborne_pc port's response
 * (runtime_pad.c mouse_to_axis, after shadPS4's EmulateJoystick): the delta scaled to a 33 ms
 * window (no frame rate dependence), magnitude * speed plus a small push faded in for slow moves,
 * capped at full deflection and eased over ~25 ms. */
static double mouse_stick_x, mouse_stick_y;
static uint64_t mouse_sample_us;
static void mouse_to_stick(float dx, float dy, float speed, int *out_x, int *out_y) {
    const uint64_t now=now_us();
    double dt_ms=mouse_sample_us ? (double)(now-mouse_sample_us)/1000.0 : 33.0;
    if (dt_ms<1.0) dt_ms=1.0;
    if (dt_ms>100.0) dt_ms=100.0;
    mouse_sample_us=now;
    const double sdx=dx*(33.0/dt_ms), sdy=dy*(33.0/dt_ms), magnitude=sqrt(sdx*sdx+sdy*sdy);
    double target_x=0.0, target_y=0.0;
    if (magnitude>=0.01) {
        double v=magnitude*speed+0.125*128.0*(magnitude<1.0 ? magnitude : 1.0);
        if (v>128.0) v=128.0;
        const double angle=atan2(sdy,sdx);
        target_x=cos(angle)*v;
        target_y=sin(angle)*v;
    }
    const double alpha=1.0-exp(-dt_ms/25.0);
    mouse_stick_x+=(target_x-mouse_stick_x)*alpha;
    mouse_stick_y+=(target_y-mouse_stick_y)*alpha;
    *out_x=(int)lround(mouse_stick_x);
    *out_y=(int)lround(mouse_stick_y);
}
/* Wheel notches become presses of WHEEL_PRESS_US with a WHEEL_GAP_US release between them, so the
 * game sees every notch even when several arrive between two reads. */
enum { WHEEL_PRESS_US=70000, WHEEL_GAP_US=50000 };
static struct { int pending; uint64_t until; bool down; } wheel[2];
static bool wheel_held(int i, int notches, uint64_t now) {
    wheel[i].pending+=notches;
    if (wheel[i].pending>8) wheel[i].pending=8;
    if (now<wheel[i].until) return wheel[i].down;
    if (wheel[i].down) { wheel[i].down=false; wheel[i].until=now+WHEEL_GAP_US; return false; }
    if (wheel[i].pending>0) { --wheel[i].pending; wheel[i].down=true; wheel[i].until=now+WHEEL_PRESS_US; return true; }
    return false;
}
/* THEORDER_LAST_INPUT=<file>: "keyboard" or "gamepad", rewritten when the player switches between
 * them, so the next start can show the prompts of the device last played with (run.py). */
static void note_device(int device) {
    static int last; static const char *path; static int checked;
    if (!checked) { path=getenv("THEORDER_LAST_INPUT"); checked=1; }
    if (device==last) return;
    last=device;
    if (!path || !*path) return;
    FILE *f=fopen(path,"w");
    if (!f) return;
    fputs(device==1 ? "keyboard\n" : "gamepad\n",f);
    fclose(f);
}
/* The mouse buttons and wheel (keyboard map sources) and mouse look on the right stick. */
static uint32_t mouse_held; /* SDL_BUTTON_MASK bits and 1<<30 / 1<<31 for the wheel */
static void mouse_input(PadData *d) {
    TheOrderMouse m;
    theordergpu_mouse_take(&m);
    if (!m.captured) {
        mouse_sample_us=0; mouse_stick_x=mouse_stick_y=0.0; mouse_held=0;
        memset(wheel,0,sizeof(wheel));
        return;
    }
    const uint64_t now=now_us();
    mouse_held=m.buttons;
    if (fabsf(m.dx)+fabsf(m.dy)>4.0f || m.wheel) note_device(1);
    if (wheel_held(0,m.wheel>0 ? m.wheel : 0,now)) mouse_held|=1u<<30;
    if (wheel_held(1,m.wheel<0 ? -m.wheel : 0,now)) mouse_held|=1u<<31;
    int mx=0, my=0;
    mouse_to_stick(m.dx,m.dy,m.sensitivity,&mx,&my);
    const int x=d->right_x-128+mx, y=d->right_y-128+my;
    d->right_x=(uint8_t)(128+(x<-128 ? -128 : x>127 ? 127 : x));
    d->right_y=(uint8_t)(128+(y<-128 ? -128 : y>127 ? 127 : y));
}
static bool key_source_down(const bool *k, int source) {
    if (source<SRC_MOUSE) return k && source>=0 && source<SDL_SCANCODE_COUNT && k[source];
    if (source==SRC_WHEEL_UP) return mouse_held & (1u<<30);
    if (source==SRC_WHEEL_DOWN) return mouse_held & (1u<<31);
    return mouse_held & SDL_BUTTON_MASK(source-SRC_MOUSE);
}

static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}
static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    if (theordergpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    load_button_maps();
    if (g) {
        for (size_t i=0;i<pad_map.count;++i)
            if (SDL_GetGamepadButton(g,(SDL_GamepadButton)pad_map.entries[i].source)) d->buttons|=pad_map.entries[i].ps;
        const uint32_t mapped=d->buttons;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        const int moved=abs(d->left_x-128)>64 || abs(d->left_y-128)>64 || abs(d->right_x-128)>64 ||
                        abs(d->right_y-128)>64;
        if (d->buttons || d->l2>60 || d->r2>60 || moved) note_device(2);
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
    }
    mouse_input(d);
    /* The keyboard and mouse work with or without a gamepad: their buttons add to the pad's, and a
     * stick follows the keys only while one of them is held. */
    uint32_t keys=0;
    for (size_t i=0;i<key_map.count;++i) if (key_source_down(k,key_map.entries[i].source)) keys|=key_map.entries[i].ps;
    if (keys) note_device(1);
    d->buttons|=keys & ~STICK_BITS;
    if (keys & BTN_L2) d->l2=255;
    if (keys & BTN_R2) d->r2=255;
    key_axis(&d->left_x,keys & LS_LEFT,keys & LS_RIGHT);
    key_axis(&d->left_y,keys & LS_UP,keys & LS_DOWN);
    key_axis(&d->right_x,keys & RS_LEFT,keys & RS_RIGHT);
    key_axis(&d->right_y,keys & RS_UP,keys & RS_DOWN);
}

/* THEORDER_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad is the left side; lx= ly= rx= ry= (0..255) override the sticks. An empty file
 * releases everything. */
static struct { uint32_t buttons; int stick[4]; } injected={0,{-1,-1,-1,-1}};
static int replay_armed;      /* 1 while a THEORDER_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) {
        path=getenv("THEORDER_PAD_FILE");
        if (!path || !*path) path=getenv("THEORDER_PAD_FILE");
        checked=1;
    }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* THEORDER_PAD_REPLAY */
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!strcmp(token,ps_names[i].name)) injected.buttons|=ps_names[i].ps & ~STICK_BITS;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* THEORDER_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; THEORDER_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in THEORDER_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("THEORDER_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("THEORDER_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
static void sample(PadData *d) {
    sample_host(d);
    if (theordergpu_overlay_captures_input()) { hold_after_capture=1; click.side=-1; return; }
    record_sample(d); /* the click sources as they are: a replay plays them again */
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    play_click(d);
    /* Options and the touchpad change what the game shows (pause, Web of Intrigue): logged. */
    static uint32_t logged_before;
    const uint32_t edges=d->buttons & ~logged_before & (BTN_OPTIONS|BTN_TOUCHPAD);
    logged_before=d->buttons;
    if (edges & BTN_OPTIONS) puts("Runtime: pad: Options pressed");
    if (edges & BTN_TOUCHPAD) puts("Runtime: pad: touchpad pressed");
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param; (void)type; (void)index;
    if (!initialized) {
        pthread_mutex_lock(&lock);
        initialized = 1;
        pthread_mutex_unlock(&lock);
    }
    pthread_mutex_lock(&lock);
    opened = 1;
    pthread_mutex_unlock(&lock);
    printf("Runtime: pad opened (handle=%d) for user %d (type=%d, index=%d)\n", PAD_HANDLE, user, type, index);
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    (void)handle;
    return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle <= 0) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data);
    static int key_trace=-1;
    if (key_trace<0) key_trace=getenv("THEORDER_KEY_TRACE")!=NULL;
    if (++reads <= 30 || (key_trace && data->buttons != 0)) /* THEORDER_KEY_TRACE: every press */
        printf("Runtime: pad read #%zu: buttons=0x%x lx=%u ly=%u rx=%u ry=%u\n",
               reads, data->buttons, data->left_x, data->left_y, data->right_x, data->right_y);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle <= 0) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle <= 0) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    (void)rgb; return 0;
}
static const RuntimeExport exports[]={
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_ok_handle},
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }

/* probe --gamepad: runs before any game state exists, so it uses its own SDL gamepad and leaves
 * the lock and statics above alone. run.py parses this line; keep the format. */
static const char *label_name(SDL_GamepadButtonLabel label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "a";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "b";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "x";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "triangle";
    default: return "unknown";
    }
}
int runtime_pad_describe(void) {
    SDL_Gamepad *g=NULL;
    const bool ready=SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    /* No event loop yet, so pads show up only after a pump: poll for about 300 ms. */
    for (int waited=0; ready && !g && waited<30; ++waited) {
        SDL_PumpEvents();
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) g=SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
        if (!g) SDL_Delay(10);
    }
    if (!g) puts("Gamepad: none");
    else {
        const char *type=SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
        const char *name=SDL_GetGamepadName(g);
        printf("Gamepad: type=%s south=%s east=%s west=%s north=%s name=%s\n",
               type ? type : "unknown",
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_SOUTH)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_EAST)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_WEST)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_NORTH)),
               name ? name : "unknown");
        SDL_CloseGamepad(g);
    }
    if (ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return g!=NULL;
}
