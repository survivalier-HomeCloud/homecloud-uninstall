/*
 * HomeCloud Uninstall — désinstalleur interactif (shell stylisé)
 *
 *  1. connexion avec le compte administrateur du serveur HomeCloud
 *  2. liste des composants HomeCloud installés sur la machine
 *  3. désinstallation au choix (exécutables, scripts, services, données…)
 *
 * Compilation : make        Utilisation : sudo ./homecloud-uninstall
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define VERSION "1.0"

/* ------------------------------------------------------------------ */
/*  Réglages : à adapter si votre installation diffère                 */
/* ------------------------------------------------------------------ */
#define DEFAULT_PORT 8650
#define DEFAULT_DATA "/srv/homecloud"
#define CONF_DIR     "/etc/homecloud"
#define KEYS_DIR     "/etc/homecloud/keys"

/* Formats du corps JSON de POST /api/login (essayés dans l'ordre,
 * on passe au suivant seulement si le serveur répond 400/422). */
static const char *LOGIN_FORMATS[] = {
    "{\"username\":\"%s\",\"password\":\"%s\"}",
    "{\"user\":\"%s\",\"pass\":\"%s\"}",
    "{\"login\":\"%s\",\"password\":\"%s\"}",
    NULL};
static const char *TOKEN_KEYS[] = {"token", "access_token", "session", NULL};

static const char *UNIT_DIRS[] = {"/etc/systemd/system", "/lib/systemd/system",
                                  "/usr/lib/systemd/system", NULL};

/* ------------------------------------------------------------------ */
/*  Table des composants                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *path;
    const char *excl; /* nom à conserver à la racine ; "@nodata" = seulement s'il n'y a plus de données */
} PathSpec;

enum { SP_NONE = 0, SP_DATA = 1 };

typedef struct {
    const char *id, *name, *desc;
    int special;
    int destructive; /* supprime des données utilisateur */
    const char *extra_env;
    const char *procs[4];
    const char *units[5];
    PathSpec paths[24];
} Comp;

static Comp COMPS[] = {
    {"server", "Serveur HomeCloud", "programme, scripts, services systemd", SP_NONE, 0, NULL,
     {"homecloud-server", NULL},
     {"homecloud.service", "homecloud-server.service", "homecloud-serverd.service", NULL},
     {{"/usr/local/bin/homecloud-server", NULL},
      {"/usr/local/sbin/homecloud-server", NULL},
      {"/usr/bin/homecloud-server", NULL},
      {"/usr/sbin/homecloud-server", NULL},
      {"/data/data/com.termux/files/usr/bin/homecloud-server", NULL},
      {"~/.termux/boot/homecloud-server", NULL},
      {"/opt/homecloud", NULL},
      {"/usr/local/share/homecloud", NULL},
      {"/usr/share/homecloud", NULL},
      {"/etc/logrotate.d/homecloud", NULL},
      {"/etc/default/homecloud", NULL},
      {"/etc/ufw/applications.d/homecloud", NULL},
      {"/var/log/homecloud.log", NULL},
      {"/var/log/homecloud", NULL},
      {"/run/homecloud.pid", NULL},
      {"/etc/homecloud", "@nodata"},
      {NULL, NULL}}},
    {"data", "Données du serveur", "disques .img chiffrés, clés LUKS, métadonnées, config", SP_DATA, 1, NULL,
     {NULL}, {NULL}, {{NULL, NULL}}},
    {"litedata", "Données HomeCloud Lite", "dossier ~/.homecloud (comptes, verrous, fichiers)", SP_NONE, 1,
     "HC_HOME", {NULL}, {NULL}, {{"~/.homecloud", NULL}, {NULL, NULL}}},
    {"client", "Client de bureau (Qt)", "application, lanceur, réglages", SP_NONE, 0, NULL,
     {"homecloud", NULL},
     {NULL},
     {{"/usr/local/bin/homecloud", NULL},
      {"/usr/bin/homecloud", NULL},
      {"/opt/homecloud-client", NULL},
      {"/usr/share/applications/homecloud.desktop", NULL},
      {"/usr/local/share/applications/homecloud.desktop", NULL},
      {"~/.local/share/applications/homecloud.desktop", NULL},
      {"~/.config/HomeCloud", NULL},
      {"~/.config/homecloud", NULL},
      {NULL, NULL}}},
    {"shell", "HomeCloud Shell", "terminal dans l'espace personnel", SP_NONE, 0, NULL,
     {"homecloud-shelld", NULL},
     {"homecloud-shell.service", "homecloud-shelld.service", NULL},
     {{"/usr/local/bin/homecloud-shelld", NULL},
      {"/usr/local/bin/homecloud-shell", NULL},
      {"/usr/bin/homecloud-shelld", NULL},
      {"/data/data/com.termux/files/usr/bin/homecloud-shelld", NULL},
      {"~/.homecloud-shell", NULL},
      {NULL, NULL}}},
    {"web", "HomeCloud Web", "interface web (port 8680)", SP_NONE, 0, NULL,
     {"homecloud-web", NULL},
     {"homecloud-web.service", NULL},
     {{"/usr/local/bin/homecloud-web", NULL},
      {"/usr/bin/homecloud-web", NULL},
      {"/etc/homecloud-web", NULL},
      {"/usr/local/share/homecloud-web", NULL},
      {NULL, NULL}}},
    {"sync", "HomeCloud Sync", "synchronisation photos/vidéos/documents", SP_NONE, 0, NULL,
     {"homecloud-sync", NULL},
     {"homecloud-sync.service", NULL},
     {{"/usr/local/bin/homecloud-sync", NULL},
      {"/usr/bin/homecloud-sync", NULL},
      {"/etc/homecloud-sync", NULL},
      {"~/.homecloud-sync", NULL},
      {NULL, NULL}}},
};
#define NCOMP ((int)(sizeof(COMPS) / sizeof(COMPS[0])))

typedef struct {
    char path[PATH_MAX];
    const char *excl;
    int kind; /* 0 normal, 1 fichier d'unité systemd, 2 dossier de config (avec effacement des clés) */
    unsigned long long bytes;
} Item;

#define MAXITEMS 128
typedef struct {
    Item it[MAXITEMS];
    int n, nproc, installed;
    unsigned long long bytes;
} Scan;

static Scan SC[16];

/* ------------------------------------------------------------------ */
/*  Globaux                                                            */
/* ------------------------------------------------------------------ */
static int g_color = 1, g_tty = 1, g_w = 72;
static int g_port = DEFAULT_PORT;
static int opt_offline = 0, opt_nocolor = 0;
static const char *opt_data = NULL;
static char g_step[256];
static struct termios g_saved_term;
static int g_term_saved = 0;
static char g_homes[16][PATH_MAX];
static int g_nhomes = 0;
static char g_roots[8][PATH_MAX];
static int g_nroots = 0;

static const char *K(const char *c) { return g_color ? c : ""; }
#define RST K("\033[0m")
#define BLD K("\033[1m")
#define DIM K("\033[38;5;245m")
#define ACC K("\033[38;5;75m")
#define GRN K("\033[38;5;78m")
#define RED K("\033[38;5;203m")
#define YEL K("\033[38;5;221m")

/* ------------------------------------------------------------------ */
/*  Affichage                                                          */
/* ------------------------------------------------------------------ */
static int vislen(const char *s) {
    int n = 0;
    for (; *s; s++) {
        if (*s == '\033') {
            while (*s && *s != 'm') s++;
            if (!*s) break;
            continue;
        }
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    }
    return n;
}
static void rep(const char *s, int n) {
    while (n-- > 0) fputs(s, stdout);
}
static void box_top(void) { printf("%s╭", ACC); rep("─", g_w - 2); printf("╮%s\n", RST); }
static void box_bot(void) { printf("%s╰", ACC); rep("─", g_w - 2); printf("╯%s\n", RST); }
static void box_sep(void) { printf("%s├", ACC); rep("─", g_w - 2); printf("┤%s\n", RST); }
static void box_row(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int pad = g_w - 4 - vislen(buf);
    if (pad < 0) pad = 0;
    printf("%s│%s %s", ACC, RST, buf);
    rep(" ", pad);
    printf(" %s│%s\n", ACC, RST);
}

static void human(unsigned long long b, char *out, size_t n) {
    const char *u[] = {"o", "Ko", "Mo", "Go", "To"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, n, "%llu %s", b, u[0]);
    else snprintf(out, n, "%.1f %s", v, u[i]);
}

static void banner(void) {
    printf("\n");
    box_top();
    box_row("");
    box_row("%s%s◆  H O M E C L O U D   U N I N S T A L L%s", BLD, ACC, RST);
    box_row("%sDésinstallation complète d'un serveur HomeCloud · v%s%s", DIM, VERSION, RST);
    box_row("");
    box_bot();
    printf("\n");
}

static void step_begin(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_step, sizeof g_step, fmt, ap);
    va_end(ap);
    if (g_tty) { printf("  %s◌%s %s…", DIM, RST, g_step); fflush(stdout); }
}
/* st : 0 ok, 1 attention, 2 échec, 3 ignoré */
static void step_end(int st, const char *detail) {
    const char *ic[] = {"✔", "▲", "✘", "–"};
    const char *co[4];
    co[0] = GRN; co[1] = YEL; co[2] = RED; co[3] = DIM;
    if (g_tty) printf("\r\033[K");
    printf("  %s%s%s %s", co[st], ic[st], RST, g_step);
    if (detail && *detail) printf(" %s(%s)%s", DIM, detail, RST);
    printf("\n");
    fflush(stdout);
}

static void on_sigint(int s) {
    (void)s;
    if (g_term_saved) tcsetattr(0, TCSAFLUSH, &g_saved_term);
    const char m[] = "\n\nInterrompu.\n";
    if (write(1, m, sizeof m - 1) < 0) {}
    _exit(130);
}

static int read_line(const char *prompt, char *buf, size_t n) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)n, stdin)) return -1;
    buf[strcspn(buf, "\r\n")] = 0;
    char *p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (p != buf) memmove(buf, p, strlen(p) + 1);
    size_t l = strlen(buf);
    while (l && (buf[l - 1] == ' ' || buf[l - 1] == '\t')) buf[--l] = 0;
    return 0;
}
static int read_password(const char *prompt, char *buf, size_t n) {
    struct termios t;
    printf("%s", prompt);
    fflush(stdout);
    if (tcgetattr(0, &g_saved_term) == 0) {
        g_term_saved = 1;
        t = g_saved_term;
        t.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(0, TCSAFLUSH, &t);
    }
    char *r = fgets(buf, (int)n, stdin);
    if (g_term_saved) { tcsetattr(0, TCSAFLUSH, &g_saved_term); g_term_saved = 0; }
    printf("\n");
    if (!r) return -1;
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}
static int ask_yes(const char *q) {
    char b[32];
    printf("  %s?%s %s %s[o/N]%s ", YEL, RST, q, DIM, RST);
    fflush(stdout);
    if (!fgets(b, sizeof b, stdin)) return 0;
    return b[0] == 'o' || b[0] == 'O' || b[0] == 'y' || b[0] == 'Y';
}
static int ask_typed(const char *word) {
    char b[64], p[160];
    snprintf(p, sizeof p, "  %s!%s Tapez %s%s%s pour confirmer : ", RED, RST, BLD, word, RST);
    if (read_line(p, b, sizeof b) < 0) return 0;
    return strcmp(b, word) == 0;
}

/* ------------------------------------------------------------------ */
/*  Sécurité des chemins                                               */
/* ------------------------------------------------------------------ */
static int is_home(const char *p) {
    for (int i = 0; i < g_nhomes; i++)
        if (!strcmp(p, g_homes[i])) return 1;
    return 0;
}
static int path_ok(const char *p) {
    static const char *bad[] = {"/", "/bin", "/boot", "/dev", "/etc", "/home", "/lib", "/lib64", "/mnt",
                                "/media", "/opt", "/proc", "/root", "/run", "/sbin", "/srv", "/sys", "/tmp",
                                "/usr", "/var", "/usr/bin", "/usr/sbin", "/usr/local", "/usr/local/bin",
                                "/usr/local/sbin", "/usr/share", "/usr/lib", "/var/lib", "/var/log",
                                "/etc/systemd", "/etc/systemd/system", "/data", "/data/data", NULL};
    if (!p || p[0] != '/') return 0;
    if (strstr(p, "/../") || strstr(p, "/./")) return 0;
    size_t l = strlen(p);
    if (l >= 3 && !strcmp(p + l - 3, "/..")) return 0;
    char c[PATH_MAX];
    snprintf(c, sizeof c, "%s", p);
    while (strlen(c) > 1 && c[strlen(c) - 1] == '/') c[strlen(c) - 1] = 0;
    for (int i = 0; bad[i]; i++)
        if (!strcmp(c, bad[i])) return 0;
    if (is_home(c)) return 0;
    int depth = 0;
    for (char *q = c; *q; q++)
        if (*q == '/') depth++;
    return depth >= 2;
}

/* ------------------------------------------------------------------ */
/*  Suppression récursive sûre (ne suit pas les liens, ne traverse pas  */
/*  les points de montage)                                              */
/* ------------------------------------------------------------------ */
typedef struct { unsigned long files; unsigned long long bytes; int errors, skipped; } RmStats;

static void rm_rec(int pfd, const char *name, dev_t dev, int depth, const char *excl, RmStats *s) {
    struct stat st;
    if (fstatat(pfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno != ENOENT) s->errors++;
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        if (st.st_dev != dev) { s->skipped++; return; }
        if (depth > 200) { s->errors++; return; }
        int fd = openat(pfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) { s->errors++; return; }
        DIR *d = fdopendir(fd);
        if (!d) { close(fd); s->errors++; return; }
        char **names = NULL;
        size_t nn = 0, cap = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (depth == 0 && excl && !strcmp(e->d_name, excl)) continue;
            if (nn == cap) {
                cap = cap ? cap * 2 : 64;
                char **t = realloc(names, cap * sizeof *names);
                if (!t) break;
                names = t;
            }
            names[nn++] = strdup(e->d_name);
        }
        for (size_t i = 0; i < nn; i++) {
            rm_rec(dirfd(d), names[i], dev, depth + 1, excl, s);
            free(names[i]);
        }
        free(names);
        closedir(d);
        if (unlinkat(pfd, name, AT_REMOVEDIR) < 0) {
            if (!(depth == 0 && excl) && errno != ENOENT) s->errors++;
        }
        return;
    }
    s->bytes += (unsigned long long)st.st_blocks * 512ULL;
    if (unlinkat(pfd, name, 0) < 0) { s->errors++; return; }
    s->files++;
    if (g_tty && s->files % 300 == 0) {
        printf("\r\033[K  %s◌%s %s… %lu fichiers", DIM, RST, g_step, s->files);
        fflush(stdout);
    }
}

static int rm_path(const char *path, const char *excl, RmStats *s) {
    if (!path_ok(path)) { s->errors++; return -1; }
    struct stat st;
    if (lstat(path, &st) < 0) return 0;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return -1;
    *slash = 0;
    const char *base = slash + 1;
    int pfd = open(tmp[0] ? tmp : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pfd < 0) { s->errors++; return -1; }
    rm_rec(pfd, base, st.st_dev, 0, excl, s);
    close(pfd);
    return 0;
}

static unsigned long long tree_size_rec(const char *path, dev_t dev, int depth) {
    struct stat st;
    if (lstat(path, &st) < 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_blocks * 512ULL;
    if (st.st_dev != dev || depth > 64) return 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    unsigned long long t = (unsigned long long)st.st_blocks * 512ULL;
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(p, sizeof p, "%s/%s", path, e->d_name) >= (int)sizeof p) continue;
        t += tree_size_rec(p, dev, depth + 1);
    }
    closedir(d);
    return t;
}
static unsigned long long tree_size(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) return 0;
    return tree_size_rec(path, st.st_dev, 0);
}

/* Écrase les fichiers d'un dossier (clés) avant suppression. */
static void wipe_dir_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) < 0 || !S_ISREG(st.st_mode)) continue;
        int fd = open(p, O_WRONLY | O_NOFOLLOW);
        if (fd < 0) continue;
        char z[4096];
        memset(z, 0, sizeof z);
        off_t left = st.st_size;
        while (left > 0) {
            ssize_t w = write(fd, z, left > (off_t)sizeof z ? sizeof z : (size_t)left);
            if (w <= 0) break;
            left -= w;
        }
        fsync(fd);
        close(fd);
    }
    closedir(d);
}

/* ------------------------------------------------------------------ */
/*  Processus, services, commandes                                     */
/* ------------------------------------------------------------------ */
static int run_cmd(char *const argv[]) {
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int find_pids(const char *comm, pid_t *out, int max) {
    char want[16];
    snprintf(want, sizeof want, "%.15s", comm);
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid == getpid()) continue;
        char p[64], b[64];
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(b, sizeof b, f)) {
            b[strcspn(b, "\n")] = 0;
            if (!strcmp(b, want) && n < max) out[n++] = pid;
        }
        fclose(f);
    }
    closedir(d);
    return n;
}

static int kill_procs(const char *comm) {
    pid_t pids[64];
    int n = find_pids(comm, pids, 64);
    if (!n) return 0;
    for (int i = 0; i < n; i++) kill(pids[i], SIGTERM);
    for (int t = 0; t < 50; t++) {
        usleep(100000);
        n = find_pids(comm, pids, 64);
        if (!n) return 1;
    }
    for (int i = 0; i < n; i++) kill(pids[i], SIGKILL);
    usleep(300000);
    return find_pids(comm, pids, 64) ? -1 : 1;
}

static int unit_path(const char *u, char *out, size_t n) {
    for (int i = 0; UNIT_DIRS[i]; i++) {
        snprintf(out, n, "%s/%s", UNIT_DIRS[i], u);
        if (access(out, F_OK) == 0) return 1;
    }
    return 0;
}

static void stop_comp(const Comp *c) {
    for (int i = 0; c->procs[i]; i++) {
        pid_t pids[4];
        if (!find_pids(c->procs[i], pids, 4)) continue;
        step_begin("Arrêt du processus %s", c->procs[i]);
        int r = kill_procs(c->procs[i]);
        step_end(r < 0 ? 2 : 0, r < 0 ? "impossible de l'arrêter" : NULL);
    }
    for (int i = 0; c->units[i]; i++) {
        char up[PATH_MAX];
        if (!unit_path(c->units[i], up, sizeof up)) continue;
        step_begin("Arrêt et désactivation du service %s", c->units[i]);
        char *a1[] = {"systemctl", "stop", (char *)c->units[i], NULL};
        char *a2[] = {"systemctl", "disable", (char *)c->units[i], NULL};
        run_cmd(a1);
        run_cmd(a2);
        step_end(0, NULL);
    }
}

/* ------------------------------------------------------------------ */
/*  Montages / disques chiffrés                                         */
/* ------------------------------------------------------------------ */
static void unescape_mnt(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && isdigit((unsigned char)r[1]) && isdigit((unsigned char)r[2]) && isdigit((unsigned char)r[3])) {
            *w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
            r += 3;
        } else *w++ = *r;
    }
    *w = 0;
}

static int cmp_len_desc(const void *a, const void *b) {
    return (int)strlen((const char *)b) - (int)strlen((const char *)a);
}

static int shell_quote_ok(const char *p) { return !strchr(p, '\''); }

static void close_disks_under(const char *root) {
    FILE *f = fopen("/proc/self/mounts", "r");
    static char targets[64][PATH_MAX];
    static char mappers[64][128];
    int nt = 0, nm = 0;
    char rootslash[PATH_MAX];
    snprintf(rootslash, sizeof rootslash, "%s/", root);
    if (f) {
        char line[2 * PATH_MAX];
        while (fgets(line, sizeof line, f)) {
            char src[PATH_MAX], tgt[PATH_MAX];
            if (sscanf(line, "%4095s %4095s", src, tgt) != 2) continue;
            unescape_mnt(tgt);
            unescape_mnt(src);
            if (strcmp(tgt, root) && strncmp(tgt, rootslash, strlen(rootslash))) continue;
            if (nt < 64) snprintf(targets[nt++], PATH_MAX, "%s", tgt);
            if (!strncmp(src, "/dev/mapper/", 12) && nm < 64) {
                int dup = 0;
                for (int i = 0; i < nm; i++)
                    if (!strcmp(mappers[i], src + 12)) dup = 1;
                if (!dup) snprintf(mappers[nm++], sizeof mappers[0], "%s", src + 12);
            }
        }
        fclose(f);
    }
    qsort(targets, (size_t)nt, PATH_MAX, cmp_len_desc);
    for (int i = 0; i < nt; i++) {
        step_begin("Démontage de %s", targets[i]);
        if (umount2(targets[i], 0) == 0) { step_end(0, NULL); continue; }
        sleep(1);
        if (umount2(targets[i], 0) == 0) { step_end(0, NULL); continue; }
        if (umount2(targets[i], MNT_DETACH) == 0) step_end(1, "démontage différé");
        else step_end(2, strerror(errno));
    }
    for (int i = 0; i < nm; i++) {
        step_begin("Fermeture du volume chiffré %s", mappers[i]);
        char *a[] = {"cryptsetup", "close", mappers[i], NULL};
        int r = run_cmd(a);
        step_end(r == 0 ? 0 : 1, r == 0 ? NULL : "déjà fermé ou en cours d'utilisation");
    }
    /* périphériques loop associés aux .img */
    char dd[PATH_MAX];
    snprintf(dd, sizeof dd, "%s/disks", root);
    DIR *d = opendir(dd);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".img")) continue;
        char img[PATH_MAX];
        snprintf(img, sizeof img, "%s/%s", dd, e->d_name);
        if (!shell_quote_ok(img)) continue;
        char cmd[PATH_MAX + 64];
        snprintf(cmd, sizeof cmd, "losetup -j '%s' 2>/dev/null", img);
        FILE *p = popen(cmd, "r");
        if (!p) continue;
        char ln[512];
        while (fgets(ln, sizeof ln, p)) {
            char *c = strchr(ln, ':');
            if (!c) continue;
            *c = 0;
            step_begin("Libération du périphérique %s", ln);
            char *a[] = {"losetup", "-d", ln, NULL};
            int r = run_cmd(a);
            step_end(r == 0 ? 0 : 1, NULL);
        }
        pclose(p);
    }
    closedir(d);
}

/* ------------------------------------------------------------------ */
/*  Configuration : port et dossier de données                          */
/* ------------------------------------------------------------------ */
static int cfg_find(const char *file, const char *const *keys, char *out, size_t n) {
    FILE *f = fopen(file, "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || !*p) continue;
        char *sep = strpbrk(p, "=: \t");
        if (!sep) continue;
        char key[64];
        size_t kl = (size_t)(sep - p);
        if (kl == 0 || kl >= sizeof key) continue;
        memcpy(key, p, kl);
        key[kl] = 0;
        for (int i = 0; keys[i]; i++) {
            if (strcasecmp(key, keys[i])) continue;
            char *v = sep;
            while (*v && (isspace((unsigned char)*v) || *v == '=' || *v == ':')) v++;
            size_t l = strlen(v);
            while (l && (isspace((unsigned char)v[l - 1]) || v[l - 1] == '"' || v[l - 1] == '\'')) v[--l] = 0;
            if (*v == '"' || *v == '\'') v++;
            snprintf(out, n, "%s", v);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int cfg_search_dir(const char *dir, const char *const *keys, char *out, size_t n) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        size_t l = strlen(e->d_name);
        if (l > 4 && (!strcmp(e->d_name + l - 4, ".log") || !strcmp(e->d_name + l - 4, ".pid") ||
                      !strcmp(e->d_name + l - 4, ".tsv")))
            continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size > 65536) continue;
        found = cfg_find(p, keys, out, n);
    }
    closedir(d);
    return found;
}

static int cfg_search(const char *const *keys, char *out, size_t n) {
    if (cfg_search_dir(CONF_DIR, keys, out, n)) return 1;
    for (int i = 0; i < g_nhomes; i++) {
        char d[PATH_MAX];
        snprintf(d, sizeof d, "%s/.homecloud", g_homes[i]);
        if (cfg_search_dir(d, keys, out, n)) return 1;
    }
    return 0;
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void add_root(const char *p) {
    if (!p || p[0] != '/' || g_nroots >= 8) return;
    char c[PATH_MAX];
    snprintf(c, sizeof c, "%s", p);
    while (strlen(c) > 1 && c[strlen(c) - 1] == '/') c[strlen(c) - 1] = 0;
    if (!path_ok(c)) return;
    char t[PATH_MAX];
    snprintf(t, sizeof t, "%s/disks", c);
    int ok = is_dir(t);
    snprintf(t, sizeof t, "%s/meta", c);
    ok = ok || is_dir(t);
    if (!ok) return;
    for (int i = 0; i < g_nroots; i++)
        if (!strcmp(g_roots[i], c)) return;
    snprintf(g_roots[g_nroots++], PATH_MAX, "%s", c);
}

static void find_data_roots(void) {
    static const char *dkeys[] = {"data", "data_dir", "datadir", "data_path", "storage", "storage_path",
                                  "dir", "root", NULL};
    char v[PATH_MAX];
    if (opt_data) add_root(opt_data);
    if (getenv("HC_DATA")) add_root(getenv("HC_DATA"));
    if (cfg_search(dkeys, v, sizeof v)) add_root(v);
    add_root(DEFAULT_DATA);
}

static void detect_port(void) {
    static const char *pkeys[] = {"port", "http_port", "api_port", NULL};
    char v[64];
    const char *e = getenv("HC_PORT");
    if (e && atoi(e) > 0) { g_port = atoi(e); return; }
    if (cfg_search(pkeys, v, sizeof v) && atoi(v) > 0 && atoi(v) < 65536) g_port = atoi(v);
}

static int data_present(void) { return g_nroots > 0 || access(KEYS_DIR, F_OK) == 0; }

/* ------------------------------------------------------------------ */
/*  Analyse des composants                                              */
/* ------------------------------------------------------------------ */
static void add_item(Scan *s, const char *path, const char *excl, int kind) {
    struct stat st;
    if (lstat(path, &st) < 0 || s->n >= MAXITEMS) return;
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->it[i].path, path)) return;
    Item *it = &s->it[s->n++];
    snprintf(it->path, sizeof it->path, "%s", path);
    it->excl = excl;
    it->kind = kind;
    it->bytes = tree_size(path);
    s->bytes += it->bytes;
}

static void scan_comp(const Comp *c, Scan *s) {
    memset(s, 0, sizeof *s);
    if (c->special == SP_DATA) {
        static const char *subs[] = {"disks", "mnt", "meta", "avatars", "tmp", NULL};
        for (int r = 0; r < g_nroots; r++)
            for (int i = 0; subs[i]; i++) {
                char p[PATH_MAX];
                snprintf(p, sizeof p, "%s/%s", g_roots[r], subs[i]);
                add_item(s, p, NULL, 0);
            }
        add_item(s, CONF_DIR, NULL, 2);
    } else {
        for (const PathSpec *p = c->paths; p->path; p++) {
            const char *excl = p->excl;
            if (excl && !strcmp(excl, "@nodata")) {
                if (data_present()) continue;
                excl = NULL;
            }
            if (p->path[0] == '~' && p->path[1] == '/') {
                for (int i = 0; i < g_nhomes; i++) {
                    char full[PATH_MAX];
                    snprintf(full, sizeof full, "%s/%s", g_homes[i], p->path + 2);
                    add_item(s, full, excl, 0);
                }
            } else add_item(s, p->path, excl, 0);
        }
        if (c->extra_env) {
            const char *v = getenv(c->extra_env);
            if (v && v[0] == '/' && path_ok(v)) add_item(s, v, NULL, 0);
        }
        for (int i = 0; c->units[i]; i++) {
            char up[PATH_MAX];
            for (int d = 0; UNIT_DIRS[d]; d++) {
                snprintf(up, sizeof up, "%s/%s", UNIT_DIRS[d], c->units[i]);
                add_item(s, up, NULL, 1);
            }
        }
    }
    for (int i = 0; c->procs[i]; i++) {
        pid_t pids[8];
        s->nproc += find_pids(c->procs[i], pids, 8);
    }
    s->installed = s->n > 0 || s->nproc > 0;
}

static void scan_all(void) {
    find_data_roots();
    for (int i = 0; i < NCOMP; i++) scan_comp(&COMPS[i], &SC[i]);
}

static void init_homes(void) {
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent())) {
        if (pw->pw_uid != 0 && pw->pw_uid < 1000) continue;
        if (!pw->pw_dir || pw->pw_dir[0] != '/' || !strcmp(pw->pw_dir, "/")) continue;
        if (!strcmp(pw->pw_shell ? pw->pw_shell : "", "/usr/sbin/nologin") && pw->pw_uid != 0) continue;
        int dup = 0;
        for (int i = 0; i < g_nhomes; i++)
            if (!strcmp(g_homes[i], pw->pw_dir)) dup = 1;
        if (!dup && g_nhomes < 16) snprintf(g_homes[g_nhomes++], PATH_MAX, "%s", pw->pw_dir);
    }
    endpwent();
    const char *h = getenv("HOME");
    if (h && h[0] == '/' && strcmp(h, "/")) {
        int dup = 0;
        for (int i = 0; i < g_nhomes; i++)
            if (!strcmp(g_homes[i], h)) dup = 1;
        if (!dup && g_nhomes < 16) snprintf(g_homes[g_nhomes++], PATH_MAX, "%s", h);
    }
}

/* ------------------------------------------------------------------ */
/*  Client HTTP minimal + authentification administrateur               */
/* ------------------------------------------------------------------ */
static int http_req(const char *method, const char *path, const char *token, const char *body,
                    char *resp, size_t rsz, int *status) {
    *status = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)g_port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    char req[4096];
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.0\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n", method, path, g_port);
    if (token) n += snprintf(req + n, sizeof req - (size_t)n, "Authorization: Bearer %s\r\n", token);
    if (body) n += snprintf(req + n, sizeof req - (size_t)n, "Content-Type: application/json\r\nContent-Length: %zu\r\n", strlen(body));
    n += snprintf(req + n, sizeof req - (size_t)n, "\r\n");
    if (body) n += snprintf(req + n, sizeof req - (size_t)n, "%s", body);
    if (n >= (int)sizeof req || send(fd, req, (size_t)n, 0) < 0) { close(fd); return -1; }
    size_t got = 0;
    ssize_t r;
    while (got < rsz - 1 && (r = recv(fd, resp + got, rsz - 1 - got, 0)) > 0) {
        got += (size_t)r;
        resp[got] = 0;
        char *hend = strstr(resp, "\r\n\r\n");
        if (hend) {
            char *cl = strcasestr(resp, "Content-Length:");
            if (cl && cl < hend) {
                size_t want = (size_t)atol(cl + 15);
                if (got - (size_t)(hend + 4 - resp) >= want) break;
            }
        }
    }
    resp[got] = 0;
    close(fd);
    if (got < 12 || strncmp(resp, "HTTP/", 5)) return -1;
    *status = atoi(resp + 9);
    return 0;
}

static const char *body_of(const char *resp) {
    const char *b = strstr(resp, "\r\n\r\n");
    return b ? b + 4 : resp;
}

static int json_str(const char *json, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = 0;
    return i > 0;
}

static void json_escape(const char *in, char *out, size_t n) {
    size_t j = 0;
    for (; *in && j + 7 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = (char)c; }
        else if (c < 0x20) j += (size_t)snprintf(out + j, n - j, "\\u%04x", c);
        else out[j++] = (char)c;
    }
    out[j] = 0;
}

static int server_reachable(void) {
    char resp[2048];
    int st;
    return http_req("GET", "/api/me", NULL, NULL, resp, sizeof resp, &st) == 0;
}

/* 0 = admin connecté, 1 = identifiants refusés, 2 = pas administrateur, -1 = serveur injoignable */
static int admin_login(const char *user, const char *pass) {
    static char resp[65536];
    char eu[512], ep[512], body[1200];
    json_escape(user, eu, sizeof eu);
    json_escape(pass, ep, sizeof ep);
    int st = 0;
    for (int f = 0; LOGIN_FORMATS[f]; f++) {
        snprintf(body, sizeof body, LOGIN_FORMATS[f], eu, ep);
        if (http_req("POST", "/api/login", NULL, body, resp, sizeof resp, &st) < 0) return -1;
        if (st == 400 || st == 422) continue;
        break;
    }
    if (st != 200) return 1;
    char token[512] = "";
    for (int i = 0; TOKEN_KEYS[i]; i++)
        if (json_str(body_of(resp), TOKEN_KEYS[i], token, sizeof token)) break;
    if (!token[0]) return 1;
    if (http_req("GET", "/api/admin/users", token, NULL, resp, sizeof resp, &st) < 0) return -1;
    memset(token, 0, sizeof token);
    return st == 200 ? 0 : 2;
}

static int try_start_server(void) {
    const Comp *c = &COMPS[0];
    int any = 0;
    for (int i = 0; c->units[i]; i++) {
        char up[PATH_MAX];
        if (!unit_path(c->units[i], up, sizeof up)) continue;
        char *a[] = {"systemctl", "start", (char *)c->units[i], NULL};
        run_cmd(a);
        any = 1;
    }
    if (!any) return 0;
    for (int t = 0; t < 40; t++) {
        if (server_reachable()) return 1;
        usleep(250000);
    }
    return 0;
}

static int login_flow(void) {
    printf("  %s◆ Authentification administrateur%s\n", BLD, RST);
    printf("  %sSeul le compte administrateur du serveur HomeCloud peut continuer.%s\n\n", DIM, RST);
    step_begin("Recherche du serveur sur 127.0.0.1:%d", g_port);
    int reach = server_reachable();
    if (!reach) {
        step_end(1, "injoignable");
        step_begin("Tentative de démarrage du service");
        reach = try_start_server();
        step_end(reach ? 0 : 2, reach ? NULL : "aucun service démarrable");
    } else step_end(0, "en ligne");
    if (!reach) {
        printf("\n  %sLe serveur doit tourner pour vérifier le compte administrateur.%s\n", YEL, RST);
        printf("  Démarrez-le (%ssudo systemctl start homecloud%s ou %shomecloud-server start%s),\n", BLD, RST, BLD, RST);
        printf("  précisez le port avec %s--port N%s, ou utilisez %s--offline%s (root requis).\n\n", BLD, RST, BLD, RST);
        return 0;
    }
    printf("\n");
    for (int tries = 0; tries < 3; tries++) {
        char user[128], pass[256];
        if (read_line("  Identifiant   : ", user, sizeof user) < 0) return 0;
        if (read_password("  Mot de passe  : ", pass, sizeof pass) < 0) return 0;
        step_begin("Vérification des droits d'administrateur");
        int r = admin_login(user, pass);
        memset(pass, 0, sizeof pass);
        if (r == 0) {
            step_end(0, NULL);
            printf("\n  %sConnecté en tant que %s%s%s%s (administrateur)%s\n\n", GRN, BLD, user, RST, GRN, RST);
            return 1;
        }
        if (r == -1) { step_end(2, "serveur injoignable"); return 0; }
        step_end(2, r == 2 ? "ce compte n'est pas administrateur" : "identifiants refusés");
        printf("\n");
    }
    printf("  %sTrop de tentatives. Fin du programme.%s\n\n", RED, RST);
    return 0;
}

static int offline_flow(void) {
    printf("  %s▲ Mode hors-ligne%s : le compte administrateur n'est pas vérifié.\n", YEL, RST);
    printf("  %sRéservé à un serveur arrêté ou cassé, sous le compte root de la machine.%s\n\n", DIM, RST);
    if (geteuid() != 0) {
        printf("  %sLe mode hors-ligne exige root (sudo).%s\n\n", RED, RST);
        return 0;
    }
    return ask_typed("JE CONFIRME");
}

/* ------------------------------------------------------------------ */
/*  Désinstallation                                                     */
/* ------------------------------------------------------------------ */
static void preview(const Comp *c, const Scan *s) {
    char hb[32];
    printf("\n  %s%s%s — %s\n", BLD, c->name, RST, c->desc);
    for (int i = 0; i < s->n; i++) {
        human(s->it[i].bytes, hb, sizeof hb);
        printf("    %s•%s %s %s(%s%s)%s\n", DIM, RST, s->it[i].path, DIM, hb,
               s->it[i].excl ? ", sauf " : "", RST);
        if (s->it[i].excl) printf("      %sconserve : %s%s\n", DIM, s->it[i].excl, RST);
    }
    for (int i = 0; c->units[i] && c->special == SP_NONE; i++) {
        char up[PATH_MAX];
        if (unit_path(c->units[i], up, sizeof up)) printf("    %s•%s service %s\n", DIM, RST, c->units[i]);
    }
    if (s->nproc) printf("    %s•%s %d processus en cours d'exécution (seront arrêtés)\n", DIM, RST, s->nproc);
    if (c->special == SP_DATA) {
        printf("    %s•%s les volumes chiffrés montés seront démontés puis fermés\n", DIM, RST);
        printf("    %s⚠ Les clés LUKS sont effacées : les disques .img ne sont plus récupérables.%s\n", YEL, RST);
        printf("    %s  Les dossiers locaux montés en lecture seule ne sont jamais supprimés.%s\n", DIM, RST);
    }
}

static void uninstall_comp(const Comp *c, const Scan *s, RmStats *total) {
    RmStats tot;
    memset(&tot, 0, sizeof tot);
    printf("\n  %s▸ %s%s\n", ACC, c->name, RST);

    if (c->special == SP_DATA) stop_comp(&COMPS[0]);
    stop_comp(c);
    if (c->special == SP_DATA)
        for (int r = 0; r < g_nroots; r++) close_disks_under(g_roots[r]);

    int units_removed = 0;
    for (int i = 0; i < s->n; i++) {
        const Item *it = &s->it[i];
        RmStats rs;
        memset(&rs, 0, sizeof rs);
        step_begin("Suppression de %s", it->path);
        if (it->kind == 2) wipe_dir_files(KEYS_DIR);
        rm_path(it->path, it->excl, &rs);
        char det[96];
        char hb[32];
        human(rs.bytes, hb, sizeof hb);
        snprintf(det, sizeof det, "%lu fichiers, %s", rs.files, hb);
        if (rs.errors || rs.skipped) {
            char d2[160];
            snprintf(d2, sizeof d2, "%s ; %d erreur(s), %d point(s) de montage ignoré(s)", det, rs.errors, rs.skipped);
            step_end(2, d2);
        } else step_end(0, det);
        if (it->kind == 1) units_removed = 1;
        tot.files += rs.files;
        tot.bytes += rs.bytes;
        tot.errors += rs.errors;
        tot.skipped += rs.skipped;
    }
    if (units_removed) {
        step_begin("Rechargement de systemd");
        char *a[] = {"systemctl", "daemon-reload", NULL};
        char *b[] = {"systemctl", "reset-failed", NULL};
        run_cmd(a);
        run_cmd(b);
        step_end(0, NULL);
    }
    if (c->special == SP_DATA)
        for (int r = 0; r < g_nroots; r++) rmdir(g_roots[r]);
    rmdir(CONF_DIR);

    char hb[32];
    human(tot.bytes, hb, sizeof hb);
    printf("  %s%s terminé : %lu fichiers supprimés, %s libérés%s%s\n", tot.errors ? YEL : GRN, c->name,
           tot.files, hb, tot.errors ? " (avec erreurs)" : "", RST);
    total->files += tot.files;
    total->bytes += tot.bytes;
    total->errors += tot.errors;
}

static void print_menu(void) {
    char hb[32];
    box_top();
    box_row("%s%sApplications HomeCloud détectées%s", BLD, ACC, RST);
    box_sep();
    for (int i = 0; i < NCOMP; i++) {
        const Scan *s = &SC[i];
        char state[64], line[512];
        if (s->installed) {
            human(s->bytes, hb, sizeof hb);
            snprintf(state, sizeof state, "%s●%s %-9s %s%9s%s", GRN, RST, s->nproc ? "actif" : "installé", DIM, hb, RST);
        } else snprintf(state, sizeof state, "%s○ %-9s %9s%s", DIM, "absent", "", RST);
        char nm[128];
        int padn = 26 - vislen(COMPS[i].name);
        snprintf(nm, sizeof nm, "%s%*s", COMPS[i].name, padn > 0 ? padn : 0, "");
        snprintf(line, sizeof line, "%s%d%s  %s%s%s %s", ACC, i + 1, RST, s->installed ? BLD : DIM,
                 nm, RST, state);
        box_row("%s", line);
    }
    box_sep();
    box_row("%s[1-%d]%s désinstaller  %s[a]%s tout  %s[r]%s actualiser  %s[q]%s quitter", ACC, NCOMP, RST,
            ACC, RST, ACC, RST, ACC, RST);
    box_bot();
    printf("\n");
}

static void do_single(int idx) {
    const Comp *c = &COMPS[idx];
    Scan *s = &SC[idx];
    if (!s->installed) {
        printf("\n  %s%s n'est pas installé.%s\n\n", DIM, c->name, RST);
        return;
    }
    preview(c, s);
    printf("\n");
    int ok;
    if (c->destructive) {
        printf("  %s⚠ Cette opération supprime définitivement des données.%s\n", RED, RST);
        ok = ask_typed("SUPPRIMER");
    } else ok = ask_yes("Désinstaller ce composant ?");
    if (!ok) { printf("\n  %sAnnulé.%s\n\n", DIM, RST); return; }
    RmStats t;
    memset(&t, 0, sizeof t);
    uninstall_comp(c, s, &t);
    printf("\n");
}

static void do_all(void) {
    int any = 0;
    for (int i = 0; i < NCOMP; i++)
        if (SC[i].installed) { preview(&COMPS[i], &SC[i]); any = 1; }
    if (!any) { printf("\n  %sRien à supprimer.%s\n\n", DIM, RST); return; }
    printf("\n  %s⚠ Tout HomeCloud sera supprimé de cette machine, données comprises.%s\n", RED, RST);
    if (!ask_typed("TOUT SUPPRIMER")) { printf("\n  %sAnnulé.%s\n\n", DIM, RST); return; }
    RmStats t;
    memset(&t, 0, sizeof t);
    /* clients et services annexes d'abord, serveur, puis données */
    const char *order[] = {"web", "sync", "shell", "client", "server", "data", "litedata", NULL};
    for (int o = 0; order[o]; o++)
        for (int i = 0; i < NCOMP; i++)
            if (!strcmp(COMPS[i].id, order[o]) && SC[i].installed) uninstall_comp(&COMPS[i], &SC[i], &t);
    char hb[32];
    human(t.bytes, hb, sizeof hb);
    printf("\n");
    box_top();
    box_row("%s%sDésinstallation terminée%s", BLD, t.errors ? YEL : GRN, RST);
    box_row("%lu fichiers supprimés · %s libérés", t.files, hb);
    if (t.errors) box_row("%s%d erreur(s) : relancez en root pour finir%s", YEL, t.errors, RST);
    box_bot();
    printf("\n  %sPour retirer ce programme : rm %s%s\n\n", DIM, "homecloud-uninstall", RST);
}

static void usage(void) {
    printf("HomeCloud Uninstall %s\n\n"
           "Usage : sudo homecloud-uninstall [options]\n\n"
           "  --port N      port de l'API du serveur (défaut : détecté, sinon %d)\n"
           "  --data DIR    dossier de données du serveur (si non détecté)\n"
           "  --offline     ne pas vérifier le compte admin (root requis, confirmation)\n"
           "  --no-color    désactiver les couleurs\n"
           "  -h, --help    cette aide\n",
           VERSION, DEFAULT_PORT);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) opt_data = argv[++i];
        else if (!strcmp(argv[i], "--offline")) opt_offline = 1;
        else if (!strcmp(argv[i], "--no-color")) opt_nocolor = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else { fprintf(stderr, "Option inconnue : %s\n", argv[i]); usage(); return 1; }
    }
    if (!isatty(0)) { fprintf(stderr, "Un terminal interactif est requis.\n"); return 1; }
    g_tty = isatty(1);
    g_color = g_tty && !getenv("NO_COLOR") && !opt_nocolor;
    struct winsize ws;
    if (g_tty && ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        g_w = ws.ws_col - 2;
        if (g_w > 78) g_w = 78;
        if (g_w < 62) g_w = 62;
    }
    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    init_homes();
    if (!opt_data || g_port == DEFAULT_PORT) detect_port();
    banner();
    if (geteuid() != 0)
        printf("  %s▲ Vous n'êtes pas root : seuls les éléments de votre compte pourront être supprimés.%s\n"
               "  %s  Relancez avec sudo pour une désinstallation complète.%s\n\n", YEL, RST, DIM, RST);

    if (opt_offline ? !offline_flow() : !login_flow()) return 2;

    for (;;) {
        scan_all();
        print_menu();
        char in[64];
        if (read_line("  Votre choix > ", in, sizeof in) < 0) break;
        if (!in[0]) continue;
        if (!strcmp(in, "q") || !strcmp(in, "quit") || !strcmp(in, "exit")) break;
        if (!strcmp(in, "r")) { printf("\n"); continue; }
        if (!strcmp(in, "a")) { do_all(); continue; }
        int n = atoi(in);
        if (n >= 1 && n <= NCOMP) { do_single(n - 1); continue; }
        printf("\n  %sChoix invalide.%s\n\n", RED, RST);
    }
    printf("\n  %sÀ bientôt.%s\n\n", DIM, RST);
    return 0;
}
