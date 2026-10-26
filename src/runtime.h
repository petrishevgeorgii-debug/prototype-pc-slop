#ifndef THEORDER_RUNTIME_H
#define THEORDER_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include "compat_win.h"
/* No signals on Windows: the exception handler resumes the thread in port_longjmp
 * (gpu/shim/port_toggles.h uses the same buffer type). */
typedef port_jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) port_setjmp(buffer)
#else
#include <setjmp.h>
#endif
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
/* Called at the end of runtime_report (the loader adds its own counters). */
void runtime_set_report_hook(void (*hook)(void));
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
void runtime_set_guest_args(int argc, const char **argv);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
void runtime_file_report(void);
/* The last files the game opened, oldest first (the F10 hang report, probe.c). */
void runtime_file_report_recent(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
/* Host path of a guest path through the mount table; 0 or a host errno. */
int runtime_file_host_path(const char *guest, char *out, size_t size);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
/* Windows: reserves the PS4 user address range as a placeholder (no-op elsewhere). */
void runtime_memory_reserve(void);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
/* AjmDecMp3ParseFrame (guest layout). */
typedef struct {
    uint64_t frame_size;
    uint32_t num_channels, samples_per_channel, bitrate, sample_rate, encoder_delay, num_frames, total_samples, ofl_type;
} Mp3Frame;
typedef struct Mp3Decoder Mp3Decoder;
int runtime_mp3_parse(const unsigned char *data, uint32_t size, int parse_ofl, int layer3_only, Mp3Frame *frame);
Mp3Decoder *runtime_mp3_open(void);
void runtime_mp3_close(Mp3Decoder *decoder);
void runtime_mp3_reset(Mp3Decoder *decoder);
int runtime_mp3_decode(Mp3Decoder *decoder, const unsigned char *data, int size, float *pcm, int max_samples, int *channels);
uintptr_t runtime_videodec_resolve(const char *name);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
/* probe --gamepad: prints one "Gamepad:" line for the first pad found (returns 1) or none (0). */
int runtime_pad_describe(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
/* OrbisSaveDataParam (1328 bytes) of a save directory of the current title. */
#define RUNTIME_SAVE_PARAM_SIZE 1328
int runtime_savedata_param(int32_t user, const char *dir, void *param);
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size);
void runtime_thread_attach_host(const char *name);
/* PROTOTYPE-specific APIs (runtime_proto.c). */
uintptr_t runtime_proto_resolve(const char *name);
void runtime_proto_report(void);
/* Live fibers and semaphores with waiters, for the F10 hang report (probe.c). */
void runtime_fiber_report_state(void);
void runtime_sema_report_waiting(void);
/* A compressed game file (.rz) the game opened, read part of and has not read for `ms`: its
 * path into `path`; 1 if there is one (a stalled load, for the automatic hang report). */
int runtime_file_stalled(unsigned ms,char *path,size_t size);
/* The guest image's base (probe.c), for guest offsets in diagnostics; 0 before it is mapped. */
extern uintptr_t runtime_guest_image;
/* PROTOTYPE's engine event Set/Reset, each whole under a lock (runtime_sema.c; probe.c points
 * the game's own functions here). */
ABI int32_t runtime_event_set(void *event,uint32_t n);
ABI int32_t runtime_event_reset(void *event,uint32_t n);
/* PROTOTYPE mouse look on the on-foot camera (runtime_pad.c): probe.c redirects the camera's call
 * of its stick helper (original) to runtime_chase_camera_input. */
void runtime_set_chase_camera_input(void *original);
/* A vibration the port adds to the game's (runtime_pad.c), as the vibration options allow: kind
 * RUNTIME_PULSE_GAME (the port's game effects: extra_vibration), RUNTIME_PULSE_MENU_PRESS /
 * _MENU_MOVE (the game's menus: menu_vibration). id 0: a one-off; else the effect with that id is
 * replaced, and ended (a short fade) with ms 0. Motor levels 0..1, length in ms; rise_ms 0: a tap,
 * else a swell that rises over rise_ms and holds; period_ms: a beat every period. */
enum { RUNTIME_PULSE_GAME=0, RUNTIME_PULSE_MENU_PRESS=1, RUNTIME_PULSE_MENU_MOVE=2 };
/* How an effect feels on DualSense HD haptics (runtime_haptics.c); the rumble motors only follow
 * its levels. As game/native_registry.h Native::Texture. */
enum { RUNTIME_TEXTURE_PLAIN=0, RUNTIME_TEXTURE_CRACK, RUNTIME_TEXTURE_THUD, RUNTIME_TEXTURE_TUG,
       RUNTIME_TEXTURE_STEP, RUNTIME_TEXTURE_SCRAPE, RUNTIME_TEXTURE_SWELL, RUNTIME_TEXTURE_HIT,
       RUNTIME_TEXTURE_CLICK, RUNTIME_TEXTURES };
void runtime_pad_effect(int kind,int id,float large,float small,unsigned ms,unsigned rise_ms,unsigned period_ms,
                        int texture);
/* What the haptics play now (runtime_pad.c, for runtime_haptics.c): the port's effects and the
 * game's own rumble, their motor levels already scaled by the Vibration setting and their
 * envelopes (`level` without a rhythm's beat: a Step texture beats by itself). */
typedef struct { int slot, texture; float large, small; double age_s, period_s; } RuntimeHapticVoice;
int runtime_pad_haptic_voices(RuntimeHapticVoice *out,int max,float *game_large,float *game_small);
/* DualSense HD haptics over USB (runtime_haptics.c): the controller's audio device (4 channels,
 * the haptic actuators on 3 and 4). poll (the pad thread) opens or closes it as the setting and the
 * controller come and go; active: it plays (the rumble motors stay still then); feed: the game's
 * main mix (SDL channel order, after its volume), whose impacts the haptics also play. */
void runtime_haptics_poll(void);
int runtime_haptics_active(void);
void runtime_haptics_feed(const void *frames,int count,int channels,int is_float);
ABI uint8_t runtime_chase_camera_input(void *input,float *x,float *y,float *boost);
#endif
