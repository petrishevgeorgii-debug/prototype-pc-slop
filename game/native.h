/* SPDX-License-Identifier: GPL-2.0-or-later
 * port: the native game layer (docs/NATIVE.md) - parts of the game rewritten in C++ that run in
 * place of the original code. The loader (src/probe.c) calls native_game_install once the eboot
 * is mapped, relocated and patched, before anything runs.
 */
#ifndef THEORDER_GAME_NATIVE_H
#define THEORDER_GAME_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NativeHost {
    uint8_t *image;          /* the eboot at its virtual address 0 */
    uint64_t image_size;
    /* Executable memory within 2 GiB of the image (stubs); writable until the game starts. */
    uint8_t *(*alloc_code)(size_t bytes);
    /* Read/write memory within 2 GiB of the image, for values the game's code reads through
     * rip-relative operands that the native layer redirects. */
    uint8_t *data;
    size_t data_size;
    /* The port's settings for the game's own menus (gpu/theordergpu.h), or NULL. */
    const struct TheOrderGameMenu *menu;
    /* A vibration added to the game's (runtime_pad_effect: kind, id, motor levels 0..1, ms, rise
     * ms, period ms, the texture DualSense HD haptics play it with), or NULL. */
    void (*pad_effect)(int kind, int id, float large, float small, unsigned ms, unsigned rise_ms, unsigned period_ms,
                       int texture);
    /* Registers what Discord shows (src/runtime_discord.c), or NULL: `source` is asked every few
     * seconds on another thread; it returns 1 while a game is being played (else the menus) and
     * writes the story mission's id ("e01m02", or "") into `mission`. */
    void (*presence_source)(int (*source)(char *mission, int size));
} NativeHost;

/* Installs every native replacement whose original code is found unchanged; prints what it did.
 * THEORDER_NATIVE=0 turns the layer off; THEORDER_NATIVE=-<group>,-<name> leaves groups or single
 * replacements out. */
void native_game_install(const NativeHost *host);

#ifdef __cplusplus
}
#endif

#endif
