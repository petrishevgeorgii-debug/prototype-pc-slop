/* Discord Rich Presence: "Playing PROTOTYPE" with what the player is doing, through the Discord
 * client's local IPC (a named pipe on Windows, a Unix socket elsewhere: \\.\pipe\discord-ipc-N,
 * $XDG_RUNTIME_DIR/discord-ipc-N), so no Discord SDK is needed. Frames are an op (0 handshake,
 * 1 a command), a length and JSON. Without Discord running, it tries again every 20 seconds.
 *
 * Shown: the menus or playing (the game layer's source, game/p1/presence.cpp), the story mission
 * (its id, e.g. e03m01, named from THEORDER_DISCORD_MISSIONS, which run.py writes from the game's
 * own text in its language), the time since the game started, the application's "logo" image and
 * a button to the port's page. THEORDER_DISCORD_APP_ID names the Discord application (its name is
 * the game Discord shows); THEORDER_DISCORD=0 turns it off. */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <wchar.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define PORT_URL "https://github.com/petrishevgeorgii-debug/prototype-pc-slop"

static int (*volatile presence_source)(char *mission, int size);
void runtime_discord_source(int (*source)(char *mission, int size)) { presence_source = source; }

/* ---- mission names ---- */

typedef struct { char id[16]; char name[96]; } Mission;
static Mission *missions;
static int mission_count;

static void copy_text(char *to, size_t size, const char *from) {
    size_t n = strlen(from);
    if (n >= size) n = size - 1;
    memcpy(to, from, n);
    to[n] = 0;
}

static void load_missions(void) {
    const char *path = getenv("THEORDER_DISCORD_MISSIONS");
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *name = tab + 1;
        name[strcspn(name, "\r\n")] = 0;
        Mission *grown = realloc(missions, sizeof(Mission) * (size_t)(mission_count + 1));
        if (!grown) break;
        missions = grown;
        copy_text(missions[mission_count].id, sizeof(missions[0].id), line);
        copy_text(missions[mission_count].name, sizeof(missions[0].name), name);
        ++mission_count;
    }
    fclose(f);
}

static int same_id(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) if ((*a | 0x20) != (*b | 0x20)) return 0;
    return !*a && !*b;
}

static const char *mission_name(const char *id) {
    for (int i = 0; i < mission_count; ++i)
        if (same_id(missions[i].id, id)) return missions[i].name;
    return NULL;
}

/* ---- JSON ---- */

static size_t json_string(char *out, size_t size, const char *text) {
    size_t n = 0;
    if (n < size) out[n++] = '"';
    for (const unsigned char *p = (const unsigned char *)text; *p && n + 7 < size; ++p) {
        if (*p == '"' || *p == '\\') { out[n++] = '\\'; out[n++] = (char)*p; }
        else if (*p < 0x20) n += (size_t)snprintf(out + n, size - n, "\\u%04x", *p);
        else out[n++] = (char)*p;
    }
    if (n < size) out[n++] = '"';
    if (n < size) out[n] = 0; else if (size) out[size - 1] = 0;
    return n;
}

/* The activity JSON for the current state (`details` empty: none). */
static void activity_json(char *out, size_t size, const char *details, const char *state, int64_t start,
                          int russian) {
    char d[256], s[256];
    json_string(d, sizeof(d), details);
    json_string(s, sizeof(s), state);
    snprintf(out, size,
             "{\"details\":%s%s%s,\"timestamps\":{\"start\":%lld},"
             "\"assets\":{\"large_image\":\"logo\",\"large_text\":\"PROTOTYPE PC Port\"},"
             "\"buttons\":[{\"label\":\"%s\",\"url\":\"" PORT_URL "\"}]}",
             d, *state ? ",\"state\":" : "", *state ? s : "", (long long)start,
             russian ? "Скачать PC-порт" : "Get the PC port");
}

/* ---- IPC ---- */

#ifdef _WIN32
typedef HANDLE Pipe;
#define NO_PIPE INVALID_HANDLE_VALUE
static Pipe pipe_open(void) {
    for (int i = 0; i < 10; ++i) {
        wchar_t name[64];
        _snwprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return NO_PIPE;
}
static int pipe_write(Pipe p, const void *data, size_t n) {
    DWORD done = 0;
    return WriteFile(p, data, (DWORD)n, &done, NULL) && done == n;
}
static int pipe_read(Pipe p, void *data, size_t n) {
    size_t got = 0;
    while (got < n) {
        DWORD done = 0;
        if (!ReadFile(p, (char *)data + got, (DWORD)(n - got), &done, NULL) || !done) return 0;
        got += done;
    }
    return 1;
}
static void pipe_close(Pipe p) { CloseHandle(p); }
static long current_pid(void) { return (long)GetCurrentProcessId(); }
static void sleep_s(unsigned s) { Sleep(s * 1000u); }
#else
typedef int Pipe;
#define NO_PIPE (-1)
static Pipe pipe_open(void) {
    const char *dirs[] = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), "/tmp"};
    const char *subs[] = {"", "/app/com.discordapp.Discord", "/snap.discord"};
    for (int d = 0; d < 3; ++d) {
        if (!dirs[d] || !*dirs[d]) continue;
        for (int s = 0; s < 3; ++s) for (int i = 0; i < 10; ++i) {
            struct sockaddr_un addr = {.sun_family = AF_UNIX};
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%s%s/discord-ipc-%d", dirs[d], subs[s], i);
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return NO_PIPE;
            if (!connect(fd, (struct sockaddr *)&addr, sizeof(addr))) return fd;
            close(fd);
        }
    }
    return NO_PIPE;
}
static int pipe_write(Pipe p, const void *data, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = send(p, (const char *)data + done, n - done, MSG_NOSIGNAL);
        if (w <= 0) return 0;
        done += (size_t)w;
    }
    return 1;
}
static int pipe_read(Pipe p, void *data, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(p, (char *)data + got, n - got, 0);
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}
static void pipe_close(Pipe p) { close(p); }
static long current_pid(void) { return (long)getpid(); }
static void sleep_s(unsigned s) { sleep(s); }
#endif

static int send_frame(Pipe p, uint32_t op, const char *json) {
    const uint32_t length = (uint32_t)strlen(json);
    unsigned char head[8];
    memcpy(head, &op, 4);
    memcpy(head + 4, &length, 4);
    return pipe_write(p, head, 8) && pipe_write(p, json, length);
}

/* One reply (Discord answers every frame); its JSON into `out`. 0: the connection is gone. */
static int read_frame(Pipe p, uint32_t *op, char *out, size_t size) {
    unsigned char head[8];
    if (!pipe_read(p, head, 8)) return 0;
    uint32_t length;
    memcpy(op, head, 4);
    memcpy(&length, head + 4, 4);
    if (length > 1 << 20) return 0;
    char *body = malloc(length + 1u);
    if (!body || !pipe_read(p, body, length)) { free(body); return 0; }
    body[length] = 0;
    snprintf(out, size, "%s", body);
    free(body);
    return 1;
}

/* ---- the thread ---- */

static void *discord_thread(void *arg) {
    const char *app = (const char *)arg;
    const int64_t start = (int64_t)time(NULL);
    /* The status speaks the game's language when its mission names are Russian (UTF-8 Cyrillic). */
    int russian = 0;
    for (int i = 0; i < mission_count && !russian; ++i)
        for (const unsigned char *c = (const unsigned char *)missions[i].name; *c; ++c)
            if (*c == 0xd0 || *c == 0xd1) { russian = 1; break; }
    unsigned nonce = 0;
    int warned = 0;
    for (;;) {
        Pipe p = pipe_open();
        if (p == NO_PIPE) {
            if (!warned) { warned = 1; puts("Discord: not running; the status appears once it is"); fflush(stdout); }
            sleep_s(20);
            continue;
        }
        char buffer[2048], reply[1024];
        uint32_t op = 0;
        snprintf(buffer, sizeof(buffer), "{\"v\":1,\"client_id\":\"%s\"}", app);
        if (!send_frame(p, 0, buffer) || !read_frame(p, &op, reply, sizeof(reply)) || op != 1) {
            printf("Discord: the handshake failed%s%s\n", op == 2 ? ": " : "", op == 2 ? reply : "");
            fflush(stdout);
            pipe_close(p);
            sleep_s(60);
            continue;
        }
        puts("Discord: connected; the status shows the game");
        fflush(stdout);
        char last[1536] = "";
        for (;;) {
            char mission[32] = "", details[160], state[160] = "";
            int (*source)(char *, int) = presence_source;
            const int playing = source ? source(mission, sizeof(mission)) : 1;
            const char *name = mission[0] ? mission_name(mission) : NULL;
            if (!playing) snprintf(details, sizeof(details), "%s", russian ? "В меню" : "In the menus");
            else snprintf(details, sizeof(details), "%s", russian ? "Играет за Алекса Мерсера" : "Playing as Alex Mercer");
            if (playing && name) snprintf(state, sizeof(state), "%s%s", russian ? "Сюжет: " : "Story: ", name);
            char activity[1536];
            activity_json(activity, sizeof(activity), details, state, start, russian);
            if (strcmp(activity, last)) {
                snprintf(buffer, sizeof(buffer),
                         "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":%s},\"nonce\":\"%u\"}",
                         current_pid(), activity, ++nonce);
                if (!send_frame(p, 1, buffer) || !read_frame(p, &op, reply, sizeof(reply))) break;
                if (strstr(reply, "\"evt\":\"ERROR\"")) {
                    printf("Discord: the status was refused: %.300s\n", reply);
                    fflush(stdout);
                }
                snprintf(last, sizeof(last), "%s", activity);
                printf("Discord: %s%s%s\n", details, state[0] ? " - " : "", state);
                fflush(stdout);
            }
            sleep_s(5);
        }
        puts("Discord: the connection closed; trying again");
        fflush(stdout);
        pipe_close(p);
        sleep_s(20);
    }
    return NULL;
}

void runtime_discord_start(void) {
    const char *off = getenv("THEORDER_DISCORD"), *app = getenv("THEORDER_DISCORD_APP_ID");
    if ((off && *off == '0') || !app || !*app) return;
    for (const char *c = app; *c; ++c) if (*c < '0' || *c > '9') { puts("Discord: the application id is not a number"); return; }
    load_missions();
    static char id[32];
    snprintf(id, sizeof(id), "%s", app);
    pthread_t thread;
    if (pthread_create(&thread, NULL, discord_thread, id) == 0) pthread_detach(thread);
    printf("Discord: status on (application %s, %d mission names)\n", id, mission_count);
    fflush(stdout);
}
