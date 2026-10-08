/* Golden-vector replay of the displacement pipeline (Math/math_displacement.c +
 * Math/math_quality.c) on a recorded excerpt of the 19 h phasor stream.
 *
 * tests/data/phasor_excerpt.bin   26000 consecutive batches (10.6 min) of real phasors, 34 B each
 *                                 (8 x float32 LE: iB qB iA qA iS1 qS1 iS2 qS2, then uint16 seq)
 * tests/data/golden_displacement.csv  what Services/svc_displacement.c produced on it at fw 0.10.73,
 *                                 BEFORE the maths was extracted into Math/ (see tests/data/README.md)
 *
 * The replay below does what svc_displacement.c's process_one_batch() does, in the same order, with a
 * fake clock, and must reproduce the golden values: the published display stream (value, doubtful
 * flags, cadence), the sampled per-batch readings, and the precision measurements started every 1500
 * batches. Two disturbances are injected at fixed places: batch 9000 is dropped (a seq jump) and batch
 * 15000 gets a degenerate excitation (A == B). The test runs from tests/ (make) or from the repo root. */
#include "test.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../Math/math_phasor.c"
#include "../Math/math_window.c"
#include "../Math/math_displacement.c"
#include "../Math/math_quality.c"

/* Settings the golden was produced with. */
#define K_MICRO          21300
#define PGA_S1           16U
#define PGA_S2           16U
#define ZERO_PPM_S1      1234
#define ZERO_PPM_S2      (-4321)
#define PHASE_CDEG_S1    (-600)
#define PHASE_CDEG_S2    (-915)
#define INVERT_S2        1        /* the precision result is reported through the invert flag */

/* config.h values (the firmware's own, restated so this test does not pull in the HAL headers). */
#define BATCH_CYCLES       64U
#define DISPLAY_TAPS       25U
#define DISPLAY_DECIMATION 10U
#define PREC_WINDOW        81U
#define QUALITY_K          6.0f
#define PREC_TIMEOUT_MS    5000U
#define FLOOR_DOUBLING_S   1200.0f
#define RESEED_S           600.0f
#define BATCH_RATE_HZ      (2604.1667f / (float)BATCH_CYCLES)

#pragma pack(push, 1)
typedef struct { float p[8]; uint16_t seq; } Rec;
#pragma pack(pop)

static FILE *open_data(const char *name)
{
    char path[256];
    const char *prefix[] = { "data/", "tests/data/", "../tests/data/" };
    for (unsigned i = 0; i < sizeof prefix / sizeof prefix[0]; ++i) {
        snprintf(path, sizeof path, "%s%s", prefix[i], name);
        FILE *f = fopen(path, "rb");
        if (f) return f;
    }
    return NULL;
}

static Rec *load_excerpt(long *n_out)
{
    FILE *f = open_data("phasor_excerpt.bin");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    long n = sz / (long)sizeof(Rec);
    Rec *r = malloc((size_t)sz);
    if (r && fread(r, sizeof(Rec), (size_t)n, f) != (size_t)n) { free(r); r = NULL; }
    fclose(f);
    *n_out = n;
    return r;
}

typedef struct { char kind[8]; long batch; double v[6]; } GoldenLine;

/* Parses golden_displacement.csv, skipping '#' lines. */
static GoldenLine *load_golden(long *n_out)
{
    FILE *f = open_data("golden_displacement.csv");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    GoldenLine *g = malloc(cap * sizeof *g);
    char line[256];
    while (g && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (n == cap) { cap *= 2; g = realloc(g, cap * sizeof *g); if (!g) break; }
        GoldenLine *e = &g[n];
        memset(e, 0, sizeof *e);
        char *tok = strtok(line, ",");
        if (!tok) continue;
        strncpy(e->kind, tok, sizeof e->kind - 1);
        tok = strtok(NULL, ",");
        e->batch = tok ? atol(tok) : 0;
        for (int k = 0; k < 6; ++k) {
            tok = strtok(NULL, ",");
            e->v[k] = tok ? atof(tok) : 0.0;
        }
        n++;
    }
    fclose(f);
    *n_out = (long)n;
    return g;
}

/* Close enough for a different compiler / FMA contraction; flags and counts are compared exactly. */
static int near(double got, double want)
{
    double tol = 1e-5 * fabs(want) + 1e-9;
    return fabs(got - want) <= tol;
}

static const GoldenLine *find(const GoldenLine *g, long ng, const char *kind, long batch)
{
    for (long k = 0; k < ng; ++k) {
        if (g[k].batch == batch && strcmp(g[k].kind, kind) == 0) return &g[k];
        if (g[k].batch > batch) break;     /* the file is in time order */
    }
    return NULL;
}

TEST(replay_reproduces_the_golden_reference)
{
    long n = 0, ng = 0;
    Rec *r = load_excerpt(&n);
    GoldenLine *g = load_golden(&ng);
    CHECK(r != NULL && g != NULL);
    if (!r || !g) { free(r); free(g); return; }
    CHECK_EQ(n, 26000);

    MathSensorCal cal[2];
    float c1, s1, c2, s2;
    math_phase_sincos(PHASE_CDEG_S1, &c1, &s1);
    math_phase_sincos(PHASE_CDEG_S2, &c2, &s2);
    math_sensor_cal_make(&cal[0], K_MICRO, PGA_S1, ZERO_PPM_S1, c1, s1);
    math_sensor_cal_make(&cal[1], K_MICRO, PGA_S2, ZERO_PPM_S2, c2, s2);

    static MathDisplay ds;
    static MathPrecision prec;
    math_display_init(&ds, DISPLAY_TAPS, DISPLAY_DECIMATION, PREC_WINDOW, QUALITY_K,
                      0.693147f / (FLOOR_DOUBLING_S * BATCH_RATE_HZ),
                      (uint32_t)(RESEED_S * BATCH_RATE_HZ));
    math_display_reset(&ds);   /* a start */

    uint16_t last_dseq = ds.seq;
    int prec_active = 0; long prec_start = 0; uint32_t prec_start_ms = 0;
    uint16_t degenerate = 0;
    long n_disp = 0, n_prec = 0, n_raw = 0, mismatches = 0;
    float delta1 = 0, delta2 = 0, res1 = 0, res2 = 0; int ok = 0;

    for (long i = 0; i < n; ++i) {
        if (i == 9000) continue;                                   /* dropped batch */
        MathBatchSums s;
        s.iB = (int64_t)r[i].p[0]; s.qB = (int64_t)r[i].p[1];
        s.iA = (int64_t)r[i].p[2]; s.qA = (int64_t)r[i].p[3];
        s.iS1 = (int64_t)r[i].p[4]; s.qS1 = (int64_t)r[i].p[5];
        s.iS2 = (int64_t)r[i].p[6]; s.qS2 = (int64_t)r[i].p[7];
        if (i == 15000) { s.iA = s.iB; s.qA = s.qB; }              /* degenerate excitation */
        uint32_t now_ms = (uint32_t)((double)i * 24.5760);

        if (!prec_active && i >= 300 && (i % 1500) == 300) {
            math_precision_start(&prec);
            prec_active = 1; prec_start = i; prec_start_ms = now_ms;
        }

        MathBatchResult res;
        if (!math_batch_demod(&s, cal, &res)) {
            degenerate++;
            math_display_skip(&ds);
            ok = 0;
        } else {
            if (!math_display_feed(&ds, r[i].seq, (uint16_t)BATCH_CYCLES, res.delta, res.residual)) {
                math_precision_break(&prec);
            }
            delta1 = res.delta[0]; delta2 = res.delta[1];
            res1 = res.residual[0]; res2 = res.residual[1];
            ok = 1;
            if (prec_active) {
                bool timed_out = (uint32_t)(now_ms - prec_start_ms) >= PREC_TIMEOUT_MS;
                MathPrecisionStep st = math_precision_batch(&prec, &ds, timed_out);
                if (st != MATH_PRECISION_WAITING) {
                    float d1 = (st == MATH_PRECISION_OK) ? prec.result[0] : 0.0f;
                    float d2 = (st == MATH_PRECISION_OK) ? (INVERT_S2 ? -prec.result[1] : prec.result[1]) : 0.0f;
                    float dd = d1 - d2;
                    int failed = (st == MATH_PRECISION_TIMEOUT);
                    n_prec++;
                    /* golden: prec,batch,d1,d2,diff,failed,batches-since-trigger */
                    const GoldenLine *e = find(g, ng, "prec", i);
                    if (!e || !near(d1, e->v[0]) || !near(d2, e->v[1]) || !near(dd, e->v[2])
                        || failed != (int)e->v[3] || (i - prec_start) != (long)e->v[4]) {
                        mismatches++;
                        printf("    precision mismatch at batch %ld: got %.9g %.9g %.9g failed=%d len=%ld\n",
                               i, d1, d2, dd, failed, i - prec_start);
                    }
                    prec_active = 0;
                }
            }
        }

        if ((i % 50) == 0) {                                       /* golden: raw,batch,d1,d2,res1,res2,ok */
            n_raw++;
            const GoldenLine *e = find(g, ng, "raw", i);
            if (!e || !near(delta1, e->v[0]) || !near(delta2, e->v[1])
                || !near(res1, e->v[2]) || !near(res2, e->v[3]) || ok != (int)e->v[4]) {
                mismatches++;
                printf("    raw mismatch at batch %ld\n", i);
            }
        }
        if (ds.seq != last_dseq) {                                 /* golden: disp,batch,out1,out2,dbt1,dbt2,valid,seq */
            last_dseq = ds.seq;
            n_disp++;
            const GoldenLine *e = find(g, ng, "disp", i);
            if (!e || !near(ds.out[0], e->v[0]) || !near(ds.out[1], e->v[1])
                || (int)ds.doubtful[0] != (int)e->v[2] || (int)ds.doubtful[1] != (int)e->v[3]
                || (int)ds.valid != (int)e->v[4]) {
                mismatches++;
                printf("    display mismatch at batch %ld\n", i);
            }
        }
    }

    /* every golden line must have been matched by a replayed event, and vice versa */
    long g_disp = 0, g_prec = 0, g_raw = 0;
    for (long k = 0; k < ng; ++k) {
        if (strcmp(g[k].kind, "disp") == 0) g_disp++;
        else if (strcmp(g[k].kind, "prec") == 0) g_prec++;
        else if (strcmp(g[k].kind, "raw") == 0) g_raw++;
        else if (strcmp(g[k].kind, "sum") == 0) CHECK_EQ(degenerate, (long)g[k].v[0]);
    }
    CHECK_EQ(mismatches, 0);
    CHECK_EQ(n_disp, g_disp);
    CHECK_EQ(n_prec, g_prec);
    CHECK_EQ(n_raw, g_raw);
    CHECK(n_disp > 2000);                 /* the golden really covers a long stream ... */
    CHECK(g_prec >= 15);                  /* ... and many precision measurements */
    free(r); free(g);
}

TEST(the_excerpt_contains_both_quiet_and_disturbed_windows)
{
    /* Guards the golden's usefulness: if someone replaces the excerpt with a quiet stretch the
     * doubtful-flag logic would silently stop being exercised. */
    long ng = 0;
    GoldenLine *g = load_golden(&ng);
    CHECK(g != NULL);
    if (!g) return;
    long flagged = 0, total = 0;
    for (long k = 0; k < ng; ++k) {
        if (strcmp(g[k].kind, "disp") == 0) {
            total++;
            if (g[k].v[2] != 0.0 || g[k].v[3] != 0.0) flagged++;
        }
    }
    CHECK(total > 2000);
    CHECK(flagged > total / 50);          /* more than 2 % flagged */
    CHECK(flagged < total / 2);           /* and not everything */
    free(g);
}

int main(void)
{
    RUN(replay_reproduces_the_golden_reference);
    RUN(the_excerpt_contains_both_quiet_and_disturbed_windows);
    return test_summary();
}
