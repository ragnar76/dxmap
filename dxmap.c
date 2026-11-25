/*
 * DxMap - treemap of disk usage for Atari TOS / MiNT / MagiC (GEM, WinDom)
 * Copyright (C) 2026 Bernd Mueller
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mint/osbind.h>
#include <mint/mintbind.h>
#include <mint/ostruct.h>
#include <windom.h>

#ifndef __CDECL
# define __CDECL
#endif

long _stksize = 32768L;

#define AE  "\204"
#define OE  "\224"
#define UE  "\201"
#define OEC "\231"

#define APPNAME     "DxMap"
#define APPVERSION  "0.1.0"

typedef unsigned long long u64;
typedef unsigned long      u32;

#define NHUES       12
#define MAXCELLS    60000L
#define PATHLEN     1024

typedef struct node {
    struct node *parent, *child, *next, *todo;
    char *name;
    u64 size;
    unsigned char isdir, hue;
} Node;

typedef struct { short x, y, w, h; Node *n; } Cell;

static WINDOW *mainwin;
static short vh, cw, ch, bw, bh;
static Node *root, *view_root, *sel, *todo;
static int   scanning, endprog, oom, cells_full, want_open;
static u32   n_files, n_dirs, scan_steps;
static clock_t scan_t0, scan_ticks;             /* scan start / duration, 200 Hz ticks */
static int   scan_aborted;

/* Measuring (key T): where does the time go? All values in 200 Hz ticks (5 ms).
 * t_draw sums every redraw since the last layout, i.e. the full redraw after a
 * layout plus any small ones (selection) that follow. */
static int     show_timing;
static clock_t t_sort, t_layout, t_draw, t_draw_shown;

/* Scan breakdown (key D). t_step = time inside scan_step() (OS calls + our per-entry
 * work), measured per folder for free in on_timer. t_node/t_hue = new_node() and the
 * extension lookup inside it, measured per entry only if T was on when the scan
 * started (probing), because the clock() calls themselves cost time. */
static int     probing, probed;

/* Debounced resizing: with live resizing the AES sends WM_SIZED for every step while the
 * user drags. Layout + full redraw take about a second with ~60000 cells, so the window
 * would lag behind. Instead the window is cleared and the layout runs once no further
 * WM_SIZED came for RESIZE_DELAY ticks. */
#define RESIZE_DELAY  60    /* 300 ms */
static int     layout_pending;
static clock_t resize_t;
static clock_t t_step, t_node, t_hue;
static short scan_mask = 0x17;
static int   show_hidden = 1;

static Cell *cells;
static long  ncells, capcells;
static GRECT work;

static short planes;
static int   cushion_ok, cushions_on, show_dirs = 1;

/* Minimum size (settings, keys + and -): areas with a side shorter than min_px pixels
 * are not split any further. A folder that small becomes one grey cell, and the rest of
 * a folder whose remaining strip got that thin is lumped into one cell. Those areas were
 * only pixel crumbs anyway, but the layout used to compute every single file in them.
 * 1 = no limit (every file its own cell). */
static const int min_steps[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
#define NMIN  ((int)(sizeof min_steps / sizeof min_steps[0]))
static int   min_px = 2;
static short pen_hi[NHUES], pen_lo[NHUES], pen_top[NHUES];

static char  title[PATHLEN + 64], scan_root[PATHLEN], status_msg[PATHLEN];
/* own status bar at the bottom of the work area (the AES info line sits under the title,
 * above the menu; Atari programs usually show their status at the bottom) */
static char  status_l[PATHLEN + 64], status_r[96];
static int   busy, use_mint;
#define BAR_H  (ch + 6)

#define M_INFO     8
#define M_OPEN     10
#define M_PREF     12
#define M_QUIT     14
#define M_COLOR    16
#define M_REFRESH  17
#define M_NOBJ     18

static void fmt_size(u64 s, char *buf)
{
    static const char *unit[] = { "Bytes", "KB", "MB", "GB", "TB" };
    u64 div = 1;
    int u = 0;
    while (u < 4 && s >= div * 1024) { div *= 1024; u++; }
    if (u == 0)
        sprintf(buf, "%lu Bytes", (unsigned long)s);
    else
        sprintf(buf, "%lu.%lu %s", (unsigned long)(s / div),
                (unsigned long)(((s % div) * 10) / div), unit[u]);
}

/* appends n's path to buf (length *len); returns 0 if it does not fit */
static int append_path(const Node *n, char *buf, int max, int *len)
{
    int nl;
    if (n->parent && !append_path(n->parent, buf, max, len)) return 0;
    nl = (int)strlen(n->name);
    if (n->parent) {
        if (*len + 1 >= max) return 0;
        buf[(*len)++] = '\\';
        buf[*len] = 0;
    }
    if (*len + nl >= max) return 0;
    memcpy(buf + *len, n->name, (size_t)nl + 1);
    *len += nl;
    return 1;
}

/* Full path of n. Returns 1 if complete, 0 if it was too long. On 0 buf holds only a
 * leading part: fine for display, but never use it for file operations. */
static int build_path(const Node *n, char *buf, int max)
{
    int len = 0;
    buf[0] = 0;
    if (!n || max <= 0) return 0;
    return append_path(n, buf, max, &len);
}

static const struct { char ext[6]; unsigned char hue; } exttab[] = {
    {"PRG",1},{"APP",1},{"TOS",1},{"TTP",1},{"GTP",1},{"ACC",1},{"CPX",1},
    {"EXE",1},{"COM",1},{"DLL",1},{"SO",1},
    {"ZIP",2},{"LZH",2},{"LHA",2},{"ARC",2},{"ZOO",2},{"GZ",2},{"TGZ",2},
    {"TAR",2},{"BZ2",2},{"7Z",2},{"RAR",2},{"MSA",2},{"ST",2},{"STX",2},
    {"DIM",2},{"ATR",2},
    {"INF",3},{"CNF",3},{"CFG",3},{"INI",3},{"SYS",3},{"SET",3},{"RSC",3},
    {"RSO",3},{"DEF",3},
    {"IMG",4},{"PI1",4},{"PI2",4},{"PI3",4},{"NEO",4},{"TNY",4},{"DEG",4},
    {"GIF",4},{"JPG",4},{"JPEG",4},{"PNG",4},{"BMP",4},{"TIF",4},{"TGA",4},
    {"IFF",4},{"PCX",4},{"PC1",4},{"PC2",4},{"PC3",4},
    {"MOD",5},{"MP3",5},{"MP2",5},{"WAV",5},{"AVR",5},{"SND",5},{"SNDH",5},
    {"YM",5},{"OGG",5},{"MID",5},{"AIF",5},{"AIFF",5},{"FLAC",5},
    {"FLI",6},{"FLC",6},{"AVI",6},{"MPG",6},{"MPEG",6},{"MP4",6},{"FLM",6},
    {"MOV",6},
    {"TXT",7},{"DOC",7},{"ASC",7},{"RTF",7},{"HTM",7},{"HTML",7},{"PDF",7},
    {"HYP",7},{"1ST",7},{"MD",7},{"TEX",7},
    {"C",8},{"H",8},{"S",8},{"ASM",8},{"CPP",8},{"PAS",8},{"BAS",8},
    {"LUA",8},{"PY",8},{"O",8},{"A",8},
    {"FNT",9},{"VDI",9},{"DRV",9},{"DEV",9},{"TTF",9},{"OTF",9},{"LDG",9},
    {"SLB",9},{"XFS",9},{"XIF",9},
    {"DAT",10},{"DB",10},{"BIN",10},{"DBF",10},{"CSV",10},{"XML",10},
    {"JSON",10},{"MAP",10},
    {"BAK",11},{"TMP",11},{"OLD",11},{"$$$",11},{"LOG",11},{"CHK",11}
};

/* Extension lookup: all extensions in exttab have at most 4 characters, so they are
 * packed into one 32 bit key (upper case) and found by binary search with plain integer
 * compares, instead of a linear strcmp() over the whole table (measured with key D:
 * about 8 % of the scan time). The table is sorted once on first use. */
#define NEXT  ((int)(sizeof exttab / sizeof exttab[0]))
static u32           ext_key[NEXT];
static unsigned char ext_hue[NEXT];
static int           ext_ready;

static void init_ext(void)
{
    int i, j;
    for (i = 0; i < NEXT; i++) {
        const char *e = exttab[i].ext;
        u32 k = 0;
        while (*e) k = (k << 8) | (unsigned char)*e++;
        for (j = i; j > 0 && ext_key[j - 1] > k; j--) {     /* insertion sort, once */
            ext_key[j] = ext_key[j - 1];
            ext_hue[j] = ext_hue[j - 1];
        }
        ext_key[j] = k;
        ext_hue[j] = exttab[i].hue;
    }
    ext_ready = 1;
}

static unsigned char hue_for_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    unsigned long hash = 0;
    u32 key = 0;
    int i;

    if (!dot || dot == name || !dot[1]) return 0;
    dot++;
    for (i = 0; dot[i]; i++) {
        unsigned char c = (unsigned char)dot[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        hash = hash * 31UL + c;
        key = (key << 8) | c;
    }
    if (i <= 4) {
        int lo = 0, hi = NEXT - 1;
        if (!ext_ready) init_ext();
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            if (ext_key[mid] == key) return ext_hue[mid];
            if (ext_key[mid] < key) lo = mid + 1; else hi = mid - 1;
        }
    }
    return (unsigned char)(1 + hash % (NHUES - 1));
}

/* Block allocator for the tree: node and name come in ONE piece from 32 KB blocks
 * instead of calloc() + strdup() per entry. Measured with key D this was about a third
 * of the scan time (two mintlib mallocs per file). Single nodes are never freed: a
 * removed node (file deleted/moved) just stays unused until the next scan, which drops
 * all blocks at once (pool_free_all). */
#define POOL_BLOCK  32768L
#define POOL_ALIGN  ((long)sizeof(void *) > 4 ? 7L : 3L)   /* 4 bytes on 68k, 8 on 64 bit hosts (tests) */

typedef struct pblock { struct pblock *next; long used, size; } PBlock;
static PBlock *pool;

static void *pool_alloc(long n)
{
    char *p;
    n = (n + POOL_ALIGN) & ~POOL_ALIGN;                  /* keep nodes aligned */
    if (!pool || pool->used + n > pool->size) {
        long sz = (n + (long)sizeof(PBlock) > POOL_BLOCK) ? n + (long)sizeof(PBlock) : POOL_BLOCK;
        PBlock *b = (PBlock *)malloc(sz);
        if (!b) return NULL;
        b->next = pool;
        b->used = ((long)sizeof(PBlock) + POOL_ALIGN) & ~POOL_ALIGN;
        b->size = sz;
        pool = b;
    }
    p = (char *)pool + pool->used;
    pool->used += n;
    return p;
}

static void pool_free_all(void)
{
    while (pool) { PBlock *nx = pool->next; free(pool); pool = nx; }
}

static Node *new_node(Node *parent, const char *name, int isdir, u64 size)
{
    clock_t ta = probing ? clock() : 0, tb;
    size_t len = strlen(name);
    Node *n = (Node *)pool_alloc((long)(sizeof(Node) + len + 1));
    if (!n) return NULL;
    memset(n, 0, sizeof(Node));
    n->name = (char *)(n + 1);
    memcpy(n->name, name, len + 1);
    n->parent = parent;
    n->isdir  = (unsigned char)isdir;
    n->size   = size;
    if (parent) { n->next = parent->child; parent->child = n; }
    tb = probing ? clock() : 0;
    if (!isdir) n->hue = hue_for_name(name);
    if (probing) {
        clock_t tc = isdir ? tb : clock();
        t_hue  += tc - tb;
        t_node += tc - ta;
    }
    return n;
}


static Node *msort(Node *l)
{
    Node *a, *b, *slow, *fast, head, *t;
    if (!l || !l->next) return l;
    slow = l; fast = l->next;
    while (fast && fast->next) { slow = slow->next; fast = fast->next->next; }
    b = slow->next; slow->next = NULL;
    a = msort(l); b = msort(b);
    t = &head;
    while (a && b) {
        if (a->size >= b->size) { t->next = a; a = a->next; }
        else                    { t->next = b; b = b->next; }
        t = t->next;
    }
    t->next = a ? a : b;
    return head.next;
}

static void finish_dir(Node *d)
{
    Node *c;
    u64 sum = 0;
    for (c = d->child; c; c = c->next) {
        if (c->isdir) finish_dir(c);
        sum += c->size;
    }
    d->size  = sum;
    d->child = msort(d->child);
}

static _DTA scan_dta;

typedef struct {
    unsigned short mode;
    long           ino;
    unsigned short dev;
    short          rdev;
    unsigned short nlink, uid, gid;
    long           size, blksize, blocks;
    unsigned long  mtime, atime, ctime;
    short          attr, res1;
    long           res2[2];
} XA;
#define XA_IFMT   0170000
#define XA_IFDIR  0040000
#define XA_IFREG  0100000

static int use_dx = 1;

static int is_dots(const char *nm)
{
    return nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0));
}

static int dir_next(long h, const char *base, char *buf, size_t bufsz, XA *xa)
{
    static char full[PATHLEN];
    const char *nm;
    long r, xr = 0;

    if (use_dx) {
        r = Dxreaddir((short)bufsz, h, buf, xa, &xr);
        if (r == -32L) { use_dx = 0; return 1; }
        if (r != 0) return -1;
        nm = buf + 4;
        if (is_dots(nm) || xr != 0) return 1;
    } else {
        r = Dreaddir((short)bufsz, h, buf);
        if (r != 0) return -1;
        nm = buf + 4;
        if (is_dots(nm)) return 1;
        if (strlen(base) + strlen(nm) >= sizeof full) return 1;
        strcpy(full, base); strcat(full, nm);
        if (Fxattr(1, full, xa) < 0) return 1;
    }
    return 0;
}

static int scan_dir_mint(Node *d, const char *dir)
{
    static char buf[1024];
    static XA xa;
    char base[PATHLEN];
    size_t blen;
    long h;

    strncpy(base, dir, sizeof base - 3); base[sizeof base - 3] = 0;
    blen = strlen(base);
    if (blen == 0 || base[blen - 1] == ':') base[blen++] = '\\';
    base[blen] = 0;

    h = Dopendir(base, 0);
    if (h == -32L) return -1;
    if (h < 0) return 0;

    if (base[blen - 1] != '\\') { base[blen++] = '\\'; base[blen] = 0; }

    for (;;) {
        const char *nm;
        int k = dir_next(h, base, buf, sizeof buf, &xa);
        if (k < 0) break;
        if (k > 0) continue;
        nm = buf + 4;
        if (!show_hidden && (xa.attr & 0x06)) continue;
        if ((xa.mode & XA_IFMT) == XA_IFDIR) {
            Node *n = new_node(d, nm, 1, 0);
            if (!n) { oom = 1; break; }
            n->todo = todo; todo = n; n_dirs++;
        } else if ((xa.mode & XA_IFMT) == XA_IFREG) {
            if (!new_node(d, nm, 0, (u64)(unsigned long)xa.size)) { oom = 1; break; }
            n_files++;
        }
    }
    Dclosedir(h);
    return 0;
}

static void scan_step(void)
{
    static char path[PATHLEN];
    Node *d = todo;
    _DTA *olddta;
    long r;

    if (!d) { scanning = 0; return; }
    todo = d->todo;

    if (!build_path(d, path, (int)sizeof(path) - 8)) {     /* path too long: skip folder */
        if (!todo) scanning = 0;
        return;
    }

    if (use_mint) {
        if (scan_dir_mint(d, path) == 0) {
            if (oom) todo = NULL;
            if (!todo) scanning = 0;
            return;
        }
        use_mint = 0;
    }

    strcat(path, "\\*.*");
    olddta = Fgetdta();
    Fsetdta(&scan_dta);
    r = Fsfirst(path, scan_mask);
    while (r == 0) {
        const char *nm = scan_dta.dta_name;
        int dots = (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)));
        if (!dots && !(scan_dta.dta_attribute & 0x08)) {
            if (scan_dta.dta_attribute & 0x10) {
                Node *n = new_node(d, nm, 1, 0);
                if (!n) { oom = 1; break; }
                n->todo = todo; todo = n; n_dirs++;
            } else {
                if (!new_node(d, nm, 0, (u64)(unsigned long)scan_dta.dta_size)) {
                    oom = 1; break;
                }
                n_files++;
            }
        }
        r = Fsnext();
    }
    Fsetdta(olddta);
    if (oom) todo = NULL;
    if (!todo) scanning = 0;
}

static void add_cell(Node *n, short x, short y, short w, short h)
{
    if (ncells >= capcells) {
        long ncap = capcells + 1024;
        Cell *nc;
        if (ncells >= MAXCELLS) { cells_full = 1; return; }
        nc = (Cell *)realloc(cells, ncap * sizeof(Cell));
        if (!nc) { cells_full = 1; return; }
        cells = nc; capcells = ncap;
    }
    cells[ncells].x = x; cells[ncells].y = y;
    cells[ncells].w = w; cells[ncells].h = h;
    cells[ncells].n = n;
    ncells++;
}

static void layout_dir(Node *d, short x, short y, short w, short h);

static void place(Node *n, short x, short y, short w, short h)
{
    if (w <= 0 || h <= 0) return;
    if (n->isdir) {
        if (w < min_px || h < min_px) { add_cell(n, x, y, w, h); return; }   /* too small: one cell */
        if (show_dirs && w >= 10 && h >= 10) { x++; y++; w -= 2; h -= 2; }
        layout_dir(n, x, y, w, h);
    } else
        add_cell(n, x, y, w, h);
}

#define WT(n) ((u32)((n)->size >> shift))

static void layout_dir(Node *d, short x, short y, short w, short h)
{
    Node *c, *p, *q;
    u32 R = 0;
    int shift = 0;
    u64 total;

    if (w <= 0 || h <= 0 || !d->child || d->size == 0) return;

    total = d->size;
    while ((total >> shift) > 0xFFFFFFUL) shift++;
    for (c = d->child; c && WT(c) > 0; c = c->next) R += WT(c);

    c = d->child;
    while (c && WT(c) > 0 && R > 0 && w > 0 && h > 0) {
        if (w < min_px || h < min_px) {          /* remaining strip too thin: lump the rest */
            add_cell(d, x, y, w, h);
            break;
        }
        u64  A    = (u64)(u32)w * (u32)h;
        long side = (w >= h) ? h : w;
        long len  = (w >= h) ? w : h;
        u64  k    = (A << 16) / R;
        u64  sq   = (u64)side * (u64)side;
        u64  S = 0, amax = 0, worst_prev = 0;
        u32  Wsum = 0;
        int  n = 0, i;
        long t, pos;
        u64  cum;

        for (p = c; p && WT(p) > 0; p = p->next) {
            u64 a = ((u64)WT(p) * k) >> 16;
            u64 S2, w1, w2, worst;
            if (a < 1) a = 1;
            S2 = S + a;
            if (n == 0) amax = a;
            w1 = (sq * amax * 256) / (S2 * S2);
            w2 = (S2 * S2 * 256) / (sq * a);
            worst = (w1 > w2) ? w1 : w2;
            if (n > 0 && worst > worst_prev) break;
            S = S2; worst_prev = worst; Wsum += WT(p); n++;
        }

        if (!p || WT(p) == 0) t = len;
        else {
            t = ((long)S + side / 2) / side;
            if (t < 1) t = 1;
            if (t > len) t = len;
        }
        if (t < min_px) {                        /* row thinner than the minimum: all that is */
            add_cell(d, x, y, w, h);             /* left in this folder is crumbs, one cell   */
            break;
        }

        pos = 0; cum = 0;
        for (i = 0, q = c; i < n; i++, q = q->next) {
            u64 a = ((u64)WT(q) * k) >> 16;
            long end;
            if (a < 1) a = 1;
            cum += a;
            end = (i == n - 1) ? side : (long)((cum * (u64)side) / S);
            if (end > pos) {
                if (w >= h) place(q, x, (short)(y + pos), (short)t, (short)(end - pos));
                else        place(q, (short)(x + pos), y, (short)(end - pos), (short)t);
                pos = end;
            }
        }

        if (w >= h) { x += (short)t; w -= (short)t; }
        else        { y += (short)t; h -= (short)t; }
        R -= Wsum;
        c = p;
    }
}

static void relayout(void)
{
    clock_t t0 = clock();
    ncells = 0; cells_full = 0;
    layout_pending = 0;
    if (!mainwin) return;
    WindGet(mainwin, WF_WORKXYWH, &work.g_x, &work.g_y, &work.g_w, &work.g_h);
    if (view_root && work.g_h > BAR_H)
        layout_dir(view_root, work.g_x, work.g_y, work.g_w, (short)(work.g_h - BAR_H));
    t_layout = clock() - t0;
    t_draw = 0;
}

static void post_redraw(const GRECT *g)
{
    GRECT r;
    if (!mainwin) return;
    r = *g;
    EvntRedrawGrect(mainwin, &r);
}

static void redraw_all(void)
{
    GRECT w;
    if (!mainwin) return;
    WindGet(mainwin, WF_WORKXYWH, &w.g_x, &w.g_y, &w.g_w, &w.g_h);
    post_redraw(&w);
}

/* busy bee while the layout is computed and the cushions are drawn (about a second with
 * many cells), arrow again afterwards unless a scan is still running */
static void mouse_busy(int on)
{
    graf_mouse((on || scanning) ? BUSYBEE : ARROW, 0L);
}

static void relayout_redraw(void)
{
    mouse_busy(1);
    relayout();
    redraw_all();
    mouse_busy(0);
}

static void bar_rect(GRECT *g)
{
    g->g_x = work.g_x; g->g_w = work.g_w;
    g->g_h = BAR_H;    g->g_y = work.g_y + work.g_h - BAR_H;
}

/* full redraw of the whole window (borders, menu, work) via the message queue, so it
 * runs after the AES has finished removing a dialog or file selector */
static void post_full_redraw(void)
{
    short x, y, w, h;
    if (!mainwin) return;
    wind_get(mainwin->handle, WF_CURRXYWH, &x, &y, &w, &h);
    ApplWrite(_AESapid, WM_REDRAW, mainwin->handle, x, y, w, h);
}

static void redraw_node(const Node *n)
{
    long i;
    if (!n) return;
    for (i = 0; i < ncells; i++)
        if (cells[i].n == n) {
            GRECT g;
            g.g_x = cells[i].x; g.g_y = cells[i].y;
            g.g_w = cells[i].w; g.g_h = cells[i].h;
            post_redraw(&g);
            return;
        }
}

static void hsv2rgb(int hdeg, int s, int v, short *rgb)
{
    int i = hdeg / 60, f = hdeg % 60;
    long p = (long)v * (1000 - s) / 1000;
    long q = (long)v * (1000 - (long)s * f / 60) / 1000;
    long t = (long)v * (1000 - (long)s * (60 - f) / 60) / 1000;
    long r, g, b;
    switch (i) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    rgb[0] = (short)r; rgb[1] = (short)g; rgb[2] = (short)b;
}

static short nearest_pen(const short *target, short sys_pal[256][3], int npens)   /* no const: C won't convert short(*)[3] */
{
    int i, best = 1;
    long best_d = 0x7FFFFFFFL;
    for (i = 0; i < npens; i++) {
        long dr, dg, db, d;
        if (sys_pal[i][0] < 0) continue;                 /* pen does not exist */
        dr = (long)target[0] - sys_pal[i][0];
        dg = (long)target[1] - sys_pal[i][1];
        db = (long)target[2] - sys_pal[i][2];
        d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
            if (d == 0) break;
        }
    }
    return (short)best;
}

static void setup_colors(void)
{
    static const short hue_deg[NHUES] =
        { 0, 0, 28, 52, 95, 135, 175, 200, 220, 255, 285, 325 };
    short ext[57];
    short sys_pal[256][3];
    int npens, i, hue;

    vq_extnd(vh, 1, ext);
    planes = ext[4];
    if (planes <= 4) {
        cushion_ok = 0;
        cushions_on = 0;
        return;
    }

    /* only pens that really exist: 2^planes, at most 256 (true colour modes use 256) */
    npens = (planes >= 8) ? 256 : (1 << planes);
    for (i = 0; i < 256; i++) {
        sys_pal[i][0] = sys_pal[i][1] = sys_pal[i][2] = -1;      /* -1 = not available */
        if (i < npens && vq_color(vh, (short)i, 0, sys_pal[i]) < 0)
            sys_pal[i][0] = -1;
    }

    for (hue = 0; hue < NHUES; hue++) {
        short rgb[3];
        int sat = (hue == 0) ? 0 : 750;
        hsv2rgb(hue_deg[hue], sat, 720, rgb);
        pen_hi[hue] = nearest_pen(rgb, sys_pal, npens);
        hsv2rgb(hue_deg[hue], (hue == 0) ? 0 : 850, 380, rgb);
        pen_lo[hue] = nearest_pen(rgb, sys_pal, npens);
        hsv2rgb(hue_deg[hue], (hue == 0) ? 0 : 400, 960, rgb);
        pen_top[hue] = nearest_pen(rgb, sys_pal, npens);
    }
    cushion_ok = 1;
    cushions_on = 1;
}

static void draw_cushion(const Cell *c)
{
    short pxy[10];
    short x0 = c->x, y0 = c->y;
    short x1 = c->x + c->w - 1, y1 = c->y + c->h - 1;
    int hue = c->n->hue;

    vsf_color(vh, pen_hi[hue]);
    pxy[0] = x0; pxy[1] = y0;
    pxy[2] = x1; pxy[3] = y1;
    vr_recfl(vh, pxy);

    if (c->w >= 4 && c->h >= 4) {
        vsl_color(vh, pen_top[hue]);
        pxy[0] = x0 + 1; pxy[1] = y1 - 1;
        pxy[2] = x0 + 1; pxy[3] = y0 + 1;
        pxy[4] = x1 - 1; pxy[5] = y0 + 1;
        v_pline(vh, 3, pxy);

        vsl_color(vh, pen_lo[hue]);
        pxy[0] = x1 - 1; pxy[1] = y0 + 1;
        pxy[2] = x1 - 1; pxy[3] = y1 - 1;
        pxy[4] = x0 + 1; pxy[5] = y1 - 1;
        v_pline(vh, 3, pxy);
    }

    if (c->w >= 6 && c->h >= 6) {
        vsl_color(vh, pen_top[hue]);
        pxy[0] = x0 + 2; pxy[1] = y1 - 2;
        pxy[2] = x0 + 2; pxy[3] = y0 + 2;
        pxy[4] = x1 - 2; pxy[5] = y0 + 2;
        v_pline(vh, 3, pxy);

        vsl_color(vh, pen_lo[hue]);
        pxy[0] = x1 - 2; pxy[1] = y0 + 2;
        pxy[2] = x1 - 2; pxy[3] = y1 - 2;
        pxy[4] = x0 + 2; pxy[5] = y1 - 2;
        v_pline(vh, 3, pxy);
    }

    if (c->w >= 3 && c->h >= 3) {
        vsl_color(vh, 1);
        pxy[0] = x0; pxy[1] = y0;
        pxy[2] = x1; pxy[3] = y0;
        pxy[4] = x1; pxy[5] = y1;
        pxy[6] = x0; pxy[7] = y1;
        pxy[8] = x0; pxy[9] = y0;
        v_pline(vh, 5, pxy);
    }
}

static void draw_flat(const Cell *c)
{
    static const short flatpen[NHUES] = { 8, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14 };
    short pxy[10];
    int hue = c->n->hue;

    if (planes >= 4) {
        vsf_interior(vh, FIS_SOLID);
        vsf_color(vh, flatpen[hue]);
    } else {
        vsf_interior(vh, FIS_PATTERN);
        vsf_style(vh, 1 + hue * 2);
        vsf_color(vh, (planes == 2) ? 1 + hue % 3 : 1);
    }
    pxy[0] = c->x; pxy[1] = c->y;
    pxy[2] = c->x + c->w - 1; pxy[3] = c->y + c->h - 1;
    vr_recfl(vh, pxy);

    if (c->w >= 3 && c->h >= 3) {
        vsl_color(vh, 1);
        pxy[4] = pxy[2]; pxy[5] = pxy[3];
        pxy[6] = pxy[0]; pxy[7] = pxy[3];
        pxy[8] = pxy[0]; pxy[9] = pxy[1];
        pxy[2] = pxy[4]; pxy[3] = pxy[1];
        v_pline(vh, 5, pxy);
    }
}

static void draw_sel(const Cell *c)
{
    short pxy[10];
    int inset;
    for (inset = 0; inset < 2; inset++) {
        short x0 = c->x + inset, y0 = c->y + inset;
        short x1 = c->x + c->w - 1 - inset, y1 = c->y + c->h - 1 - inset;
        if (x1 < x0 || y1 < y0) break;
        vsl_color(vh, inset == 0 ? 1 : 0);
        pxy[0] = x0; pxy[1] = y0; pxy[2] = x1; pxy[3] = y0;
        pxy[4] = x1; pxy[5] = y1; pxy[6] = x0; pxy[7] = y1;
        pxy[8] = x0; pxy[9] = y0;
        v_pline(vh, 5, pxy);
    }
}

/* draws text, cut at the front ("...tail") if longer than maxc characters */
static void bar_text(short x, short y, const char *txt, int maxc)
{
    char buf[PATHLEN + 64];
    int len = (int)strlen(txt);
    if (maxc <= 0) return;
    if (len > maxc) {
        if (maxc <= 3) { buf[0] = 0; }
        else { strcpy(buf, "..."); strcat(buf, txt + len - (maxc - 3)); }
        txt = buf;
    }
    v_gtext(vh, x, y, (char *)txt);
}

/* status bar at the bottom of the work area; clip = pxy of the current redraw rectangle */
static void draw_bar(const short *clip)
{
    GRECT b;
    short pxy[4], d;
    int maxc, rl, room;

    bar_rect(&b);
    if (b.g_w <= 0 || b.g_y > clip[3] || b.g_y + b.g_h - 1 < clip[1]) return;

    vsf_interior(vh, FIS_SOLID);
    vsf_color(vh, planes >= 4 ? 8 : 0);                 /* light grey, white on mono/4 colours */
    pxy[0] = b.g_x; pxy[1] = b.g_y;
    pxy[2] = b.g_x + b.g_w - 1; pxy[3] = b.g_y + b.g_h - 1;
    vr_recfl(vh, pxy);
    vsl_color(vh, 1);
    pxy[3] = pxy[1];
    v_pline(vh, 2, pxy);

    vswr_mode(vh, MD_TRANS);
    vst_color(vh, 1);
    vst_alignment(vh, 0, 5, &d, &d);                    /* left, top */
    maxc = (b.g_w - cw) / cw;
    rl = (int)strlen(status_r);
    if (rl > maxc) rl = 0;                              /* window too narrow: drop the timing */
    room = maxc - (rl ? rl + 2 : 0);
    bar_text((short)(b.g_x + cw / 2), (short)(b.g_y + 3), status_l, room);
    if (rl)
        v_gtext(vh, (short)(b.g_x + b.g_w - cw / 2 - rl * cw), (short)(b.g_y + 3), status_r);
    vst_alignment(vh, 0, 0, &d, &d);
    vswr_mode(vh, MD_REPLACE);
}

static void __CDECL evnt_draw(WINDOW *win, short buff[8])
{
    GRECT clip;
    short pxy[4];
    long i;
    clock_t t0;

    clip.g_x = buff[4];
    clip.g_y = buff[5];
    clip.g_w = buff[6];
    clip.g_h = buff[7];

    vh = win->graf->handle;
    if (clip.g_w <= 0 || clip.g_h <= 0) return;
    t0 = clock();

    pxy[0] = clip.g_x; pxy[1] = clip.g_y;
    pxy[2] = clip.g_x + clip.g_w - 1; pxy[3] = clip.g_y + clip.g_h - 1;
    vs_clip(vh, 1, pxy);

    vswr_mode(vh, 1);
    vsf_perimeter(vh, 0);
    vsf_interior(vh, FIS_SOLID);
    vsf_color(vh, 0);
    vr_recfl(vh, pxy);
    vsl_type(vh, 1);
    vsl_width(vh, 1);

    if (ncells == 0) {
        vst_color(vh, 1);
        if (scanning || layout_pending)
            ;
        else if (view_root)
            v_gtext(vh, work.g_x + cw, work.g_y + 2 * ch, "(leer)");
        else
            v_gtext(vh, work.g_x + cw, work.g_y + 2 * ch,
                    "Datei > " "\231" "ffnen: Laufwerk oder Ordner w" "\204" "hlen");
    }

    if (cushions_on) vsf_interior(vh, FIS_SOLID);
    if (pxy[1] >= work.g_y + work.g_h - BAR_H) i = ncells;    /* only the status bar: skip cells */
    else i = 0;
    for (; i < ncells; i++) {
        const Cell *c = &cells[i];
        if (c->x > pxy[2] || c->y > pxy[3] ||
            c->x + c->w - 1 < pxy[0] || c->y + c->h - 1 < pxy[1])
            continue;
        if (cushions_on) draw_cushion(c); else draw_flat(c);
        if (c->n == sel) draw_sel(c);
    }

    draw_bar(pxy);
    vs_clip(vh, 0, pxy);
    t_draw += clock() - t0;
}

static void count_tree(const Node *n, u32 *f, u32 *d)
{
    const Node *c;
    for (c = n->child; c; c = c->next)
        if (c->isdir) { (*d)++; count_tree(c, f, d); }
        else (*f)++;
}

static void update_title(void)
{
    char path[PATHLEN], sz[40];
    if (view_root) {
        build_path(view_root, path, sizeof path);
        fmt_size(view_root->size, sz);
        if (scanning) snprintf(title, sizeof title, " %s: %s ", APPNAME, path);
        else {
            u32 f = 0, d = 0;                    /* counts of the current view (zoom!) */
            count_tree(view_root, &f, &d);
            snprintf(title, sizeof title, " %s: %s  (%s, %lu Dateien, %lu Ordner)%s ",
                     APPNAME, path, sz, f, d, cells_full ? "  [Anzeige begrenzt]" : "");
        }
    } else
        snprintf(title, sizeof title, " %s ", APPNAME);
    if (mainwin) WindSetStr(mainwin, WF_NAME, title);
}

static void fmt_ticks(clock_t t, char *buf)       /* 200 Hz ticks -> "12.35" seconds */
{
    sprintf(buf, "%lu.%02lu", (unsigned long)(t / 200), (unsigned long)(t % 200 / 2));
}

static void update_info(void)
{
    char path[PATHLEN], sz[40];
    char *out = status_l;
    size_t cap = sizeof status_l;

    if (scanning)
        snprintf(out, cap, "Scanne: %lu Dateien, %lu Ordner   (Esc = Abbruch)",
                 n_files, n_dirs);
    else if (oom)
        snprintf(out, cap, "Speicher voll - Ergebnis unvollst" AE "ndig");
    else if (status_msg[0])
        snprintf(out, cap, "%s", status_msg);
    else if (sel) {
        build_path(sel, path, sizeof path);
        fmt_size(sel->size, sz);
        snprintf(out, cap, "%s   %s", path, sz);
    } else if (view_root)
        snprintf(out, cap, "%lu Dateien, %lu Ordner   (Scan %lu.%lu s%s)", n_files, n_dirs,
                 (unsigned long)(scan_ticks / 200), (unsigned long)(scan_ticks % 200 / 20),
                 scan_aborted ? ", abgebrochen - unvollst" AE "ndig" : "");
    else
        snprintf(out, cap, "Kein Ordner gew" AE "hlt");

    /* timing (key T) goes to the right part, so the file name stays visible */
    status_r[0] = 0;
    if (show_timing && view_root && !scanning) {
        char a[12], b[12], c[12], d[12];
        fmt_ticks(scan_ticks, a); fmt_ticks(t_sort, b);
        fmt_ticks(t_layout, c);   fmt_ticks(t_draw, d);
        snprintf(status_r, sizeof status_r, "Scan %s  Sort %s  Layout %s  Zeichnen %s s  (%ld)",
                 a, b, c, d, ncells);
    }

    if (mainwin && work.g_w > 0) {
        GRECT g;
        clock_t keep = t_draw;          /* drawing the bar is not part of the measurement */
        bar_rect(&g);
        post_redraw(&g);
        t_draw = keep;
    }
}

static void start_scan(const char *p)
{
    char buf[PATHLEN];
    int i, len;

    strncpy(buf, p, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (i = 0; buf[i]; i++) if (buf[i] == '/') buf[i] = '\\';
    len = (int)strlen(buf);
    while (len > 0 && buf[len - 1] == '\\') buf[--len] = 0;
    if (len == 0) { buf[0] = 'A' + (char)Dgetdrv(); buf[1] = ':'; buf[2] = 0; }

    root = view_root = sel = todo = NULL;           /* drop the old tree in one go */
    ncells = 0;                                     /* cells point into the pool */
    pool_free_all();
    status_msg[0] = 0;
    oom = 0; cells_full = 0;
    n_files = n_dirs = scan_steps = 0;
    strcpy(scan_root, buf);

    root = new_node(NULL, buf, 1, 0);
    if (!root) { oom = 1; update_info(); return; }
    view_root = root;
    todo = root;
    scanning = 1;
    scan_aborted = 0;
    t_step = t_node = t_hue = 0;
    probing = probed = show_timing;
    scan_t0 = clock();
    graf_mouse(BUSYBEE, 0L);
    update_title(); update_info(); redraw_all();
}

static void finish_scan(void)
{
    scan_ticks = clock() - scan_t0;
    probing = 0;
    scanning = 0; todo = NULL;
    mouse_busy(1);                          /* sort + layout + draw still to come */
    if (root) {
        clock_t t0 = clock();
        finish_dir(root);
        t_sort = clock() - t0;
    }
    update_title();
    relayout_redraw();                      /* ends with the arrow */
    update_info();
}

static void choose_folder(void)
{
    static char fpath[PATHLEN], fname[260];        /* FSLX writes up to 65, fsel_exinput may write long names */
    int len, r;

    if (!fpath[0]) sprintf(fpath, "%c:\\", 'A' + (int)Dgetdrv());
    fname[0] = 0;
    r = FselInput(fpath, fname, "*.*", APPNAME ": Ordner w" AE "hlen", NULL, NULL);
    post_full_redraw();                            /* area under the file selector */
    if (!r) return;
    len = (int)strlen(fpath);
    if (len > 0) start_scan(fpath);
}

static void select_node(Node *n)
{
    Node *old = sel;
    status_msg[0] = 0;
    sel = n;
    redraw_node(old);
    redraw_node(n);
    update_info();
}

static void zoom_in(void)
{
    Node *a = sel;
    if (!a || scanning) return;
    while (a && a->parent != view_root) a = a->parent;
    if (a && a->isdir) {
        view_root = a;
        update_title(); relayout_redraw(); update_info();
    }
}

static void zoom_out(void)
{
    if (scanning || !view_root || !view_root->parent) return;
    view_root = view_root->parent;
    sel = NULL;
    update_title(); relayout_redraw(); update_info();
}

static void refresh(void)
{
    char p[PATHLEN];
    if (scanning) return;
    if (!scan_root[0]) { choose_folder(); return; }
    strcpy(p, scan_root);
    start_scan(p);
}

static void toggle_colors(void)
{
    if (!cushion_ok) return;
    cushions_on = !cushions_on;
    MenuIcheck(mainwin, M_COLOR, cushions_on);
    redraw_all();
}

static void set_min_px(int step)               /* step -1 / +1 through min_steps */
{
    int i = 0;
    while (i < NMIN - 1 && min_steps[i] < min_px) i++;
    i += step;
    if (i < 0) i = 0;
    if (i >= NMIN) i = NMIN - 1;
    if (min_steps[i] == min_px) return;
    min_px = min_steps[i];
    snprintf(status_msg, sizeof status_msg, "Mindestgr" OE "\236e: %d Pixel%s", min_px,
             min_px == 1 ? " (alles einzeln)" : "");
    relayout_redraw();
    update_info();
}

/* ---------------------------------------------------------------------------------
 * Settings window: a WinDom form in its own window, built in code like the menu.
 * Checkboxes are a G_BOXCHAR square ('X' or blank) plus a clickable label, both
 * TOUCHEXIT. WinDom's own 3D checkbox type (DCRBUTTON) is drawn by xobjc.ldg and would
 * fall back to nothing if that LDG is missing, so plain AES objects are used.
 * Changes go to the t_* copies; OK applies them, Sichern applies and writes DXMAP.INF,
 * Abbruch (or the closer) throws them away.
 * ------------------------------------------------------------------------------- */
enum { S_ROOT, S_T1, S_CB_DIRS, S_L_DIRS, S_CB_CUSH, S_L_CUSH, S_L_MIN, S_POP_MIN,
       S_T2, S_CB_HID, S_L_HID, S_NOTE, S_SAVE, S_CANCEL, S_OK, S_NOBJ };

static OBJECT  settree[S_NOBJ];
static WINDOW *setwin;
static int     t_dirs, t_cush, t_hid, t_min;
static char    pop_txt[32];
static char    inf_path[PATHLEN];

static void sobj(int i, int next, int head, int tail, int type, int flags, int state,
                 long spec, const char *str, int x, int y, int w, int h)
{
    OBJECT *o = &settree[i];
    o->ob_next = next; o->ob_head = head; o->ob_tail = tail;
    o->ob_type = type; o->ob_flags = flags; o->ob_state = state;
    if (str) o->ob_spec.free_string = (char *)str; else o->ob_spec.index = spec;
    o->ob_x = x; o->ob_y = y; o->ob_width = w; o->ob_height = h;
}

#define CB_SPEC(on)  ((((long)((on) ? 'X' : ' ')) << 24) | 0x00011100L)

static void set_pop_txt(int px)
{
    sprintf(pop_txt, " %d Pixel \002", px);              /* \002 = arrow down (Atari font) */
}

static void build_settings(void)
{
    static char t1[] = "Darstellung", t2[] = "Scan";
    static char l_dirs[] = "Ordnerr" AE "nder anzeigen";
    static char l_cush[] = "Kissen-Schattierung (Farbmodus)";
    static char l_min[]  = "Mindestgr" OE "\236e:";
    static char l_hid[]  = "Versteckte Dateien mitz" AE "hlen";
    static char note[]   = "(gilt beim n" AE "chsten Scan)";
    static char b_save[] = "Sichern", b_cancel[] = "Abbruch", b_ok[] = "OK";
    int Y = ch / 2;                                       /* half a text line */
    int cb = ch;                                          /* checkbox: square */
    int lx = 4 * cw + cb;                                 /* label x */
    int bh = ch + 4;
    int W = 40 * cw, H = 27 * Y + bh;
    int cflag = cushion_ok ? 0 : OS_DISABLED;

    sobj(S_ROOT,   -1, S_T1, S_OK, G_BOX, OF_NONE, OS_NORMAL, 0x00001100L, NULL, 0, 0, W, H);
    sobj(S_T1,      S_CB_DIRS, -1, -1, G_STRING, OF_NONE, OS_NORMAL, 0, t1, 2 * cw, 2 * Y, 11 * cw, ch);
    sobj(S_CB_DIRS, S_L_DIRS,  -1, -1, G_BOXCHAR, OF_TOUCHEXIT, OS_NORMAL, CB_SPEC(0), NULL, 3 * cw, 5 * Y, cb, cb);
    sobj(S_L_DIRS,  S_CB_CUSH, -1, -1, G_STRING, OF_TOUCHEXIT, OS_NORMAL, 0, l_dirs, lx, 5 * Y, 30 * cw, ch);
    sobj(S_CB_CUSH, S_L_CUSH,  -1, -1, G_BOXCHAR, OF_TOUCHEXIT, cflag, CB_SPEC(0), NULL, 3 * cw, 8 * Y, cb, cb);
    sobj(S_L_CUSH,  S_L_MIN,   -1, -1, G_STRING, OF_TOUCHEXIT, cflag, 0, l_cush, lx, 8 * Y, 31 * cw, ch);
    sobj(S_L_MIN,   S_POP_MIN, -1, -1, G_STRING, OF_NONE, OS_NORMAL, 0, l_min, 3 * cw, 11 * Y + 2, 14 * cw, ch);
    sobj(S_POP_MIN, S_T2,      -1, -1, G_BUTTON, OF_TOUCHEXIT, OS_SHADOWED, 0, pop_txt, 18 * cw, 11 * Y, 13 * cw, ch + 4);
    sobj(S_T2,      S_CB_HID,  -1, -1, G_STRING, OF_NONE, OS_NORMAL, 0, t2, 2 * cw, 15 * Y, 4 * cw, ch);
    sobj(S_CB_HID,  S_L_HID,   -1, -1, G_BOXCHAR, OF_TOUCHEXIT, OS_NORMAL, CB_SPEC(0), NULL, 3 * cw, 18 * Y, cb, cb);
    sobj(S_L_HID,   S_NOTE,    -1, -1, G_STRING, OF_TOUCHEXIT, OS_NORMAL, 0, l_hid, lx, 18 * Y, 28 * cw, ch);
    sobj(S_NOTE,    S_SAVE,    -1, -1, G_STRING, OF_NONE, OS_NORMAL, 0, note, lx, 20 * Y, 26 * cw, ch);
    sobj(S_SAVE,    S_CANCEL,  -1, -1, G_BUTTON, OF_SELECTABLE | OF_EXIT, OS_NORMAL, 0, b_save, 2 * cw, 24 * Y, 10 * cw, bh);
    sobj(S_CANCEL,  S_OK,      -1, -1, G_BUTTON, OF_SELECTABLE | OF_EXIT, OS_NORMAL, 0, b_cancel, 15 * cw, 24 * Y, 10 * cw, bh);
    sobj(S_OK,      S_ROOT,    -1, -1, G_BUTTON, OF_SELECTABLE | OF_EXIT | OF_DEFAULT | OF_LASTOB,
         OS_NORMAL, 0, b_ok, 28 * cw, 24 * Y, 10 * cw, bh);
}

static void settings_to_tree(void)
{
    settree[S_CB_DIRS].ob_spec.index = CB_SPEC(t_dirs);
    settree[S_CB_CUSH].ob_spec.index = CB_SPEC(t_cush);
    settree[S_CB_HID].ob_spec.index  = CB_SPEC(t_hid);
    set_pop_txt(t_min);
}

static void apply_settings(void)
{
    int relay = (t_dirs != show_dirs) || (t_min != min_px);
    int recolor = cushion_ok && (t_cush != cushions_on);

    show_dirs   = t_dirs;
    min_px      = t_min;
    show_hidden = t_hid;
    scan_mask   = show_hidden ? 0x17 : 0x10;
    if (recolor) {
        cushions_on = t_cush;
        MenuIcheck(mainwin, M_COLOR, cushions_on);
    }
    if (relay) relayout_redraw();
    else if (recolor) redraw_all();
}

/* DXMAP.INF: plain "key=value" lines, next to the program (start directory) */
static void load_settings(void)
{
    char line[80], key[24];
    int v;
    FILE *f = fopen(inf_path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%23[^=]=%d", key, &v) == 2) {
            if      (!strcmp(key, "dirs"))     show_dirs = v != 0;
            else if (!strcmp(key, "cushions")) { if (cushion_ok) cushions_on = v != 0; }
            else if (!strcmp(key, "hidden"))   show_hidden = v != 0;
            else if (!strcmp(key, "minpx")) {
                int i;
                for (i = 0; i < NMIN; i++) if (min_steps[i] == v) min_px = v;
            }
        }
    fclose(f);
    scan_mask = show_hidden ? 0x17 : 0x10;
}

static int save_settings(void)
{
    FILE *f = fopen(inf_path, "w");
    if (!f) return 0;
    fprintf(f, "; " APPNAME " settings\ndirs=%d\ncushions=%d\nhidden=%d\nminpx=%d\n",
            show_dirs, cushions_on, show_hidden, min_px);
    return fclose(f) == 0;
}

static void close_settings(WINDOW *win)
{
    ApplWrite(_AESapid, WM_DESTROY, win->handle, 0, 0, 0, 0);
}

static void __CDECL set_toggle(WINDOW *win, int index, int mode, void *data)
{
    int *var = (int *)data;
    int cbox = (index == S_L_DIRS || index == S_CB_DIRS) ? S_CB_DIRS :
               (index == S_L_CUSH || index == S_CB_CUSH) ? S_CB_CUSH : S_CB_HID;
    if (settree[cbox].ob_state & OS_DISABLED) return;
    *var = !*var;
    settree[cbox].ob_spec.index = CB_SPEC(*var);
    ObjcDraw(OC_FORM, win, cbox, 0);
    {   /* WinDom returns TOUCHEXIT objects on the press; the main loop waits for "any
         * button down", so the same click came in again and toggled straight back */
        short d, mb;
        do graf_mkstate(&d, &d, &mb, &d); while (mb);
    }
}

static void __CDECL set_popup(WINDOW *win, int index, int mode, void *data)
{
    static char items[NMIN][28];
    static char *list[NMIN];
    short x, y;
    int i, cur = 0, r;

    for (i = 0; i < NMIN; i++) {
        sprintf(items[i], "  %2d Pixel%s  ", min_steps[i], min_steps[i] == 1 ? " (alles einzeln)" : "");
        list[i] = items[i];
        if (min_steps[i] == t_min) cur = i;
    }
    objc_offset(settree, index, &x, &y);
    r = MenuPopUp(list, x, y, NMIN, -1, cur + 1, P_LIST | P_WNDW | P_CHCK);
    if (r >= 1 && r <= NMIN) {
        t_min = min_steps[r - 1];
        set_pop_txt(t_min);
    }
    {   /* the popup's background was not restored in monochrome (TOS 4.04), so redraw
         * the whole settings window through the queue */
        short wx, wy, ww, wh;
        wind_get(win->handle, WF_CURRXYWH, &wx, &wy, &ww, &wh);
        ApplWrite(_AESapid, WM_REDRAW, win->handle, wx, wy, ww, wh);
    }
}

static void __CDECL set_button(WINDOW *win, int index, int mode, void *data)
{
    ObjcChange(OC_FORM, win, index, OS_NORMAL, TRUE);
    if (index == S_CANCEL) { close_settings(win); return; }
    apply_settings();
    if (index == S_SAVE) {
        if (save_settings())
            snprintf(status_msg, sizeof status_msg, "Einstellungen gesichert");
        else
            snprintf(status_msg, sizeof status_msg, "Konnte %s nicht schreiben", inf_path);
        update_info();
        return;                                           /* Sichern keeps the window open */
    }
    close_settings(win);
}

static void __CDECL setwin_destroy(WINDOW *win, short buff[8])
{
    setwin = NULL;
}

static void settings(void)
{
    if (setwin) {                                         /* already open: bring it to front */
        WindSet(setwin, WF_TOP, 0, 0, 0, 0);
        return;
    }
    t_dirs = show_dirs; t_cush = cushions_on; t_hid = show_hidden; t_min = min_px;
    build_settings();
    settings_to_tree();
    setwin = FormCreate(settree, WAT_FORM, NULL, APPNAME ": Einstellungen", NULL, TRUE, FALSE);
    if (!setwin) return;
    EvntAdd(setwin, WM_DESTROY, setwin_destroy, EV_TOP);
    ObjcAttachFormFunc(setwin, S_CB_DIRS, set_toggle, &t_dirs);
    ObjcAttachFormFunc(setwin, S_L_DIRS,  set_toggle, &t_dirs);
    ObjcAttachFormFunc(setwin, S_CB_CUSH, set_toggle, &t_cush);
    ObjcAttachFormFunc(setwin, S_L_CUSH,  set_toggle, &t_cush);
    ObjcAttachFormFunc(setwin, S_CB_HID,  set_toggle, &t_hid);
    ObjcAttachFormFunc(setwin, S_L_HID,   set_toggle, &t_hid);
    ObjcAttachFormFunc(setwin, S_POP_MIN, set_popup, NULL);
    ObjcAttachFormFunc(setwin, S_SAVE,    set_button, NULL);
    ObjcAttachFormFunc(setwin, S_CANCEL,  set_button, NULL);
    ObjcAttachFormFunc(setwin, S_OK,      set_button, NULL);
}

static char lastdest[PATHLEN];
static short popup_x, popup_y;

static int ci_char(int c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c == '/') c = '\\';
    return c;
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b && ci_char((unsigned char)*a) == ci_char((unsigned char)*b)) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int ci_prefix(const char *s, const char *pre)
{
    while (*pre && *s && ci_char((unsigned char)*s) == ci_char((unsigned char)*pre)) { s++; pre++; }
    return *pre == 0;
}

static void safe_text(char *dst, const char *src, int max)
{
    int len = (int)strlen(src), i = 0;
    if (len > max) { src += len - max + 3; dst[i++] = '.'; dst[i++] = '.'; dst[i++] = '.'; }
    for (; *src && i < max; src++) {
        char c = *src;
        if (c == '|' || c == '[' || c == ']' || c == '%') c = '_';
        dst[i++] = c;
    }
    dst[i] = 0;
}

static void err_alert(const char *what, const char *name, long code)
{
    char n[48], b[300];
    safe_text(n, name, 36);
    sprintf(b, "[3][%s|%s|Fehler %ld][ OK ]", what, n, code);
    FormAlert(1, b);
}

static int file_exists(const char *path)
{
    long h = Fopen(path, 0);
    if (h >= 0) { Fclose((short)h); return 1; }
    return 0;
}

static long copy_file(const char *src, const char *dst)
{
    long hs, hd, r = 0, n, bufsz = 32768L;
    char *buf;
    _DOSTIME dt;
    int have_dt;

    buf = (char *)malloc(bufsz);
    if (!buf) { bufsz = 4096L; buf = (char *)malloc(bufsz); }
    if (!buf) return -39;

    hs = Fopen(src, 0);
    if (hs < 0) { free(buf); return hs; }
    hd = Fcreate(dst, 0);
    if (hd < 0) { Fclose((short)hs); free(buf); return hd; }

    have_dt = (Fdatime(&dt, (short)hs, 0) == 0);
    for (;;) {
        n = Fread((short)hs, bufsz, buf);
        if (n < 0) { r = n; break; }
        if (n == 0) break;
        if (Fwrite((short)hd, n, buf) != n) { r = -1; break; }
    }
    if (r == 0 && have_dt) Fdatime(&dt, (short)hd, 1);
    Fclose((short)hs);
    Fclose((short)hd);
    if (r != 0) Fdelete(dst);
    free(buf);
    return r;
}

static void size_adjust(Node *d, u64 delta, int add)
{
    for (; d; d = d->parent) {
        if (add) d->size += delta;
        else     d->size = (d->size >= delta) ? d->size - delta : 0;
        if (d->parent) d->parent->child = msort(d->parent->child);
    }
}

static void node_remove(Node *n)
{
    Node *p = n->parent, **pp;
    u64 sz = n->size;
    if (!p) return;
    for (pp = &p->child; *pp; pp = &(*pp)->next)
        if (*pp == n) { *pp = n->next; break; }
    n->next = NULL;
    if (sel == n) sel = NULL;                       /* memory stays in the pool until the next scan */
    if (n_files) n_files--;
    size_adjust(p, sz, 0);
}

static Node *find_dir(const char *path)
{
    char rp[PATHLEN], tp[PATHLEN];
    const char *p;
    Node *d;
    size_t len;

    if (!root) return NULL;
    build_path(root, rp, sizeof rp);
    strncpy(tp, path, sizeof tp - 1); tp[sizeof tp - 1] = 0;
    for (len = strlen(rp); len > 0 && (rp[len - 1] == '\\' || rp[len - 1] == '/'); ) rp[--len] = 0;
    for (len = strlen(tp); len > 0 && (tp[len - 1] == '\\' || tp[len - 1] == '/'); ) tp[--len] = 0;
    if (!ci_prefix(tp, rp)) return NULL;
    p = tp + strlen(rp);
    if (*p && *p != '\\' && *p != '/') return NULL;

    d = root;
    while (*p) {
        char comp[260];
        Node *c;
        int i = 0;
        p++;
        while (*p && *p != '\\' && *p != '/' && i < (int)sizeof comp - 1) comp[i++] = *p++;
        comp[i] = 0;
        if (!comp[0]) continue;
        for (c = d->child; c; c = c->next)
            if (c->isdir && ci_eq(c->name, comp)) break;
        if (!c) return NULL;
        d = c;
    }
    return d;
}

static void note_new_file(const char *dst, u64 size)
{
    char dir[PATHLEN];
    const char *name;
    char *a, *b, *cut;
    Node *dd, *old;

    strncpy(dir, dst, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    a = strrchr(dir, '\\'); b = strrchr(dir, '/');
    cut = (a > b) ? a : b;
    if (!cut) return;
    name = cut + 1;
    *cut = 0;
    dd = find_dir(dir);
    if (!dd) return;
    for (old = dd->child; old; old = old->next)
        if (!old->isdir && ci_eq(old->name, name)) { node_remove(old); break; }
    if (new_node(dd, name, 0, size)) {
        n_files++;
        dd->child = msort(dd->child);
        size_adjust(dd, size, 1);
    }
}

static void finish_op(void)
{
    update_title();
    relayout_redraw();
    update_info();
}

#define MAXSUB   1000
#define ITEMLEN  96

static int cmp_names(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    while (*x && *y && ci_char((unsigned char)*x) == ci_char((unsigned char)*y)) { x++; y++; }
    return ci_char((unsigned char)*x) - ci_char((unsigned char)*y);
}

static int add_name(char ***v, int *n, int *cap, const char *nm)
{
    if (*n >= MAXSUB) return 0;
    if (*n >= *cap) {
        int nc = *cap + 32;
        char **nv = (char **)realloc(*v, nc * sizeof(char *));
        if (!nv) return 0;
        *v = nv; *cap = nc;
    }
    (*v)[*n] = strdup(nm);
    if (!(*v)[*n]) return 0;
    (*n)++;
    return 1;
}

static int list_subdirs(const char *dir, char ***out)
{
    char **v = NULL;
    int n = 0, cap = 0;
    char base[PATHLEN];
    size_t blen;

    strncpy(base, dir, sizeof base - 8); base[sizeof base - 8] = 0;
    blen = strlen(base);
    if (blen == 0 || (base[blen - 1] != '\\' && base[blen - 1] != '/')) { base[blen++] = '\\'; base[blen] = 0; }

    if (use_mint) {
        static char buf[1024];
        static XA xa;
        long h = Dopendir(base, 0);
        if (h >= 0) {
            for (;;) {
                int k = dir_next(h, base, buf, sizeof buf, &xa);
                if (k < 0) break;
                if (k > 0) continue;
                if ((xa.mode & XA_IFMT) == XA_IFDIR && !add_name(&v, &n, &cap, buf + 4)) break;
            }
            Dclosedir(h);
            goto done;
        }
        if (h != -32L) goto done;
        use_mint = 0;
    }
    {
        char pat[PATHLEN];
        _DTA *olddta = Fgetdta();
        long r;
        strcpy(pat, base); strcat(pat, "*.*");
        Fsetdta(&scan_dta);
        for (r = Fsfirst(pat, 0x17); r == 0; r = Fsnext()) {
            const char *nm = scan_dta.dta_name;
            if ((scan_dta.dta_attribute & 0x10) && !is_dots(nm) && !add_name(&v, &n, &cap, nm)) break;
        }
        Fsetdta(olddta);
    }
done:
    if (n > 1) qsort(v, n, sizeof(char *), cmp_names);
    *out = v;
    return n;
}

enum { K_RECENT, K_DRIVE, K_FSEL, K_HERE, K_UP, K_DIR, K_PREV, K_MORE };

static char *make_item(const char *fmt, const char *a)
{
    char *it = (char *)malloc(ITEMLEN);
    if (it) snprintf(it, ITEMLEN, fmt, a);
    return it;
}

static int run_popup_tree(char **items, int n, short x, short y)
{
    OBJECT *tree;
    int i, maxw = 4, r;
    int ih = ch + 2;

    if (n <= 0) return -1;
    for (i = 0; i < n; i++) {
        int l = (int)strlen(items[i]);
        if (l > maxw) maxw = l;
    }
    tree = (OBJECT *)calloc((size_t)(n + 1), sizeof(OBJECT));
    if (!tree) return -1;

    tree[0].ob_next = -1;
    tree[0].ob_head = 1;
    tree[0].ob_tail = (short)n;
    tree[0].ob_type = G_BOX;
    tree[0].ob_flags = OF_NONE;
    tree[0].ob_state = OS_SHADOWED;
    tree[0].ob_spec.index = 0x00FF1100L;
    tree[0].ob_x = 0;
    tree[0].ob_y = 0;
    tree[0].ob_width = (short)(maxw * cw);
    tree[0].ob_height = (short)(n * ih);

    for (i = 0; i < n; i++) {
        OBJECT *o = &tree[i + 1];
        o->ob_next = (i == n - 1) ? 0 : (short)(i + 2);
        o->ob_head = -1;
        o->ob_tail = -1;
        o->ob_type = G_STRING;
        o->ob_flags = OF_SELECTABLE | ((i == n - 1) ? OF_LASTOB : OF_NONE);
        o->ob_state = OS_NORMAL;
        o->ob_spec.free_string = items[i];
        o->ob_x = 0;
        o->ob_y = (short)(i * ih);
        o->ob_width = (short)(maxw * cw);
        o->ob_height = (short)ih;
    }

    r = MenuPopUp(tree, x, y, 0, 0, 1, P_WNDW);
    free(tree);
    if (r >= 1 && r <= n) return r - 1;
    return -1;
}

static void free_list(char **v, int n)
{
    int i;
    if (!v) return;
    for (i = 0; i < n; i++) free(v[i]);
    free(v);
}

static void free_items(char **items, char **names, const int *kind, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        free(items[i]);
        if (kind[i] == K_DRIVE) free(names[i]);        /* K_DIR names belong to sub[] */
    }
}

/* returns 0 = cancelled, 1 = folder chosen (out ends with '\'), 2 = use the file selector.
 * A popup has no scrollbar, so long folder lists are split into pages that fit on the
 * screen: "< Vorherige" / "> Weitere" entries flip through them. The sub folder list is read
 * once per folder and kept while paging. */
static int pick_folder(const char *verb, short mx, short my, char *out, int max)
{
    char cur[PATHLEN];
    short x = mx, y = my;
    int rows = app.h / (ch + 3) - 2;
    int cap, result = 0;
    char **items = NULL, **names = NULL;
    int *kind = NULL;
    char **sub = NULL;
    int nsub = 0, have_sub = 0, page = 0;

    if (rows < 6) rows = 6;
    cap = rows + 40;                                   /* room for up to 26 drives */
    items = (char **)malloc(cap * sizeof(char *));
    names = (char **)malloc(cap * sizeof(char *));
    kind  = (int *)malloc(cap * sizeof(int));
    cur[0] = 0;
    if (!items || !names || !kind) goto out;

    for (;;) {
        int n = 0, r, i, len, fail = 0, done = 0, newdir = 0;
        int perpage = 0, first = 0, last = 0;
        short mb, ks;
        char t[64], label[ITEMLEN];

        if (!cur[0]) {                                           /* level 0: drives */
            long map = Drvmap();
            if (lastdest[0]) {
                safe_text(t, lastdest, 34);
                items[n] = make_item(" Zuletzt: %s ", t); names[n] = NULL; kind[n++] = K_RECENT;
            }
            for (i = 0; i < 26 && n < cap - 2; i++)
                if (map & (1L << i)) {
                    char dl[4];
                    dl[0] = (char)('A' + i); dl[1] = ':'; dl[2] = '\\'; dl[3] = 0;
                    snprintf(label, sizeof label, " %c:  >", 'A' + i);
                    items[n] = strdup(label); names[n] = strdup(dl); kind[n++] = K_DRIVE;
                }
            items[n] = strdup(" Dateiauswahl... "); names[n] = NULL; kind[n++] = K_FSEL;
        } else {                                                 /* folder level */
            if (!have_sub) { nsub = list_subdirs(cur, &sub); have_sub = 1; }

            safe_text(t, cur, 30);
            snprintf(label, sizeof label, " %s hierher: %s ", verb, t);
            items[n] = strdup(label); names[n] = NULL; kind[n++] = K_HERE;
            items[n] = strdup(" .. Zur" UE "ck "); names[n] = NULL; kind[n++] = K_UP;

            /* everything fits on one page, otherwise leave room for the two paging entries */
            perpage = (nsub <= rows - 2) ? nsub : rows - 4;
            if (perpage < 1) perpage = 1;
            first = page * perpage;
            if (first >= nsub) { page = 0; first = 0; }
            last = first + perpage;
            if (last > nsub) last = nsub;

            if (first > 0) {
                snprintf(label, sizeof label, " < Vorherige %d-%d ", first - perpage + 1, first);
                items[n] = strdup(label); names[n] = NULL; kind[n++] = K_PREV;
            }
            for (i = first; i < last; i++) {
                safe_text(t, sub[i], 34);
                snprintf(label, sizeof label, " %s  >", t);
                items[n] = strdup(label); names[n] = sub[i]; kind[n++] = K_DIR;
            }
            if (last < nsub) {
                int to = last + perpage;
                if (to > nsub) to = nsub;
                snprintf(label, sizeof label, " > Weitere %d-%d von %d ", last + 1, to, nsub);
                items[n] = strdup(label); names[n] = NULL; kind[n++] = K_MORE;
            }
        }

        for (i = 0; i < n; i++) if (!items[i]) fail = 1;
        if (fail) { free_items(items, names, kind, n); goto out; }

        r = run_popup_tree(items, n, x, y);
        graf_mkstate(&x, &y, &mb, &ks);

        if (r < 0 || r >= n) { free_items(items, names, kind, n); goto out; }

        switch (kind[r]) {
        case K_RECENT:
            strncpy(out, lastdest, max - 1); out[max - 1] = 0; result = 1; done = 1; break;
        case K_FSEL:
            result = 2; done = 1; break;
        case K_HERE:
            strncpy(out, cur, max - 1); out[max - 1] = 0; result = 1; done = 1; break;
        case K_DRIVE:
            strcpy(cur, names[r]); newdir = 1; break;
        case K_UP:
            len = (int)strlen(cur);
            while (len > 0 && cur[len - 1] == '\\') cur[--len] = 0;
            {
                char *bs = strrchr(cur, '\\');
                if (bs) bs[1] = 0; else cur[0] = 0;              /* "C:\" -> drive list */
            }
            newdir = 1; break;
        case K_DIR:
            if (strlen(cur) + strlen(names[r]) + 2 < sizeof cur) {
                strcat(cur, names[r]); strcat(cur, "\\");
            }
            newdir = 1; break;
        case K_PREV:
            if (page > 0) page--;
            break;
        case K_MORE:
            page++;
            break;
        }
        free_items(items, names, kind, n);
        if (done) goto out;
        if (newdir) { free_list(sub, nsub); sub = NULL; nsub = 0; have_sub = 0; page = 0; }
    }
out:
    free_list(sub, nsub);
    free(items); free(names); free(kind);
    return result;
}

static int choose_dest_fsel(const Node *n, const char *ttl, char *dst, int max)
{
    static char dpath[PATHLEN], dname[260];
    int len;

    if (lastdest[0]) strcpy(dpath, lastdest);
    else {
        if (!build_path(n->parent, dpath, sizeof dpath - 2))
            snprintf(dpath, sizeof dpath - 2, "%s", scan_root);   /* fsel needs a '\\' in it */
        len = (int)strlen(dpath);
        if (len > 0 && dpath[len - 1] != '\\') { dpath[len++] = '\\'; dpath[len] = 0; }
    }
    strncpy(dname, n->name, sizeof dname - 1); dname[sizeof dname - 1] = 0;
    len = FselInput(dpath, dname, "*.*", (char *)ttl, NULL, NULL);
    post_full_redraw();
    if (!len) return 0;
    if (!dname[0]) return 0;

    strncpy(lastdest, dpath, sizeof lastdest - 1); lastdest[sizeof lastdest - 1] = 0;
    len = (int)strlen(dpath);
    if (len > 0 && dpath[len - 1] != '\\' && dpath[len - 1] != '/') { dpath[len++] = '\\'; dpath[len] = 0; }
    if (len + (int)strlen(dname) + 1 >= max) return 0;
    strcpy(dst, dpath);
    strcat(dst, dname);
    return 1;
}

static int choose_dest(const Node *n, const char *verb, char *dst, int max)
{
    char folder[PATHLEN], ttl[48];
    int r, len;

    r = pick_folder(verb, popup_x, popup_y, folder, sizeof folder);
    if (r == 0) return 0;
    if (r == 2) {
        snprintf(ttl, sizeof ttl, "%s nach", verb);
        return choose_dest_fsel(n, ttl, dst, max);
    }
    strncpy(lastdest, folder, sizeof lastdest - 1); lastdest[sizeof lastdest - 1] = 0;
    len = (int)strlen(folder);
    if (len > 0 && folder[len - 1] != '\\' && folder[len - 1] != '/') { folder[len++] = '\\'; folder[len] = 0; }
    if (len + (int)strlen(n->name) + 1 >= max) return 0;
    strcpy(dst, folder);
    strcat(dst, n->name);
    return 1;
}

/* Same file? Under MiNT/MagiC one file can have several spellings (U:\c\x, C:\X, case
 * differences, links), so compare device + inode there. String compare only as fallback. */
static int same_file(const char *a, const char *b)
{
    if (use_mint) {
        static XA xa, xb;
        if (Fxattr(0, a, &xa) == 0) {
            if (Fxattr(0, b, &xb) != 0) return 0;        /* target does not exist */
            return xa.dev == xb.dev && xa.ino == xb.ino;
        }
    }
    return ci_eq(a, b);
}

static void path_too_long(const Node *n)
{
    err_alert("Pfad zu lang", n->name, -64L);           /* -64 = ERANGE */
}

static int dest_ok(const char *src, const char *dst)
{
    if (same_file(src, dst)) {
        FormAlert(1, "[1][Quelle und Ziel sind|identisch.][ OK ]");
        return 0;
    }
    if (file_exists(dst)) {
        if (FormAlert(2, "[2][Die Zieldatei existiert.|\232berschreiben?][Ja|Nein]") != 1)
            return 0;
        return 2;
    }
    return 1;
}

static void op_copy(Node *n)
{
    char src[PATHLEN], dst[PATHLEN], nm[300];
    long r;
    int ok;
    u64 size = n->size;

    if (!build_path(n, src, sizeof src)) { path_too_long(n); return; }
    if (!choose_dest(n, "Kopieren", dst, sizeof dst)) return;
    if (!(ok = dest_ok(src, dst))) return;

    graf_mouse(BUSYBEE, 0L);
    r = copy_file(src, dst);
    graf_mouse(ARROW, 0L);
    if (r < 0) { err_alert("Kopieren fehlgeschlagen", n->name, r); return; }

    strncpy(nm, n->name, sizeof nm - 1); nm[sizeof nm - 1] = 0;
    note_new_file(dst, size);
    snprintf(status_msg, sizeof status_msg, "Kopiert: %s", nm);
    finish_op();
}

static void op_move(Node *n)
{
    char src[PATHLEN], dst[PATHLEN], nm[300];
    long r;
    int ok, copied = 0;
    u64 size = n->size;

    if (!build_path(n, src, sizeof src)) { path_too_long(n); return; }
    if (!choose_dest(n, "Verschieben", dst, sizeof dst)) return;
    if (!(ok = dest_ok(src, dst))) return;
    strncpy(nm, n->name, sizeof nm - 1); nm[sizeof nm - 1] = 0;

    graf_mouse(BUSYBEE, 0L);
    if (ok == 2) Fdelete(dst);
    r = Frename(0, src, dst);
    if (r < 0) {
        r = copy_file(src, dst);
        if (r == 0) {
            copied = 1;
            if (Fdelete(src) < 0) copied = 2;
        }
    }
    graf_mouse(ARROW, 0L);
    if (r < 0) { err_alert("Verschieben fehlgeschlagen", nm, r); return; }

    if (copied != 2) node_remove(n);
    note_new_file(dst, size);
    if (copied == 2)
        snprintf(status_msg, sizeof status_msg, "Kopiert, Quelle nicht gel" OE "scht: %s", nm);
    else
        snprintf(status_msg, sizeof status_msg, "Verschoben: %s", nm);
    finish_op();
}

static void op_delete(Node *n)
{
    char src[PATHLEN], b[300], t[48], sz[40], nm[300];
    long r;

    safe_text(t, n->name, 36);
    fmt_size(n->size, sz);
    sprintf(b, "[2][Datei l" OE "schen?|%s|%s][L" OE "schen|Abbruch]", t, sz);
    if (FormAlert(2, b) != 1) return;

    if (!build_path(n, src, sizeof src)) { path_too_long(n); return; }
    strncpy(nm, n->name, sizeof nm - 1); nm[sizeof nm - 1] = 0;
    r = Fdelete(src);
    if (r < 0) { err_alert("L" OE "schen fehlgeschlagen", nm, r); return; }

    node_remove(n);
    snprintf(status_msg, sizeof status_msg, "Gel" OE "scht: %s", nm);
    finish_op();
}

static void file_popup(Node *n, short mx, short my)
{
    static char *items[3] = { "  Kopieren...  ", "  Verschieben...  ", "  L" OE "schen  " };
    int r;

    busy = 1;
    popup_x = mx; popup_y = my;
    select_node(n);
    r = run_popup_tree(items, 3, mx, my);
    if      (r == 0) op_copy(n);
    else if (r == 1) op_move(n);
    else if (r == 2) op_delete(n);
    busy = 0;
}

static OBJECT menutree[M_NOBJ];

static void obj(int i, int next, int head, int tail, int type, int flags,
                int state, long spec, const char *str,
                int x, int y, int w, int h)
{
    OBJECT *o = &menutree[i];
    o->ob_next = next; o->ob_head = head; o->ob_tail = tail;
    o->ob_type = type; o->ob_flags = flags; o->ob_state = state;
    if (str) o->ob_spec.free_string = (char *)str; else o->ob_spec.index = spec;
    o->ob_x = x; o->ob_y = y; o->ob_width = w; o->ob_height = h;
}

static void build_menu(int scrw)
{
    static char t0[] = " " APPNAME " ", t1[] = " Datei ", t2[] = " Ansicht ";
    static char i_info[]  = "  Info...  ^I  ";
    static char i_open[]  = "  " OEC "ffnen...         ^O  ";
    static char i_pref[]  = "  Einstellungen...  ^E  ";
    static char i_quit[]  = "  Beenden           ^Q  ";
    static char i_color[] = "  Farbmodus      ^C  ";
    static char i_upd[]   = "  Aktualisieren  ^R  ";
    static char sep1[40], sep2[40];
    int barh = ch + 3;
    int l0 = (int)strlen(t0), l1 = (int)strlen(t1), l2 = (int)strlen(t2);
    int w0 = (int)strlen(i_info);
    int w1 = (int)strlen(i_pref);
    int w2 = (int)strlen(i_upd);
    int tx0 = 0, tx1 = l0 * cw, tx2 = (l0 + l1) * cw;
    int base = cw;
    int i;

    for (i = 0; i < w1; i++) sep1[i] = sep2[i] = '-';
    sep1[w1] = sep2[w1] = 0;

    obj(0, -1, 1, 6, G_IBOX, OF_NONE, 0, 0L, NULL, 0, 0, scrw, barh + 6 * ch);
    obj(1, 6, 2, 2, G_BOX, OF_NONE, 0, 0x00001100L, NULL, 0, 0, scrw, barh);
    obj(2, 1, 3, 5, G_IBOX, OF_NONE, 0, 0L, NULL, base, 0, (l0 + l1 + l2) * cw, barh);
    obj(3, 4, -1, -1, G_TITLE, OF_NONE, 0, 0L, t0, tx0, 0, l0 * cw, barh);
    obj(4, 5, -1, -1, G_TITLE, OF_NONE, 0, 0L, t1, tx1, 0, l1 * cw, barh);
    obj(5, 2, -1, -1, G_TITLE, OF_NONE, 0, 0L, t2, tx2, 0, l2 * cw, barh);
    obj(6, 0, 7, 15, G_IBOX, OF_NONE, 0, 0L, NULL, 0, barh, scrw, 6 * ch);

    obj(7, 9, 8, 8, G_BOX, OF_NONE, 0, 0x00FF1100L, NULL, base + tx0, 0, w0 * cw, ch);
    obj(M_INFO, 7, -1, -1, G_STRING, OF_NONE, 0, 0L, i_info, 0, 0, w0 * cw, ch);

    obj(9, 15, 10, 14, G_BOX, OF_NONE, 0, 0x00FF1100L, NULL, base + tx1, 0, w1 * cw, 5 * ch);
    obj(M_OPEN, 11, -1, -1, G_STRING, OF_NONE, 0, 0L, i_open, 0, 0 * ch, w1 * cw, ch);
    obj(11, 12, -1, -1, G_STRING, OF_NONE, OS_DISABLED, 0L, sep1, 0, 1 * ch, w1 * cw, ch);
    obj(M_PREF, 13, -1, -1, G_STRING, OF_NONE, 0, 0L, i_pref, 0, 2 * ch, w1 * cw, ch);
    obj(13, 14, -1, -1, G_STRING, OF_NONE, OS_DISABLED, 0L, sep2, 0, 3 * ch, w1 * cw, ch);
    obj(M_QUIT, 9, -1, -1, G_STRING, OF_NONE, 0, 0L, i_quit, 0, 4 * ch, w1 * cw, ch);

    obj(15, 6, 16, 17, G_BOX, OF_NONE, 0, 0x00FF1100L, NULL, base + tx2, 0, w2 * cw, 2 * ch);
    obj(M_COLOR, 17, -1, -1, G_STRING, OF_NONE, 0, 0L, i_color, 0, 0, w2 * cw, ch);
    obj(M_REFRESH, 15, -1, -1, G_STRING, OF_LASTOB, 0, 0L, i_upd, 0, ch, w2 * cw, ch);

    if (!cushion_ok) menutree[M_COLOR].ob_state |= OS_DISABLED;
}

/* After a menu entry: un-hilight the title and redraw the WHOLE menu row (plus a few
 * pixels below) via the message queue. WinDom's frm_menu restores the drop-down
 * background with a 10 pixel margin that reaches into the menu row (saved while the
 * title was inverted), and MenuTnormal() only redraws the exact title rectangle, so
 * remnants could stay (3D AES draw titles slightly larger). */
static void menu_done(WINDOW *win, int ttl)
{
    short x, y, w, h;
    /* WindSetPtr(WF_MENU) works on a COPY of menutree (ObjcDup), so the state has to be
     * cleared in win->menu.root, not in menutree[] */
    if (ttl > 0 && win->menu.root) win->menu.root[ttl].ob_state &= ~OS_SELECTED;
    wind_get(win->handle, WF_WORKXYWH, &x, &y, &w, &h);     /* AES work area incl. menu */
    ApplWrite(_AESapid, WM_REDRAW, win->handle, x, y, w, menutree[1].ob_height + 4);
}

/* ---------------------------------------------------------------------------------
 * Text windows (Info, scan details): a WinDom form in its own window instead of a
 * modal alert, so under MiNT/MagiC the rest of the system keeps running. Lines are
 * G_STRINGs plus an OK button; OK or the closer close it, and the AES sends the
 * redraw for what was under it. Opening it again just tops it (FormCreate finds the
 * window by its tree) and refreshes the text.
 * ------------------------------------------------------------------------------- */
#define TW_MAXL  12
typedef struct {
    OBJECT  tree[TW_MAXL + 2];
    char    line[TW_MAXL][64];
    char    name[48];
    WINDOW *win;
} TextWin;
static TextWin tw_info, tw_det;

static void __CDECL tw_destroy(WINDOW *win, short buff[8])
{
    if (tw_info.win == win) tw_info.win = NULL;
    if (tw_det.win  == win) tw_det.win  = NULL;
}

static void __CDECL tw_ok(WINDOW *win, int index, int mode, void *data)
{
    ObjcChange(OC_FORM, win, index, OS_NORMAL, FALSE);
    ApplWrite(_AESapid, WM_DESTROY, win->handle, 0, 0, 0, 0);
}

static void text_window(TextWin *t, int n)
{
    OBJECT *o = t->tree;
    int i, maxl = 20, bh = ch + 4, W, H, ok = n + 1;
    short rx = o[0].ob_x, ry = o[0].ob_y;               /* keep the position if open */

    for (i = 0; i < n; i++) if ((int)strlen(t->line[i]) > maxl) maxl = (int)strlen(t->line[i]);
    W = (maxl + 4) * cw;
    H = (n + 3) * ch + bh;
    memset(o, 0, sizeof t->tree);
    o[0].ob_next = -1; o[0].ob_head = 1; o[0].ob_tail = ok;
    o[0].ob_type = G_BOX; o[0].ob_spec.index = 0x00001100L;
    o[0].ob_x = t->win ? rx : 0; o[0].ob_y = t->win ? ry : 0;
    o[0].ob_width = W; o[0].ob_height = H;
    for (i = 0; i < n; i++) {
        OBJECT *l = &o[i + 1];
        l->ob_next = i + 2; l->ob_head = l->ob_tail = -1;
        l->ob_type = G_STRING; l->ob_spec.free_string = t->line[i];
        l->ob_x = 2 * cw; l->ob_y = (i + 1) * ch;
        l->ob_width = (int)strlen(t->line[i]) * cw; l->ob_height = ch;
    }
    o[ok].ob_next = 0; o[ok].ob_head = o[ok].ob_tail = -1;
    o[ok].ob_type = G_BUTTON; o[ok].ob_spec.free_string = "OK";
    o[ok].ob_flags = OF_SELECTABLE | OF_EXIT | OF_DEFAULT | OF_LASTOB;
    o[ok].ob_width = 10 * cw; o[ok].ob_height = bh;
    o[ok].ob_x = (W - 10 * cw) / 2; o[ok].ob_y = (n + 2) * ch;

    if (t->win) {                                        /* already open: top + refresh */
        short x, y, w, h;
        WindSet(t->win, WF_TOP, 0, 0, 0, 0);
        wind_get(t->win->handle, WF_CURRXYWH, &x, &y, &w, &h);
        ApplWrite(_AESapid, WM_REDRAW, t->win->handle, x, y, w, h);
        return;
    }
    t->win = FormCreate(o, WAT_FORM, NULL, t->name, NULL, TRUE, FALSE);
    if (!t->win) return;
    EvntAdd(t->win, WM_DESTROY, tw_destroy, EV_TOP);
    ObjcAttachFormFunc(t->win, ok, tw_ok, NULL);
}

static void show_info(void)
{
    static const char *txt[] = {
        APPNAME " " APPVERSION " f" UE "r Atari",
        "Treemap der Plattenbelegung",
        "",
        "\275 2026 Bernd Mueller",
        "Freie Software unter der GNU GPL v3 oder",
        "sp" AE "ter, ohne jede Gew" AE "hr.",
        "",
        "Klick: w" AE "hlen     Doppelklick/Return: Zoom",
        "Backspace: heraus   Rechtsklick: Datei-Men" UE,
        "+ / -: Mindestgr" OE "\236e   T: Messung   D: Details",
        "Esc: Scan abbrechen   Strg+Q: Beenden"
    };
    int i, n = (int)(sizeof txt / sizeof txt[0]);
    for (i = 0; i < n; i++) { strncpy(tw_info.line[i], txt[i], 63); tw_info.line[i][63] = 0; }
    strcpy(tw_info.name, " " APPNAME ": Info ");
    text_window(&tw_info, n);
}

static void show_scan_details(void)
{
    char a1[12], a2[12], a3[12], a4[12], a5[12];
    const char *mode = !use_mint ? "Fsfirst" : use_dx ? "Dxreaddir" : "Dreaddir+Fxattr";
    clock_t rest = scan_ticks > t_step ? scan_ticks - t_step : 0;
    char (*L)[64] = tw_det.line;

    fmt_ticks(scan_ticks, a1);
    fmt_ticks(rest, a5);
    sprintf(L[0], "Scan gesamt     %7s s", a1);
    sprintf(L[1], "Lesemethode     %s", mode);
    if (probed) {
        fmt_ticks(t_step > t_node ? t_step - t_node : 0, a2);
        fmt_ticks(t_node - t_hue, a3);
        fmt_ticks(t_hue, a4);
        sprintf(L[2], "OS lesen        %7s s", a2);
        sprintf(L[3], "Knoten/malloc   %7s s", a3);
        sprintf(L[4], "Endung/Farbe    %7s s", a4);
    } else {
        fmt_ticks(t_step, a2);
        sprintf(L[2], "Ordner lesen    %7s s", a2);
        sprintf(L[3], "Knoten/malloc         -");
        sprintf(L[4], "Endung/Farbe          -");
    }
    sprintf(L[5], "GUI/Rest        %7s s", a5);
    sprintf(L[6], "Dateien/Ordner  %lu / %lu", n_files, n_dirs);
    sprintf(L[7], "%s", probed ? "(gemessen mit T; die Messung kostet selbst Zeit)"
                               : "Genauer: T vor dem Scan einschalten.");
    strcpy(tw_det.name, " " APPNAME ": Scan-Details ");
    text_window(&tw_det, 8);
}

/* WinDom's frm_menu draws the drop-down boxes straight onto the screen and puts the
 * saved background back with vro_cpyfm afterwards, without any WM_REDRAW. Under XaAES
 * (ARAnyM) that left a stripe of old pixels under the menu, even when a menu was only
 * opened and closed. So: the hilight hook notes that a menu was opened, and once
 * frm_menu has returned the main loop sends a WM_REDRAW for everything a drop-down
 * (plus WinDom's 10 pixel margin) can have covered. */
static int menu_used;

static void __CDECL menu_hilight(APPvar *ap, WINDOW *win, int title, int item)
{
    menu_used = 1;
}

static void redraw_menu_area(WINDOW *win)
{
    static const int boxes[] = { 7, 9, 15 };
    short x, y, w, h;
    int i, hmax = 0;
    for (i = 0; i < 3; i++)
        if (menutree[boxes[i]].ob_height > hmax) hmax = menutree[boxes[i]].ob_height;
    wind_get(win->handle, WF_WORKXYWH, &x, &y, &w, &h);       /* AES work area incl. menu */
    h = menutree[1].ob_height + 1 + hmax + 10 + 4;            /* bar, line, box, margin, frame */
    ApplWrite(_AESapid, WM_REDRAW, win->handle, x, y, w, h);
}

static void __CDECL menu_unhilight(WINDOW *win, short msg[])
{
    menu_done(win, msg[3]);
}

static void __CDECL mi_info(WINDOW *win, int index, int ttl, void *data)
{
    menu_done(win, ttl);
    show_info();
}

static void __CDECL mi_open(WINDOW *win, int index, int ttl, void *data)
{
    menu_done(win, ttl);
    if (!scanning) choose_folder();
}

static void __CDECL mi_pref(WINDOW *win, int index, int ttl, void *data)
{
    menu_done(win, ttl);
    settings();
}

static void __CDECL mi_quit(WINDOW *win, int index, int ttl, void *data)
{
    ApplWrite(_AESapid, WM_DESTROY, win->handle, 0, 0, 0, 0);
}

static void __CDECL mi_color(WINDOW *win, int index, int ttl, void *data)
{
    menu_done(win, ttl);
    toggle_colors();
}

static void __CDECL mi_refresh(WINDOW *win, int index, int ttl, void *data)
{
    menu_done(win, ttl);
    refresh();
}

static void __CDECL on_geom(WINDOW *win, short buff[8])
{
    GRECT w;
    long i;
    WindGet(win, WF_WORKXYWH, &w.g_x, &w.g_y, &w.g_w, &w.g_h);
    if (w.g_w != work.g_w || w.g_h != work.g_h) {
        if (buff[0] == WM_SIZED) {          /* clear now, layout when the user stops */
            work = w;
            ncells = 0;
            layout_pending = 1;
            resize_t = clock();
            mouse_busy(1);                  /* until the cushions are back */
            redraw_all();
        } else
            relayout_redraw();              /* fuller, iconify etc.: one step, do it now */
    } else if (w.g_x != work.g_x || w.g_y != work.g_y) {
        short dx = w.g_x - work.g_x;
        short dy = w.g_y - work.g_y;
        for (i = 0; i < ncells; i++) {
            cells[i].x += dx;
            cells[i].y += dy;
        }
        work = w;
    }
}

static void __CDECL on_destroy(WINDOW *win, short buff[8])
{
    endprog = 1;
}

static void __CDECL on_apterm(WINDOW *win, short buff[8])
{
    if (mainwin) ApplWrite(_AESapid, WM_DESTROY, mainwin->handle, 0, 0, 0, 0);
    else endprog = 1;
}

/* Scan in time slices: read folders for up to SCAN_SLICE ticks (200 Hz) per event round
 * instead of one folder per round, so the AES round trip no longer dominates. Between
 * slices EvntWindom() runs, so Esc, menus and window moves stay responsive. The status
 * line is refreshed at most every INFO_EVERY ticks, since each update redraws the bar. */
#define SCAN_SLICE  10      /* 50 ms */
#define INFO_EVERY  50      /* 250 ms */

static void __CDECL on_timer(WINDOW *win, short buff[8])
{
    static clock_t last_info;
    clock_t t0, last, now;

    if (busy || !scanning) return;
    t0 = last = clock();
    do {
        scan_step();
        scan_steps++;
        now = clock();
        t_step += now - last;
        last = now;
    } while (scanning && (clock_t)(now - t0) < SCAN_SLICE);

    if (!scanning) { finish_scan(); return; }
    if ((clock_t)(t0 - last_info) >= INFO_EVERY) { last_info = t0; update_info(); }
}

static void __CDECL on_key(WINDOW *win, short buff[8])
{
    int k = evnt.keybd & 0xFF;
    if (evnt.mkstate & K_CTRL) {                 /* menu shortcuts, by scan code */
        switch ((evnt.keybd >> 8) & 0xFF) {
        case 0x17: show_info(); return;                                   /* ^I */
        case 0x18: if (!scanning) choose_folder(); return;                /* ^O */
        case 0x12: settings(); return;                                    /* ^E */
        case 0x10: ApplWrite(_AESapid, WM_DESTROY, win->handle, 0, 0, 0, 0); return;  /* ^Q */
        case 0x2E: toggle_colors(); return;                               /* ^C */
        case 0x13: refresh(); return;                                     /* ^R */
        }
    }
    if (k >= 'a' && k <= 'z') k -= 32;
    switch (k) {
    case 'Q': case 0x11: ApplWrite(_AESapid, WM_DESTROY, win->handle, 0, 0, 0, 0); break;
    case 'O': if (!scanning) choose_folder(); break;
    case 'R': refresh(); break;
    case 'C': toggle_colors(); break;
    case 'D': if (!scanning && root) show_scan_details(); break;
    case 'T': show_timing = !show_timing; t_draw_shown = t_draw; update_info(); break;
    case '+': set_min_px(+1); break;
    case '-': set_min_px(-1); break;
    case 'G': show_dirs = !show_dirs; relayout_redraw(); break;
    case '\r': case '\n': zoom_in(); break;
    case 0x08: zoom_out(); break;
    case 0x1B: if (scanning) { scan_aborted = 1; finish_scan(); } break;
    case 'H': case '?': show_info(); break;
    default: break;
    }
}

static void __CDECL on_click(WINDOW *win, short buff[8])
{
    short mx = buff[3] ? buff[3] : evnt.mx;
    short my = buff[4] ? buff[4] : evnt.my;
    short mb = buff[5] ? buff[5] : evnt.mbut;
    long i;
    if (scanning) return;
    for (i = 0; i < ncells; i++) {
        const Cell *c = &cells[i];
        if (mx >= c->x && mx < c->x + c->w && my >= c->y && my < c->y + c->h) {
            if (mb & 2) {
                file_popup(c->n, mx, my);
            } else {
                select_node(c->n);
                if (evnt.nb_click >= 2) zoom_in();
            }
            return;
        }
    }
}

int main(int argc, char **argv)
{
    ApplInit();
    ApplSet(APS_FLAG, FLG_NOPAL, 1, 0, 0);
    graf_handle(&cw, &ch, &bw, &bh);

    if (Pgetpid() >= 0) {
        use_mint = 1;
        Pdomain(1);
    }

    mainwin = WindCreate(NAME | CLOSER | FULLER | MOVER | SIZER,
                         app.x, app.y, app.w, app.h);
    if (!mainwin) {
        FormAlert(1, "[3][Kein Fenster frei!][ OK ]");
        ApplExit();
        return 1;
    }
    mainwin->w_min = 200;
    mainwin->h_min = 120;
    vh = mainwin->graf->handle;
    setup_colors();
    {                                               /* DXMAP.INF in the start directory */
        char d[PATHLEN];
        d[0] = 0;
        Dgetpath(d, 0);
        snprintf(inf_path, sizeof inf_path, "%c:%s\\DXMAP.INF", 'A' + (int)Dgetdrv(), d);
        load_settings();
    }
    build_menu(app.w);

    EvntAdd(mainwin, WM_REDRAW,    evnt_draw,  EV_BOT);
    EvntAdd(mainwin, WM_MOVED,     on_geom,    EV_BOT);
    EvntAdd(mainwin, WM_SIZED,     on_geom,    EV_BOT);
    EvntAdd(mainwin, WM_REPOSED,   on_geom,    EV_BOT);
    EvntAdd(mainwin, WM_FULLED,    on_geom,    EV_BOT);
    EvntAdd(mainwin, WM_UNICONIFY, on_geom,    EV_BOT);
    EvntAdd(mainwin, WM_DESTROY,   on_destroy, EV_TOP);
    EvntAttach(mainwin, WM_XKEYBD,  on_key);
    EvntAttach(mainwin, WM_XBUTTON, on_click);
    EvntAttach(NULL, WM_XTIMER, on_timer);
    EvntAttach(NULL, AP_TERM,   on_apterm);

    WindSetPtr(mainwin, WF_MENU, menutree, menu_unhilight);
    WindSetPtr(mainwin, WF_HILIGHT, (void *)menu_hilight, NULL);
    ObjcAttachMenuFunc(mainwin, M_INFO,    mi_info,    NULL);
    ObjcAttachMenuFunc(mainwin, M_OPEN,    mi_open,    NULL);
    ObjcAttachMenuFunc(mainwin, M_PREF,    mi_pref,    NULL);
    ObjcAttachMenuFunc(mainwin, M_QUIT,    mi_quit,    NULL);
    ObjcAttachMenuFunc(mainwin, M_COLOR,   mi_color,   NULL);
    ObjcAttachMenuFunc(mainwin, M_REFRESH, mi_refresh, NULL);
    MenuIcheck(mainwin, M_COLOR, cushions_on);

    update_title();
    update_info();
    {
        short ww = (short)(app.w * 2 / 3), wh = (short)(app.h * 2 / 3);
        WindOpen(mainwin, app.x + (app.w - ww) / 2, app.y + (app.h - wh) / 2, ww, wh);
    }
    relayout();

    if (argc > 1 && argv[1][0]) start_scan(argv[1]);
    else                        want_open = 1;

    while (!endprog) {
        int pending = show_timing && !scanning && t_draw != t_draw_shown;
        int flags = MU_MESAG | MU_KEYBD | MU_BUTTON |
                    ((scanning || pending || layout_pending) ? MU_TIMER : 0);
        evnt.bclick = 258; evnt.bmask = 3; evnt.bstate = 0;
        evnt.timer = scanning ? 0 : 100;          /* pending display/layout: wake up in 100 ms */
        EvntWindom(flags);
        if (menu_used && !endprog) {             /* a drop-down was open: repair under it */
            menu_used = 0;
            redraw_menu_area(mainwin);
        }
        if (layout_pending && (clock_t)(clock() - resize_t) >= RESIZE_DELAY) {
            relayout_redraw();
            update_info();
        }
        if (show_timing && !scanning && t_draw != t_draw_shown) {   /* redraw done: show its time */
            static clock_t last;                  /* at most every 250 ms */
            clock_t now = clock();
            if ((clock_t)(now - last) >= 50) {
                last = now;
                t_draw_shown = t_draw;
                update_info();
            }
        }
        if (want_open && !endprog) { want_open = 0; choose_folder(); }
    }

    pool_free_all();
    free(cells);
    ApplExit();
    return 0;
}