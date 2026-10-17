/* Experimental bitwise context-mixing coder for block mode 28 (EXP-0079).
 * Included after the arithmetic primitives in bite.c. Each byte is coded as
 * eight binary decisions. Hashed order-1..6, word, column and sparse contexts
 * keep nonstationary bit-history states; adaptive maps turn states into
 * probabilities, a match model predicts the byte after the longest recent
 * repeat, and a logistic mixer with context-selected weights combines all
 * inputs before two adaptive probability maps refine the result. Every
 * context is reconstructed from bytes already decoded in the same block,
 * and all arithmetic is integer-only, so encoder and decoder stay identical.
 */

#ifndef CM_STATE_CAP
#define CM_STATE_CAP 30
#endif
#ifndef CM_STATE_DISCOUNT
#define CM_STATE_DISCOUNT 2
#endif
#ifndef CM_MAP_LIMIT
#define CM_MAP_LIMIT 1023u
#endif
#ifndef CM_MIXER_SHIFT
#define CM_MIXER_SHIFT 10
#endif
#ifndef CM_MIXER_INIT
#define CM_MIXER_INIT 19661
#endif
#ifndef CM_APM_RATE
#define CM_APM_RATE 7
#endif
#ifndef CM_MATCH_MIN
#define CM_MATCH_MIN 6u
#endif
#ifndef CM_TABLE_MAX_BITS
#define CM_TABLE_MAX_BITS 24u
#endif

#ifndef CM_MIXERS
#define CM_MIXERS 4u
#endif
#ifndef CM_FINAL_SHIFT
#define CM_FINAL_SHIFT 12
#endif

#ifndef CM_LR_MIN
#define CM_LR_MIN 8
#endif
#ifndef CM_LR_BOOST
#define CM_LR_BOOST 16
#endif
#ifndef CM_LR_HALF
#define CM_LR_HALF 4096u
#endif

#ifndef CM_LINEAR
#define CM_LINEAR 1
#endif
#ifndef CM_LINEAR_SHIFT
#define CM_LINEAR_SHIFT 2
#endif

#ifndef CM_EXTRA1
#define CM_EXTRA1 3
#endif
#ifndef CM_EXTRA2
#define CM_EXTRA2 5
#endif
#ifndef CM_EXTRA3
#define CM_EXTRA3 9
#endif
#ifndef CM_EXTRA4
#define CM_EXTRA4 7
#endif
#ifndef CM_EXTRA5
#define CM_EXTRA5 6
#endif
#ifndef CM_W_MIX
#define CM_W_MIX 2
#endif
#ifndef CM_W_APM0
#define CM_W_APM0 1
#endif
#ifndef CM_W_APM1
#define CM_W_APM1 1
#endif
#ifndef CM_W_APM3
#define CM_W_APM3 1
#endif
#ifndef CM_P_MIN
#define CM_P_MIN 16
#endif
#define CM_CONTEXTS (9u + (CM_EXTRA1 != 0) + (CM_EXTRA2 != 0) + (CM_EXTRA3 != 0) + \
                     (CM_EXTRA4 != 0) + (CM_EXTRA5 != 0))
#define CM_MATCH_INPUT (CM_CONTEXTS * 2u)
#define CM_INPUTS (CM_MATCH_INPUT + 3u)
#define CM_MATCH_MAX 65535u

typedef struct {
    uint8_t next[256][2];
    uint8_t counts[256][2];
} CmStateTable;

typedef struct {
    uint8_t *table;
    size_t mask;
    uint8_t *bucket;
    uint32_t hash;
} CmContext;

typedef struct {
    CmStateTable states;
    const uint8_t *history;
    size_t pos;
    unsigned c0;
    unsigned bit_count;
    unsigned nibble;
    uint32_t c4;
    uint32_t c8;
    uint32_t word;
    uint16_t indirect[65536];
    uint32_t previous_word;
    size_t line_start;
    size_t previous_line_start;
    CmContext contexts[CM_CONTEXTS];
    uint32_t state_maps[CM_CONTEXTS][256];
    uint32_t *match_table;
    size_t match_mask;
    size_t match_ptr;
    size_t match_length;
    unsigned match_byte;
    uint32_t match_map[64 * 2];
    unsigned match_index;
    int32_t *weights[CM_MIXERS];
    unsigned weight_set[CM_MIXERS];
    int layer_stretch[CM_MIXERS + 1u];
    int layer_probability[CM_MIXERS];
    int32_t *final_weights;
    unsigned final_set;
    int inputs[CM_INPUTS];
    int mixer_stretch;
    int mixer_probability;
    uint16_t *apm_order0;
    uint16_t *apm_order1;
    size_t apm0_index;
    size_t apm1_index;
    uint16_t *apm_match;
    size_t apm3_index;
    int probability;
} CmModel;

static const int cm_squash_points[33] = {
    1, 2, 4, 6, 10, 17, 27, 45, 74, 120, 194, 311, 488, 747, 1102, 1546,
    2048, 2550, 2994, 3349, 3608, 3785, 3902, 3976, 4022, 4051, 4069, 4079,
    4086, 4090, 4092, 4094, 4095
};

static int cm_stretch_table[4096];
static int cm_stretch_ready;

/* Logistic function on the 12-bit stretch domain, -2047..2047 -> 1..4095. */
static int cm_squash(int d) {
    if (d > 2047) d = 2047;
    if (d < -2047) d = -2047;
    unsigned offset = (unsigned)(d + 2048);
    unsigned index = offset >> 7u;
    unsigned weight = offset & 127u;
    int value = (cm_squash_points[index] * (int)(128u - weight) +
                 cm_squash_points[index + 1u] * (int)weight + 64) >> 7;
    return value < 1 ? 1 : (value > 4095 ? 4095 : value);
}

static void cm_init_stretch(void) {
    if (cm_stretch_ready) return;
    int pi = 0;
    for (int x = -2047; x <= 2047; ++x) {
        int v = cm_squash(x);
        for (int j = pi; j <= v; ++j) cm_stretch_table[j] = x;
        pi = v + 1;
    }
    for (int j = pi; j < 4096; ++j) cm_stretch_table[j] = 2047;
    cm_stretch_ready = 1;
}

static int cm_stretch(int p) { return cm_stretch_table[p]; }

/* Floor division by 2^shift without relying on signed right shifts. */
static int64_t cm_floor_shift(int64_t value, unsigned shift) {
    if (value >= 0) return value >> shift;
    return -(int64_t)(((uint64_t)(-(value + 1))) >> shift) - 1;
}

/* Bounded (zeros, ones) bit histories: the opposite count is discounted. */
static void cm_build_states(CmStateTable *table) {
    int ids[CM_STATE_CAP + 1][CM_STATE_CAP + 1];
    unsigned queue[256][2];
    for (unsigned a = 0; a <= CM_STATE_CAP; ++a)
        for (unsigned b = 0; b <= CM_STATE_CAP; ++b) ids[a][b] = -1;
    unsigned head = 0, tail = 1;
    queue[0][0] = 0; queue[0][1] = 0; ids[0][0] = 0;
    while (head < tail) {
        unsigned n[2] = {queue[head][0], queue[head][1]};
        unsigned id = head++;
        table->counts[id][0] = (uint8_t)n[0];
        table->counts[id][1] = (uint8_t)n[1];
        for (unsigned y = 0; y < 2; ++y) {
            unsigned m[2] = {n[0], n[1]};
            if (m[y] < CM_STATE_CAP) ++m[y];
            if (m[1u - y] > CM_STATE_DISCOUNT)
                m[1u - y] = (m[1u - y] + CM_STATE_DISCOUNT) / 2u;
            if (ids[m[0]][m[1]] < 0) {
                if (tail >= 256) fail("context-mix state table overflow");
                ids[m[0]][m[1]] = (int)tail;
                queue[tail][0] = m[0];
                queue[tail][1] = m[1];
                ++tail;
            }
            table->next[id][y] = (uint8_t)ids[m[0]][m[1]];
        }
    }
    for (unsigned id = tail; id < 256; ++id) {
        table->next[id][0] = table->next[id][1] = 0;
        table->counts[id][0] = table->counts[id][1] = 0;
    }
}

static uint32_t cm_map_initial(unsigned zeros, unsigned ones) {
    uint32_t p = (uint32_t)(((uint64_t)(2u * ones + 1u) << 22u) /
                            (2u * (zeros + ones) + 2u));
    return p << 10u;
}

static int cm_map_p(const uint32_t *map) { return (int)(*map >> 20u); }

static void cm_map_update(uint32_t *map, unsigned bit, unsigned limit) {
    uint32_t count = *map & 1023u;
    uint32_t p = *map >> 10u;
    uint32_t target = bit ? (1u << 22u) - 1u : 0u;
    uint64_t reciprocal = 131072u / (2u * count + 3u);
    if (target > p) p += (uint32_t)(((uint64_t)(target - p) * reciprocal) >> 16u);
    else p -= (uint32_t)(((uint64_t)(p - target) * reciprocal) >> 16u);
    if (count < limit) ++count;
    *map = (p << 10u) | count;
}

static uint32_t cm_hash(uint32_t value, uint32_t salt) {
    uint32_t h = (value + salt * 0x9e3779b1u) * 0x85ebca6bu;
    h ^= h >> 15u;
    h *= 0xc2b2ae35u;
    h ^= h >> 13u;
    return h;
}

static unsigned cm_table_bits(size_t size) {
    unsigned bits = 16;
    while (bits < CM_TABLE_MAX_BITS && ((size_t)1 << bits) < size * 2u) ++bits;
    return bits;
}

static void cm_model_init(CmModel *model, const uint8_t *history, size_t size) {
    memset(model, 0, sizeof(*model));
    cm_init_stretch();
    cm_build_states(&model->states);
    model->history = history;
    model->c0 = 1;
    model->nibble = 1;
    unsigned bits = cm_table_bits(size);
    for (unsigned i = 0; i < CM_CONTEXTS; ++i) {
        size_t table_size = (size_t)1 << (i < 2 ? (bits < 20 ? bits : 20) : bits);
        model->contexts[i].table = (uint8_t *)calloc(table_size, 1);
        if (!model->contexts[i].table) fail("out of memory");
        model->contexts[i].mask = table_size / 16u - 1u;
        for (unsigned s = 0; s < 256; ++s)
            model->state_maps[i][s] = cm_map_initial(model->states.counts[s][0],
                                                     model->states.counts[s][1]);
    }
    size_t match_size = (size_t)1 << (bits - 2u);
    model->match_table = (uint32_t *)calloc(match_size, sizeof(*model->match_table));
    if (!model->match_table) fail("out of memory");
    model->match_mask = match_size - 1u;
    for (unsigned i = 0; i < 128; ++i) model->match_map[i] = 1u << 31u;
    for (unsigned m = 0; m < CM_MIXERS; ++m) {
        model->weights[m] = (int32_t *)malloc(256u * CM_INPUTS * sizeof(int32_t));
        if (!model->weights[m]) fail("out of memory");
        for (unsigned i = 0; i < 256u * CM_INPUTS; ++i) model->weights[m][i] = CM_MIXER_INIT;
    }
    model->final_weights = (int32_t *)malloc(256u * (CM_MIXERS + 1u) * sizeof(int32_t));
    if (!model->final_weights) fail("out of memory");
    for (unsigned i = 0; i < 256u * (CM_MIXERS + 1u); ++i)
        model->final_weights[i] = i % (CM_MIXERS + 1u) == CM_MIXERS
            ? 0 : (int32_t)(65536u / CM_MIXERS);
    model->apm_order0 = (uint16_t *)malloc(256u * 33u * sizeof(uint16_t));
    model->apm_order1 = (uint16_t *)malloc(65536u * 33u * sizeof(uint16_t));
    model->apm_match = (uint16_t *)malloc(128u * 256u * 33u * sizeof(uint16_t));
    if (!model->apm_order0 || !model->apm_order1 || !model->apm_match)
        fail("out of memory");
    for (unsigned c = 0; c < 65536u; ++c) {
        for (unsigned j = 0; j < 33; ++j) {
            uint16_t value = (uint16_t)(cm_squash(((int)j - 16) * 128) * 16);
            if (c < 256u) model->apm_order0[c * 33u + j] = value;
            model->apm_order1[c * 33u + j] = value;
            if (c < 32768u) model->apm_match[c * 33u + j] = value;
        }
    }
}

static void cm_model_free(CmModel *model) {
    for (unsigned i = 0; i < CM_CONTEXTS; ++i) free(model->contexts[i].table);
    free(model->match_table);
    for (unsigned m = 0; m < CM_MIXERS; ++m) free(model->weights[m]);
    free(model->final_weights);
    free(model->apm_order0);
    free(model->apm_order1);
    free(model->apm_match);
}

static uint8_t *cm_find_bucket(CmContext *context, uint32_t hash,
                               const CmStateTable *states) {
    size_t index = (hash >> 8u) & context->mask;
    uint8_t check = (uint8_t)hash;
    uint8_t *first = context->table + index * 16u;
    if (first[0] == check) return first;
    uint8_t *second = context->table + (index ^ 1u) * 16u;
    if (second[0] == check) return second;
    unsigned first_priority = states->counts[first[1]][0] + states->counts[first[1]][1];
    unsigned second_priority = states->counts[second[1]][0] + states->counts[second[1]][1];
    uint8_t *victim = second_priority < first_priority ? second : first;
    memset(victim, 0, 16);
    victim[0] = check;
    return victim;
}

static void cm_select_buckets(CmModel *model) {
    for (unsigned i = 0; i < CM_CONTEXTS; ++i) {
        uint32_t hash = cm_hash(model->contexts[i].hash ^ (model->c0 * 0x2f0b3c2du), i + 1u);
        model->contexts[i].bucket = cm_find_bucket(&model->contexts[i], hash, &model->states);
    }
}

static uint32_t cm_extra_hash(const CmModel *model, int kind, unsigned above) {
    uint32_t c4 = model->c4, c8 = model->c8;
    switch (kind) {
    case 1: return cm_hash(c4 ^ ((c8 & 0xffu) * 0x3d4d51cbu), 101);              /* order 5 */
    case 2: return cm_hash(c4 ^ cm_hash(c8, 77), 102);                        /* order 8 */
    case 3: return cm_hash((c4 >> 8u) & 0xffu, 103);                          /* skip 1 */
    case 4: return cm_hash(c4 & 0xff00ff00u, 104);                            /* bytes 2,4 */
    case 5: return cm_hash(model->word ^ ((c4 & 0xffu) << 24u), 105);        /* word+o1 */
    case 6: return cm_hash(model->indirect[c4 & 0xffffu] | ((c4 & 0xffu) << 16u), 106);
    case 7: return cm_hash((c4 >> 16u) & 0xffffu, 107);                       /* bytes 3,4 */
    case 8: return cm_hash(above | ((c4 & 0xffffu) << 8u), 108);              /* above+o2 */
    case 9: return cm_hash(c8 & 0xffffffffu, 109);                            /* bytes 5..8 */
    case 10: return cm_hash(model->indirect[c4 & 0xffffu] | ((c4 & 0xffffu) << 16u), 110);
    default: return 0;
    }
}

static void cm_byte_update(CmModel *model) {
    const uint8_t *h = model->history;
    size_t pos = model->pos;
    unsigned byte = h[pos - 1u];
    model->c8 = (model->c8 << 8u) | (model->c4 >> 24u);

    model->c4 = (model->c4 << 8u) | byte;
    unsigned lower = byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
    if ((lower >= 'a' && lower <= 'z') || lower >= 128u) {
        model->word = (model->word ^ lower) * 0x01000193u;
    } else if (model->word) {
        model->previous_word = model->word;
        model->word = 0;
    }
    if (byte == '\n') {
        model->previous_line_start = model->line_start;
        model->line_start = pos;
    }
    size_t column = pos - model->line_start;
    unsigned above = 0;
    if (model->previous_line_start + column < model->line_start)
        above = h[model->previous_line_start + column];
    uint32_t c4 = model->c4;
    model->contexts[0].hash = 0;
    model->contexts[1].hash = c4 & 0xffu;
    model->contexts[2].hash = cm_hash(c4 & 0xffffu, 2);
    model->contexts[3].hash = cm_hash(c4 & 0xffffffu, 3);
    model->contexts[4].hash = cm_hash(c4, 4);
    model->contexts[5].hash = cm_hash(c4 ^ cm_hash(model->c8 & 0xffffu, 55), 5);
    model->contexts[6].hash = cm_hash(model->word ^ cm_hash(model->previous_word, 66), 6);
    model->contexts[7].hash = cm_hash((uint32_t)(column > 255 ? 255 : column) |
                                      (above << 8u) | ((c4 & 0xffu) << 16u), 7);
    model->contexts[8].hash = cm_hash((c4 & 0xffff0000u) | ((model->c8 & 0xffu) << 8u), 8);
    unsigned extra = 9;
    if (CM_EXTRA1) model->contexts[extra++].hash = cm_extra_hash(model, CM_EXTRA1, above);
    if (CM_EXTRA2) model->contexts[extra++].hash = cm_extra_hash(model, CM_EXTRA2, above);
    if (CM_EXTRA3) model->contexts[extra++].hash = cm_extra_hash(model, CM_EXTRA3, above);
    if (CM_EXTRA4) model->contexts[extra++].hash = cm_extra_hash(model, CM_EXTRA4, above);
    if (CM_EXTRA5) model->contexts[extra++].hash = cm_extra_hash(model, CM_EXTRA5, above);
    {
        unsigned key = (model->c4 >> 8u) & 0xffffu;
        model->indirect[key] = (uint16_t)((model->indirect[key] << 8u) | byte);
    }

    if (model->match_length && model->match_byte == byte) {
        ++model->match_ptr;
        if (model->match_length < CM_MATCH_MAX) ++model->match_length;
    } else {
        model->match_length = 0;
    }
    if (pos >= CM_MATCH_MIN) {
        uint32_t key = 0;
        for (unsigned i = 0; i < CM_MATCH_MIN; ++i) key = (key + h[pos - 1u - i] + 1u) * 0x2c9277b5u;
        size_t slot = (key >> 7u) & model->match_mask;
        if (!model->match_length) {
            size_t candidate = model->match_table[slot];
            if (candidate) {
                size_t length = 0;
                while (length < CM_MATCH_MAX && length < candidate &&
                       h[candidate - 1u - length] == h[pos - 1u - length])
                    ++length;
                if (length >= CM_MATCH_MIN) {
                    model->match_length = length;
                    model->match_ptr = candidate;
                }
            }
        }
        model->match_table[slot] = (uint32_t)pos;
    }
    model->match_byte = model->match_length ? h[model->match_ptr] : 0u;
}

static unsigned cm_length_bucket(size_t length) {
    if (length < 16u) return (unsigned)length;
    if (length < 32u) return 16u + (unsigned)((length - 16u) >> 1u);
    if (length < 64u) return 24u + (unsigned)((length - 32u) >> 2u);
    if (length < 512u) return 32u + (unsigned)((length - 64u) >> 4u);
    return 63u;
}

static int cm_predict(CmModel *model) {
    int *x = model->inputs;
    for (unsigned i = 0; i < CM_CONTEXTS; ++i) {
        uint8_t state = model->contexts[i].bucket[model->nibble];
        int p = cm_map_p(&model->state_maps[i][state]);
        x[2u * i] = cm_stretch(p);
        x[2u * i + 1u] = CM_LINEAR ? (int)cm_floor_shift(p - 2048, CM_LINEAR_SHIFT) : 0;
    }
    unsigned expected = 0;
    if (model->match_length) {
        unsigned shift = 7u - model->bit_count;
        if (((model->match_byte | 256u) >> (shift + 1u)) == model->c0) {
            expected = (model->match_byte >> shift) & 1u;
            model->match_index = cm_length_bucket(model->match_length) * 2u + expected;
        } else {
            model->match_length = 0;
            model->match_index = 0;
        }
    } else {
        model->match_index = 0;
    }
    if (model->match_index) {
        int st = cm_stretch(cm_map_p(&model->match_map[model->match_index]));
        x[CM_MATCH_INPUT] = st;
        int bucket = (int)cm_length_bucket(model->match_length);
        x[CM_MATCH_INPUT + 1u] = expected ? bucket * 32 : -bucket * 32;
    } else {
        x[CM_MATCH_INPUT] = 0;
        x[CM_MATCH_INPUT + 1u] = 0;
    }
    x[CM_INPUTS - 1u] = 256;
    unsigned length_set = model->match_index
        ? 1u + (cm_length_bucket(model->match_length) >> 2u) : 0u;
    model->weight_set[0] = model->c0;
    if (CM_MIXERS > 1u) model->weight_set[1] = (length_set & 15u) * 16u +
                                               model->bit_count * 2u + expected;
    if (CM_MIXERS > 2u) model->weight_set[2] = model->c4 & 0xffu;
    if (CM_MIXERS > 3u) model->weight_set[3] = (model->c4 >> 8u) & 0xffu;
    for (unsigned m = 0; m < CM_MIXERS; ++m) {
        const int32_t *w = model->weights[m] + (size_t)model->weight_set[m] * CM_INPUTS;
        int64_t dot = 0;
        for (unsigned i = 0; i < CM_INPUTS; ++i) dot += (int64_t)w[i] * x[i];
        int64_t mixed = cm_floor_shift(dot, 16);
        if (mixed > 2047) mixed = 2047;
        if (mixed < -2047) mixed = -2047;
        model->layer_stretch[m] = (int)mixed;
        model->layer_probability[m] = cm_squash((int)mixed);
    }
    model->layer_stretch[CM_MIXERS] = 256;
    model->final_set = model->c0;
    {
        const int32_t *w = model->final_weights + (size_t)model->final_set * (CM_MIXERS + 1u);
        int64_t dot = 0;
        for (unsigned m = 0; m <= CM_MIXERS; ++m) dot += (int64_t)w[m] * model->layer_stretch[m];
        int64_t mixed = cm_floor_shift(dot, 16);
        if (mixed > 2047) mixed = 2047;
        if (mixed < -2047) mixed = -2047;
        model->mixer_stretch = (int)mixed;
        model->mixer_probability = cm_squash((int)mixed);
    }

    unsigned offset = (unsigned)(model->mixer_stretch + 2048);
    unsigned lo = offset >> 7u;
    unsigned weight = offset & 127u;
    size_t index0 = model->c0 * 33u + lo;
    size_t index1 = ((model->c4 & 0xffu) << 8u | model->c0) * 33u + lo;
    size_t index3 = (model->match_index * 256u + model->c0) * 33u + lo;
    int p0 = (model->apm_order0[index0] * (int)(128u - weight) +
              model->apm_order0[index0 + 1u] * (int)weight) >> 7;
    int p1 = (model->apm_order1[index1] * (int)(128u - weight) +
              model->apm_order1[index1 + 1u] * (int)weight) >> 7;
    int p3 = (model->apm_match[index3] * (int)(128u - weight) +
              model->apm_match[index3 + 1u] * (int)weight) >> 7;
    model->apm0_index = index0 + (weight >> 6u);
    model->apm1_index = index1 + (weight >> 6u);
    model->apm3_index = index3 + (weight >> 6u);
    int64_t sum = (int64_t)model->mixer_probability * 16 * CM_W_MIX + (int64_t)p0 * CM_W_APM0 +
                  (int64_t)p1 * CM_W_APM1 + (int64_t)p3 * CM_W_APM3;
    int p = (int)(sum / (CM_W_MIX + CM_W_APM0 + CM_W_APM1 + CM_W_APM3));
    if (p < CM_P_MIN) p = CM_P_MIN;
    if (p > 65536 - CM_P_MIN) p = 65536 - CM_P_MIN;
    model->probability = p;
    return p;
}

static void cm_apm_update(uint16_t *entry, unsigned bit) {
    unsigned target = bit ? 65535u : 0u;
    if (target > *entry) *entry = (uint16_t)(*entry + ((target - *entry) >> CM_APM_RATE));
    else *entry = (uint16_t)(*entry - ((*entry - target) >> CM_APM_RATE));
}

static void cm_update(CmModel *model, unsigned bit) {
    for (unsigned i = 0; i < CM_CONTEXTS; ++i) {
        uint8_t *slot = &model->contexts[i].bucket[model->nibble];
        cm_map_update(&model->state_maps[i][*slot], bit, CM_MAP_LIMIT);
        *slot = model->states.next[*slot][bit];
    }
    if (model->match_index)
        cm_map_update(&model->match_map[model->match_index], bit, CM_MAP_LIMIT);
    int rate = CM_LR_MIN + (int)((uint64_t)CM_LR_BOOST * CM_LR_HALF /
                                 (CM_LR_HALF + model->pos));
    for (unsigned m = 0; m < CM_MIXERS; ++m) {
        int error = ((int)(bit << 12u) - model->layer_probability[m]) * rate;
        int32_t *w = model->weights[m] + (size_t)model->weight_set[m] * CM_INPUTS;
        for (unsigned i = 0; i < CM_INPUTS; ++i)
            w[i] += (int32_t)cm_floor_shift((int64_t)model->inputs[i] * error,
                                            CM_MIXER_SHIFT + 3);
    }
    {
        int error = (int)(bit << 12u) - model->mixer_probability;
        int32_t *w = model->final_weights + (size_t)model->final_set * (CM_MIXERS + 1u);
        for (unsigned m = 0; m <= CM_MIXERS; ++m)
            w[m] += (int32_t)cm_floor_shift((int64_t)model->layer_stretch[m] * error,
                                            CM_FINAL_SHIFT);
    }
    cm_apm_update(&model->apm_order0[model->apm0_index], bit);
    cm_apm_update(&model->apm_order1[model->apm1_index], bit);
    cm_apm_update(&model->apm_match[model->apm3_index], bit);

    model->c0 = model->c0 * 2u + bit;
    model->nibble = model->nibble * 2u + bit;
    ++model->bit_count;
    if (model->bit_count == 8u) {
        model->c0 = 1;
        model->nibble = 1;
        model->bit_count = 0;
        ++model->pos;
        cm_byte_update(model);
        cm_select_buckets(model);
    } else if (model->bit_count == 4u) {
        model->nibble = 1;
        cm_select_buckets(model);
    }
}

static Buffer cm_encode(const uint8_t *input, size_t size) {
    Buffer output = {0};
    if (!size || size > WHOLE_BLOCK_SIZE) fail("invalid context-mix block size");
    reserve(&output, size / 2u + 16u);
    CmModel *model = (CmModel *)malloc(sizeof(*model));
    if (!model) fail("out of memory");
    cm_model_init(model, input, size);
    cm_select_buckets(model);
    uint32_t low = 0, high = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        for (int shift = 7; shift >= 0; --shift) {
            unsigned bit = (input[i] >> shift) & 1u;
            uint32_t p = (uint32_t)cm_predict(model);
            uint32_t mid = low + (uint32_t)(((uint64_t)(high - low) * p) >> 16u);
            if (bit) high = mid;
            else low = mid + 1u;
            cm_update(model, bit);
            while (((low ^ high) & 0xff000000u) == 0) {
                append_byte(&output, (uint8_t)(high >> 24u));
                low <<= 8u;
                high = (high << 8u) | 255u;
            }
        }
    }
    append_u32le(&output, 0);
    output.data[output.size - 4u] = (uint8_t)(low >> 24u);
    output.data[output.size - 3u] = (uint8_t)(low >> 16u);
    output.data[output.size - 2u] = (uint8_t)(low >> 8u);
    output.data[output.size - 1u] = (uint8_t)low;
    cm_model_free(model);
    free(model);
    return output;
}

static Buffer cm_decode(const uint8_t *input, size_t input_size, size_t size) {
    if (!size || size > WHOLE_BLOCK_SIZE) fail("invalid context-mix block size");
    if (input_size < 4u) fail("truncated context-mix stream");
    Buffer output = {0};
    reserve(&output, size);
    CmModel *model = (CmModel *)malloc(sizeof(*model));
    if (!model) fail("out of memory");
    cm_model_init(model, output.data, size);
    cm_select_buckets(model);
    uint32_t low = 0, high = UINT32_MAX, code = 0;
    size_t ip = 0;
    for (; ip < 4u; ++ip) code = (code << 8u) | input[ip];
    for (size_t i = 0; i < size; ++i) {
        unsigned byte = 0;
        for (unsigned b = 0; b < 8u; ++b) {
            uint32_t p = (uint32_t)cm_predict(model);
            uint32_t mid = low + (uint32_t)(((uint64_t)(high - low) * p) >> 16u);
            unsigned bit = code <= mid;
            if (bit) high = mid;
            else low = mid + 1u;
            byte = byte * 2u + bit;
            if (b == 7u) output.data[output.size++] = (uint8_t)byte;
            cm_update(model, bit);
            while (((low ^ high) & 0xff000000u) == 0) {
                if (ip >= input_size) fail("truncated context-mix stream");
                code = (code << 8u) | input[ip++];
                low <<= 8u;
                high = (high << 8u) | 255u;
            }
        }
    }
    if (ip != input_size) fail("trailing context-mix data");
    cm_model_free(model);
    free(model);
    return output;
}
