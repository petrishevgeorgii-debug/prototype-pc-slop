#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include <math.h>
#include "../src/runtime_pad.c"
#include "../src/runtime_haptics.c"
#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif

static int capture;
int theordergpu_overlay_captures_input(void) { return capture; }
static TheOrderMouse test_mouse;
void theordergpu_mouse_take(TheOrderMouse *out) { *out=test_mouse; test_mouse.dx=test_mouse.dy=0; test_mouse.wheel=0; }
static TheOrderPadOptions test_options={100,1,1,1,0,0};
void theordergpu_pad_options(TheOrderPadOptions *out) { *out=test_options; }
/* What the virtual controller was asked for. */
static Uint8 test_led[3]={1,1,1};
static Uint16 test_rumble[2]={1,1};
static bool SDLCALL test_set_led(void *u, Uint8 r, Uint8 g, Uint8 b) { (void)u; test_led[0]=r; test_led[1]=g; test_led[2]=b; return true; }
static bool SDLCALL test_set_rumble(void *u, Uint16 low, Uint16 high) { (void)u; test_rumble[0]=low; test_rumble[1]=high; return true; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}
static PadData read_pad(void) {
    PadData d;
    assert(pad_read_state(1,&d)==0);
    return d;
}
/* One finger in the middle of the left (480) or right (1440) half of the touchpad. */
static int touch_at(const PadData *d, int x) {
    return d->touch_count==1 && d->touches[0].x==x && d->touches[0].y==471;
}
static void pump(void) { SDL_UpdateJoysticks(); SDL_UpdateGamepads(); }

int main(void) {
    const char *tmp=getenv("TMPDIR") ? getenv("TMPDIR") : getenv("TEMP") ? getenv("TEMP") : "/tmp";
    char path[1024];
    snprintf(path,sizeof(path),"%s/port-pad-test-XXXXXX",tmp);
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("THEORDER_PAD_FILE",path,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);

    /* A click without a touch surface (Back, Tab, Backspace, the pad file) plays like a finger:
     * the touch comes first, the button CLICK_LEAD_US later, and the touch stays CLICK_TAIL_US
     * after the release. God of War III opens photo mode on the release of a left click with
     * the finger still down; a touch that appears and vanishes with the button is ignored. */
    inject(path,"cross l3 touchpad_left");
    PadData data=read_pad();
    assert((data.buttons & (BTN_CROSS|BTN_L3))==(BTN_CROSS|BTN_L3));
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    inject(path,"");
    data=read_pad();
    assert(data.buttons==0 && touch_at(&data,480)); /* released: the finger lingers */
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* A tap shorter than the lead still clicks, for CLICK_HOLD_US. */
    inject(path,"touchpad_right");
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    inject(path,"");
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(60000);
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* L1 + the right click is a left click (photo mode on a pad without a touchpad). The side
     * is chosen when the click starts and kept until it ends. */
    inject(path,"l1 touchpad_right");
    data=read_pad();
    assert((data.buttons & BTN_L1) && touch_at(&data,480));
    inject(path,"touchpad_right");
    data=read_pad();
    assert(!(data.buttons & BTN_L1) && touch_at(&data,480));
    inject(path,"");
    data=read_pad(); /* the release is seen here: the tail starts */
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* The settings menu takes the input and ends a click; the source starts a new one after. */
    inject(path,"touchpad_right");
    usleep(60000);
    data=read_pad();
    assert(touch_at(&data,1440));
    capture=1;
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    capture=0;
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    inject(path,"");
    data=read_pad(); /* the release is seen here: the tail starts */
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="port test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    desc.SetLED=test_set_led;
    desc.Rumble=test_set_rumble;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    /* A real touchpad: the fingers and the click go to the game as they are. */
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    pump();
    data=read_pad();
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==0);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==1);
    capture=1;
    data=read_pad();
    assert(data.touch_count==0 && data.buttons==0);
    capture=0;
    /* The click without a finger is played as a left click. */
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    pump();
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    pump();
    data=read_pad();
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    /* Back (Share, View) is the right click: the pause menu. With L1 it is the left click. */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,true));
    pump();
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,false));
    pump();
    data=read_pad();
    usleep(220000);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,true));
    pump();
    data=read_pad();
    assert((data.buttons & BTN_L1) && !(data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & (BTN_L1|BTN_TOUCHPAD))==(BTN_L1|BTN_TOUCHPAD) && touch_at(&data,480));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,false));
    pump();
    data=read_pad();
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    /* Light bar and vibration: the game's colour and motors reach the controller, the motors
     * scaled by the vibration option; the options apply on the next read. */
    assert(pad_light_bar(1,(const uint8_t[]){200,10,20,0})==0);
    assert(test_led[0]==200 && test_led[1]==10 && test_led[2]==20);
    assert(pad_vibration(1,(const uint8_t[]){255,128})==0);
    assert(test_rumble[0]==65535 && test_rumble[1]==128*257);
    test_options.vibration=50;
    data=read_pad();
    assert(test_rumble[0]==65535/2 && test_rumble[1]==128*257/2);
    assert(pad_vibration(1,(const uint8_t[]){0,0})==0);
    assert(test_rumble[0]==0 && test_rumble[1]==0);
    test_options.light_bar=0;
    data=read_pad();
    assert(test_led[0]==0 && test_led[1]==0 && test_led[2]==0x40); /* the player's blue again */
    test_options.light_bar=1;
    assert(pad_reset_light_bar(1)==0);
    assert(test_led[2]==0x40);
    /* Pulses the port adds (menus, consuming): over the game's motors, scaled, gone after their
     * time; a menu move only with menu_vibration 2. */
    test_options.vibration=100;
    runtime_pad_effect(RUNTIME_PULSE_MENU_MOVE,0,1.0f,1.0f,200,0,0,0);
    assert(test_rumble[0]==0 && test_rumble[1]==0);
    runtime_pad_effect(RUNTIME_PULSE_MENU_PRESS,0,0.5f,1.0f,200,0,0,0);
    assert(test_rumble[0]==128*257 && test_rumble[1]==65535);
    test_options.menu_vibration=0;
    usleep(250000);
    data=read_pad();
    assert(test_rumble[0]==0 && test_rumble[1]==0);
    runtime_pad_effect(RUNTIME_PULSE_MENU_PRESS,0,1.0f,1.0f,200,0,0,0);
    assert(test_rumble[0]==0);
    runtime_pad_effect(RUNTIME_PULSE_GAME,0,1.0f,0.5f,200,0,0,0);
    assert(test_rumble[0]==65535 && test_rumble[1]==128*257);
    usleep(250000);
    data=read_pad();
    assert(test_rumble[0]==0 && test_rumble[1]==0);
    /* A swell (consuming): starts at a third, full after its rise, ended with a short fade. */
    runtime_pad_effect(RUNTIME_PULSE_GAME,1,1.0f,1.0f,2500,200,0,0);
    assert(test_rumble[0]>65535/4 && test_rumble[0]<65535/2);
    usleep(250000);
    data=read_pad();
    assert(test_rumble[0]==65535);
    runtime_pad_effect(RUNTIME_PULSE_GAME,1,0,0,0,0,0,0);
    usleep(200000);
    data=read_pad();
    assert(test_rumble[0]==0 && test_rumble[1]==0);
    /* Effects at once take the larger of each motor; one with an id is ended on its own; a
     * rhythm beats over a steady 40%. */
    runtime_pad_effect(RUNTIME_PULSE_GAME,2,1.0f,0.2f,3000,0,200,0);
    runtime_pad_effect(RUNTIME_PULSE_GAME,0,0.1f,0.8f,3000,100,0,0);
    data=read_pad();
    assert(test_rumble[0]==65535 && test_rumble[1]>65535/4);
    usleep(120000);
    data=read_pad();
    assert(test_rumble[0]>65535*3/10 && test_rumble[0]<65535/2);
    runtime_pad_effect(RUNTIME_PULSE_GAME,2,0,0,0,0,0,0);
    usleep(200000);
    data=read_pad();
    assert(test_rumble[0]==26*257 && test_rumble[1]==204*257);
    /* Game effects off: none start (one already going still ends). */
    test_options.extra_vibration=0;
    runtime_pad_effect(RUNTIME_PULSE_GAME,3,1.0f,1.0f,2000,0,0,0);
    data=read_pad();
    assert(test_rumble[0]==26*257);
    test_options.extra_vibration=1;
    usleep(2900000);
    data=read_pad();
    assert(test_rumble[0]==0 && test_rumble[1]==0);
    test_options.menu_vibration=1;
    /* DualSense HD haptics (runtime_haptics.c): while they play, the rumble motors stay still and the
     * effects come out of channels 3 and 4 only, with their textures; the game's sound adds its
     * sudden loud parts, not a steady tone. */
    {
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");
        assert(SDL_InitSubSystem(SDL_INIT_AUDIO));
        const SDL_AudioSpec spec={SDL_AUDIO_F32,4,48000};
        SDL_AudioStream *haptic=SDL_CreateAudioStream(&spec,&spec);
        assert(haptic);
        atomic_store(&active,1);
        runtime_pad_effect(RUNTIME_PULSE_GAME,0,1.0f,1.0f,300,0,0,RUNTIME_TEXTURE_CRACK);
        data=read_pad();
        assert(test_rumble[0]==0 && test_rumble[1]==0);
        static float out[480*4];
        haptics_callback(NULL,haptic,(int)sizeof(out),(int)sizeof(out));
        assert(SDL_GetAudioStreamData(haptic,out,(int)sizeof(out))==(int)sizeof(out));
        float speaker=0.0f, actuators=0.0f;
        for (int f=0;f<480;++f) {
            speaker+=fabsf(out[f*4])+fabsf(out[f*4+1]);
            actuators+=fabsf(out[f*4+2]);
            assert(out[f*4+2]==out[f*4+3] && out[f*4+2]>=-1.0f && out[f*4+2]<=1.0f);
        }
        assert(speaker==0.0f && actuators>10.0f);
        /* the game's sound: a steady tone gives nothing once settled, a sudden boom does */
        atomic_store(&sound_level,2);
        static float tone[4800*2];
        for (int f=0;f<4800;++f) tone[f*2]=tone[f*2+1]=0.1f*sinf((float)f*6.2831853f*60.0f/48000.0f);
        for (int i=0;i<20;++i) runtime_haptics_feed(tone,4800,2,1);
        unsigned w=atomic_load(&ring_write);
        float steady=0.0f;
        for (int f=0;f<960;++f) steady+=fabsf(ring[(w-1-f) & (RING-1)]);
        for (int f=0;f<4800;++f) tone[f*2]=tone[f*2+1]=0.9f*sinf((float)f*6.2831853f*60.0f/48000.0f);
        runtime_haptics_feed(tone,4800,2,1);
        w=atomic_load(&ring_write);
        float boom=0.0f;
        for (int f=0;f<960;++f) boom+=fabsf(ring[(w-3840+f) & (RING-1)]);
        assert(steady<1.0f && boom>20.0f);
        atomic_store(&active,0);
        atomic_store(&sound_level,0);
        SDL_DestroyAudioStream(haptic);
        runtime_pad_effect(RUNTIME_PULSE_GAME,0,0.0f,0.0f,1,0,0,0);
    }
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));

    /* THEORDER_KEY_MAP: "!" drops every default, the first entry for a button replaces its
     * defaults and later ones add, "a+b" chords, mouse and wheel sources, stick targets. */
    ButtonMap map=key_map;
    assert(apply_button_map(&map,"!cross=Space,cross=Return,l1+r1=Tab,square=Mouse Left,"
                                 "up=Wheel Up,lstick_up=W,bogus=E,circle=Nope",true)==6);
    assert(map.count==6);
    assert(map.entries[0].source==SDL_SCANCODE_SPACE && map.entries[0].ps==BTN_CROSS);
    assert(map.entries[1].source==SDL_SCANCODE_RETURN && map.entries[1].ps==BTN_CROSS);
    assert(map.entries[2].ps==(BTN_L1|BTN_R1));
    assert(map.entries[3].source==SRC_MOUSE+SDL_BUTTON_LEFT);
    assert(map.entries[4].source==SRC_WHEEL_UP && map.entries[5].ps==LS_UP);
    map=key_map;
    const size_t defaults=map.count;
    assert(apply_button_map(&map,"cross=Q",true)==1);
    assert(map.count==defaults-1); /* Space and Return gone, Q added */
    ButtonMap pads=pad_map;
    assert(apply_button_map(&pads,"lstick_up=a",false)==0);
    /* Mouse buttons and wheel notches through the keyboard map (captured mouse only). */
    key_map=(ButtonMap){{{SRC_MOUSE+SDL_BUTTON_LEFT,BTN_SQUARE},{SRC_WHEEL_DOWN,BTN_DOWN},{SDL_SCANCODE_W,LS_UP}},3};
    test_mouse=(TheOrderMouse){.captured=1,.buttons=SDL_BUTTON_LMASK,.wheel=-2,.sensitivity=1.0f};
    data=read_pad();
    assert((data.buttons & (BTN_SQUARE|BTN_DOWN))==(BTN_SQUARE|BTN_DOWN) && !(data.buttons & STICK_BITS));
    test_mouse.buttons=0;
    usleep(80000);
    data=read_pad();
    assert(!(data.buttons & BTN_DOWN)); /* the gap between two notches */
    usleep(60000);
    data=read_pad();
    assert(data.buttons & BTN_DOWN); /* the second notch */
    test_mouse.captured=0;
    data=read_pad();
    assert(!(data.buttons & (BTN_SQUARE|BTN_DOWN)));
    /* PROTOTYPE's power wheel: while L1 is held the mouse points the right stick, and the
     * direction stays until the mouse moves again. */
    static int camera_stand_in;
    runtime_set_chase_camera_input(&camera_stand_in);
    key_map=(ButtonMap){{{SRC_MOUSE+SDL_BUTTON_X1,BTN_L1}},1};
    test_mouse=(TheOrderMouse){.captured=1,.buttons=SDL_BUTTON_X1MASK,.sensitivity=1.0f};
    data=read_pad();
    assert(data.buttons & BTN_L1);
    test_mouse.dx=30.0f;
    data=read_pad();
    assert(data.right_x==255 && data.right_y==128);
    data=read_pad();
    assert(data.right_x==255 && data.right_y==128);
    test_mouse.dx=-40.0f; test_mouse.dy=-60.0f; /* past the ring: up and a little left */
    data=read_pad();
    assert(data.right_y<20 && data.right_x>60 && data.right_x<128);
    test_mouse.dx=4.0f; /* inside the dead zone after a release: nothing */
    test_mouse.buttons=0;
    data=read_pad();
    data=read_pad();
    assert(!(data.buttons & BTN_L1));
    runtime_set_chase_camera_input(NULL);
    /* The mouse in the game's menus: the right button is Circle (back), the mouse not captured. */
    test_mouse=(TheOrderMouse){.menu_back=1,.buttons=SDL_BUTTON_RMASK};
    data=read_pad();
    assert(data.buttons & BTN_CIRCLE);
    test_mouse.menu_back=0;
    data=read_pad();
    assert(!(data.buttons & BTN_CIRCLE));

    SDL_Quit();
    unlink(path);
    puts("PASS: pad ABI, staged left/right clicks, L1 chord, SDL touch coordinates, overlay capture, key map, "
         "light bar, vibration, pulses, power wheel, HD haptics");
}
