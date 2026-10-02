/* qgdal — GDAL/OGR for q (peachq and kdb+), via the standard k.h C API.
 *
 * Vector files load as q tables: one column per attribute plus a `geom column of WKB
 * byte vectors, the same representation qgeos uses, so the two libraries compose.
 * Rasters load as float vectors. CRS reprojection works on WKB columns.
 *
 * Built as its own shared library so GEOS-only users never need GDAL installed.
 * MIT licensed. Links GDAL (MIT/X) dynamically.
 */
/* GDAL headers first: k.h defines short macros (R, U, nf, ...) that collide with them */
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <cpl_conv.h>
#include <cpl_error.h>
#include <cpl_string.h>
#define KXVER 3
#include "k.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ---------- setup and errors ---------- */

static int ready;
static void ensure_init(void) {
    if (ready) return;
    GDALAllRegister();
    CPLSetErrorHandler(CPLQuietErrorHandler);   /* errors come back as q errors, not stderr noise */
    ready = 1;
}

static _Thread_local char errbuf[512];

/* q error: "gdal: <context>: <last GDAL message>" (interned, so it outlives this call) */
static S emsg(const char *what) {
    const char *m = CPLGetLastErrorMsg();
    if (m && *m) snprintf(errbuf, sizeof errbuf, "gdal: %s: %s", what, m);
    else snprintf(errbuf, sizeof errbuf, "gdal: %s", what);
    CPLErrorReset();
    return ss(errbuf);
}
/* NB: krr() returns NULL (as in kdb+), so a pending error is never a value to test. */
static K gerr(const char *what) { return krr(emsg(what)); }

/* ---------- q argument helpers ---------- */

/* path from `:file, `file or "file"; writes into buf, returns 0 on a bad type */
static int path_arg(K x, char *buf, size_t n) {
    if (x->t == -KS) { const char *s = x->s; if (*s == ':') s++; snprintf(buf, n, "%s", s); return 1; }
    if (x->t == KC) { size_t m = (size_t)x->n < n - 1 ? (size_t)x->n : n - 1; memcpy(buf, kC(x), m); buf[m] = 0; return 1; }
    if (x->t == -KC) { buf[0] = (char)x->g; buf[1] = 0; return 1; }
    return 0;
}

/* plain string argument (string, char or symbol); "" allowed */
static int str_arg(K x, char *buf, size_t n) {
    if (x->t == -KS) { snprintf(buf, n, "%s", x->s); return 1; }
    return path_arg(x, buf, n);
}

static K str_k(const char *s) { return kp((S)(s ? s : "")); }

static K sym_vec(int n, const char **names) {
    K r = ktn(KS, n);
    for (int i = 0; i < n; i++) kS(r)[i] = ss((S)names[i]);
    return r;
}

/* dictionary from parallel C arrays of names and values (takes ownership of vals) */
static K dict(int n, const char **names, K *vals) {
    K v = ktn(0, n);
    for (int i = 0; i < n; i++) kK(v)[i] = vals[i];
    return xD(sym_vec(n, names), v);
}

/* ---------- CRS ---------- */

/* SRS from anything OSR understands ("EPSG:4326", proj string, WKT), with lon/lat axis order */
static OGRSpatialReferenceH srs_from(const char *s) {
    OGRSpatialReferenceH h = OSRNewSpatialReference(NULL);
    if (OSRSetFromUserInput(h, s) != OGRERR_NONE) { OSRDestroySpatialReference(h); return NULL; }
    OSRSetAxisMappingStrategy(h, OAMS_TRADITIONAL_GIS_ORDER);
    return h;
}

/* short CRS name: "EPSG:4326" when known, otherwise WKT; "" for none */
static K srs_k(OGRSpatialReferenceH h) {
    if (!h) return str_k("");
    const char *auth = OSRGetAuthorityName(h, NULL), *code = OSRGetAuthorityCode(h, NULL);
    if (auth && code) { char b[64]; snprintf(b, sizeof b, "%s:%s", auth, code); return str_k(b); }
    char *w = NULL;
    OSRExportToWkt(h, &w);
    K r = str_k(w);
    CPLFree(w);
    return r;
}

/* ---------- geometry <-> WKB ---------- */

static K geom_wkb(OGRGeometryH g) {
    if (!g || OGR_G_IsEmpty(g)) return ktn(KG, 0);
    int n = OGR_G_WkbSize(g);
    K r = ktn(KG, n);
    OGR_G_ExportToWkb(g, wkbNDR, kG(r));
    return r;
}

/* NULL geometry for a null (empty) blob; *bad set on a malformed one */
static OGRGeometryH wkb_geom(K b, int *bad) {
    *bad = 0;
    if (b->t != KG) { *bad = 1; return NULL; }
    if (b->n == 0) return NULL;
    OGRGeometryH g = NULL;
    if (OGR_G_CreateFromWkb(kG(b), NULL, &g, (int)b->n) != OGRERR_NONE) { *bad = 1; return NULL; }
    return g;
}

/* ---------- vector: open and pick a layer ---------- */

static GDALDatasetH open_vector(K path, char *pbuf, size_t n) {
    if (!path_arg(path, pbuf, n)) return NULL;
    CPLErrorReset();
    return GDALOpenEx(pbuf, GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL);
}

/* layer by ` (first), `name, "name" or long index */
static OGRLayerH pick_layer(GDALDatasetH ds, K which) {
    if (which->t == -KS && !*which->s) return GDALDatasetGetLayer(ds, 0);
    if (which->t == -KJ) return GDALDatasetGetLayer(ds, (int)which->j);
    if (which->t == -KI) return GDALDatasetGetLayer(ds, which->i);
    char b[512];
    if (!str_arg(which, b, sizeof b)) return NULL;
    return GDALDatasetGetLayerByName(ds, b);
}

/* layers[path] -> symbol list of layer names */
K qgdal_layers(K path) {
    ensure_init();
    char p[4096];
    GDALDatasetH ds = open_vector(path, p, sizeof p);
    if (!ds) return path_arg(path, p, sizeof p) ? gerr("open") : krr("type");
    int n = GDALDatasetGetLayerCount(ds);
    K r = ktn(KS, n);
    for (int i = 0; i < n; i++) kS(r)[i] = ss((S)OGR_L_GetName(GDALDatasetGetLayer(ds, i)));
    GDALClose(ds);
    return r;
}

static const char *ftype_name(OGRFieldDefnH f) {
    OGRFieldType t = OGR_Fld_GetType(f);
    if (t == OFTInteger && OGR_Fld_GetSubType(f) == OFSTBoolean) return "Boolean";
    return OGR_GetFieldTypeName(t);
}

/* info[path;layer] -> dict: name, count, geomtype, crs, extent, fields (table of name/type) */
K qgdal_info(K path, K which) {
    ensure_init();
    char p[4096];
    GDALDatasetH ds = open_vector(path, p, sizeof p);
    if (!ds) return path_arg(path, p, sizeof p) ? gerr("open") : krr("type");
    OGRLayerH L = pick_layer(ds, which);
    if (!L) { GDALClose(ds); return krr("layer"); }
    OGRFeatureDefnH d = OGR_L_GetLayerDefn(L);
    int nf_ = OGR_FD_GetFieldCount(d);
    K fn = ktn(KS, nf_), ft = ktn(KS, nf_);
    for (int i = 0; i < nf_; i++) {
        OGRFieldDefnH f = OGR_FD_GetFieldDefn(d, i);
        kS(fn)[i] = ss((S)OGR_Fld_GetNameRef(f));
        kS(ft)[i] = ss((S)ftype_name(f));
    }
    const char *cn[] = { "field", "kind" };  /* not `type: a q keyword breaks qSQL */
    K fields = xT(xD(sym_vec(2, cn), knk(2, fn, ft)));
    OGREnvelope e;
    K ext = ktn(KF, 4);
    if (OGR_L_GetExtent(L, &e, 1) == OGRERR_NONE) {
        kF(ext)[0] = e.MinX; kF(ext)[1] = e.MinY; kF(ext)[2] = e.MaxX; kF(ext)[3] = e.MaxY;
    } else for (int i = 0; i < 4; i++) kF(ext)[i] = nf;
    const char *names[] = { "name", "count", "geomtype", "crs", "extent", "fields" };
    K vals[] = {
        ks((S)OGR_L_GetName(L)),
        kj(OGR_L_GetFeatureCount(L, 1)),
        ks((S)OGRGeometryTypeToName(OGR_L_GetGeomType(L))),
        srs_k(OGR_L_GetSpatialRef(L)),
        ext,
        fields,
    };
    GDALClose(ds);
    return dict(6, names, vals);
}

/* ---------- vector: read ---------- */

/* q column for an OGR field; NULL when the type maps to a general list */
typedef struct { I qt; K col; } colspec;

static I q_type_for(OGRFieldDefnH f) {
    switch (OGR_Fld_GetType(f)) {
    case OFTInteger:   return OGR_Fld_GetSubType(f) == OFSTBoolean ? KB : KI;
    case OFTInteger64: return KJ;
    case OFTReal:      return KF;
    case OFTDate:      return KD;
    case OFTDateTime:  return KP;
    case OFTTime:      return KT;
    default:           return 0;    /* strings, binary, lists: general list */
    }
}

/* nanoseconds/ms helpers for kdb's 2000.01.01 epoch */
static J days2000(int y, int m, int d) { return (J)ymd(y, m, d); }

static void set_cell(K col, I qt, J i, OGRFeatureH ft, int fi, OGRFieldDefnH fd) {
    int isnull = !OGR_F_IsFieldSetAndNotNull(ft, fi);
    switch (qt) {
    case KB: kG(col)[i] = isnull ? 0 : (G)(OGR_F_GetFieldAsInteger(ft, fi) != 0); return;
    case KI: kI(col)[i] = isnull ? ni : OGR_F_GetFieldAsInteger(ft, fi); return;
    case KJ: kJ(col)[i] = isnull ? nj : OGR_F_GetFieldAsInteger64(ft, fi); return;
    case KF: kF(col)[i] = isnull ? nf : OGR_F_GetFieldAsDouble(ft, fi); return;
    case KD: case KP: case KT: {
        int y, mo, d, h, mi, tz; float s;
        if (isnull || !OGR_F_GetFieldAsDateTimeEx(ft, fi, &y, &mo, &d, &h, &mi, &s, &tz)) {
            if (qt == KD) kI(col)[i] = ni; else if (qt == KP) kJ(col)[i] = nj; else kI(col)[i] = ni;
            return;
        }
        double secs = h * 3600.0 + mi * 60.0 + s;
        /* tz: 0 unknown, 1 local, 100 UTC, 100±n = offset in 15-minute steps; store UTC when known */
        if (tz > 1 && tz != 100) secs -= (tz - 100) * 15 * 60.0;
        if (qt == KD) kI(col)[i] = (I)days2000(y, mo, d);
        else if (qt == KT) kI(col)[i] = (I)llround(secs * 1000.0);
        else kJ(col)[i] = days2000(y, mo, d) * 86400000000000LL + (J)llround(secs * 1e9);
        return;
    }
    default: break;
    }
    /* general list cell */
    K v;
    OGRFieldType t = OGR_Fld_GetType(fd);
    if (isnull) v = t == OFTBinary ? ktn(KG, 0) : ktn(KC, 0);
    else if (t == OFTBinary) { int n; GByte *b = OGR_F_GetFieldAsBinary(ft, fi, &n); v = ktn(KG, n); memcpy(kG(v), b, (size_t)n); }
    else if (t == OFTIntegerList) { int n; const int *a = OGR_F_GetFieldAsIntegerList(ft, fi, &n); v = ktn(KI, n); memcpy(kI(v), a, (size_t)n * sizeof(int)); }
    else if (t == OFTInteger64List) { int n; const GIntBig *a = OGR_F_GetFieldAsInteger64List(ft, fi, &n); v = ktn(KJ, n); for (int k = 0; k < n; k++) kJ(v)[k] = a[k]; }
    else if (t == OFTRealList) { int n; const double *a = OGR_F_GetFieldAsDoubleList(ft, fi, &n); v = ktn(KF, n); memcpy(kF(v), a, (size_t)n * sizeof(double)); }
    else if (t == OFTStringList) { char **a = OGR_F_GetFieldAsStringList(ft, fi); int n = CSLCount(a); v = ktn(0, n); for (int k = 0; k < n; k++) kK(v)[k] = str_k(a[k]); }
    else v = str_k(OGR_F_GetFieldAsString(ft, fi));
    kK(col)[i] = v;
}

/* read[path;layer] -> table: attribute columns + `geom (WKB). layer: ` for the first.
 * Strings load as strings (cast with `$ for categories); dates as date, datetimes as
 * timestamp (UTC when the source has an offset), times as time, nulls as q nulls. */
K qgdal_read(K path, K which) {
    ensure_init();
    char p[4096];
    GDALDatasetH ds = open_vector(path, p, sizeof p);
    if (!ds) return path_arg(path, p, sizeof p) ? gerr("open") : krr("type");
    OGRLayerH L = pick_layer(ds, which);
    if (!L) { GDALClose(ds); return krr("layer"); }

    OGRFeatureDefnH d = OGR_L_GetLayerDefn(L);
    int nfld = OGR_FD_GetFieldCount(d);
    J cap = OGR_L_GetFeatureCount(L, 0);   /* may be -1 (unknown) or approximate: grow as needed */
    if (cap < 16) cap = 16;

    I *qt = calloc((size_t)nfld + 1, sizeof(I));
    K *cols = calloc((size_t)nfld + 1, sizeof(K));
    for (int f = 0; f < nfld; f++) { qt[f] = q_type_for(OGR_FD_GetFieldDefn(d, f)); cols[f] = ktn(qt[f], cap); }
    K geoms = ktn(0, cap);

    J n = 0;
    OGRFeatureH ft;
    OGR_L_ResetReading(L);
    while ((ft = OGR_L_GetNextFeature(L)) != NULL) {
        if (n == cap) {   /* grow every column; kdb has no realloc in k.h, so copy */
            J nc = cap * 2;
            for (int f = 0; f <= nfld; f++) {
                K old = f < nfld ? cols[f] : geoms;
                I t = f < nfld ? qt[f] : 0;
                K nw = ktn(t, nc);
                size_t w = t == 0 ? sizeof(K) : t == KB ? 1 : (t == KI || t == KD || t == KT) ? 4 : 8;
                memcpy(kG(nw), kG(old), (size_t)n * w);
                old->n = 0; r0(old);            /* elements moved, not copied */
                if (f < nfld) cols[f] = nw; else geoms = nw;
            }
            cap = nc;
        }
        for (int f = 0; f < nfld; f++) set_cell(cols[f], qt[f], n, ft, f, OGR_FD_GetFieldDefn(d, f));
        kK(geoms)[n] = geom_wkb(OGR_F_GetGeometryRef(ft));
        n++;
        OGR_F_Destroy(ft);
    }
    int had_err = CPLGetLastErrorType() >= CE_Failure;

    /* column names; a source field already called geom keeps its name, ours becomes geom1 */
    K names = ktn(KS, nfld + 1);
    int clash = 0;
    for (int f = 0; f < nfld; f++) {
        const char *nm = OGR_Fld_GetNameRef(OGR_FD_GetFieldDefn(d, f));
        if (!strcmp(nm, "geom")) clash = 1;
        kS(names)[f] = ss((S)nm);
    }
    kS(names)[nfld] = ss(clash ? "geom1" : "geom");
    K vals = ktn(0, nfld + 1);
    for (int f = 0; f < nfld; f++) { cols[f]->n = n; kK(vals)[f] = cols[f]; }
    geoms->n = n; kK(vals)[nfld] = geoms;
    free(qt); free(cols);
    if (had_err) { r0(names); r0(vals); K e = gerr("read"); GDALClose(ds); return e; }
    GDALClose(ds);
    return xT(xD(names, vals));
}

/* ---------- vector: write ---------- */

static const char *driver_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    static const char *map[][2] = {
        { ".gpkg", "GPKG" }, { ".geojson", "GeoJSON" }, { ".json", "GeoJSON" },
        { ".geojsonl", "GeoJSONSeq" }, { ".geojsons", "GeoJSONSeq" },
        { ".shp", "ESRI Shapefile" }, { ".fgb", "FlatGeobuf" }, { ".csv", "CSV" },
        { ".parquet", "Parquet" }, { ".gml", "GML" }, { ".kml", "KML" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(dot, map[i][0])) return map[i][1];
    return NULL;
}

/* is this column a list of byte vectors (a geometry column)? */
static int is_geom_col(K c) {
    if (c->t != 0) return 0;
    for (J i = 0; i < c->n; i++) if (kK(c)[i]->t != KG) return 0;
    return 1;
}

static int is_str_col(K c) {
    if (c->t != 0) return 0;
    for (J i = 0; i < c->n; i++) if (kK(c)[i]->t != KC && kK(c)[i]->t != -KC) return 0;
    return 1;
}

static int ogr_type_for(K c, OGRFieldType *t, OGRFieldSubType *st) {
    *st = OFSTNone;
    switch (c->t) {
    case KB: *t = OFTInteger; *st = OFSTBoolean; return 1;
    case KG: case KH: case KI: *t = OFTInteger; return 1;
    case KJ: *t = OFTInteger64; return 1;
    case KE: case KF: *t = OFTReal; return 1;
    case KS: *t = OFTString; return 1;
    case KD: *t = OFTDate; return 1;
    case KP: case KZ: *t = OFTDateTime; return 1;
    case KT: *t = OFTTime; return 1;
    case 0:  if (is_str_col(c)) { *t = OFTString; return 1; } return 0;
    default: return 0;
    }
}

static void put_cell(OGRFeatureH ft, int fi, K c, J i) {
    switch (c->t) {
    case KB: OGR_F_SetFieldInteger(ft, fi, kG(c)[i]); return;
    case KG: OGR_F_SetFieldInteger(ft, fi, kG(c)[i]); return;
    case KH: if (kH(c)[i] != nh) OGR_F_SetFieldInteger(ft, fi, kH(c)[i]); else OGR_F_SetFieldNull(ft, fi); return;
    case KI: if (kI(c)[i] != ni) OGR_F_SetFieldInteger(ft, fi, kI(c)[i]); else OGR_F_SetFieldNull(ft, fi); return;
    case KJ: if (kJ(c)[i] != nj) OGR_F_SetFieldInteger64(ft, fi, kJ(c)[i]); else OGR_F_SetFieldNull(ft, fi); return;
    case KE: if (!isnan(kE(c)[i])) OGR_F_SetFieldDouble(ft, fi, kE(c)[i]); else OGR_F_SetFieldNull(ft, fi); return;
    case KF: if (!isnan(kF(c)[i])) OGR_F_SetFieldDouble(ft, fi, kF(c)[i]); else OGR_F_SetFieldNull(ft, fi); return;
    case KS: OGR_F_SetFieldString(ft, fi, kS(c)[i]); return;
    case KD: {
        I v = kI(c)[i];
        if (v == ni) { OGR_F_SetFieldNull(ft, fi); return; }
        I y = dj(v);   /* yyyymmdd */
        OGR_F_SetFieldDateTimeEx(ft, fi, y / 10000, (y / 100) % 100, y % 100, 0, 0, 0, 0);
        return;
    }
    case KP: case KZ: {
        J ns;
        if (c->t == KP) { ns = kJ(c)[i]; if (ns == nj) { OGR_F_SetFieldNull(ft, fi); return; } }
        else { double z = kF(c)[i]; if (isnan(z)) { OGR_F_SetFieldNull(ft, fi); return; } ns = (J)llround(z * 86400e9); }
        J day = ns / 86400000000000LL, rem = ns % 86400000000000LL;
        if (rem < 0) { rem += 86400000000000LL; day--; }
        I y = dj((I)day);
        double s = rem / 1e9;
        int h = (int)(s / 3600); s -= h * 3600.0;
        int mi = (int)(s / 60); s -= mi * 60.0;
        OGR_F_SetFieldDateTimeEx(ft, fi, y / 10000, (y / 100) % 100, y % 100, h, mi, (float)s, 100);
        return;
    }
    case KT: {
        I ms = kI(c)[i];
        if (ms == ni) { OGR_F_SetFieldNull(ft, fi); return; }
        OGR_F_SetFieldDateTimeEx(ft, fi, 0, 0, 0, ms / 3600000, (ms / 60000) % 60, (ms % 60000) / 1000.0f, 0);
        return;
    }
    case 0: {
        K s = kK(c)[i];
        char *buf;
        if (s->t == -KC) { char one[2] = { (char)s->g, 0 }; OGR_F_SetFieldString(ft, fi, one); return; }
        buf = malloc((size_t)s->n + 1); memcpy(buf, kC(s), (size_t)s->n); buf[s->n] = 0;
        OGR_F_SetFieldString(ft, fi, buf); free(buf);
        return;
    }
    }
}

/* write[path;layer;table;crs] -> number of features written.
 * Driver comes from the extension (.gpkg .geojson .shp .fgb .csv .parquet .gml .kml).
 * The first column of byte vectors is the geometry; crs is "EPSG:4326" etc, or "" for none.
 * Refuses to overwrite: an existing file is an error (delete it first with hdel). */
K qgdal_write(K path, K lyr, K tbl, K crs) {
    ensure_init();
    char p[4096], ln[512], cs[1024];
    if (!path_arg(path, p, sizeof p) || !str_arg(lyr, ln, sizeof ln) || !str_arg(crs, cs, sizeof cs)) return krr("type");
    if (tbl->t != XT) return krr("type");
    if (!*ln) { const char *b = strrchr(p, '/'); snprintf(ln, sizeof ln, "%.500s", b ? b + 1 : p); char *d = strrchr(ln, '.'); if (d) *d = 0; }
    const char *dn = driver_for(p);
    if (!dn) return krr("gdal: write: unknown extension (use .gpkg .geojson .shp .fgb .csv .parquet .gml .kml)");
    struct stat st_;
    if (stat(p, &st_) == 0) return krr("gdal: write: file exists");
    GDALDriverH drv = GDALGetDriverByName(dn);
    if (!drv) return krr(ss((S)"gdal: write: driver not available in this GDAL build"));

    K names = kK(tbl->k)[0], cols = kK(tbl->k)[1];
    int nc = (int)names->n;
    int gcol = -1;
    for (int c = 0; c < nc; c++) if (is_geom_col(kK(cols)[c])) { gcol = c; break; }

    OGRSpatialReferenceH srs = NULL;
    if (*cs && !(srs = srs_from(cs))) return gerr("write: bad crs");

    CPLErrorReset();
    GDALDatasetH ds = GDALCreate(drv, p, 0, 0, 0, GDT_Unknown, NULL);
    if (!ds) { if (srs) OSRDestroySpatialReference(srs); return gerr("write: create"); }
    OGRLayerH L = GDALDatasetCreateLayer(ds, ln, srs, gcol >= 0 ? wkbUnknown : wkbNone, NULL);
    if (srs) OSRDestroySpatialReference(srs);
    if (!L) { GDALClose(ds); return gerr("write: layer"); }

    int *fidx = malloc(sizeof(int) * (size_t)(nc ? nc : 1));
    S err = NULL;   /* message, raised once at the end: krr() itself returns NULL */
    for (int c = 0; c < nc && !err; c++) {
        fidx[c] = -1;
        if (c == gcol) continue;
        OGRFieldType t; OGRFieldSubType sub;
        if (!ogr_type_for(kK(cols)[c], &t, &sub)) { err = ss((S)"gdal: write: unsupported column type"); break; }
        OGRFieldDefnH fd = OGR_Fld_Create(kS(names)[c], t);
        OGR_Fld_SetSubType(fd, sub);
        if (OGR_L_CreateField(L, fd, 1) != OGRERR_NONE) err = emsg("write: field");
        OGR_Fld_Destroy(fd);
        fidx[c] = OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(L), kS(names)[c]);
    }

    J nrows = nc ? kK(cols)[0]->n : 0, written = 0;
    OGRFeatureDefnH d = OGR_L_GetLayerDefn(L);
    int tx = OGR_L_TestCapability(L, OLCTransactions) && OGR_L_StartTransaction(L) == OGRERR_NONE;
    for (J i = 0; i < nrows && !err; i++) {
        OGRFeatureH ft = OGR_F_Create(d);
        for (int c = 0; c < nc; c++) if (fidx[c] >= 0) put_cell(ft, fidx[c], kK(cols)[c], i);
        if (gcol >= 0) {
            int bad;
            OGRGeometryH g = wkb_geom(kK(kK(cols)[gcol])[i], &bad);
            if (bad) { OGR_F_Destroy(ft); err = ss((S)"gdal: write: bad wkb"); break; }
            if (g) OGR_F_SetGeometryDirectly(ft, g);
        }
        if (OGR_L_CreateFeature(L, ft) != OGRERR_NONE) err = emsg("write: feature");
        else written++;
        OGR_F_Destroy(ft);
    }
    if (tx) {
        if (err) (void)!OGR_L_RollbackTransaction(L);
        else if (OGR_L_CommitTransaction(L) != OGRERR_NONE) err = emsg("write: commit");
    }
    free(fidx);
    GDALClose(ds);
    if (err) { VSIUnlink(p); return krr(err); }   /* don't leave a half-written file we created */
    return kj(written);
}

/* ---------- CRS reprojection ---------- */

/* transform[geoms;from;to] -> geoms reprojected. CRS as "EPSG:4326", proj string or WKT.
 * Coordinates are always x=longitude/easting, y=latitude/northing. */
K qgdal_transform(K x, K from, K to) {
    ensure_init();
    char fs[1024], ts[1024];
    if (!str_arg(from, fs, sizeof fs) || !str_arg(to, ts, sizeof ts)) return krr("type");
    int atom = x->t == KG;
    if (!atom && x->t != 0) return krr("type");
    CPLErrorReset();
    OGRSpatialReferenceH a = srs_from(fs), b = a ? srs_from(ts) : NULL;
    if (!a || !b) { if (a) OSRDestroySpatialReference(a); return gerr("transform: bad crs"); }
    OGRCoordinateTransformationH ct = OCTNewCoordinateTransformation(a, b);
    OSRDestroySpatialReference(a); OSRDestroySpatialReference(b);
    if (!ct) return gerr("transform: no transformation between these CRS");

    J n = atom ? 1 : x->n;
    K r = atom ? NULL : ktn(0, n);
    S err = NULL;   /* message, raised once at the end: krr() itself returns NULL */
    J i = 0;
    for (; i < n; i++) {
        int bad;
        OGRGeometryH g = wkb_geom(atom ? x : kK(x)[i], &bad);
        if (bad) { err = ss((S)"gdal: transform: bad wkb"); break; }
        if (g && OGR_G_Transform(g, ct) != OGRERR_NONE) { OGR_G_DestroyGeometry(g); err = emsg("transform"); break; }
        K e = geom_wkb(g);
        if (g) OGR_G_DestroyGeometry(g);
        if (atom) { r = e; break; }
        kK(r)[i] = e;
    }
    OCTDestroyCoordinateTransformation(ct);
    if (err) { if (r) { r->n = i; r0(r); } return krr(err); }
    return r;
}

/* ---------- raster ---------- */

static GDALDatasetH open_raster(K path, char *p, size_t n) {
    if (!path_arg(path, p, n)) return NULL;
    CPLErrorReset();
    return GDALOpenEx(p, GDAL_OF_RASTER | GDAL_OF_READONLY, NULL, NULL, NULL);
}

/* rinfo[path] -> dict: width, height, bands, transform (6 floats), crs, nodata (per band), dtype (per band) */
K qgdal_rinfo(K path) {
    ensure_init();
    char p[4096];
    GDALDatasetH ds = open_raster(path, p, sizeof p);
    if (!ds) return path_arg(path, p, sizeof p) ? gerr("open") : krr("type");
    int nb = GDALGetRasterCount(ds);
    double gt[6] = { 0, 1, 0, 0, 0, 1 };
    GDALGetGeoTransform(ds, gt);
    K tr = ktn(KF, 6); memcpy(kF(tr), gt, sizeof gt);
    K nd = ktn(KF, nb), dt = ktn(KS, nb);
    for (int b = 0; b < nb; b++) {
        GDALRasterBandH h = GDALGetRasterBand(ds, b + 1);
        int has; double v = GDALGetRasterNoDataValue(h, &has);
        kF(nd)[b] = has ? v : nf;
        kS(dt)[b] = ss((S)GDALGetDataTypeName(GDALGetRasterDataType(h)));
    }
    const char *names[] = { "width", "height", "bands", "transform", "crs", "nodata", "dtype" };
    K vals[] = { kj(GDALGetRasterXSize(ds)), kj(GDALGetRasterYSize(ds)), kj(nb), tr,
                 srs_k(GDALGetSpatialRef(ds)), nd, dt };
    GDALClose(ds);
    return dict(7, names, vals);
}

static GDALRasterBandH band_arg(GDALDatasetH ds, K band) {
    J b = band->t == -KJ ? band->j : band->t == -KI ? band->i : -1;
    if (b < 1 || b > GDALGetRasterCount(ds)) return NULL;
    return GDALGetRasterBand(ds, (int)b);
}

/* rread[path;band] -> float vector, row-major (width*height), nodata as 0n. band is 1-based. */
K qgdal_rread(K path, K band) {
    ensure_init();
    char p[4096];
    GDALDatasetH ds = open_raster(path, p, sizeof p);
    if (!ds) return path_arg(path, p, sizeof p) ? gerr("open") : krr("type");
    GDALRasterBandH h = band_arg(ds, band);
    if (!h) { GDALClose(ds); return krr("band"); }
    int w = GDALGetRasterXSize(ds), ht = GDALGetRasterYSize(ds);
    K r = ktn(KF, (J)w * ht);
    if (GDALRasterIO(h, GF_Read, 0, 0, w, ht, kF(r), w, ht, GDT_Float64, 0, 0) != CE_None) {
        r0(r); K e = gerr("rread"); GDALClose(ds); return e;
    }
    int has; double v = GDALGetRasterNoDataValue(h, &has);
    if (has) for (J i = 0; i < r->n; i++) if (kF(r)[i] == v || (isnan(v) && isnan(kF(r)[i]))) kF(r)[i] = nf;
    GDALClose(ds);
    return r;
}

/* rsample[path;band;points] -> float per point: the value of the pixel it falls in.
 * Points are WKB (in the raster's CRS); outside the raster, nodata, or null -> 0n. */
K qgdal_rsample(K path, K band, K pts) {
    ensure_init();
    char p[4096];
    int atom = pts->t == KG;
    if (!atom && pts->t != 0) return krr("type");
    GDALDatasetH ds = open_raster(path, p, sizeof p);
    if (!ds) return path_arg(path, p, sizeof p) ? gerr("open") : krr("type");
    GDALRasterBandH h = band_arg(ds, band);
    if (!h) { GDALClose(ds); return krr("band"); }
    double gt[6], inv[6];
    if (GDALGetGeoTransform(ds, gt) != CE_None || !GDALInvGeoTransform(gt, inv)) { GDALClose(ds); return gerr("rsample: no geotransform"); }
    int w = GDALGetRasterXSize(ds), ht = GDALGetRasterYSize(ds);
    int has; double nd = GDALGetRasterNoDataValue(h, &has);

    J n = atom ? 1 : pts->n;
    K r = ktn(KF, n);
    S err = NULL;   /* message, raised once at the end: krr() itself returns NULL */
    for (J i = 0; i < n; i++) {
        int bad;
        OGRGeometryH g = wkb_geom(atom ? pts : kK(pts)[i], &bad);
        if (bad) { err = ss((S)"gdal: rsample: bad wkb"); break; }
        double v = nf;
        if (g && wkbFlatten(OGR_G_GetGeometryType(g)) == wkbPoint) {
            double X = OGR_G_GetX(g, 0), Y = OGR_G_GetY(g, 0);
            double px = inv[0] + X * inv[1] + Y * inv[2], py = inv[3] + X * inv[4] + Y * inv[5];
            if (px >= 0 && py >= 0 && px < w && py < ht) {
                double cell;
                if (GDALRasterIO(h, GF_Read, (int)px, (int)py, 1, 1, &cell, 1, 1, GDT_Float64, 0, 0) == CE_None
                    && !(has && (cell == nd || (isnan(nd) && isnan(cell)))))
                    v = cell;
            }
        }
        if (g) OGR_G_DestroyGeometry(g);
        kF(r)[i] = v;
    }
    GDALClose(ds);
    if (err) { r0(r); return krr(err); }
    if (atom) { double v = kF(r)[0]; r0(r); return kf(v); }
    return r;
}

/* version[] -> GDAL version string */
K qgdal_version(K x) { (void)x; return kp((S)GDALVersionInfo("RELEASE_NAME")); }
