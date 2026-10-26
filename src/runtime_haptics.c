/* DualSense HD haptics over USB. On a USB cable the DualSense is also a sound card with four
 * channels: 1 and 2 its speaker and headphone jack, 3 and 4 the left and right haptic actuators
 * (voice coils, like a small speaker each: they play waveforms, not just a motor speed). The
 * plain rumble (runtime_pad.c, SDL_RumbleGamepad) is the controller imitating motors on the same
 * actuators, and it mutes the audio haptics while it runs: so with the haptics on, everything is
 * played here and the rumble stays at 0.
 *
 * What plays: the port's effects (runtime_pad_effect) with their textures (a whipfist crack, wall
 * run steps, landings...: runtime.h RUNTIME_TEXTURE_*), the game's own rumble (a low and a high
 * tone at its motor levels), and the game's sound: its main mix (runtime_audio.c
 * runtime_haptics_feed) low-passed, with only its sudden loud parts kept (explosions, impacts,
 * gunfire; not the music's or the city's steady hum). Over Bluetooth there is no such sound card:
 * the plain rumble goes on as before.
 *
 * The device is found by name ("DualSense": "Speakers (DualSense Wireless Controller)" on
 * Windows, the Edge too) and must have 4 channels: Windows can have it set to stereo, which would
 * mix the haptics into the speaker; then the log says to set it to 4.0 (Quadraphonic). */
#define _GNU_SOURCE
#include "runtime.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <SDL3/SDL.h>
#include "gpu/theordergpu.h"

#define RATE 48000
#define RING 16384 /* game sound frames waiting for the haptics (power of two) */
#define VOICES 12

static SDL_AudioStream *stream;  /* the pad thread opens and closes it */
static SDL_AudioDeviceID device;
static atomic_int active;
static uint64_t next_poll_ms;
static int warned_stereo;

/* The game's sound, filtered on the audio output's threads, read on the haptics' audio thread. */
static float ring[RING];
static atomic_uint ring_write, ring_read;
static struct { float lp1, lp2, fast, slow; } feed_state;
static atomic_int sound_level; /* 0..3 (TheOrderPadOptions::haptics_sound) as last polled */

static int has_dualsense_name(const char *name) {
    if (!name) return 0;
    static const char key[]="dualsense";
    for (const char *p=name; *p; ++p) {
        size_t i=0;
        while (key[i] && p[i] && (p[i]|0x20)==key[i]) ++i;
        if (!key[i]) return 1;
    }
    return 0;
}

/* ---- synthesis (the haptics' audio thread) ---- */

static uint32_t noise_seed=0x12345678u;
static float noise(void) { noise_seed=noise_seed*1664525u+1013904223u; return (float)(int32_t)noise_seed/2147483648.0f; }
static const double TAU=6.283185307179586;
static double phase[VOICES][2], start_age[VOICES];
static float scrape_lp[VOICES];
static double rumble_phase[2];

/* One frame of a voice `age` seconds after it started: the waveform of its texture at its levels. */
static float voice_frame(const RuntimeHapticVoice *v, int slot, double age) {
    const float L=v->large>1.0f ? 1.0f : v->large, S=v->small>1.0f ? 1.0f : v->small;
    double *ph=phase[slot];
    const double dt=1.0/RATE;
    float out=0.0f;
    switch (v->texture) {
    case RUNTIME_TEXTURE_CRACK: /* a whip's snap: a sharp bright burst over a short low knock */
        ph[0]+=TAU*240.0*dt; ph[1]+=TAU*80.0*dt;
        out=S*(float)exp(-age/0.012)*(0.6f*noise()+0.8f*(float)sin(ph[0])) + L*(float)exp(-age/0.03)*(float)sin(ph[1]);
        break;
    case RUNTIME_TEXTURE_THUD: { /* a landing: a click, then a low boom that drops in pitch */
        const double f=45.0+45.0*exp(-age/0.04);
        ph[0]+=TAU*f*dt;
        out=fmaxf(L,S)*(float)exp(-age/0.09)*(float)sin(ph[0]) + S*(float)exp(-age/0.006)*noise();
        break;
    }
    case RUNTIME_TEXTURE_TUG: { /* a rope going taut: a pull that sags */
        const double f=70.0+70.0*exp(-age/0.08);
        ph[0]+=TAU*f*dt;
        out=fmaxf(L,S)*(float)(age<0.05 ? 1.0 : exp(-(age-0.05)/0.08))*(float)sin(ph[0]);
        break;
    }
    case RUNTIME_TEXTURE_STEP: { /* running: a footfall at every beat */
        const double p=v->period_s>0.0 ? v->period_s : 0.2, t=fmod(age,p);
        if (t<dt) ph[0]=0.0;
        ph[0]+=TAU*85.0*dt;
        out=L*(float)exp(-t/0.018)*(float)sin(ph[0]) + 0.5f*S*(float)exp(-t/0.006)*noise();
        break;
    }
    case RUNTIME_TEXTURE_SCRAPE: { /* sliding down a wall: a rough rumble */
        scrape_lp[slot]+=0.05f*(noise()-scrape_lp[slot]);
        ph[0]+=TAU*160.0*dt;
        out=fmaxf(L,S)*(2.5f*scrape_lp[slot]+0.4f*(float)sin(ph[0]));
        break;
    }
    case RUNTIME_TEXTURE_SWELL: /* tendrils, a devastator building: two tones that waver */
        ph[0]+=TAU*60.0*dt; ph[1]+=TAU*130.0*dt;
        out=L*(float)sin(ph[0])*(0.85f+0.15f*(float)sin(TAU*7.0*age)) +
            S*(float)sin(ph[1])*(0.8f+0.2f*(float)sin(TAU*11.0*age));
        break;
    case RUNTIME_TEXTURE_HIT: /* a blow: a crunch and a short low body */
        ph[0]+=TAU*65.0*dt; ph[1]+=TAU*220.0*dt;
        out=L*(float)exp(-age/0.05)*(float)sin(ph[0]) + S*(float)exp(-age/0.01)*(0.7f*noise()+0.5f*(float)sin(ph[1]));
        break;
    case RUNTIME_TEXTURE_CLICK: /* the menus: a crisp tick */
        ph[0]+=TAU*230.0*dt; ph[1]+=TAU*110.0*dt;
        out=S*(float)exp(-age/0.008)*(float)sin(ph[0]) + L*(float)exp(-age/0.015)*(float)sin(ph[1]);
        break;
    default: /* plain: the motors as tones, low for the large one, higher for the small one */
        ph[0]+=TAU*70.0*dt; ph[1]+=TAU*180.0*dt;
        out=L*(float)sin(ph[0]) + S*(float)sin(ph[1]);
        break;
    }
    if (ph[0]>TAU) ph[0]-=TAU;
    if (ph[1]>TAU) ph[1]-=TAU;
    return out;
}

static void SDLCALL haptics_callback(void *userdata, SDL_AudioStream *s, int additional, int total) {
    (void)userdata; (void)total;
    int frames=additional/(int)(4*sizeof(float));
    if (frames<=0) return;
    RuntimeHapticVoice voices[VOICES];
    float game_large, game_small;
    const int n=runtime_pad_haptic_voices(voices,VOICES,&game_large,&game_small);
    for (int i=0;i<n;++i) {
        const int slot=voices[i].slot % VOICES;
        /* a new effect in this slot (its age went back): its waveforms start from zero */
        if (voices[i].age_s<start_age[slot]) { phase[slot][0]=phase[slot][1]=0.0; scrape_lp[slot]=0.0f; }
        start_age[slot]=voices[i].age_s;
    }
    /* the game's sound, kept no more than ~40 ms behind */
    unsigned w=atomic_load(&ring_write), r=atomic_load(&ring_read);
    if (w-r>RING) r=w-RING;
    if (w-r>(unsigned)(RATE/25)) r=w-(unsigned)(RATE/50);
    float buffer[256*4];
    double offset=0.0;
    while (frames>0) {
        const int chunk=frames>256 ? 256 : frames;
        for (int f=0;f<chunk;++f) {
            float h=0.0f;
            for (int i=0;i<n;++i) h+=voice_frame(&voices[i],voices[i].slot % VOICES,voices[i].age_s+offset+(double)f/RATE);
            if (game_large>0.0f || game_small>0.0f) {
                rumble_phase[0]+=TAU*55.0/RATE; rumble_phase[1]+=TAU*160.0/RATE;
                if (rumble_phase[0]>TAU) rumble_phase[0]-=TAU;
                if (rumble_phase[1]>TAU) rumble_phase[1]-=TAU;
                h+=game_large*(float)sin(rumble_phase[0])+game_small*0.7f*(float)sin(rumble_phase[1]);
            }
            if (r!=w) { h+=ring[r & (RING-1)]; ++r; }
            h=tanhf(h*0.9f); /* several at once: soft limits, no clipping */
            buffer[f*4+0]=buffer[f*4+1]=0.0f; /* the speaker and the headphones: silent */
            buffer[f*4+2]=buffer[f*4+3]=h;    /* the left and right actuators */
        }
        SDL_PutAudioStreamData(s,buffer,chunk*4*(int)sizeof(float));
        frames-=chunk; offset+=(double)chunk/RATE;
    }
    atomic_store(&ring_read,r);
}

/* ---- the game's sound (runtime_audio.c, the main port's threads) ---- */

void runtime_haptics_feed(const void *frames, int count, int channels, int is_float) {
    const int level=atomic_load(&sound_level);
    if (!atomic_load(&active) || level<=0 || count<=0 || (channels!=2 && channels!=8)) return;
    static const float gains[4]={0.0f,1.5f,3.0f,5.0f};
    const float gain=gains[level>3 ? 3 : level];
    const float a=1.0f-expf(-(float)TAU*140.0f/RATE);           /* two low-pass stages, 140 Hz */
    const float fast_up=1.0f-expf(-1.0f/(0.002f*RATE)), fast_down=1.0f-expf(-1.0f/(0.04f*RATE));
    const float slow=1.0f-expf(-1.0f/(0.6f*RATE));
    unsigned w=atomic_load(&ring_write);
    for (int f=0;f<count;++f) {
        float c[8];
        for (int i=0;i<channels;++i) {
            if (is_float) memcpy(&c[i],(const char *)frames+((size_t)f*channels+i)*4,4);
            else { int16_t v; memcpy(&v,(const char *)frames+((size_t)f*channels+i)*2,2); c[i]=(float)v/32768.0f; }
        }
        /* SDL 7.1 order: FL FR C LFE BL BR SL SR; the LFE weighs most */
        const float m=channels==2 ? 0.5f*(c[0]+c[1])
                                  : 0.35f*(c[0]+c[1])+0.3f*c[2]+0.8f*c[3]+0.15f*(c[4]+c[5]+c[6]+c[7]);
        feed_state.lp1+=a*(m-feed_state.lp1);
        feed_state.lp2+=a*(feed_state.lp1-feed_state.lp2);
        const float x=feed_state.lp2, ax=fabsf(x);
        feed_state.fast+=(ax>feed_state.fast ? fast_up : fast_down)*(ax-feed_state.fast);
        feed_state.slow+=slow*(ax-feed_state.slow);
        /* only what rises well over the last half second, and not the quiet floor */
        float keep=feed_state.fast>1e-4f ? (feed_state.fast-1.8f*feed_state.slow-0.004f)/feed_state.fast : 0.0f;
        keep=keep<0.0f ? 0.0f : keep>1.0f ? 1.0f : keep;
        ring[w & (RING-1)]=x*keep*gain;
        ++w;
    }
    atomic_store(&ring_write,w);
}

/* ---- the device (the pad thread) ---- */

static void close_device(const char *why) {
    if (!stream) return;
    atomic_store(&active,0);
    SDL_DestroyAudioStream(stream);
    stream=NULL; device=0;
    printf("Haptics: off (%s); the controller rumbles as before\n",why);
    fflush(stdout);
}

int runtime_haptics_active(void) { return atomic_load(&active); }

void runtime_haptics_poll(void) {
    const uint64_t now=SDL_GetTicks();
    if (now<next_poll_ms) return;
    next_poll_ms=now+2000;
    TheOrderPadOptions options;
    theordergpu_pad_options(&options);
    atomic_store(&sound_level,options.haptics_sound);
    const char *env=getenv("THEORDER_HAPTICS"), *audio=getenv("THEORDER_AUDIO");
    if (!options.haptics || (env && *env=='0') || (audio && !strcmp(audio,"none"))) { close_device("setting"); return; }
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    int count=0;
    SDL_AudioDeviceID *ids=SDL_GetAudioPlaybackDevices(&count);
    SDL_AudioDeviceID found=0;
    int channels=0;
    const char *name=NULL;
    for (int i=0;ids && i<count;++i) {
        const char *n=SDL_GetAudioDeviceName(ids[i]);
        if (!has_dualsense_name(n)) continue;
        SDL_AudioSpec spec; int sample_frames;
        if (!SDL_GetAudioDeviceFormat(ids[i],&spec,&sample_frames)) continue;
        found=ids[i]; channels=spec.channels; name=n;
        if (channels>=4) break;
    }
    if (stream && found!=device) close_device("the controller's USB audio is gone");
    if (stream || !found) { SDL_free(ids); return; }
    if (channels<4) {
        if (!warned_stereo) {
            warned_stereo=1;
            printf("Haptics: %s has %d channels: HD haptics need 4. In Windows' Sound settings, set the "
                   "controller's speaker to 4.0 / Quadraphonic (Configure)\n",name,channels);
            fflush(stdout);
        }
        SDL_free(ids);
        return;
    }
    const SDL_AudioSpec want={SDL_AUDIO_F32,4,RATE};
    stream=SDL_OpenAudioDeviceStream(found,&want,haptics_callback,NULL);
    if (!stream) {
        printf("Haptics: %s could not be opened (%s)\n",name,SDL_GetError());
        fflush(stdout);
        next_poll_ms=now+10000;
        SDL_free(ids);
        return;
    }
    device=found;
    atomic_store(&ring_read,atomic_load(&ring_write));
    atomic_store(&active,1);
    SDL_ResumeAudioStreamDevice(stream);
    printf("Haptics: on, %s (%d channels): DualSense HD haptics play the vibration\n",name,channels);
    fflush(stdout);
    SDL_free(ids);
}
