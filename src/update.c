#define _DEFAULT_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sftp.h" // spawn_piped
#include "update.h"
#include "util.h"

#define REPO "hugoocoto/isf"
#define API "https://api.github.com/repos/" REPO
#define DOWNLOAD "https://github.com/" REPO "/releases/download"

/* What this build is: the releases have one file per architecture, as a
 * static binary and as an AppImage. NULL: no release for this one. */
static const char *const arch =
#if defined(__x86_64__)
        "x86_64";
#elif defined(__aarch64__)
        "aarch64";
#else
        NULL;
#endif

/* The folder PATH is in, malloc'd */
static char *
folder_of(const char *path)
{
        const char *slash = strrchr(path, '/');
        return slash ? strndup(path, slash == path ? 1 : (size_t) (slash - path)) : strdup(".");
}

/* Is PROGRAM somewhere in the PATH? */
static int
have(const char *program)
{
        const char *path = getenv("PATH");
        if (path == NULL) return 0;
        for (const char *p = path; *p;) {
                const char *end = strchr(p, ':');
                if (end == NULL) end = p + strlen(p);
                char where[PATH_MAX];
                snprintf(where, sizeof where, "%.*s/%s", (int) (end - p), p, program);
                if (access(where, X_OK) == 0) return 1;
                p = *end ? end + 1 : end;
        }
        return 0;
}

/* Run ARGV and read all it writes, into OUT. Returns 0, or -1 (logged) if it
 * couldn't run or ended badly. */
static int
run_read(char *const argv[], CharBuf *out)
{
        int to, from;
        pid_t pid = spawn_piped(argv, &to, &from);
        if (pid == -1) {
                LOG_ERR("Cannot run '%s'", argv[0]);
                return -1;
        }
        close(to);
        for (;;) {
                char buf[16384];
                ssize_t n = read(from, buf, sizeof buf);
                if (n == -1 && errno == EINTR) continue;
                if (n <= 0) break;
                for (ssize_t i = 0; i < n; i++)
                        Da_append(out, buf[i]);
        }
        close(from);
        int status = 0;
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
                ;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                LOG("Error", "'%s' failed (exit status %d)", argv[0], WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                return -1;
        }
        return 0;
}

/* Fetch URL into OUT (NUL terminated), or to the file AS if it isn't NULL,
 * with curl or wget. Returns 0, or -1 (logged). */
static int
fetch(const char *url, CharBuf *out, const char *as)
{
        CharBuf sink = { 0 };
        if (out == NULL) out = &sink;
        int curl = have("curl");
        if (!curl && !have("wget")) {
                LOG("Error", "Updating needs curl or wget, and neither is here");
                Da_destroy(&sink);
                return -1;
        }
        char *const with_curl[] = { "curl", "-fsSL", "--max-time", "300", "-o", (char *) (as ? as : "-"),
                                    (char *) url, NULL };
        char *const with_wget[] = { "wget", "-q", "-O", (char *) (as ? as : "-"), (char *) url, NULL };
        int ok                  = run_read(curl ? with_curl : with_wget, out);
        Da_append(out, 0);
        out->count--;
        Da_destroy(&sink);
        return ok;
}

/* The value of "KEY": "..." in JSON, into BUF. Returns 0, or -1 if it isn't
 * there (or is something else). */
static int
json_string(const char *json, const char *key, char *buf, size_t size)
{
        char want[64];
        snprintf(want, sizeof want, "\"%s\"", key);
        const char *p = strstr(json, want);
        if (p == NULL) return -1;
        p += strlen(want);
        while (*p == ' ' || *p == ':')
                p++;
        if (*p != '"') return -1;
        size_t n = 0;
        for (p++; *p && *p != '"' && n + 1 < size; p++) {
                if (*p == '\\' && p[1]) p++; // no escapes in what we read
                buf[n++] = *p;
        }
        buf[n] = 0;
        return *p == '"' ? 0 : -1;
}

/* This isf's own file: the AppImage if it runs as one. Returns 0 or -1. */
static int
self_path(char *buf, size_t size)
{
        const char *appimage = getenv("APPIMAGE");
        if (appimage && *appimage) {
                snprintf(buf, size, "%s", appimage);
                return 0;
        }
        ssize_t n = readlink("/proc/self/exe", buf, size - 1);
        if (n <= 0) {
                LOG_ERR("Cannot tell where isf is");
                return -1;
        }
        buf[n] = 0;
        return 0;
}

/* The short commit isf was built from, as git describe wrote it in the
 * version ("...-g<hex>", and "-dirty" if the tree had changes), or NULL */
static const char *
built_from(void)
{
        static char sha[64];
        const char *g = NULL;
        for (const char *p = VERSION; *p; p++)
                if (*p == '-' && p[1] == 'g') g = p + 2;
        if (g == NULL) return NULL;
        size_t n = 0;
        while (n + 1 < sizeof sha && g[n] && strchr("0123456789abcdef", g[n]))
                n++;
        if (n < 4 || (g[n] && strcmp(g + n, "-dirty"))) return NULL;
        memcpy(sha, g, n);
        sha[n] = 0;
        return sha;
}

/* Is F a working isf? (A truncated download, or one for another machine,
 * isn't.) AppImages need FUSE to run, so those are only looked at. */
static int
looks_like_isf(const char *path, int appimage)
{
        struct stat st;
        char magic[4] = { 0 };
        int fd        = open(path, O_RDONLY | O_CLOEXEC);
        if (fd == -1) return 0;
        int ok = fstat(fd, &st) == 0 && st.st_size > 100000 && read(fd, magic, 4) == 4 &&
                 !memcmp(magic, "\177ELF", 4);
        close(fd);
        if (!ok || appimage) return ok;

        CharBuf out        = { 0 };
        char *const argv[] = { (char *) path, "--version", NULL };
        ok                 = run_read(argv, &out) == 0;
        Da_append(&out, 0);
        ok = ok && !strncmp(out.items, "isf ", 4);
        Da_destroy(&out);
        return ok;
}

/* The newest release: its tag into TAG, and what it was built from into SHA
 * (empty if it doesn't say). A build from a tag follows the tagged releases,
 * one from main the nightly. Returns 0, or -1 (logged). */
static int
newest(char *tag, size_t tag_size, char *sha, size_t sha_size)
{
        *sha        = 0;
        int nightly = strncmp(VERSION, "v", 1) != 0 || strchr(VERSION, 'g') != NULL;
        CharBuf json = { 0 };
        if (fetch(nightly ? API "/git/ref/tags/nightly" : API "/releases/latest", &json, NULL)) {
                Da_destroy(&json);
                return -1;
        }
        int bad = nightly ? json_string(json.items, "sha", sha, sha_size) :
                            json_string(json.items, "tag_name", tag, tag_size);
        if (nightly) snprintf(tag, tag_size, "nightly");
        int limited = strstr(json.items, "rate limit") != NULL;
        Da_destroy(&json);
        if (bad) {
                if (limited)
                        LOG("Error", "GitHub is turning away requests from here for now (its rate limit): try later");
                else
                        LOG("Error", "Cannot tell what the newest isf is: GitHub answered with something else");
                return -1;
        }
        return 0;
}

int
update_run(int take_it)
{
        if (arch == NULL) {
                LOG("Error", "There are no releases for this machine's architecture: build isf from source");
                return 2;
        }
        char self[PATH_MAX];
        if (self_path(self, sizeof self)) return 2;
        int appimage = getenv("APPIMAGE") && *getenv("APPIMAGE");
        char asset[64];
        snprintf(asset, sizeof asset, "isf-%s%s", arch, appimage ? ".AppImage" : "");

        char tag[128] = { 0 }, sha[128] = { 0 };
        if (newest(tag, sizeof tag, sha, sizeof sha)) return 2;

        /* The same one? A tagged build knows its tag, a nightly its commit. */
        const char *from = built_from();
        int same         = *sha && from ? !strncmp(sha, from, strlen(from)) : !strcmp(tag, VERSION);
        if (same) {
                printf("isf: %s is the newest there is\n", VERSION);
                return 0;
        }
        printf("isf: this is isf %s; %s is newer%s%.8s%s\n", VERSION, tag, *sha ? " (" : "", *sha ? sha : "",
               *sha ? ")" : "");
        if (!take_it) {
                printf("isf: run 'isf --update' to take it\n");
                return 1; // a newer one is out there
        }

        /* Into a temp file next to this one, then in its place: a rename, so
         * isf is never half written (and this one goes on running) */
        char *dir = folder_of(self);
        char *tmp = NULL;
        int fd    = temp_create(dir, &tmp);
        free(dir);
        if (fd == -1) {
                LOG_ERR("Cannot write next to '%s' (whoever owns it has to do this)", self);
                return 2;
        }
        close(fd);

        char url[512];
        snprintf(url, sizeof url, "%s/%s/%s", DOWNLOAD, tag, asset);
        printf("isf: taking %s\n", url);
        int bad = fetch(url, NULL, tmp) || chmod(tmp, 0755) == -1 || !looks_like_isf(tmp, appimage);
        if (!bad && rename(tmp, self) == -1) {
                LOG_ERR("Cannot put it in the place of '%s'", self);
                bad = 1;
        }
        if (bad) {
                unlink(tmp);
                LOG("Error", "isf wasn't updated, and '%s' is untouched", self);
                free(tmp);
                return 2;
        }
        free(tmp);
        printf("isf: updated '%s'. The isf on the other side has to be the same one.\n", self);
        return 0;
}
