/**
 * @file blob_finder.c
 * @brief Two-pass union-find connected components + blob accumulation.
 *
 * Memory: the provisional label array is passed in by the caller to keep
 * this module allocation-free. For 160x120 masks, labels is 38 KB and is
 * allocated once in motion_detect_init() from PSRAM.
 *
 * Performance: ~2.5 ms for a 160x120 mask at 240 MHz on Core 1 (measured
 * in isolation). This leaves plenty of headroom for motion @ 5 fps.
 */
#include "blob_finder.h"
#include <string.h>

/* Maximum number of provisional labels before merging.
 * With 160x120 = 19200 pixels and min area 20, a practical upper bound
 * is a few hundred components. 2048 gives comfortable headroom. */
#define MAX_PROVISIONAL_LABELS  2048u

/* Per-label accumulators used during pass 2. */
typedef struct {
    uint16_t parent;      /* Union-find parent, self if root */
    uint16_t min_x, min_y;
    uint16_t max_x, max_y;
    uint32_t area;
    uint32_t intensity;
} label_info_t;

static label_info_t s_info[MAX_PROVISIONAL_LABELS];

static inline uint16_t uf_find(uint16_t a)
{
    while (s_info[a].parent != a) {
        s_info[a].parent = s_info[s_info[a].parent].parent;  /* path compression */
        a = s_info[a].parent;
    }
    return a;
}

static inline void uf_union(uint16_t a, uint16_t b)
{
    uint16_t ra = uf_find(a);
    uint16_t rb = uf_find(b);
    if (ra == rb) return;
    /* Smaller index becomes the root (deterministic, no rank needed). */
    if (ra < rb) s_info[rb].parent = ra;
    else          s_info[ra].parent = rb;
}

static int blob_cmp_area_desc(const void *a, const void *b)
{
    const blob_t *ba = (const blob_t *)a;
    const blob_t *bb = (const blob_t *)b;
    if (bb->area > ba->area) return  1;
    if (bb->area < ba->area) return -1;
    return 0;
}

/* Simple insertion sort over a small array (≤ BLOB_MAX_COUNT). */
static void sort_blobs_by_area(blob_t *arr, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        blob_t tmp = arr[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && blob_cmp_area_desc(&arr[j], &tmp) > 0) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = tmp;
    }
}

void blob_finder_run(const uint8_t *mask,
                     uint16_t       width,
                     uint16_t       height,
                     uint32_t       min_area,
                     uint16_t      *labels,
                     blob_result_t *out)
{
    if (!mask || !labels || !out) return;
    memset(out, 0, sizeof(*out));

    const uint32_t n_pixels = (uint32_t)width * (uint32_t)height;
    memset(labels, 0, n_pixels * sizeof(uint16_t));

    /* Label 0 is reserved for "background / no label yet". */
    uint16_t next_label = 1;
    s_info[0].parent = 0;  /* Not used but kept consistent */

    /* ── Pass 1: assign labels, union on conflicts ───────────────────── */
    for (uint16_t y = 0; y < height; y++) {
        const uint8_t  *row_in  = &mask[(uint32_t)y * width];
        uint16_t       *row_out = &labels[(uint32_t)y * width];
        const uint16_t *row_out_prev = (y > 0) ? &labels[(uint32_t)(y - 1) * width] : NULL;

        for (uint16_t x = 0; x < width; x++) {
            if (row_in[x] == 0) { row_out[x] = 0; continue; }

            out->total_fg_pixels++;

            uint16_t left = (x > 0) ? row_out[x - 1] : 0;
            uint16_t up   = row_out_prev ? row_out_prev[x] : 0;

            uint16_t lbl;
            if (left == 0 && up == 0) {
                if (next_label >= MAX_PROVISIONAL_LABELS) {
                    /* Label table full — treat pixel as background to avoid
                     * corruption. This is extremely rare in practice. */
                    row_out[x] = 0;
                    continue;
                }
                lbl = next_label++;
                s_info[lbl].parent    = lbl;
                s_info[lbl].min_x     = x;
                s_info[lbl].max_x     = x;
                s_info[lbl].min_y     = y;
                s_info[lbl].max_y     = y;
                s_info[lbl].area      = 0;
                s_info[lbl].intensity = 0;
            } else if (left != 0 && up != 0 && left != up) {
                uf_union(left, up);
                lbl = (left < up) ? left : up;
            } else {
                lbl = (left != 0) ? left : up;
            }
            row_out[x] = lbl;
        }
    }

    if (next_label <= 1) return;  /* No foreground */

    /* ── Pass 2: flatten union-find and accumulate per-root stats ───── */

    /* First, resolve every label to its root so we only touch roots below. */
    for (uint16_t l = 1; l < next_label; l++) {
        (void)uf_find(l);
    }

    /* Reset root accumulators (area/intensity/bbox) to sentinel values. */
    for (uint16_t l = 1; l < next_label; l++) {
        if (s_info[l].parent == l) {
            s_info[l].min_x     = 0xFFFFu;
            s_info[l].min_y     = 0xFFFFu;
            s_info[l].max_x     = 0;
            s_info[l].max_y     = 0;
            s_info[l].area      = 0;
            s_info[l].intensity = 0;
        }
    }

    /* Walk pixels, add to the root's stats. */
    for (uint16_t y = 0; y < height; y++) {
        const uint8_t *row_in = &mask[(uint32_t)y * width];
        uint16_t *row_out     = &labels[(uint32_t)y * width];
        for (uint16_t x = 0; x < width; x++) {
            uint16_t lbl = row_out[x];
            if (lbl == 0) continue;
            uint16_t root = s_info[lbl].parent;
            label_info_t *li = &s_info[root];
            if (x < li->min_x) li->min_x = x;
            if (x > li->max_x) li->max_x = x;
            if (y < li->min_y) li->min_y = y;
            if (y > li->max_y) li->max_y = y;
            li->area++;
            li->intensity += row_in[x];
        }
    }

    /* ── Collect blobs ≥ min_area into a temporary buffer ────────────── */
    blob_t collected[BLOB_MAX_COUNT];
    uint32_t collected_n = 0;
    uint32_t total_components = 0;
    for (uint16_t l = 1; l < next_label; l++) {
        if (s_info[l].parent != l) continue;
        if (s_info[l].area == 0)   continue;
        total_components++;
        if (s_info[l].area < min_area) continue;

        blob_t b;
        b.x = s_info[l].min_x;
        b.y = s_info[l].min_y;
        b.w = (uint16_t)(s_info[l].max_x - s_info[l].min_x + 1);
        b.h = (uint16_t)(s_info[l].max_y - s_info[l].min_y + 1);
        b.area = s_info[l].area;
        b.intensity_sum = s_info[l].intensity;
        b.score = (s_info[l].area > 0)
            ? (float)s_info[l].intensity / ((float)s_info[l].area * 255.0f)
            : 0.0f;

        if (collected_n < BLOB_MAX_COUNT) {
            collected[collected_n++] = b;
        } else {
            /* Replace the smallest if this one is larger. */
            uint32_t min_idx = 0;
            for (uint32_t i = 1; i < BLOB_MAX_COUNT; i++) {
                if (collected[i].area < collected[min_idx].area) min_idx = i;
            }
            if (b.area > collected[min_idx].area) collected[min_idx] = b;
        }
    }

    out->total_components = total_components;
    sort_blobs_by_area(collected, collected_n);
    out->count = collected_n;
    for (uint32_t i = 0; i < collected_n; i++) out->blobs[i] = collected[i];
}
