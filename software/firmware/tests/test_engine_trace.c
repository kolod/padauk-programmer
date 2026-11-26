/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The chip engine against the reference traces (T014, specs/002-chip-engine/contracts/traces.md): every trace in
 * software/tests/vectors/traces/ (generated from docs/easy-pdk-programmer-protocol.md by gen_traces.py) is replayed —
 * the fake IC holds the trace's chip and the memory pattern of the generator — and the HAL events recorded by the fake
 * lines must match the trace: set points, line modes, sent values and bit counts, received bit counts exactly; pauses,
 * clock-high times and delays as minimums. Consecutive receives on the same lines, consecutive delays and consecutive
 * idle calls are merged on both sides (how the engine splits them is not part of the protocol). */
#include "pdk_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip_engine.h"
#include "chips/chip_table.h"
#include "engine_internal.h"
#include "fake_hal.h"
#include "fake_icp.h"
#include "fake_icp_ic.h"
#include "proto/proto_defs.h"

#ifndef PDK_TRACE_DIR
#error "PDK_TRACE_DIR must point to software/tests/vectors/traces"
#endif

#define MAX_EV 2048

typedef struct {
    fake_ev_kind_t kind;
    int line, clk, dir, bits;
    long long value;      /* -1: not compared */
    uint32_t mV;
    uint32_t min_ns;      /* clock / delay */
    uint32_t min_us;      /* vdd / vpp: time to the next event */
    uint32_t gap_min_us;  /* vdd / vpp: time since the previous event */
    uint32_t t_us;        /* recorded side only */
    uint32_t ns;          /* recorded side only */
} ev_t;

static ev_t want[MAX_EV], got[MAX_EV];
static int nwant, ngot;
static char chip_name[16], operation[16];
static long in_addr, in_count, in_answer;
static long in_addrs[8];
static int n_addrs;

void pdk_test_setup(void)
{
    fake_reset();
    eng_ses.pause_us = 0;
    eng_ses.state = SES_IDLE;
}

/* ---------------------------------------------------------------- the trace reader (one event per line) */

static const char *field(const char *line, const char *key)
{
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\": ", key);
    const char *p = strstr(line, pat);
    return p ? p + strlen(pat) : NULL;
}

static long long num(const char *line, const char *key, long long dflt)
{
    const char *p = field(line, key);
    return p && *p != 'n' ? strtoll(p, NULL, 10) : dflt;
}

static int str_is(const char *line, const char *key, const char *value)
{
    const char *p = field(line, key);
    size_t n = strlen(value);
    return p && p[0] == '"' && !strncmp(p + 1, value, n) && p[1 + n] == '"';
}

static int line_of(const char *line, const char *key)
{
    static const char *names[] = {"CLK", "DAT", "PA4", "PA0", "PA7"};
    for (int i = 0; i < 5; i++)
        if (str_is(line, key, names[i]))
            return i;
    return -1;
}

static int dir_of(const char *line)
{
    static const char *names[] = {"out_low", "out_high", "in", "in_pulldown", "in_pullup"};
    for (int i = 0; i < 5; i++)
        if (str_is(line, "dir", names[i]))
            return i;
    return -1;
}

static void load(const char *file)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", PDK_TRACE_DIR, file);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    char line[512];
    nwant = 0;
    in_addr = in_count = 0;
    in_answer = -1;
    while (fgets(line, sizeof line, f)) {
        const char *p;
        if ((p = field(line, "chip")))
            sscanf(p, "\"%15[^\"]\"", chip_name);
        if ((p = field(line, "operation")))
            sscanf(p, "\"%15[^\"]\"", operation);
        if (field(line, "input")) {
            in_addr = num(line, "addr", 0);
            in_count = num(line, "count", 0);
            in_answer = num(line, "answer_id", -1);
            n_addrs = 0;
            const char *a = field(line, "addrs");
            if (a && *a == '[')
                for (a++; *a && *a != ']' && n_addrs < 8;) {
                    in_addrs[n_addrs++] = strtol(a, (char **)&a, 10);
                    while (*a == ',' || *a == ' ')
                        a++;
                }
        }
        if (!field(line, "e"))
            continue;
        ev_t *e = &want[nwant++];
        memset(e, 0, sizeof *e);
        e->value = -1;
        e->line = e->clk = e->dir = -1;
        if (str_is(line, "e", "vdd") || str_is(line, "e", "vpp")) {
            e->kind = str_is(line, "e", "vdd") ? FEV_VDD : FEV_VPP;
            e->mV = (uint32_t)num(line, "mV", 0);
            e->min_us = (uint32_t)num(line, "min_us", 0);
            e->gap_min_us = (uint32_t)num(line, "gap_min_us", 0);
        } else if (str_is(line, "e", "mode")) {
            e->kind = FEV_MODE;
            e->line = line_of(line, "line");
            e->dir = dir_of(line);
        } else if (str_is(line, "e", "send")) {
            e->kind = FEV_SEND;
            e->line = line_of(line, "data");
            e->clk = line_of(line, "clk");
            e->value = num(line, "value", 0);
            e->bits = (int)num(line, "bits", 0);
        } else if (str_is(line, "e", "recv")) {
            e->kind = FEV_RECV;
            e->line = line_of(line, "data");
            e->clk = line_of(line, "clk");
            e->bits = (int)num(line, "bits", 0);
        } else if (str_is(line, "e", "clock")) {
            e->kind = FEV_CLOCK;
            e->min_ns = (uint32_t)num(line, "min_ns", 0);
        } else if (str_is(line, "e", "delay")) {
            e->kind = FEV_DELAY;
            e->min_ns = (uint32_t)num(line, "min_us", 0) * 1000u;
        } else if (str_is(line, "e", "idle")) {
            e->kind = FEV_IDLE;
        } else {
            fprintf(stderr, "unknown event in %s: %s", file, line);
            exit(1);
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------------- normalisation and comparison */

static int merge(ev_t *v, int n)
{
    int out = 0;
    for (int i = 0; i < n; i++) {
        ev_t *prev = out ? &v[out - 1] : NULL;
        if (prev && v[i].kind == prev->kind &&
            ((v[i].kind == FEV_RECV && v[i].line == prev->line && v[i].clk == prev->clk) || v[i].kind == FEV_DELAY ||
             v[i].kind == FEV_IDLE)) {
            prev->bits += v[i].bits;
            prev->min_ns += v[i].min_ns;
            prev->ns += v[i].ns;
            continue;
        }
        v[out++] = v[i];
    }
    return out;
}

static void record(void)
{
    const fake_ev_t *ev;
    size_t n = fake_icp_events(&ev);
    ngot = 0;
    for (size_t i = 0; i < n && ngot < MAX_EV; i++) {
        ev_t *e = &got[ngot++];
        memset(e, 0, sizeof *e);
        e->kind = ev[i].kind;
        e->line = ev[i].line;
        e->clk = ev[i].clk;
        e->dir = ev[i].dir;
        e->bits = ev[i].bits;
        e->value = ev[i].value;
        e->mV = ev[i].mV;
        e->ns = ev[i].ns;
        e->t_us = ev[i].t_us;
    }
}

static int compare(const char *file)
{
    nwant = merge(want, nwant);
    ngot = merge(got, ngot);
    for (int i = 0; i < nwant; i++) {
        if (i >= ngot) {
            fprintf(stderr, "%s: event %d missing (kind %d)\n", file, i, want[i].kind);
            return 0;
        }
        const ev_t *w = &want[i], *g = &got[i];
        int ok = w->kind == g->kind;
        switch (w->kind) {
        case FEV_VDD:
        case FEV_VPP:
            ok = ok && w->mV == g->mV;
            if (ok && w->min_us && i + 1 < ngot)
                ok = got[i + 1].t_us - g->t_us >= w->min_us;
            if (ok && w->gap_min_us) {   /* the pause since the supplies were last switched off (the previous leave) */
                int j = i - 1;
                while (j >= 0 && !((got[j].kind == FEV_VDD || got[j].kind == FEV_VPP) && got[j].mV == 0))
                    j--;
                ok = j >= 0 && g->t_us - got[j].t_us >= w->gap_min_us;
            }
            break;
        case FEV_MODE: ok = ok && w->line == g->line && w->dir == g->dir; break;
        case FEV_SEND: ok = ok && w->line == g->line && w->clk == g->clk && w->bits == g->bits && w->value == g->value; break;
        case FEV_RECV: ok = ok && w->line == g->line && w->clk == g->clk && w->bits == g->bits; break;
        case FEV_CLOCK:
        case FEV_DELAY: ok = ok && g->ns >= w->min_ns; break;
        default: break;
        }
        if (!ok) {
            fprintf(stderr, "%s: event %d differs: want kind %d line %d dir %d bits %d value %lld mV %u min %u/%u us; "
                            "got kind %d line %d dir %d bits %d value %lld mV %u ns %u\n",
                    file, i, w->kind, w->line, w->dir, w->bits, w->value, w->mV, w->min_us, w->gap_min_us, g->kind,
                    g->line, g->dir, g->bits, g->value, g->mV, g->ns);
            return 0;
        }
    }
    if (ngot != nwant) {
        fprintf(stderr, "%s: %d extra events (first kind %d)\n", file, ngot - nwant, got[nwant].kind);
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- replay */

static void sink(void *ctx, uint16_t addr, uint16_t word) { (void)ctx, (void)addr, (void)word; }

static int replay(const char *file)
{
    pdk_test_setup();
    load(file);
    const chip_info_t *c = NULL;
    for (size_t i = 0; i < chip_count(); i++)
        if (chip_name_matches(chip_at(i), chip_name))
            c = chip_at(i);
    if (!c) {
        fprintf(stderr, "%s: unknown chip %s\n", file, chip_name);
        return 0;
    }
    fake_ic_insert(c);
    for (unsigned a = 0; a < c->code_words; a++)   /* the generator's memory pattern */
        fake_ic_mem()[a] = (uint16_t)((a * 0x1235u + 7u) & chip_word_mask(c));
    if (in_answer >= 0)
        fake_ic_set_id((uint16_t)in_answer);
    if (!strcmp(operation, "probe")) {
        uint16_t id;
        uint8_t fam;
        chip_engine_probe(&id, &fam, NULL);
    } else if (!strcmp(operation, "read")) {
        if (chip_engine_read(c, (uint16_t)in_addr, (uint16_t)in_count, sink, NULL, NULL) == PDK_OK)
            chip_engine_safe_state();   /* the end of the operation; a failed read has left already */
    } else if (!strcmp(operation, "erase")) {
        chip_engine_erase(c, NULL);
    } else if (!strcmp(operation, "write")) {
        for (int i = 0; i < n_addrs; i++) {   /* the generator's block: the memory pattern, the last word blank */
            uint16_t words[16];
            for (unsigned j = 0; j < c->write_block; j++)
                words[j] = (uint16_t)(((in_addrs[i] + j) * 0x1235u + 7u) & chip_word_mask(c));
            words[c->write_block - 1] = chip_word_mask(c);
            chip_engine_write_block(c, (uint16_t)in_addrs[i], words, c->write_block, NULL);
        }
        chip_engine_safe_state();
    } else {
        fprintf(stderr, "%s: unknown operation %s\n", file, operation);
        return 0;
    }
    record();
    return compare(file);
}

#define TRACE(name_)                                    \
    TEST(name_) { ASSERT_TRUE(replay(#name_ ".json")); }

TRACE(flash1_probe)
TRACE(flash1_read)
TRACE(flash2_probe)
TRACE(flash2_read)
TRACE(otp12_probe)
TRACE(otp12_read)
TRACE(otp21_probe)
TRACE(otp21_read)
TRACE(otp22_probe)
TRACE(otp22_read)
TRACE(otp31_probe)
TRACE(otp31_read)
TRACE(flash1_erase)
TRACE(flash1_write_block)
TRACE(flash1_fuse)
TRACE(flash2_erase)
TRACE(flash2_write_block)
TRACE(flash2_fuse)
TRACE(safety_flash_wrong_ack)
TRACE(safety_otp_wrong_id)
