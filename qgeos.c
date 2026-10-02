/* qgeos — GEOS geometry functions for q (peachq and kdb+), via the standard k.h C API.
 *
 * Geometries are WKB byte vectors (q type 4h). A column of geometries is a general
 * list (0h) of byte vectors. Every function accepts an atom (one geometry) or a list,
 * and binary functions broadcast an atom against a list, so whole columns go to C in
 * one call. An empty byte vector is a null geometry; results for it are null
 * (0n for floats, 0b for booleans, empty bytes for geometries).
 *
 * MIT licensed. Links GEOS (LGPL) dynamically through its C API.
 */
/* geos_c.h first: k.h defines short macros (nf, U, ...) that collide with its parameter names */
#include <geos_c.h>
#define KXVER 3
#include "k.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- per-thread GEOS state ---------- */

static _Thread_local GEOSContextHandle_t ctx;
static _Thread_local GEOSWKBReader *wkbr;
static _Thread_local GEOSWKBWriter *wkbw;
static _Thread_local GEOSWKTReader *wktr;
static _Thread_local GEOSWKTWriter *wktw;
static _Thread_local char errbuf[256];

static void on_error(const char *msg, void *u) {
    (void)u;
    snprintf(errbuf, sizeof errbuf, "geos: %s", msg);
}

static void ensure_ctx(void) {
    if (ctx) return;
    ctx = GEOS_init_r();
    GEOSContext_setErrorMessageHandler_r(ctx, on_error, NULL);
    wkbr = GEOSWKBReader_create_r(ctx);
    wkbw = GEOSWKBWriter_create_r(ctx);
    wktr = GEOSWKTReader_create_r(ctx);
    wktw = GEOSWKTWriter_create_r(ctx);
    GEOSWKTWriter_setTrim_r(ctx, wktw, 1);
}

/* q error from the last GEOS message (or a fallback). ss() interns, so the string outlives us. */
static S emsg(const char *fallback) {
    S s = ss(errbuf[0] ? errbuf : (S)fallback);
    errbuf[0] = 0;
    return s;
}
/* NB: krr() returns NULL (as in kdb+), so a pending error is never a value to test. */
static K gerr(const char *fallback) { return krr(emsg(fallback)); }

/* ---------- argument shapes ---------- */

typedef struct { K x; J n; int atom; } garg;

/* A geometry argument: one byte vector (atom) or a general list of byte vectors. */
static int geom_arg(K x, garg *a) {
    a->x = x;
    if (x->t == KG) { a->atom = 1; a->n = 1; return 1; }
    if (x->t == 0) {
        for (J i = 0; i < x->n; i++) if (kK(x)[i]->t != KG) return 0;
        a->atom = 0; a->n = x->n; return 1;
    }
    return 0;
}

static K geom_at(garg *a, J i) { return a->atom ? a->x : kK(a->x)[a->atom ? 0 : i]; }

/* Parse WKB. Returns 1 ok (with *g NULL for a null geometry), 0 on a malformed blob. */
static int read_geom(K b, GEOSGeometry **g) {
    *g = NULL;
    if (b->n == 0) return 1;
    *g = GEOSWKBReader_read_r(ctx, wkbr, kG(b), (size_t)b->n);
    return *g != NULL;
}

static K write_geom(const GEOSGeometry *g) {
    if (!g) return ktn(KG, 0);
    size_t n = 0;
    unsigned char *buf = GEOSWKBWriter_write_r(ctx, wkbw, g, &n);
    if (!buf) return NULL;
    K r = ktn(KG, (J)n);
    memcpy(kG(r), buf, n);
    GEOSFree_r(ctx, buf);
    return r;
}

/* Combined length of two broadcast arguments, or -1 on mismatch. */
static J bcast_len(garg *a, garg *b) {
    if (a->atom && b->atom) return 1;
    if (a->atom) return b->n;
    if (b->atom) return a->n;
    return a->n == b->n ? a->n : -1;
}

/* Result containers: atom when every input was an atom, else a vector/list. */
static K out_vec(int atom, I t, J n) { return atom ? NULL : ktn(t, n); }

/* ---------- WKT <-> WKB ---------- */

static K wkt_one(K s) {
    if (s->t == -KC) { char c[2] = { (char)s->g, 0 }; GEOSGeometry *g = GEOSWKTReader_read_r(ctx, wktr, c); if (!g) return NULL; K r = write_geom(g); GEOSGeom_destroy_r(ctx, g); return r; }
    if (s->t != KC) return NULL;
    if (s->n == 0) return ktn(KG, 0);
    char *buf = malloc((size_t)s->n + 1);
    memcpy(buf, kC(s), (size_t)s->n); buf[s->n] = 0;
    GEOSGeometry *g = GEOSWKTReader_read_r(ctx, wktr, buf);
    free(buf);
    if (!g) return NULL;
    K r = write_geom(g);
    GEOSGeom_destroy_r(ctx, g);
    return r;
}

/* fromwkt "POINT(1 2)"  or  fromwkt ("POINT(1 2)";"POINT(3 4)") */
K qgeos_fromwkt(K x) {
    ensure_ctx();
    if (x->t == KC || x->t == -KC) { K r = wkt_one(x); return r ? r : gerr("fromwkt: bad wkt"); }
    if (x->t != 0) return krr("type");
    K r = ktn(0, x->n);
    for (J i = 0; i < x->n; i++) {
        K e = wkt_one(kK(x)[i]);
        if (!e) { r->n = i; r0(r); return gerr("fromwkt: bad wkt"); }
        kK(r)[i] = e;
    }
    return r;
}

static K wkt_str(const GEOSGeometry *g) {
    if (!g) return ktn(KC, 0);
    char *s = GEOSWKTWriter_write_r(ctx, wktw, g);
    if (!s) return NULL;
    K r = kpn(s, (J)strlen(s));
    GEOSFree_r(ctx, s);
    return r;
}

/* towkt g  ->  string, or list of strings */
K qgeos_towkt(K x) {
    ensure_ctx();
    garg a; if (!geom_arg(x, &a)) return krr("type");
    K r = out_vec(a.atom, 0, a.n);
    for (J i = 0; i < a.n; i++) {
        GEOSGeometry *g;
        if (!read_geom(geom_at(&a, i), &g)) { if (r) { r->n = i; r0(r); } return gerr("towkt: bad wkb"); }
        K s = wkt_str(g);
        if (g) GEOSGeom_destroy_r(ctx, g);
        if (!s) { if (r) { r->n = i; r0(r); } return gerr("towkt"); }
        if (a.atom) return s;
        kK(r)[i] = s;
    }
    return r;
}

/* point[x;y] — floats or longs, atoms or equal-length vectors (atoms broadcast) */
static int num_at(K v, J i, double *d) {
    switch (v->t) {
    case -KF: *d = v->f; return 1;
    case -KJ: *d = (double)v->j; return 1;
    case -KI: *d = (double)v->i; return 1;
    case KF:  *d = kF(v)[i]; return 1;
    case KJ:  *d = (double)kJ(v)[i]; return 1;
    case KI:  *d = (double)kI(v)[i]; return 1;
    default:  return 0;
    }
}

K qgeos_point(K x, K y) {
    ensure_ctx();
    int xa = x->t < 0, ya = y->t < 0;
    J nx = xa ? 1 : x->n, ny = ya ? 1 : y->n;
    J n = xa ? ny : ya ? nx : (nx == ny ? nx : -1);
    if (n < 0) return krr("length");
    double dx, dy;
    if (!num_at(x, 0, &dx) && n) return krr("type");
    if (!num_at(y, 0, &dy) && n) return krr("type");
    K r = out_vec(xa && ya, 0, n);
    for (J i = 0; i < n; i++) {
        num_at(x, xa ? 0 : i, &dx);
        num_at(y, ya ? 0 : i, &dy);
        GEOSGeometry *g = GEOSGeom_createPointFromXY_r(ctx, dx, dy);
        K e = g ? write_geom(g) : NULL;
        if (g) GEOSGeom_destroy_r(ctx, g);
        if (!e) { if (r) { r->n = i; r0(r); } return gerr("point"); }
        if (!r) return e;
        kK(r)[i] = e;
    }
    return r;
}

/* ---------- unary: geometry -> float ---------- */

typedef int (*float_fn)(GEOSContextHandle_t, const GEOSGeometry *, double *);

static K unary_float(K x, float_fn f, const char *name) {
    ensure_ctx();
    garg a; if (!geom_arg(x, &a)) return krr("type");
    K r = out_vec(a.atom, KF, a.n);
    for (J i = 0; i < a.n; i++) {
        GEOSGeometry *g; double d = nf;
        if (!read_geom(geom_at(&a, i), &g)) { if (r) r0(r); return gerr(name); }
        if (g) { int ok = f(ctx, g, &d); GEOSGeom_destroy_r(ctx, g); if (!ok) { if (r) r0(r); return gerr(name); } }
        if (!r) return kf(d);
        kF(r)[i] = d;
    }
    return r;
}

K qgeos_area(K x)   { return unary_float(x, GEOSArea_r, "area"); }
K qgeos_length(K x) { return unary_float(x, GEOSLength_r, "length"); }

/* ---------- unary: geometry -> geometry ---------- */

typedef GEOSGeometry *(*geom_fn)(GEOSContextHandle_t, const GEOSGeometry *);

static K unary_geom(K x, geom_fn f, const char *name) {
    ensure_ctx();
    garg a; if (!geom_arg(x, &a)) return krr("type");
    K r = out_vec(a.atom, 0, a.n);
    for (J i = 0; i < a.n; i++) {
        GEOSGeometry *g, *o = NULL;
        if (!read_geom(geom_at(&a, i), &g)) goto fail;
        if (g) { o = f(ctx, g); GEOSGeom_destroy_r(ctx, g); if (!o) goto fail; }
        K e = write_geom(o);
        if (o) GEOSGeom_destroy_r(ctx, o);
        if (!e) goto fail;
        if (!r) return e;
        kK(r)[i] = e;
        continue;
    fail:
        if (r) { r->n = i; r0(r); }
        return gerr(name);
    }
    return r;
}

K qgeos_centroid(K x) { return unary_geom(x, GEOSGetCentroid_r, "centroid"); }
K qgeos_envelope(K x) { return unary_geom(x, GEOSEnvelope_r, "envelope"); }
K qgeos_hull(K x)     { return unary_geom(x, GEOSConvexHull_r, "hull"); }

/* buffer[g; radius] — radius is a float or long atom; 8 segments per quarter circle */
K qgeos_buffer(K x, K rad) {
    ensure_ctx();
    double d;
    if (!num_at(rad, 0, &d) || rad->t >= 0) return krr("type");
    garg a; if (!geom_arg(x, &a)) return krr("type");
    K r = out_vec(a.atom, 0, a.n);
    for (J i = 0; i < a.n; i++) {
        GEOSGeometry *g, *o = NULL;
        if (!read_geom(geom_at(&a, i), &g)) goto fail;
        if (g) { o = GEOSBuffer_r(ctx, g, d, 8); GEOSGeom_destroy_r(ctx, g); if (!o) goto fail; }
        K e = write_geom(o);
        if (o) GEOSGeom_destroy_r(ctx, o);
        if (!e) goto fail;
        if (!r) return e;
        kK(r)[i] = e;
        continue;
    fail:
        if (r) { r->n = i; r0(r); }
        return gerr("buffer");
    }
    return r;
}

/* ---------- binary: distance ---------- */

K qgeos_distance(K x, K y) {
    ensure_ctx();
    garg a, b;
    if (!geom_arg(x, &a) || !geom_arg(y, &b)) return krr("type");
    J n = bcast_len(&a, &b);
    if (n < 0) return krr("length");
    int atom = a.atom && b.atom;
    /* parse an atom side once, not n times */
    GEOSGeometry *ga = NULL, *gb = NULL;
    if (a.atom && !read_geom(a.x, &ga)) return gerr("distance: bad wkb");
    if (b.atom && !read_geom(b.x, &gb)) { if (ga) GEOSGeom_destroy_r(ctx, ga); return gerr("distance: bad wkb"); }
    K r = out_vec(atom, KF, n);
    S err = NULL;   /* message, raised once at the end: krr() itself returns NULL */
    for (J i = 0; i < n; i++) {
        GEOSGeometry *gi = ga, *gj = gb;
        if (!a.atom && !read_geom(kK(a.x)[i], &gi)) { err = emsg("distance: bad wkb"); break; }
        if (!b.atom && !read_geom(kK(b.x)[i], &gj)) { if (!a.atom && gi) GEOSGeom_destroy_r(ctx, gi); err = emsg("distance: bad wkb"); break; }
        double d = nf; int ok = 1;
        if (gi && gj) ok = GEOSDistance_r(ctx, gi, gj, &d);
        if (!a.atom && gi) GEOSGeom_destroy_r(ctx, gi);
        if (!b.atom && gj) GEOSGeom_destroy_r(ctx, gj);
        if (!ok) { err = emsg("distance"); break; }
        if (atom) { r = kf(d); break; }
        kF(r)[i] = d;
    }
    if (ga) GEOSGeom_destroy_r(ctx, ga);
    if (gb) GEOSGeom_destroy_r(ctx, gb);
    if (err) { if (r) r0(r); return krr(err); }
    return r;
}

/* ---------- binary predicates ---------- */

typedef char (*pred_fn)(GEOSContextHandle_t, const GEOSGeometry *, const GEOSGeometry *);
typedef char (*prep_fn)(GEOSContextHandle_t, const GEOSPreparedGeometry *, const GEOSGeometry *);

/* plain(a,b); prep(P(a), b) for an atom on the left; flip(P(b), a) for an atom on the right */
typedef struct { const char *name; pred_fn plain; prep_fn prep; prep_fn flip; } pred_def;

static const pred_def P_INTERSECTS = { "intersects", GEOSIntersects_r, GEOSPreparedIntersects_r, GEOSPreparedIntersects_r };
static const pred_def P_CONTAINS   = { "contains",   GEOSContains_r,   GEOSPreparedContains_r,   GEOSPreparedWithin_r };
static const pred_def P_WITHIN     = { "within",     GEOSWithin_r,     GEOSPreparedWithin_r,     GEOSPreparedContains_r };

static K binary_pred(K x, K y, const pred_def *p) {
    ensure_ctx();
    garg a, b;
    if (!geom_arg(x, &a) || !geom_arg(y, &b)) return krr("type");
    J n = bcast_len(&a, &b);
    if (n < 0) return krr("length");
    int atom = a.atom && b.atom;

    /* atom vs list: prepare the atom side once (spatial index on its edges) */
    if (!atom && (a.atom || b.atom)) {
        garg *fixed = a.atom ? &a : &b, *vary = a.atom ? &b : &a;
        prep_fn f = a.atom ? p->prep : p->flip;
        GEOSGeometry *gf;
        if (!read_geom(fixed->x, &gf)) return gerr(p->name);
        K r = ktn(KB, n);
        if (!gf) { memset(kG(r), 0, (size_t)n); return r; }
        const GEOSPreparedGeometry *pg = GEOSPrepare_r(ctx, gf);
        S err = NULL;   /* message, raised once at the end: krr() itself returns NULL */
        for (J i = 0; i < n; i++) {
            GEOSGeometry *g;
            if (!read_geom(kK(vary->x)[i], &g)) { err = emsg(p->name); break; }
            char c = 0;
            if (g) { c = f(ctx, pg, g); GEOSGeom_destroy_r(ctx, g); }
            if (c == 2) { err = emsg(p->name); break; }
            kG(r)[i] = (G)c;
        }
        GEOSPreparedGeom_destroy_r(ctx, pg);
        GEOSGeom_destroy_r(ctx, gf);
        if (err) { r0(r); return krr(err); }
        return r;
    }

    /* atom vs atom, or list vs list */
    K r = atom ? NULL : ktn(KB, n);
    for (J i = 0; i < n; i++) {
        GEOSGeometry *gi, *gj;
        if (!read_geom(geom_at(&a, i), &gi)) { if (r) r0(r); return gerr(p->name); }
        if (!read_geom(geom_at(&b, i), &gj)) { if (gi) GEOSGeom_destroy_r(ctx, gi); if (r) r0(r); return gerr(p->name); }
        char c = 0;
        if (gi && gj) c = p->plain(ctx, gi, gj);
        if (gi) GEOSGeom_destroy_r(ctx, gi);
        if (gj) GEOSGeom_destroy_r(ctx, gj);
        if (c == 2) { if (r) r0(r); return gerr(p->name); }
        if (atom) return kb(c);
        kG(r)[i] = (G)c;
    }
    return r;
}

K qgeos_intersects(K x, K y) { return binary_pred(x, y, &P_INTERSECTS); }
K qgeos_contains(K x, K y)   { return binary_pred(x, y, &P_CONTAINS); }
K qgeos_within(K x, K y)     { return binary_pred(x, y, &P_WITHIN); }

/* ---------- spatial index: sjoin and nearest ---------- */

/* One indexed geometry. Tree items point at these, so a hit maps straight back to its row. */
typedef struct { GEOSGeometry *g; const GEOSPreparedGeometry *pg; J idx; } item;

typedef struct { item *v; J n; } items;

static void items_free(items *s) {
    for (J i = 0; i < s->n; i++) {
        if (s->v[i].pg) GEOSPreparedGeom_destroy_r(ctx, s->v[i].pg);
        if (s->v[i].g) GEOSGeom_destroy_r(ctx, s->v[i].g);
    }
    free(s->v);
}

/* Parse every geometry of an argument. Null and empty geometries get g = NULL. */
static int items_read(garg *a, items *s) {
    s->n = a->n;
    s->v = calloc((size_t)(a->n ? a->n : 1), sizeof(item));
    for (J i = 0; i < a->n; i++) {
        s->v[i].idx = i;
        if (!read_geom(geom_at(a, i), &s->v[i].g)) { s->n = i; items_free(s); return 0; }
        if (s->v[i].g && GEOSisEmpty_r(ctx, s->v[i].g)) { GEOSGeom_destroy_r(ctx, s->v[i].g); s->v[i].g = NULL; }
    }
    return 1;
}

/* Tree over the non-null items; the caller destroys it before freeing the items. */
static GEOSSTRtree *tree_build(items *s) {
    GEOSSTRtree *t = GEOSSTRtree_create_r(ctx, 10);
    for (J i = 0; i < s->n; i++)
        if (s->v[i].g) GEOSSTRtree_insert_r(ctx, t, s->v[i].g, &s->v[i]);
    return t;
}

/* growable long vector for candidate and result indices */
typedef struct { J *v; J n, cap; } jbuf;
static void jpush(jbuf *b, J x) {
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 64; b->v = realloc(b->v, (size_t)b->cap * sizeof(J)); }
    b->v[b->n++] = x;
}
static void collect(void *it, void *ud) { jpush((jbuf *)ud, ((item *)it)->idx); }
static int cmpj(const void *a, const void *b) { J x = *(const J *)a, y = *(const J *)b; return (x > y) - (x < y); }

static K jbuf_k(jbuf *b) { K r = ktn(KJ, b->n); if (b->n) memcpy(kJ(r), b->v, (size_t)b->n * sizeof(J)); return r; }

/* sjoin[left;right;`pred] -> (li;ri): every pair where pred[left li; right ri] is true.
 * pred is `intersects, `contains or `within. The tree and the prepared geometries are built
 * on the right side, so put the side with fewer, larger shapes (e.g. polygons) on the right.
 * Pairs come out ordered by li, then ri. */
K qgeos_sjoin(K x, K y, K pred) {
    ensure_ctx();
    if (pred->t != -KS) return krr("type");
    const pred_def *p =
        !strcmp(pred->s, "intersects") ? &P_INTERSECTS :
        !strcmp(pred->s, "contains")   ? &P_CONTAINS :
        !strcmp(pred->s, "within")     ? &P_WITHIN : NULL;
    if (!p) return krr("domain");
    garg a, b;
    if (!geom_arg(x, &a) || !geom_arg(y, &b)) return krr("type");

    items lft, rgt;
    if (!items_read(&b, &rgt)) return gerr("sjoin: bad wkb");
    if (!items_read(&a, &lft)) { items_free(&rgt); return gerr("sjoin: bad wkb"); }
    GEOSSTRtree *t = tree_build(&rgt);

    jbuf cand = {0}, li = {0}, ri = {0};
    S err = NULL;   /* message, raised once at the end: krr() itself returns NULL */
    for (J i = 0; i < lft.n && !err; i++) {
        GEOSGeometry *g = lft.v[i].g;
        if (!g) continue;
        cand.n = 0;
        GEOSSTRtree_query_r(ctx, t, g, collect, &cand);
        if (cand.n > 1) qsort(cand.v, (size_t)cand.n, sizeof(J), cmpj);
        for (J k = 0; k < cand.n; k++) {
            item *r = &rgt.v[cand.v[k]];
            if (!r->pg) r->pg = GEOSPrepare_r(ctx, r->g);   /* prepared on first use, then reused */
            char c = p->flip(ctx, r->pg, g);               /* pred(left, right) == flip(P(right), left) */
            if (c == 2) { err = emsg(p->name); break; }
            if (c) { jpush(&li, i); jpush(&ri, r->idx); }
        }
    }
    GEOSSTRtree_destroy_r(ctx, t);
    items_free(&lft); items_free(&rgt);
    free(cand.v);
    K res = err ? NULL : knk(2, jbuf_k(&li), jbuf_k(&ri));
    free(li.v); free(ri.v);
    return err ? krr(err) : res;
}

/* distance callback for the tree's nearest-neighbour search. One argument is the query (no
 * prepared form), the other a tree item, which is prepared on first use so repeat visits
 * use its edge index instead of a full scan. */
static int item_dist(const void *a, const void *b, double *d, void *ud) {
    const item *q = ud;
    item *t = (item *)(a == q ? b : a);
    const item *o = a == q ? a : b;
    if (!t->pg) t->pg = GEOSPrepare_r(ctx, t->g);
    return GEOSPreparedDistance_r(ctx, t->pg, o->g, d);
}

/* nearest[left;right] -> for each left geometry, the index of the closest right geometry
 * (0N when the left one is null or right has no geometries). Ties go to whichever the tree finds first. */
K qgeos_nearest(K x, K y) {
    ensure_ctx();
    garg a, b;
    if (!geom_arg(x, &a) || !geom_arg(y, &b)) return krr("type");
    items lft, rgt;
    if (!items_read(&b, &rgt)) return gerr("nearest: bad wkb");
    if (!items_read(&a, &lft)) { items_free(&rgt); return gerr("nearest: bad wkb"); }
    GEOSSTRtree *t = tree_build(&rgt);
    int any = 0;
    for (J j = 0; j < rgt.n; j++) if (rgt.v[j].g) { any = 1; break; }

    K r = ktn(KJ, lft.n);
    jbuf cand = {0};
    for (J i = 0; i < lft.n; i++) {
        J hit = nj;
        GEOSGeometry *g = lft.v[i].g;
        if (any && g) {
            /* fast path: anything touching g is at distance 0, so the lowest such index wins */
            cand.n = 0;
            GEOSSTRtree_query_r(ctx, t, g, collect, &cand);
            if (cand.n > 1) qsort(cand.v, (size_t)cand.n, sizeof(J), cmpj);
            for (J k = 0; k < cand.n && hit == nj; k++) {
                item *c = &rgt.v[cand.v[k]];
                if (!c->pg) c->pg = GEOSPrepare_r(ctx, c->g);
                if (GEOSPreparedIntersects_r(ctx, c->pg, g) == 1) hit = c->idx;
            }
            if (hit == nj) {
                const item *n = GEOSSTRtree_nearest_generic_r(ctx, t, &lft.v[i], g, item_dist, &lft.v[i]);
                if (n) hit = n->idx;
            }
        }
        kJ(r)[i] = hit;
    }
    free(cand.v);
    GEOSSTRtree_destroy_r(ctx, t);
    items_free(&lft); items_free(&rgt);
    if (a.atom) { J v = kJ(r)[0]; r0(r); return kj(v); }
    return r;
}

/* version[] -> GEOS version string */
K qgeos_version(K x) { (void)x; return kp((S)GEOSversion()); }
