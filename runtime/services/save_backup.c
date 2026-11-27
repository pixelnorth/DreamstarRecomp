// Save backups and imports (Kirby's start screen, runtime/ui/launcher.mm).
// Backups: the first time a play session mounts a save (fs.c open_host_fs), its committed files are copied to
// <save>/backups/<date>/<mount>/ before the game can write them; the newest BACKUPS_KEPT are kept. Restoring one
// copies it back over the save folder (the current save is backed up first, so a restore can be undone).
// Import: a save folder made on a Switch by a save manager (JKSV: <title>/<backup name>/, Checkpoint:
// saves/<title id> <title>/<date>/) - the folder holding the save's files, or the title's folder (its newest backup
// is taken). It replaces slot save_0, the current save backed up first.
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../hle.h"
#include "../platform.h"
#ifndef _WIN32
#include <unistd.h>  // (rmdir)
#endif

#define BACKUPS_KEPT 10

extern const char *g_save_dir;  // services/fs.c
static const char *save_root(void) { return getenv("LARECOMP_SAVE_DIR") ? getenv("LARECOMP_SAVE_DIR") : g_save_dir; }

static bool is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static bool is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return false; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) ok &= fwrite(buf, 1, n, out) == n;
    ok &= !ferror(in);
    fclose(in);
    ok &= fclose(out) == 0;
    if (!ok) remove(to);
    return ok;
}
static void make_dirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
    mkdir(p, 0755);
}
// the committed files of one folder (no working copies) into another; false if any failed
static bool copy_files(const char *from, const char *to, int *count) {
    DIR *d = opendir(from);
    if (!d) return false;
    make_dirs(to);
    bool ok = true;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || (l > 8 && !strcmp(e->d_name + l - 8, ".pending"))) continue;
        char a[1300], b[1300];
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        if (!is_file(a)) continue;
        if (copy_file(a, b)) { if (count) ++*count; }
        else ok = false;
    }
    closedir(d);
    return ok;
}
static void remove_tree(const char *path) {
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1300];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (is_dir(p)) remove_tree(p);
        else remove(p);
    }
    if (d) closedir(d);
    rmdir(path);
}

// backups, newest first (names are dates: "2026-11-24_12-30-05" sorts in time order)
static int cmp_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }
int save_backup_list(char (*names)[32], int max) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/backups", save_root());
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max)
        if (e->d_name[0] != '.' && strlen(e->d_name) < 32) snprintf(names[n++], 32, "%s", e->d_name);
    if (d) closedir(d);
    qsort(names, (size_t)n, 32, cmp_desc);
    return n;
}

// a backup of the save's mounts (every save_N folder, or just `only`) now; its name in `name`
static bool backup_now(const char *only, char name[32]) {
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    strftime(name, 32, "%Y-%m-%d_%H-%M-%S", &tm);
    char dst[1200];
    snprintf(dst, sizeof dst, "%s/backups/%s", save_root(), name);
    for (int k = 2; is_dir(dst) && k < 100; k++)  // (two in the same second: a restore right after a launch)
        snprintf(dst, sizeof dst, "%s/backups/%s-%d", save_root(), name, k), snprintf(name + 19, 13, "-%d", k);
    DIR *d = opendir(save_root());
    struct dirent *e;
    int files = 0;
    bool ok = true;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "save_", 5) || (only && strcmp(e->d_name, only))) continue;
        char a[1300], b[1300];
        snprintf(a, sizeof a, "%s/%s", save_root(), e->d_name);
        snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        if (is_dir(a)) ok &= copy_files(a, b, &files);
    }
    if (d) closedir(d);
    if (!files) { remove_tree(dst); return ok; }  // (nothing saved yet)
    LOG("[save] backup %s (%d files)%s\n", dst, files, ok ? "" : " - SOME FILES FAILED");
    char names[64][32];
    int n = save_backup_list(names, 64);
    for (int i = BACKUPS_KEPT; i < n; i++) {
        char old[1200];
        snprintf(old, sizeof old, "%s/backups/%s", save_root(), names[i]);
        remove_tree(old);
    }
    return ok;
}

// fs.c: a save mount opened (no other copy of it live): once per run and mount, before the game can write
void save_backup_on_mount(const char *sub) {
    static char done[8][16];
    if (strncmp(sub, "save_", 5) || getenv("LARECOMP_NO_SAVE_BACKUP")) return;
    for (int i = 0; i < 8; i++) {
        if (!strcmp(done[i], sub)) return;
        if (!done[i][0]) { snprintf(done[i], sizeof done[i], "%s", sub); break; }
    }
    char name[32];
    backup_now(sub, name);
}

// replaces the save folder's mounts with the backup's (the current save backed up first)
bool save_backup_restore(const char *name, char *msg, size_t n) {
    char src[1200], cur[32];
    snprintf(src, sizeof src, "%s/backups/%s", save_root(), name);
    if (!is_dir(src)) { snprintf(msg, n, "That backup is gone."); return false; }
    if (!backup_now(NULL, cur)) { snprintf(msg, n, "Could not back up the current save first: nothing changed."); return false; }
    DIR *d = opendir(src);
    struct dirent *e;
    bool ok = true;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "save_", 5)) continue;
        char a[1300], b[1300];
        snprintf(a, sizeof a, "%s/%s", src, e->d_name);
        snprintf(b, sizeof b, "%s/%s", save_root(), e->d_name);
        remove_tree(b);
        ok &= copy_files(a, b, NULL);
    }
    if (d) closedir(d);
    snprintf(msg, n, ok ? "Restored the backup of %s." : "Some files of %s could not be restored.", name);
    LOG("[save] restored backup %s%s\n", name, ok ? "" : " (some files failed)");
    return ok;
}

// the folder of a Switch save manager's backup holding `want`: `dir` itself, or its newest subfolder holding it
static bool find_backup_dir(const char *dir, const char *want, char *out, size_t n) {
    char p[1300];
    snprintf(p, sizeof p, "%s/%s", dir, want);
    if (is_file(p)) { snprintf(out, n, "%s", dir); return true; }
    DIR *d = opendir(dir);
    struct dirent *e;
    time_t best = 0;
    bool found = false;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[1300];
        struct stat st;
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        snprintf(p, sizeof p, "%s/%s", sub, want);
        if (is_dir(sub) && !stat(p, &st) && (!found || st.st_mtime > best)) {
            best = st.st_mtime, found = true;
            snprintf(out, n, "%s", sub);
        }
    }
    if (d) closedir(d);
    return found;
}

// a Switch save (JKSV / Checkpoint folder) into slot save_0
bool save_import(const char *dir, const char *want, char *msg, size_t n) {
    char from[1300], cur[32], to[1100];
    size_t l = strlen(dir);
    if (l > 4 && !strcmp(dir + l - 4, ".zip")) { snprintf(msg, n, "Unzip the backup first, then pick its folder."); return false; }
    if (!find_backup_dir(dir, want, from, sizeof from)) {
        snprintf(msg, n, "No %s in that folder or the folders in it.", want);
        return false;
    }
    if (!backup_now(NULL, cur)) { snprintf(msg, n, "Could not back up the current save first: nothing changed."); return false; }
    snprintf(to, sizeof to, "%s/save_0", save_root());
    remove_tree(to);
    int files = 0;
    bool ok = copy_files(from, to, &files);
    snprintf(msg, n, ok ? "Save imported. The previous save is in the backups." : "Some files could not be copied.");
    LOG("[save] imported %s -> %s (%d files)%s\n", from, to, files, ok ? "" : " - SOME FILES FAILED");
    return ok;
}
