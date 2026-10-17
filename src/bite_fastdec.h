/* EXP-0078 block mode 27: fast-decode LZ with static per-block tANS tables.
 * Included in bite.c after the wide match index. Sequences are
 * (literal length, match length, offset) triples coded with zstd-style
 * value buckets plus raw extra bits; a four-entry recent-distance cache
 * provides repeat codes. Literals use either four interleaved order-0 tANS
 * streams or a clustered static context model keyed on preceding bytes.
 * The decoder loop contains no adaptive model updates.
 */

#define FD_LL_DIRECT 16u
#define FD_ML_DIRECT 32u
#define FD_OF_DIRECT 4u
#define FD_REPS 4u
#define FD_LL_CODES 62u
#define FD_ML_CODES 60u
#define FD_OF_CODES 52u
#define FD_MIN_LOG 5u
#define FD_LIT_MAX_LOG 11u
#define FD_SEQ_MAX_LOG 10u
#define FD_MAX_LOG 12u
#define FD_MAX_KEYS 1024u
#define FD_MAX_TABLES 64u
#define FD_MAX_ORDER 6u
#define FD_COPY_SLACK 32u
#define FD_OPT_NUM 4096u
#define FD_SUFFICIENT 512u
#define FD_PRICE_INF UINT64_MAX

static unsigned fd_highbit(uint32_t value) { return 31u - (unsigned)__builtin_clz(value); }

static uint64_t fd_load64(const uint8_t *p) {
    uint64_t value;
    memcpy(&value, p, sizeof(value));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap64(value);
#endif
    return value;
}

static void fd_copy16(uint8_t *dst, const uint8_t *src) { memcpy(dst, src, 16); }

static void fd_append_varint(Buffer *output, uint64_t value) {
    while (value >= 0x80u) {
        append_byte(output, (uint8_t)(value | 0x80u));
        value >>= 7;
    }
    append_byte(output, (uint8_t)value);
}

static uint64_t fd_read_varint(const uint8_t *input, size_t size, size_t *pos) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (*pos >= size) fail("truncated mode-27 varint");
        uint8_t byte = input[(*pos)++];
        value |= (uint64_t)(byte & 0x7fu) << shift;
        if (!(byte & 0x80u)) return value;
    }
    fail("invalid mode-27 varint");
    return 0;
}

/* Value buckets: direct codes, then two codes per power of two. */
static unsigned fd_value_code(uint32_t value, unsigned direct, unsigned *bits, uint32_t *extra) {
    if (value < direct) { *bits = 0; *extra = 0; return value; }
    uint32_t x = value - direct + 4u;
    unsigned high = fd_highbit(x);
    *bits = high - 1u;
    *extra = x & ((1u << (high - 1u)) - 1u);
    return direct + 2u * (high - 2u) + ((x >> (high - 1u)) & 1u);
}

static void fd_code_bases(unsigned direct, unsigned codes, uint32_t *base, uint8_t *bits) {
    for (unsigned code = 0; code < codes; ++code) {
        if (code < direct) { base[code] = code; bits[code] = 0; continue; }
        unsigned high = (code - direct) / 2u + 2u;
        uint32_t x = (uint32_t)(2u | ((code - direct) & 1u)) << (high - 1u);
        base[code] = x - 4u + direct;
        bits[code] = (uint8_t)(high - 1u);
    }
}

/* Deterministic fixed-point log2 with 16 fractional bits. */
static uint32_t *fd_log_cache;

static uint32_t fd_log2_q16_slow(uint32_t value) {
    unsigned exponent = fd_highbit(value);
    uint64_t normalized = (uint64_t)value << (31u - exponent);
    uint32_t fraction = 0;
    for (unsigned bit = 0; bit < 16; ++bit) {
        normalized = normalized * normalized >> 31u;
        if (normalized >= (UINT64_C(2) << 31u)) {
            normalized >>= 1u;
            fraction |= 1u << (15u - bit);
        }
    }
    return (uint32_t)(exponent << 16u) + fraction;
}

static uint32_t fd_log2_q16(uint32_t value) {
    if (!value) fail("log2 of zero");
    if (value < 65536u) {
        if (!fd_log_cache) {
            fd_log_cache = (uint32_t *)malloc(65536u * sizeof(*fd_log_cache));
            if (!fd_log_cache) fail("out of memory");
            fd_log_cache[0] = 0;
            for (uint32_t v = 1; v < 65536u; ++v) fd_log_cache[v] = fd_log2_q16_slow(v);
        }
        return fd_log_cache[value];
    }
    return fd_log2_q16_slow(value);
}

/* Forward LSB-first bit packing for table headers. */
typedef struct {
    Buffer *output;
    uint64_t bits;
    unsigned count;
} FdBitWriter;

static void fd_put(FdBitWriter *writer, uint32_t value, unsigned count) {
    if (!count) return;
    writer->bits |= (uint64_t)(value & (count == 32 ? 0xffffffffu : ((1u << count) - 1u)))
                    << writer->count;
    writer->count += count;
    while (writer->count >= 8) {
        append_byte(writer->output, (uint8_t)writer->bits);
        writer->bits >>= 8;
        writer->count -= 8;
    }
}

static void fd_put_finish(FdBitWriter *writer) {
    if (writer->count) append_byte(writer->output, (uint8_t)writer->bits);
    writer->bits = 0;
    writer->count = 0;
}

static void fd_put_golomb(FdBitWriter *writer, uint32_t value, unsigned k) {
    uint32_t q = (value >> k) + 1u;
    unsigned high = fd_highbit(q);
    fd_put(writer, 0, high);
    fd_put(writer, 1, 1);
    fd_put(writer, q, high);
    fd_put(writer, value, k);
}

static unsigned fd_golomb_bits(uint32_t value, unsigned k) {
    return 2u * fd_highbit((value >> k) + 1u) + 1u + k;
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t position;
} FdBitReader;

static uint32_t fd_get(FdBitReader *reader, unsigned count) {
    uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) {
        size_t byte = reader->position >> 3;
        if (byte >= reader->size) fail("truncated mode-27 table header");
        value |= (uint32_t)((reader->data[byte] >> (reader->position & 7u)) & 1u) << i;
        ++reader->position;
    }
    return value;
}

static uint32_t fd_get_golomb(FdBitReader *reader, unsigned k) {
    unsigned high = 0;
    while (!fd_get(reader, 1))
        if (++high > 24) fail("invalid mode-27 table header code");
    uint32_t q = (1u << high) | fd_get(reader, high);
    uint64_t value = ((uint64_t)(q - 1u) << k) | fd_get(reader, k);
    if (value > 0xffffffu) fail("invalid mode-27 table header value");
    return (uint32_t)value;
}

/* Backward bitstream: written LSB-first, read from the end. */
typedef struct {
    Buffer data;
    uint64_t bits;
    unsigned count;
} FdBackWriter;

static void fd_back_put(FdBackWriter *writer, uint32_t value, unsigned count) {
    if (!count) return;
    writer->bits |= (uint64_t)(value & ((1u << count) - 1u)) << writer->count;
    writer->count += count;
    while (writer->count >= 8) {
        append_byte(&writer->data, (uint8_t)writer->bits);
        writer->bits >>= 8;
        writer->count -= 8;
    }
}

static Buffer fd_back_finish(FdBackWriter *writer) {
    fd_back_put(writer, 1, 1);
    if (writer->count) append_byte(&writer->data, (uint8_t)writer->bits);
    Buffer output = writer->data;
    if (output.size < 8) {
        size_t pad = 8 - output.size;
        reserve(&output, pad);
        memmove(output.data + pad, output.data, output.size);
        memset(output.data, 0, pad);
        output.size += pad;
    }
    writer->data = (Buffer){0};
    return output;
}

typedef struct {
    uint64_t bits;
    unsigned consumed;
    const uint8_t *ptr;
    const uint8_t *start;
} FdBackReader;

static void fd_back_init(FdBackReader *reader, const uint8_t *data, size_t size) {
    if (size < 8 || !data[size - 1u]) fail("invalid mode-27 bitstream");
    reader->start = data;
    reader->ptr = data + size - 8u;
    reader->bits = fd_load64(reader->ptr);
    reader->consumed = (unsigned)__builtin_clzll(reader->bits) + 1u;
}

static inline uint32_t fd_back_get(FdBackReader *reader, unsigned count) {
    uint32_t value = (uint32_t)(((reader->bits << (reader->consumed & 63u)) >> 1) >>
                                ((63u - count) & 63u));
    reader->consumed += count;
    return value;
}

static inline void fd_back_reload(FdBackReader *reader) {
    if (reader->ptr >= reader->start + 8) {
        reader->ptr -= reader->consumed >> 3;
        reader->consumed &= 7u;
        reader->bits = fd_load64(reader->ptr);
        return;
    }
    if (reader->consumed > 64u) fail("mode-27 bitstream overrun");
    size_t step = reader->consumed >> 3;
    size_t available = (size_t)(reader->ptr - reader->start);
    if (step > available) step = available;
    reader->ptr -= step;
    reader->consumed -= (unsigned)(step * 8u);
    reader->bits = fd_load64(reader->ptr);
}

static void fd_back_check_end(FdBackReader *reader) {
    if (reader->consumed > 64u) fail("mode-27 bitstream overrun");
    fd_back_reload(reader);
    size_t remaining = (size_t)(reader->ptr - reader->start) * 8u + (64u - reader->consumed);
    if (remaining >= 64u) fail("unused mode-27 bitstream data");
    if (remaining && (reader->bits & ((UINT64_C(1) << remaining) - 1u)))
        fail("invalid mode-27 bitstream padding");
}

/* tANS tables. */
typedef struct {
    unsigned log;
    unsigned last;
    uint16_t norm[256];
} FdTable;

typedef struct {
    uint16_t base;
    uint8_t symbol;
    uint8_t bits;
} FdDecodeEntry;

typedef struct {
    unsigned log;
    uint16_t states[1u << FD_MAX_LOG];
    uint32_t delta_bits[256];
    int32_t delta_find[256];
} FdEncoder;

static void fd_spread(const FdTable *table, uint8_t *symbols) {
    uint32_t size = 1u << table->log, mask = size - 1u;
    uint32_t step = (size >> 1) + (size >> 3) + 3u, position = 0;
    for (unsigned symbol = 0; symbol <= table->last; ++symbol)
        for (unsigned i = 0; i < table->norm[symbol]; ++i) {
            symbols[position] = (uint8_t)symbol;
            position = (position + step) & mask;
        }
    if (position) fail("mode-27 table spread failed");
}

static void fd_build_decoder(const FdTable *table, FdDecodeEntry *entries) {
    uint8_t symbols[1u << FD_MAX_LOG];
    uint32_t next[256];
    uint32_t size = 1u << table->log;
    fd_spread(table, symbols);
    for (unsigned symbol = 0; symbol <= table->last; ++symbol) next[symbol] = table->norm[symbol];
    for (uint32_t u = 0; u < size; ++u) {
        unsigned symbol = symbols[u];
        uint32_t x = next[symbol]++;
        unsigned bits = table->log - fd_highbit(x);
        entries[u].base = (uint16_t)((x << bits) - size);
        entries[u].symbol = (uint8_t)symbol;
        entries[u].bits = (uint8_t)bits;
    }
}

static void fd_build_encoder(const FdTable *table, FdEncoder *encoder) {
    uint8_t symbols[1u << FD_MAX_LOG];
    uint32_t cumulative[257];
    uint32_t size = 1u << table->log;
    encoder->log = table->log;
    fd_spread(table, symbols);
    cumulative[0] = 0;
    for (unsigned symbol = 0; symbol < 256; ++symbol)
        cumulative[symbol + 1u] = cumulative[symbol] +
            (symbol <= table->last ? table->norm[symbol] : 0u);
    uint32_t work[256];
    memcpy(work, cumulative, sizeof(work));
    for (uint32_t u = 0; u < size; ++u)
        encoder->states[work[symbols[u]]++] = (uint16_t)(size + u);
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        uint32_t n = symbol <= table->last ? table->norm[symbol] : 0u;
        encoder->delta_bits[symbol] = 0;
        encoder->delta_find[symbol] = 0;
        if (n == 1) {
            encoder->delta_bits[symbol] = (table->log << 16) - size;
            encoder->delta_find[symbol] = (int32_t)cumulative[symbol] - 1;
        } else if (n > 1) {
            unsigned max_bits = table->log - fd_highbit(n - 1u);
            uint32_t min_state = n << max_bits;
            encoder->delta_bits[symbol] = (max_bits << 16) - min_state;
            encoder->delta_find[symbol] = (int32_t)cumulative[symbol] - (int32_t)n;
        }
    }
}

static inline void fd_encode_symbol(FdBackWriter *writer, const FdEncoder *encoder,
                                    uint32_t *state, unsigned symbol) {
    unsigned bits = (*state + encoder->delta_bits[symbol]) >> 16;
    fd_back_put(writer, *state, bits);
    *state = encoder->states[(int32_t)(*state >> bits) + encoder->delta_find[symbol]];
}

/* Normalization: proportional rounding, then first-order greedy correction. */
static void fd_normalize(const uint32_t *counts, unsigned symbols, unsigned log, FdTable *table) {
    uint64_t total = 0;
    unsigned last = 0;
    for (unsigned s = 0; s < symbols; ++s) {
        total += counts[s];
        if (counts[s]) last = s;
    }
    table->log = log;
    table->last = last;
    memset(table->norm, 0, sizeof(table->norm));
    uint32_t size = 1u << log;
    int64_t sum = 0;
    for (unsigned s = 0; s <= last; ++s) {
        if (!counts[s]) continue;
        uint64_t n = ((uint64_t)counts[s] * size + total / 2u) / total;
        if (!n) n = 1;
        table->norm[s] = (uint16_t)n;
        sum += (int64_t)n;
    }
    while (sum > (int64_t)size) {
        int best = -1;
        for (unsigned s = 0; s <= last; ++s) {
            if (table->norm[s] <= 1) continue;
            if (best < 0 ||
                (uint64_t)counts[s] * (2u * table->norm[best] - 1u) <
                (uint64_t)counts[best] * (2u * table->norm[s] - 1u))
                best = (int)s;
        }
        if (best < 0) fail("mode-27 normalization failed");
        --table->norm[best];
        --sum;
    }
    while (sum < (int64_t)size) {
        int best = -1;
        for (unsigned s = 0; s <= last; ++s) {
            if (!counts[s]) continue;
            if (best < 0 ||
                (uint64_t)counts[s] * (2u * table->norm[best] + 1u) >
                (uint64_t)counts[best] * (2u * table->norm[s] + 1u))
                best = (int)s;
        }
        ++table->norm[best];
        ++sum;
    }
}

static unsigned fd_header_k(const FdTable *table, uint64_t *bits_out) {
    uint64_t best_bits = UINT64_MAX;
    unsigned best_k = 0;
    for (unsigned k = 0; k < 8; ++k) {
        uint64_t bits = 3u + 8u + 3u;
        for (unsigned s = 0; s < table->last; ++s) {
            bits += fd_golomb_bits(table->norm[s], k);
            if (!table->norm[s]) {
                unsigned run = 0;
                while (s + 1u + run < table->last && !table->norm[s + 1u + run]) ++run;
                bits += fd_golomb_bits(run, 0);
                s += run;
            }
        }
        if (bits < best_bits) { best_bits = bits; best_k = k; }
    }
    if (bits_out) *bits_out = best_bits;
    return best_k;
}

static void fd_write_table(FdBitWriter *writer, const FdTable *table) {
    unsigned k = fd_header_k(table, NULL);
    fd_put(writer, table->log - FD_MIN_LOG, 3);
    fd_put(writer, table->last, 8);
    fd_put(writer, k, 3);
    for (unsigned s = 0; s < table->last; ++s) {
        fd_put_golomb(writer, table->norm[s], k);
        if (!table->norm[s]) {
            unsigned run = 0;
            while (s + 1u + run < table->last && !table->norm[s + 1u + run]) ++run;
            fd_put_golomb(writer, run, 0);
            s += run;
        }
    }
}

static void fd_read_table(FdBitReader *reader, FdTable *table, unsigned symbols,
                          unsigned max_log) {
    table->log = fd_get(reader, 3) + FD_MIN_LOG;
    table->last = fd_get(reader, 8);
    unsigned k = fd_get(reader, 3);
    if (table->log > max_log || table->last >= symbols) fail("invalid mode-27 table");
    memset(table->norm, 0, sizeof(table->norm));
    uint32_t size = 1u << table->log, sum = 0;
    for (unsigned s = 0; s < table->last; ++s) {
        uint32_t n = fd_get_golomb(reader, k);
        if (n > size - sum) fail("invalid mode-27 table counts");
        table->norm[s] = (uint16_t)n;
        sum += n;
        if (!n) {
            uint32_t run = fd_get_golomb(reader, 0);
            if (run > table->last - s - 1u) fail("invalid mode-27 zero run");
            s += run;
        }
    }
    if (sum >= size) fail("invalid mode-27 table total");
    table->norm[table->last] = (uint16_t)(size - sum);
}

/* Exact header bits plus Q16 coded bits for one normalized table. */
static uint64_t fd_table_cost(const uint32_t *counts, const FdTable *table) {
    uint64_t header;
    fd_header_k(table, &header);
    uint64_t cost = header << 16;
    for (unsigned s = 0; s <= table->last; ++s)
        if (counts[s])
            cost += (uint64_t)counts[s] *
                    ((table->log << 16) - fd_log2_q16(table->norm[s]));
    return cost;
}

static uint64_t fd_choose_table(const uint32_t *counts, unsigned symbols, unsigned max_log,
                                FdTable *table) {
    unsigned distinct = 0;
    uint64_t total = 0;
    for (unsigned s = 0; s < symbols; ++s) {
        if (counts[s]) ++distinct;
        total += counts[s];
    }
    unsigned min_log = FD_MIN_LOG;
    while ((1u << min_log) < distinct) ++min_log;
    if (min_log > max_log) fail("mode-27 alphabet too large");
    uint64_t best = UINT64_MAX;
    FdTable candidate;
    for (unsigned log = min_log; log <= max_log; ++log) {
        if (log > min_log && (1u << (log - 1u)) > total) break;
        fd_normalize(counts, symbols, log, &candidate);
        uint64_t cost = fd_table_cost(counts, &candidate);
        if (cost < best) { best = cost; *table = candidate; }
    }
    return best;
}

static void fd_table_prices(const FdTable *table, unsigned symbols, uint32_t *prices) {
    for (unsigned s = 0; s < symbols; ++s)
        prices[s] = s <= table->last && table->norm[s]
            ? ((table->log << 16) - fd_log2_q16(table->norm[s])) >> 8
            : (table->log + 2u) << 8;
}

/* Sequences produced by the parsers. */
typedef struct {
    uint32_t literal_length;
    uint32_t match_length;
    uint32_t distance;
} FdSequence;

typedef struct {
    FdSequence *items;
    size_t count;
    size_t capacity;
} FdSequences;

static void fd_push_sequence(FdSequences *list, uint32_t literal_length,
                             uint32_t match_length, uint32_t distance) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2u : 1024u;
        FdSequence *items = (FdSequence *)realloc(list->items, capacity * sizeof(*items));
        if (!items) fail("out of memory");
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = (FdSequence){literal_length, match_length, distance};
}

static unsigned fd_rep_code(const uint32_t reps[FD_REPS], uint32_t distance) {
    for (unsigned slot = 0; slot < FD_REPS; ++slot)
        if (reps[slot] == distance) return slot;
    return FD_REPS;
}

static void fd_rep_update(uint32_t reps[FD_REPS], uint32_t distance, unsigned code) {
    unsigned start = code < FD_REPS ? code : FD_REPS - 1u;
    for (unsigned slot = start; slot > 0; --slot) reps[slot] = reps[slot - 1u];
    reps[0] = distance;
}

static void fd_initial_reps(uint32_t reps[FD_REPS]) {
    reps[0] = 1; reps[1] = 4; reps[2] = 8; reps[3] = 16;
}

/* Price model exported by an encoding and consumed by the next parse. */
typedef struct {
    uint32_t *literal;      /* per block position, Q8 bits */
    uint32_t ll[FD_LL_CODES];
    uint32_t ml[FD_ML_CODES];
    uint32_t of[FD_OF_CODES];
} FdPrices;

/* Literal context keys from up to FD_MAX_ORDER preceding bytes. */
typedef struct {
    unsigned order;
    unsigned alphabet;
    uint8_t rank[256];
} FdKeyModel;

static inline unsigned fd_key_at(const FdKeyModel *model, const uint8_t *data, size_t pos) {
    unsigned key = 0;
    for (unsigned i = model->order; i > 0; --i)
        key = key * model->alphabet + (pos >= i ? model->rank[data[pos - i]] : model->rank[0]);
    return key;
}

typedef struct {
    Buffer section;
    uint8_t mode;
    uint64_t cost_bits;
} FdLiteralSection;

static void fd_encode_literals_order0(const uint8_t *literals, size_t count,
                                      const uint32_t *counts, FdLiteralSection *out,
                                      uint32_t *symbol_prices) {
    FdTable table;
    fd_choose_table(counts, 256, FD_LIT_MAX_LOG, &table);
    if (symbol_prices) fd_table_prices(&table, 256, symbol_prices);
    FdEncoder *encoder = (FdEncoder *)malloc(sizeof(*encoder));
    if (!encoder) fail("out of memory");
    fd_build_encoder(&table, encoder);
    Buffer streams[4];
    size_t quarter = (count + 3u) / 4u;
    for (unsigned stream = 0; stream < 4; ++stream) {
        size_t begin = quarter * stream < count ? quarter * stream : count;
        size_t end = begin + quarter < count ? begin + quarter : count;
        FdBackWriter writer = {0};
        uint32_t state = 1u << table.log;
        for (size_t i = end; i > begin; --i)
            fd_encode_symbol(&writer, encoder, &state, literals[i - 1u]);
        fd_back_put(&writer, state, table.log);
        streams[stream] = fd_back_finish(&writer);
    }
    free(encoder);
    Buffer section = {0};
    FdBitWriter header = {&section, 0, 0};
    fd_write_table(&header, &table);
    fd_put_finish(&header);
    for (unsigned stream = 0; stream < 3; ++stream)
        fd_append_varint(&section, streams[stream].size);
    for (unsigned stream = 0; stream < 4; ++stream) {
        append_bytes(&section, streams[stream].data, streams[stream].size);
        free(streams[stream].data);
    }
    out->section = section;
    out->mode = 1;
    out->cost_bits = (uint64_t)section.size * 8u;
}

/* Agglomerative clustering of key histograms, then k-means refinement. */
typedef struct {
    uint32_t *histogram;
    uint64_t total;
    uint64_t cost;
    int active;
} FdCluster;

static uint64_t fd_histogram_cost(const uint32_t *histogram, unsigned symbols, uint64_t total) {
    if (!total) return 0;
    uint64_t cost = 0;
    unsigned nonzero = 0;
    uint32_t total_log = total < 0xffffffffu ? fd_log2_q16((uint32_t)total)
                                             : fd_log2_q16((uint32_t)(total >> 8)) + (8u << 16);
    for (unsigned s = 0; s < symbols; ++s) {
        if (!histogram[s]) continue;
        ++nonzero;
        cost += (uint64_t)histogram[s] * (total_log - fd_log2_q16(histogram[s]));
    }
    return cost + ((uint64_t)(nonzero * 6u + 24u) << 16);
}

static uint64_t fd_merged_cost(const FdCluster *a, const FdCluster *b, unsigned symbols,
                               uint32_t *scratch) {
    for (unsigned s = 0; s < symbols; ++s) scratch[s] = a->histogram[s] + b->histogram[s];
    return fd_histogram_cost(scratch, symbols, a->total + b->total);
}

static unsigned fd_cluster_keys(const uint32_t *histograms, unsigned keys, unsigned symbols,
                                unsigned max_tables, uint8_t *map) {
    FdCluster *clusters = (FdCluster *)calloc(keys, sizeof(*clusters));
    uint32_t *storage = (uint32_t *)calloc((size_t)keys * symbols, sizeof(*storage));
    int64_t *best_delta = (int64_t *)malloc(keys * sizeof(*best_delta));
    int *best_partner = (int *)malloc(keys * sizeof(*best_partner));
    int *owner = (int *)malloc(keys * sizeof(*owner));
    uint32_t *scratch = (uint32_t *)malloc(symbols * sizeof(*scratch));
    if (!clusters || !storage || !best_delta || !best_partner || !owner || !scratch)
        fail("out of memory");
    unsigned active = 0;
    for (unsigned key = 0; key < keys; ++key) {
        clusters[key].histogram = storage + (size_t)key * symbols;
        memcpy(clusters[key].histogram, histograms + (size_t)key * symbols,
               symbols * sizeof(*storage));
        for (unsigned s = 0; s < symbols; ++s) clusters[key].total += clusters[key].histogram[s];
        clusters[key].active = clusters[key].total != 0;
        clusters[key].cost = fd_histogram_cost(clusters[key].histogram, symbols,
                                               clusters[key].total);
        owner[key] = clusters[key].active ? (int)key : -1;
        active += (unsigned)clusters[key].active;
    }
    if (!active) {
        memset(map, 0, keys);
        free(scratch); free(owner); free(best_partner); free(best_delta);
        free(storage); free(clusters);
        return 1;
    }
    for (unsigned a = 0; a < keys; ++a) {
        best_partner[a] = -1;
        best_delta[a] = INT64_MAX;
        if (!clusters[a].active) continue;
        for (unsigned b = a + 1u; b < keys; ++b) {
            if (!clusters[b].active) continue;
            int64_t delta = (int64_t)fd_merged_cost(&clusters[a], &clusters[b], symbols, scratch) -
                            (int64_t)clusters[a].cost - (int64_t)clusters[b].cost;
            if (delta < best_delta[a]) { best_delta[a] = delta; best_partner[a] = (int)b; }
        }
    }
    while (active > 1) {
        int a = -1;
        for (unsigned i = 0; i < keys; ++i)
            if (clusters[i].active && best_partner[i] >= 0 &&
                (a < 0 || best_delta[i] < best_delta[a]))
                a = (int)i;
        if (a < 0) break;
        if (best_delta[a] >= 0 && active <= max_tables) break;
        int b = best_partner[a];
        for (unsigned s = 0; s < symbols; ++s)
            clusters[a].histogram[s] += clusters[b].histogram[s];
        clusters[a].total += clusters[b].total;
        clusters[a].cost = fd_histogram_cost(clusters[a].histogram, symbols, clusters[a].total);
        clusters[b].active = 0;
        for (unsigned key = 0; key < keys; ++key)
            if (owner[key] == b) owner[key] = a;
        --active;
        /* Partners are stored on the lower index; refresh affected entries. */
        for (unsigned i = 0; i < keys; ++i) {
            if (!clusters[i].active) continue;
            if ((int)i == a || best_partner[i] == a || best_partner[i] == b) {
                best_partner[i] = -1;
                best_delta[i] = INT64_MAX;
                for (unsigned j = i + 1u; j < keys; ++j) {
                    if (!clusters[j].active) continue;
                    int64_t delta = (int64_t)fd_merged_cost(&clusters[i], &clusters[j],
                                                            symbols, scratch) -
                                    (int64_t)clusters[i].cost - (int64_t)clusters[j].cost;
                    if (delta < best_delta[i]) { best_delta[i] = delta; best_partner[i] = (int)j; }
                }
            } else if ((int)i < a) {
                int64_t delta = (int64_t)fd_merged_cost(&clusters[i], &clusters[a], symbols,
                                                        scratch) -
                                (int64_t)clusters[i].cost - (int64_t)clusters[a].cost;
                if (delta < best_delta[i]) { best_delta[i] = delta; best_partner[i] = a; }
            }
        }
    }
    /* Compact cluster ids. */
    int remap[FD_MAX_KEYS];
    unsigned tables = 0;
    for (unsigned i = 0; i < keys; ++i) remap[i] = clusters[i].active ? (int)tables++ : -1;
    int *cluster_of = (int *)malloc(tables * sizeof(*cluster_of));
    if (!cluster_of) fail("out of memory");
    for (unsigned i = 0; i < keys; ++i) if (remap[i] >= 0) cluster_of[remap[i]] = (int)i;
    for (unsigned key = 0; key < keys; ++key) map[key] = owner[key] >= 0 ? (uint8_t)remap[owner[key]] : 0;
    /* k-means refinement on per-key cross-entropy. */
    uint32_t *prices = (uint32_t *)malloc((size_t)tables * symbols * sizeof(*prices));
    uint32_t *sums = (uint32_t *)calloc((size_t)tables * symbols, sizeof(*sums));
    if (!prices || !sums) fail("out of memory");
    for (unsigned round = 0; round < 3 && tables > 1; ++round) {
        memset(sums, 0, (size_t)tables * symbols * sizeof(*sums));
        for (unsigned key = 0; key < keys; ++key)
            for (unsigned s = 0; s < symbols; ++s)
                sums[(size_t)map[key] * symbols + s] += histograms[(size_t)key * symbols + s];
        for (unsigned t = 0; t < tables; ++t) {
            uint64_t total = 0;
            for (unsigned s = 0; s < symbols; ++s) total += sums[(size_t)t * symbols + s];
            uint32_t total_log = fd_log2_q16((uint32_t)(total + symbols));
            for (unsigned s = 0; s < symbols; ++s) {
                uint32_t c = sums[(size_t)t * symbols + s];
                prices[(size_t)t * symbols + s] = c
                    ? total_log - fd_log2_q16(c)
                    : total_log + (3u << 16);
            }
        }
        int changed = 0;
        for (unsigned key = 0; key < keys; ++key) {
            const uint32_t *h = histograms + (size_t)key * symbols;
            uint64_t best = UINT64_MAX;
            unsigned best_table = map[key];
            int any = 0;
            for (unsigned s = 0; s < symbols; ++s) any |= h[s] != 0;
            if (!any) continue;
            for (unsigned t = 0; t < tables; ++t) {
                uint64_t cost = 0;
                for (unsigned s = 0; s < symbols; ++s)
                    if (h[s]) cost += (uint64_t)h[s] * prices[(size_t)t * symbols + s];
                if (cost < best) { best = cost; best_table = t; }
            }
            if (best_table != map[key]) { map[key] = (uint8_t)best_table; changed = 1; }
        }
        if (!changed) break;
    }
    /* Drop tables emptied by refinement. */
    unsigned used[FD_MAX_TABLES + 1u] = {0};
    for (unsigned key = 0; key < keys; ++key) {
        int any = 0;
        for (unsigned s = 0; s < symbols; ++s) any |= histograms[(size_t)key * symbols + s] != 0;
        if (any) used[map[key]] = 1;
    }
    uint8_t compact[FD_MAX_TABLES + 1u];
    unsigned final_tables = 0;
    for (unsigned t = 0; t < tables; ++t) compact[t] = (uint8_t)(used[t] ? final_tables++ : 0);
    if (!final_tables) final_tables = 1;
    for (unsigned key = 0; key < keys; ++key) map[key] = compact[map[key]];
    free(sums); free(prices); free(cluster_of);
    free(scratch); free(owner); free(best_partner); free(best_delta);
    free(storage); free(clusters);
    return final_tables;
}

static unsigned fd_bits_for(unsigned values) {
    unsigned bits = 0;
    while ((1u << bits) < values) ++bits;
    return bits;
}

/* Context literal section; returns 0 when the key space is unsuitable. */
static int fd_encode_literals_context(const uint8_t *input, size_t start, size_t block_size,
                                      const FdSequences *seqs, const uint8_t *literals,
                                      size_t count, unsigned order, FdLiteralSection *out,
                                      uint32_t *position_prices) {
    FdKeyModel model = {0};
    model.order = order;
    size_t history = start > order ? start - order : 0;
    uint8_t present[256] = {0};
    present[0] = 1;
    for (size_t i = history; i < start + block_size; ++i) present[input[i]] = 1;
    unsigned alphabet = 0;
    for (unsigned b = 0; b < 256; ++b) model.rank[b] = present[b] ? (uint8_t)alphabet++ : 0;
    model.alphabet = alphabet;
    uint64_t key_count = 1;
    for (unsigned i = 0; i < order; ++i) key_count *= alphabet;
    if (key_count > FD_MAX_KEYS) return 0;
    unsigned keys = (unsigned)key_count;

    /* Dense literal alphabet for clustering. */
    uint8_t dense[256], symbol_of[256];
    unsigned literal_symbols = 0;
    uint8_t seen[256] = {0};
    for (size_t i = 0; i < count; ++i) seen[literals[i]] = 1;
    for (unsigned b = 0; b < 256; ++b)
        if (seen[b]) { dense[b] = (uint8_t)literal_symbols; symbol_of[literal_symbols++] = (uint8_t)b; }
    if (!literal_symbols) return 0;

    uint16_t *literal_keys = (uint16_t *)malloc((count ? count : 1u) * sizeof(*literal_keys));
    uint32_t *histograms = (uint32_t *)calloc((size_t)keys * literal_symbols, sizeof(*histograms));
    if (!literal_keys || !histograms) fail("out of memory");
    size_t literal_index = 0, pos = start;
    for (size_t i = 0; i <= seqs->count; ++i) {
        size_t run = i < seqs->count ? seqs->items[i].literal_length
                                     : count - literal_index;
        for (size_t j = 0; j < run; ++j, ++pos, ++literal_index) {
            unsigned key = fd_key_at(&model, input, pos);
            literal_keys[literal_index] = (uint16_t)key;
            ++histograms[(size_t)key * literal_symbols + dense[literals[literal_index]]];
        }
        if (i < seqs->count) pos += seqs->items[i].match_length;
    }
    if (literal_index != count) fail("mode-27 literal accounting failed");

    uint8_t map[FD_MAX_KEYS];
    unsigned tables = fd_cluster_keys(histograms, keys, literal_symbols, FD_MAX_TABLES, map);
    FdTable *table = (FdTable *)malloc(tables * sizeof(*table));
    uint32_t (*counts)[256] = (uint32_t (*)[256])calloc(tables, sizeof(*counts));
    if (!table || !counts) fail("out of memory");
    for (size_t i = 0; i < count; ++i) ++counts[map[literal_keys[i]]][literals[i]];
    for (unsigned t = 0; t < tables; ++t) {
        int any = 0;
        for (unsigned s = 0; s < 256; ++s) any |= counts[t][s] != 0;
        if (!any) counts[t][symbol_of[0]] = 1;
        fd_choose_table(counts[t], 256, FD_LIT_MAX_LOG, &table[t]);
    }
    FdEncoder *encoders = (FdEncoder *)malloc(tables * sizeof(*encoders));
    uint32_t *states = (uint32_t *)malloc(tables * sizeof(*states));
    if (!encoders || !states) fail("out of memory");
    for (unsigned t = 0; t < tables; ++t) {
        fd_build_encoder(&table[t], &encoders[t]);
        states[t] = 1u << table[t].log;
    }
    FdBackWriter writer = {0};
    for (size_t i = count; i > 0; --i) {
        unsigned t = map[literal_keys[i - 1u]];
        fd_encode_symbol(&writer, &encoders[t], &states[t], literals[i - 1u]);
    }
    for (unsigned t = tables; t > 0; --t)
        fd_back_put(&writer, states[t - 1u], table[t - 1u].log);
    Buffer stream = fd_back_finish(&writer);

    Buffer section = {0};
    FdBitWriter header = {&section, 0, 0};
    fd_put(&header, order - 1u, 3);
    for (unsigned b = 0; b < 256; ++b) fd_put(&header, b ? present[b] : 0u, 1);
    fd_put(&header, tables - 1u, 6);
    unsigned map_bits = fd_bits_for(tables);
    for (unsigned key = 0; key < keys; ++key) fd_put(&header, map[key], map_bits);
    for (unsigned t = 0; t < tables; ++t) fd_write_table(&header, &table[t]);
    fd_put_finish(&header);
    append_bytes(&section, stream.data, stream.size);
    free(stream.data);

    if (position_prices) {
        uint32_t (*prices)[256] = (uint32_t (*)[256])malloc(tables * sizeof(*prices));
        if (!prices) fail("out of memory");
        for (unsigned t = 0; t < tables; ++t) fd_table_prices(&table[t], 256, prices[t]);
        for (size_t i = 0; i < block_size; ++i)
            position_prices[i] = prices[map[fd_key_at(&model, input, start + i)]][input[start + i]];
        free(prices);
    }
    free(states); free(encoders); free(counts); free(table);
    free(histograms); free(literal_keys);
    out->section = section;
    out->mode = 2;
    out->cost_bits = (uint64_t)section.size * 8u;
    return 1;
}

static int fd_sequence_valid(const uint8_t *input, size_t start, size_t block_size,
                             const FdSequences *seqs) {
    size_t pos = start, end = start + block_size;
    for (size_t i = 0; i < seqs->count; ++i) {
        const FdSequence *s = &seqs->items[i];
        if (s->literal_length > end - pos) return 0;
        pos += s->literal_length;
        if (s->match_length < MIN_MATCH || s->match_length > end - pos ||
            !s->distance || s->distance > pos || s->distance > WIDE_WINDOW)
            return 0;
        if (memcmp(input + pos, input + pos - s->distance, s->match_length) != 0) return 0;
        pos += s->match_length;
    }
    return 1;
}

/* Encode a block: literal section choice plus the sequence section. */
static Buffer fd_encode_block(const uint8_t *input, size_t start, size_t block_size,
                              const FdSequences *seqs, FdPrices *prices) {
    if (!fd_sequence_valid(input, start, block_size, seqs)) fail("invalid mode-27 parse");
    size_t literal_count = block_size;
    for (size_t i = 0; i < seqs->count; ++i) literal_count -= seqs->items[i].match_length;
    uint8_t *literals = (uint8_t *)malloc(literal_count ? literal_count : 1u);
    if (!literals) fail("out of memory");
    uint32_t literal_counts[256] = {0};
    {
        size_t pos = start, index = 0;
        for (size_t i = 0; i <= seqs->count; ++i) {
            size_t run = i < seqs->count ? seqs->items[i].literal_length
                                         : literal_count - index;
            memcpy(literals + index, input + pos, run);
            for (size_t j = 0; j < run; ++j) ++literal_counts[input[pos + j]];
            index += run;
            pos += run;
            if (i < seqs->count) pos += seqs->items[i].match_length;
        }
    }

    FdLiteralSection best = {{0}, 0, (uint64_t)literal_count * 8u};
    uint32_t *best_prices = prices ? prices->literal : NULL;
    uint32_t *trial_prices = prices && block_size
        ? (uint32_t *)malloc(block_size * sizeof(*trial_prices)) : NULL;
    if (prices && block_size && !trial_prices) fail("out of memory");
    if (prices)
        for (size_t i = 0; i < block_size; ++i) best_prices[i] = 8u << 8;
    if (literal_count) {
        FdLiteralSection candidate;
        uint32_t symbol_prices[256];
        fd_encode_literals_order0(literals, literal_count, literal_counts, &candidate,
                                  symbol_prices);
        if (candidate.cost_bits < best.cost_bits) {
            free(best.section.data);
            best = candidate;
            if (prices)
                for (size_t i = 0; i < block_size; ++i)
                    best_prices[i] = symbol_prices[input[start + i]];
        } else {
            free(candidate.section.data);
        }
        for (unsigned order = 1; order <= FD_MAX_ORDER; ++order) {
            if (!fd_encode_literals_context(input, start, block_size, seqs, literals,
                                            literal_count, order, &candidate, trial_prices))
                break;
            if (candidate.cost_bits < best.cost_bits) {
                free(best.section.data);
                best = candidate;
                if (prices) memcpy(best_prices, trial_prices, block_size * sizeof(*trial_prices));
            } else {
                free(candidate.section.data);
            }
        }
    }
    free(trial_prices);
    if (!best.mode) {
        best.section = (Buffer){0};
        append_bytes(&best.section, literals, literal_count);
    }

    Buffer output = {0};
    fd_append_varint(&output, literal_count);
    fd_append_varint(&output, seqs->count);
    append_byte(&output, best.mode);
    fd_append_varint(&output, best.section.size);
    append_bytes(&output, best.section.data, best.section.size);
    free(best.section.data);
    free(literals);

    if (seqs->count) {
        size_t n = seqs->count;
        uint8_t *codes = (uint8_t *)malloc(n * 3u);
        uint32_t *extras = (uint32_t *)malloc(n * 3u * sizeof(*extras));
        uint8_t *extra_bits = (uint8_t *)malloc(n * 3u);
        if (!codes || !extras || !extra_bits) fail("out of memory");
        uint32_t ll_counts[FD_LL_CODES] = {0}, ml_counts[FD_ML_CODES] = {0};
        uint32_t of_counts[FD_OF_CODES] = {0};
        uint32_t reps[FD_REPS];
        fd_initial_reps(reps);
        for (size_t i = 0; i < n; ++i) {
            const FdSequence *s = &seqs->items[i];
            unsigned bits;
            uint32_t extra;
            codes[3u * i] = (uint8_t)fd_value_code(s->literal_length, FD_LL_DIRECT, &bits, &extra);
            extras[3u * i] = extra; extra_bits[3u * i] = (uint8_t)bits;
            codes[3u * i + 1u] = (uint8_t)fd_value_code(s->match_length - MIN_MATCH,
                                                        FD_ML_DIRECT, &bits, &extra);
            extras[3u * i + 1u] = extra; extra_bits[3u * i + 1u] = (uint8_t)bits;
            unsigned rep = fd_rep_code(reps, s->distance);
            if (rep < FD_REPS) {
                codes[3u * i + 2u] = (uint8_t)rep;
                extras[3u * i + 2u] = 0; extra_bits[3u * i + 2u] = 0;
            } else {
                codes[3u * i + 2u] = (uint8_t)(FD_REPS +
                    fd_value_code(s->distance - 1u, FD_OF_DIRECT, &bits, &extra));
                extras[3u * i + 2u] = extra; extra_bits[3u * i + 2u] = (uint8_t)bits;
            }
            fd_rep_update(reps, s->distance, rep);
            ++ll_counts[codes[3u * i]];
            ++ml_counts[codes[3u * i + 1u]];
            ++of_counts[codes[3u * i + 2u]];
        }
        FdTable tables[3];
        fd_choose_table(ll_counts, FD_LL_CODES, FD_SEQ_MAX_LOG, &tables[0]);
        fd_choose_table(ml_counts, FD_ML_CODES, FD_SEQ_MAX_LOG, &tables[1]);
        fd_choose_table(of_counts, FD_OF_CODES, FD_SEQ_MAX_LOG, &tables[2]);
        if (prices) {
            fd_table_prices(&tables[0], FD_LL_CODES, prices->ll);
            fd_table_prices(&tables[1], FD_ML_CODES, prices->ml);
            fd_table_prices(&tables[2], FD_OF_CODES, prices->of);
        }
        FdEncoder *encoders = (FdEncoder *)malloc(3u * sizeof(*encoders));
        if (!encoders) fail("out of memory");
        uint32_t states[3];
        for (unsigned t = 0; t < 3; ++t) {
            fd_build_encoder(&tables[t], &encoders[t]);
            states[t] = 1u << tables[t].log;
        }
        FdBackWriter writer = {0};
        for (size_t i = n; i > 0; --i) {
            size_t k = 3u * (i - 1u);
            fd_encode_symbol(&writer, &encoders[2], &states[2], codes[k + 2u]);
            fd_encode_symbol(&writer, &encoders[1], &states[1], codes[k + 1u]);
            fd_encode_symbol(&writer, &encoders[0], &states[0], codes[k]);
            fd_back_put(&writer, extras[k], extra_bits[k]);
            fd_back_put(&writer, extras[k + 1u], extra_bits[k + 1u]);
            fd_back_put(&writer, extras[k + 2u], extra_bits[k + 2u]);
        }
        fd_back_put(&writer, states[2], tables[2].log);
        fd_back_put(&writer, states[1], tables[1].log);
        fd_back_put(&writer, states[0], tables[0].log);
        Buffer stream = fd_back_finish(&writer);
        FdBitWriter header = {&output, 0, 0};
        for (unsigned t = 0; t < 3; ++t) fd_write_table(&header, &tables[t]);
        fd_put_finish(&header);
        append_bytes(&output, stream.data, stream.size);
        free(stream.data);
        free(encoders);
        free(extra_bits); free(extras); free(codes);
    } else if (prices) {
        for (unsigned c = 0; c < FD_LL_CODES; ++c) prices->ll[c] = 6u << 8;
        for (unsigned c = 0; c < FD_ML_CODES; ++c) prices->ml[c] = 6u << 8;
        for (unsigned c = 0; c < FD_OF_CODES; ++c) prices->of[c] = 6u << 8;
    }
    return output;
}

/* Greedy seed parse over the shared wide index. */
static void fd_greedy_parse(size_t start, size_t block_size,
                            const WideMatchIndex *index, FdSequences *seqs) {
    size_t pos = start, end = start + block_size, run = 0;
    seqs->count = 0;
    while (pos < end) {
        size_t length = index->lengths[pos];
        if (length > end - pos) length = end - pos;
        if (length >= MIN_MATCH && pos + 1u < end && index->lengths[pos + 1u] > length + 1u)
            length = 0;
        if (length < MIN_MATCH) { ++pos; ++run; continue; }
        fd_push_sequence(seqs, (uint32_t)run, (uint32_t)length, index->offsets[pos]);
        run = 0;
        pos += length;
    }
}

typedef struct {
    uint64_t price;
    uint32_t literal_length;
    uint32_t match_length;
    uint32_t distance;
    uint32_t reps[FD_REPS];
} FdNode;

typedef struct {
    uint32_t ll_table[4096];
    uint32_t *ml_table;
    uint32_t of_rep[FD_REPS];
    uint32_t of_code[FD_OF_CODES];
    const uint32_t *literal;
    uint32_t ll_codes[FD_LL_CODES];
} FdPriceCache;

static inline uint32_t fd_ll_price(const FdPriceCache *cache, uint32_t length) {
    if (length < 4096u) return cache->ll_table[length];
    unsigned bits;
    uint32_t extra;
    unsigned code = fd_value_code(length, FD_LL_DIRECT, &bits, &extra);
    return cache->ll_codes[code] + (bits << 8);
}

static inline uint32_t fd_offset_price(const FdPriceCache *cache, uint32_t distance) {
    unsigned bits;
    uint32_t extra;
    unsigned code = fd_value_code(distance - 1u, FD_OF_DIRECT, &bits, &extra);
    return cache->of_code[FD_REPS + code] + (bits << 8);
}

static void fd_price_cache_init(FdPriceCache *cache, const FdPrices *prices) {
    memcpy(cache->ll_codes, prices->ll, sizeof(cache->ll_codes));
    for (uint32_t length = 0; length < 4096u; ++length) {
        unsigned bits;
        uint32_t extra;
        unsigned code = fd_value_code(length, FD_LL_DIRECT, &bits, &extra);
        cache->ll_table[length] = prices->ll[code] + (bits << 8);
    }
    cache->ml_table = (uint32_t *)malloc((WIDE_MAX_MATCH + 1u) * sizeof(*cache->ml_table));
    if (!cache->ml_table) fail("out of memory");
    for (uint32_t length = 0; length <= WIDE_MAX_MATCH; ++length) {
        if (length < MIN_MATCH) { cache->ml_table[length] = 0; continue; }
        unsigned bits;
        uint32_t extra;
        unsigned code = fd_value_code(length - MIN_MATCH, FD_ML_DIRECT, &bits, &extra);
        cache->ml_table[length] = prices->ml[code] + (bits << 8);
    }
    for (unsigned rep = 0; rep < FD_REPS; ++rep) cache->of_rep[rep] = prices->of[rep];
    memcpy(cache->of_code, prices->of, sizeof(cache->of_code));
    cache->literal = prices->literal;
}

/* Forward optimal parse priced against the previous mode-27 encoding. */
static void fd_optimal_parse(const uint8_t *input, size_t start, size_t block_size,
                             const WideMatchIndex *index, const FdPrices *prices,
                             unsigned chain, FdSequences *seqs) {
    FdPriceCache *cache = (FdPriceCache *)malloc(sizeof(*cache));
    if (!cache) fail("out of memory");
    fd_price_cache_init(cache, prices);
    size_t node_capacity = FD_OPT_NUM + WIDE_MAX_MATCH + 2u;
    FdNode *nodes = (FdNode *)malloc(node_capacity * sizeof(*nodes));
    uint32_t *path = (uint32_t *)malloc(node_capacity * sizeof(*path));
    if (!nodes || !path) fail("out of memory");
    seqs->count = 0;
    uint32_t reps[FD_REPS];
    fd_initial_reps(reps);
    size_t base = 0;
    uint32_t pending = 0;
    size_t end = block_size;
    while (base < end) {
        nodes[0].price = fd_ll_price(cache, pending);
        nodes[0].literal_length = pending;
        nodes[0].match_length = 0;
        nodes[0].distance = 0;
        memcpy(nodes[0].reps, reps, sizeof(reps));
        size_t last = 0, reach = 0, cur = 0, stop = 0;
        int forced = 0;
        for (;;) {
            if (cur > 0 && cur >= reach) { stop = cur; break; }
            if (cur >= FD_OPT_NUM || base + cur >= end) { stop = cur; break; }
            const FdNode *node = &nodes[cur];
            size_t absolute = start + base + cur;
            if (last < cur + 1u) { nodes[cur + 1u].price = FD_PRICE_INF; last = cur + 1u; }
            uint64_t literal_price = node->price + cache->literal[base + cur] +
                fd_ll_price(cache, node->literal_length + 1u) -
                fd_ll_price(cache, node->literal_length);
            if (literal_price < nodes[cur + 1u].price) {
                FdNode *next = &nodes[cur + 1u];
                next->price = literal_price;
                next->literal_length = node->literal_length + 1u;
                next->match_length = 0;
                next->distance = 0;
                memcpy(next->reps, node->reps, sizeof(next->reps));
            }
            size_t limit = end - base - cur;
            if (limit > WIDE_MAX_MATCH) limit = WIDE_MAX_MATCH;
            if (limit < MIN_MATCH) { ++cur; continue; }
            uint64_t match_base = node->price + cache->ll_table[0];
            uint32_t candidate_lengths[FD_REPS + 72];
            uint32_t candidate_distances[FD_REPS + 72];
            uint32_t candidate_prices[FD_REPS + 72];
            uint32_t candidate_min[FD_REPS + 72];
            unsigned candidates = 0;
            size_t longest = 0;
            for (unsigned rep = 0; rep < FD_REPS; ++rep) {
                uint32_t distance = node->reps[rep];
                if (!distance || distance > absolute || distance > WIDE_WINDOW) continue;
                int duplicate = 0;
                for (unsigned r = 0; r < rep; ++r) duplicate |= node->reps[r] == distance;
                if (duplicate) continue;
                const uint8_t *a = input + absolute, *b = a - distance;
                size_t length = 0;
                while (length < limit && a[length] == b[length]) ++length;
                if (length < MIN_MATCH) continue;
                candidate_lengths[candidates] = (uint32_t)length;
                candidate_distances[candidates] = distance;
                candidate_prices[candidates] = cache->of_rep[rep];
                candidate_min[candidates] = MIN_MATCH;
                ++candidates;
                if (length > longest) longest = length;
            }
            size_t best_length = MIN_MATCH - 1u;
            size_t index_length = index->lengths[absolute];
            if (index_length > limit) index_length = limit;
            if (index_length >= MIN_MATCH) {
                int32_t candidate = index->previous[absolute];
                unsigned depth = 0;
                while (candidate >= 0 && depth++ < chain && candidates < FD_REPS + 70u) {
                    size_t c = (size_t)candidate;
                    size_t distance = absolute - c;
                    if (distance > WIDE_WINDOW) break;
                    if (input[c + best_length] == input[absolute + best_length]) {
                        size_t length = 0;
                        while (length < limit && input[c + length] == input[absolute + length])
                            ++length;
                        if (length > best_length) {
                            unsigned rep = fd_rep_code(node->reps, (uint32_t)distance);
                            candidate_lengths[candidates] = (uint32_t)length;
                            candidate_distances[candidates] = (uint32_t)distance;
                            candidate_prices[candidates] = rep < FD_REPS
                                ? cache->of_rep[rep]
                                : fd_offset_price(cache, (uint32_t)distance);
                            candidate_min[candidates] = (uint32_t)best_length + 1u;
                            ++candidates;
                            best_length = length;
                            if (length >= limit) break;
                        }
                    }
                    candidate = index->previous[c];
                }
                if (index_length > best_length) {
                    uint32_t distance = index->offsets[absolute];
                    unsigned rep = fd_rep_code(node->reps, distance);
                    candidate_lengths[candidates] = (uint32_t)index_length;
                    candidate_distances[candidates] = distance;
                    candidate_prices[candidates] = rep < FD_REPS
                        ? cache->of_rep[rep] : fd_offset_price(cache, distance);
                    candidate_min[candidates] = (uint32_t)best_length + 1u;
                    ++candidates;
                    best_length = index_length;
                }
                if (best_length > longest) longest = best_length;
            }
            if (longest >= FD_SUFFICIENT) {
                unsigned chosen = 0;
                for (unsigned i = 1; i < candidates; ++i)
                    if (candidate_lengths[i] > candidate_lengths[chosen]) chosen = i;
                size_t target = cur + candidate_lengths[chosen];
                FdNode *next = &nodes[target];
                next->price = match_base + cache->ml_table[candidate_lengths[chosen]] +
                              candidate_prices[chosen];
                next->literal_length = 0;
                next->match_length = candidate_lengths[chosen];
                next->distance = candidate_distances[chosen];
                memcpy(next->reps, node->reps, sizeof(next->reps));
                fd_rep_update(next->reps, next->distance,
                              fd_rep_code(node->reps, next->distance));
                stop = target;
                forced = 1;
                break;
            }
            for (unsigned i = 0; i < candidates; ++i) {
                size_t max_length = candidate_lengths[i];
                if (cur + max_length > last) {
                    for (size_t j = last + 1u; j <= cur + max_length; ++j)
                        nodes[j].price = FD_PRICE_INF;
                    last = cur + max_length;
                }
                if (cur + max_length > reach) reach = cur + max_length;
                uint64_t offset_base = match_base + candidate_prices[i];
                for (size_t length = candidate_min[i]; length <= max_length; ++length) {
                    uint64_t price = offset_base + cache->ml_table[length];
                    FdNode *next = &nodes[cur + length];
                    if (price < next->price) {
                        next->price = price;
                        next->literal_length = 0;
                        next->match_length = (uint32_t)length;
                        next->distance = candidate_distances[i];
                        memcpy(next->reps, node->reps, sizeof(next->reps));
                        fd_rep_update(next->reps, candidate_distances[i],
                                      fd_rep_code(node->reps, candidate_distances[i]));
                    }
                }
            }
            ++cur;
        }
        (void)forced;
        if (!stop) stop = 1;
        /* Backtrack from the segment end. */
        size_t steps = 0, position = stop;
        while (position > 0) {
            path[steps++] = (uint32_t)position;
            const FdNode *node = &nodes[position];
            position -= node->match_length ? node->match_length : 1u;
        }
        for (size_t i = steps; i > 0; --i) {
            size_t at = path[i - 1u];
            const FdNode *node = &nodes[at];
            if (node->match_length) {
                const FdNode *from = &nodes[at - node->match_length];
                fd_push_sequence(seqs, from->literal_length, node->match_length,
                                 node->distance);
            }
        }
        pending = nodes[stop].literal_length;
        memcpy(reps, nodes[stop].reps, sizeof(reps));
        base += stop;
    }
    free(path);
    free(nodes);
    free(cache->ml_table);
    free(cache);
}

static Buffer fd_compress_block(const uint8_t *input, size_t start, size_t block_size,
                                const WideMatchIndex *index, unsigned chain, unsigned passes) {
    FdPrices prices;
    prices.literal = (uint32_t *)malloc((block_size ? block_size : 1u) * sizeof(*prices.literal));
    if (!prices.literal) fail("out of memory");
    FdSequences seqs = {0};
    fd_greedy_parse(start, block_size, index, &seqs);
    Buffer best = fd_encode_block(input, start, block_size, &seqs, &prices);
    int profile = getenv("BITE_PROFILE") != NULL;
    if (profile) fprintf(stderr, "bite profile: mode27 greedy=%zu\n", best.size);
    for (unsigned pass = 0; pass < passes; ++pass) {
        fd_optimal_parse(input, start, block_size, index, &prices, chain, &seqs);
        Buffer candidate = fd_encode_block(input, start, block_size, &seqs, &prices);
        if (profile) fprintf(stderr, "bite profile: mode27 pass%u=%zu\n", pass, candidate.size);
        if (candidate.size < best.size) {
            free(best.data);
            best = candidate;
        } else {
            free(candidate.data);
        }
    }
    free(seqs.items);
    free(prices.literal);
    return best;
}

/* Decoder. */
typedef struct {
    FdDecodeEntry entries[1u << FD_MAX_LOG];
} FdDecodeTable;

static void fd_decode_literals_order0(const uint8_t *section, size_t size, uint8_t *literals,
                                      size_t count) {
    FdBitReader header = {section, size, 0};
    FdTable table;
    fd_read_table(&header, &table, 256, FD_LIT_MAX_LOG);
    size_t pos = (header.position + 7u) >> 3;
    size_t sizes[4];
    size_t total = 0;
    for (unsigned stream = 0; stream < 3; ++stream) {
        uint64_t value = fd_read_varint(section, size, &pos);
        if (value > size) fail("invalid mode-27 literal stream size");
        sizes[stream] = (size_t)value;
        total += sizes[stream];
    }
    if (total > size - pos) fail("invalid mode-27 literal stream sizes");
    sizes[3] = size - pos - total;
    FdDecodeTable *decoder = (FdDecodeTable *)malloc(sizeof(*decoder));
    if (!decoder) fail("out of memory");
    fd_build_decoder(&table, decoder->entries);
    const FdDecodeEntry *entries = decoder->entries;
    FdBackReader readers[4];
    uint32_t states[4];
    uint8_t *outputs[4];
    size_t quarter = (count + 3u) / 4u;
    size_t remaining[4];
    for (unsigned stream = 0; stream < 4; ++stream) {
        fd_back_init(&readers[stream], section + pos, sizes[stream]);
        pos += sizes[stream];
        states[stream] = fd_back_get(&readers[stream], table.log);
        size_t begin = quarter * stream < count ? quarter * stream : count;
        size_t end = begin + quarter < count ? begin + quarter : count;
        outputs[stream] = literals + begin;
        remaining[stream] = end - begin;
    }
    size_t common = remaining[3];
    for (unsigned stream = 0; stream < 3; ++stream)
        if (remaining[stream] < common) common = remaining[stream];
    size_t i = 0;
    for (; i + 4u <= common; i += 4u) {
        for (unsigned k = 0; k < 4; ++k) {
            for (unsigned stream = 0; stream < 4; ++stream) {
                FdDecodeEntry e = entries[states[stream]];
                outputs[stream][i + k] = e.symbol;
                states[stream] = e.base + fd_back_get(&readers[stream], e.bits);
            }
        }
        for (unsigned stream = 0; stream < 4; ++stream) fd_back_reload(&readers[stream]);
    }
    for (unsigned stream = 0; stream < 4; ++stream) {
        for (size_t j = i; j < remaining[stream]; ++j) {
            FdDecodeEntry e = entries[states[stream]];
            outputs[stream][j] = e.symbol;
            states[stream] = e.base + fd_back_get(&readers[stream], e.bits);
            fd_back_reload(&readers[stream]);
        }
        if (states[stream]) fail("invalid mode-27 literal final state");
        fd_back_check_end(&readers[stream]);
    }
    free(decoder);
}

typedef struct {
    FdKeyModel model;
    unsigned tables;
    uint8_t map[FD_MAX_KEYS];
    FdDecodeTable *decoders;
    uint32_t states[FD_MAX_TABLES];
    FdBackReader reader;
} FdContextLiterals;

static void fd_context_literals_init(FdContextLiterals *ctx, const uint8_t *section,
                                     size_t size) {
    FdBitReader header = {section, size, 0};
    ctx->model.order = fd_get(&header, 3) + 1u;
    if (ctx->model.order > FD_MAX_ORDER) fail("invalid mode-27 literal order");
    unsigned alphabet = 0;
    for (unsigned b = 0; b < 256; ++b) {
        unsigned present = fd_get(&header, 1);
        if (b == 0) present = 1;
        ctx->model.rank[b] = present ? (uint8_t)alphabet++ : 0;
    }
    ctx->model.alphabet = alphabet;
    uint64_t keys = 1;
    for (unsigned i = 0; i < ctx->model.order; ++i) keys *= alphabet;
    if (keys > FD_MAX_KEYS) fail("invalid mode-27 literal key space");
    ctx->tables = fd_get(&header, 6) + 1u;
    unsigned map_bits = fd_bits_for(ctx->tables);
    for (unsigned key = 0; key < keys; ++key) {
        ctx->map[key] = (uint8_t)fd_get(&header, map_bits);
        if (ctx->map[key] >= ctx->tables) fail("invalid mode-27 literal context map");
    }
    ctx->decoders = (FdDecodeTable *)malloc(ctx->tables * sizeof(*ctx->decoders));
    if (!ctx->decoders) fail("out of memory");
    unsigned logs[FD_MAX_TABLES];
    for (unsigned t = 0; t < ctx->tables; ++t) {
        FdTable table;
        fd_read_table(&header, &table, 256, FD_LIT_MAX_LOG);
        fd_build_decoder(&table, ctx->decoders[t].entries);
        logs[t] = table.log;
    }
    size_t pos = (header.position + 7u) >> 3;
    if (pos > size) fail("truncated mode-27 literal header");
    fd_back_init(&ctx->reader, section + pos, size - pos);
    for (unsigned t = 0; t < ctx->tables; ++t) {
        ctx->states[t] = fd_back_get(&ctx->reader, logs[t]);
        fd_back_reload(&ctx->reader);
    }
}

static inline uint8_t fd_context_literal(FdContextLiterals *ctx, const uint8_t *output,
                                         size_t pos) {
    unsigned t = ctx->map[fd_key_at(&ctx->model, output, pos)];
    FdDecodeEntry e = ctx->decoders[t].entries[ctx->states[t]];
    ctx->states[t] = e.base + fd_back_get(&ctx->reader, e.bits);
    fd_back_reload(&ctx->reader);
    return e.symbol;
}

static void fd_decode_block(Buffer *output, const uint8_t *input, size_t input_size,
                            size_t decoded_size) {
    size_t pos = 0;
    uint64_t literal_count64 = fd_read_varint(input, input_size, &pos);
    uint64_t sequence_count64 = fd_read_varint(input, input_size, &pos);
    if (literal_count64 > decoded_size || sequence_count64 > decoded_size / MIN_MATCH)
        fail("invalid mode-27 counts");
    size_t literal_count = (size_t)literal_count64;
    size_t sequence_count = (size_t)sequence_count64;
    if (pos >= input_size) fail("truncated mode-27 block");
    uint8_t literal_mode = input[pos++];
    uint64_t section_size = fd_read_varint(input, input_size, &pos);
    if (literal_mode > 2 || section_size > input_size - pos) fail("invalid mode-27 literals");
    const uint8_t *section = input + pos;
    pos += (size_t)section_size;

    uint8_t *literals = NULL;
    FdContextLiterals *ctx = NULL;
    if (literal_mode == 2) {
        if (!literal_count) fail("invalid mode-27 literal mode");
        ctx = (FdContextLiterals *)malloc(sizeof(*ctx));
        if (!ctx) fail("out of memory");
        fd_context_literals_init(ctx, section, (size_t)section_size);
    } else {
        literals = (uint8_t *)malloc(literal_count + FD_COPY_SLACK);
        if (!literals) fail("out of memory");
        if (literal_mode == 0) {
            if (section_size != literal_count) fail("invalid mode-27 raw literals");
            if (literal_count) memcpy(literals, section, literal_count);
        } else {
            if (!literal_count) fail("invalid mode-27 literal mode");
            fd_decode_literals_order0(section, (size_t)section_size, literals, literal_count);
        }
    }

    reserve(output, decoded_size + FD_COPY_SLACK);
    uint8_t *const out_base = output->data;
    uint8_t *op = output->data + output->size;
    uint8_t *const out_end = op + decoded_size;
    const uint8_t *lp = literals;
    size_t literals_left = literal_count;

    if (sequence_count) {
        FdBitReader header = {input + pos, input_size - pos, 0};
        FdTable tables[3];
        fd_read_table(&header, &tables[0], FD_LL_CODES, FD_SEQ_MAX_LOG);
        fd_read_table(&header, &tables[1], FD_ML_CODES, FD_SEQ_MAX_LOG);
        fd_read_table(&header, &tables[2], FD_OF_CODES, FD_SEQ_MAX_LOG);
        pos += (header.position + 7u) >> 3;
        if (pos > input_size) fail("truncated mode-27 sequence header");
        FdDecodeTable *decoders = (FdDecodeTable *)malloc(3u * sizeof(*decoders));
        if (!decoders) fail("out of memory");
        for (unsigned t = 0; t < 3; ++t) fd_build_decoder(&tables[t], decoders[t].entries);
        uint32_t ll_base[FD_LL_CODES], ml_base[FD_ML_CODES], of_base[FD_OF_CODES];
        uint8_t ll_bits[FD_LL_CODES], ml_bits[FD_ML_CODES], of_bits[FD_OF_CODES];
        fd_code_bases(FD_LL_DIRECT, FD_LL_CODES, ll_base, ll_bits);
        fd_code_bases(FD_ML_DIRECT, FD_ML_CODES, ml_base, ml_bits);
        fd_code_bases(FD_OF_DIRECT, FD_OF_CODES - FD_REPS, of_base + FD_REPS, of_bits + FD_REPS);
        for (unsigned i = 0; i < FD_REPS; ++i) { of_base[i] = 0; of_bits[i] = 0; }
        FdBackReader reader;
        fd_back_init(&reader, input + pos, input_size - pos);
        uint32_t ll_state = fd_back_get(&reader, tables[0].log);
        uint32_t ml_state = fd_back_get(&reader, tables[1].log);
        uint32_t of_state = fd_back_get(&reader, tables[2].log);
        const FdDecodeEntry *ll_table = decoders[0].entries;
        const FdDecodeEntry *ml_table = decoders[1].entries;
        const FdDecodeEntry *of_table = decoders[2].entries;
        uint32_t reps[FD_REPS];
        fd_initial_reps(reps);
        for (size_t i = 0; i < sequence_count; ++i) {
            FdDecodeEntry le = ll_table[ll_state];
            FdDecodeEntry me = ml_table[ml_state];
            FdDecodeEntry oe = of_table[of_state];
            fd_back_reload(&reader);
            unsigned of_code = oe.symbol;
            uint32_t distance;
            if (of_code < FD_REPS) {
                distance = reps[of_code];
                if (of_code) {
                    if (of_code > 2) reps[3] = reps[2];
                    if (of_code > 1) reps[2] = reps[1];
                    reps[1] = reps[0];
                    reps[0] = distance;
                }
            } else {
                distance = of_base[of_code] + fd_back_get(&reader, of_bits[of_code]) + 1u;
                reps[3] = reps[2]; reps[2] = reps[1]; reps[1] = reps[0];
                reps[0] = distance;
            }
            size_t match_length = ml_base[me.symbol] + fd_back_get(&reader, ml_bits[me.symbol]) +
                                  MIN_MATCH;
            fd_back_reload(&reader);
            size_t literal_length = ll_base[le.symbol] + fd_back_get(&reader, ll_bits[le.symbol]);
            ll_state = le.base + fd_back_get(&reader, le.bits);
            ml_state = me.base + fd_back_get(&reader, me.bits);
            of_state = oe.base + fd_back_get(&reader, oe.bits);
            if (literal_length > literals_left ||
                literal_length + match_length > (size_t)(out_end - op))
                fail("invalid mode-27 sequence");
            literals_left -= literal_length;
            if (ctx) {
                size_t at = (size_t)(op - out_base);
                for (size_t j = 0; j < literal_length; ++j)
                    out_base[at + j] = fd_context_literal(ctx, out_base, at + j);
            } else {
                fd_copy16(op, lp);
                if (literal_length > 16) {
                    for (size_t j = 16; j < literal_length; j += 16) fd_copy16(op + j, lp + j);
                }
                lp += literal_length;
            }
            op += literal_length;
            if (distance > (size_t)(op - out_base) || distance > WIDE_WINDOW)
                fail("invalid mode-27 distance");
            const uint8_t *match = op - distance;
            if (distance >= 16) {
                for (size_t j = 0; j < match_length; j += 16) fd_copy16(op + j, match + j);
            } else if (distance >= 8) {
                for (size_t j = 0; j < match_length; j += 8) memcpy(op + j, match + j, 8);
            } else {
                for (size_t j = 0; j < match_length; ++j) op[j] = match[j];
            }
            op += match_length;
        }
        if (ll_state || ml_state || of_state) fail("invalid mode-27 sequence final state");
        fd_back_check_end(&reader);
        free(decoders);
    } else if (pos != input_size) {
        fail("trailing mode-27 data");
    }
    if (literals_left != (size_t)(out_end - op)) fail("invalid mode-27 trailing literals");
    if (ctx) {
        size_t at = (size_t)(op - out_base);
        for (size_t j = 0; j < literals_left; ++j)
            out_base[at + j] = fd_context_literal(ctx, out_base, at + j);
        for (unsigned t = 0; t < ctx->tables; ++t)
            if (ctx->states[t]) fail("invalid mode-27 literal final state");
        fd_back_check_end(&ctx->reader);
        free(ctx->decoders);
        free(ctx);
    } else if (literals_left) {
        memcpy(op, lp, literals_left);
    }
    output->size += decoded_size;
    free(literals);
}

/* `cdec`: wide index plus mode-27 blocks, with raw fallback. */
#define FD_BLOCK_SIZE WHOLE_BLOCK_SIZE
#define FD_INDEX_CHAIN EXTREME_WIDE_MAX_CHAIN
#define FD_PARSE_CHAIN 64u
#define FD_PASSES 3u

static Buffer compress_fast_decode_blocked(const uint8_t *input, size_t input_size,
                                           Buffer output) {
    WideMatchIndex index = build_wide_match_index(input, input_size, FD_BLOCK_SIZE,
                                                  FD_INDEX_CHAIN, FD_PARSE_CHAIN);
    size_t pos = 0;
    while (pos < input_size) {
        size_t block_size = input_size - pos;
        if (block_size > FD_BLOCK_SIZE) block_size = FD_BLOCK_SIZE;
        Buffer payload = fd_compress_block(input, pos, block_size, &index,
                                           FD_PARSE_CHAIN, FD_PASSES);
        if (payload.size < block_size) {
            append_byte(&output, 27);
            append_u32le(&output, (uint32_t)block_size);
            append_u32le(&output, (uint32_t)payload.size);
            append_bytes(&output, payload.data, payload.size);
        } else {
            append_byte(&output, 0);
            append_u32le(&output, (uint32_t)block_size);
            append_u32le(&output, (uint32_t)block_size);
            append_bytes(&output, input + pos, block_size);
        }
        free(payload.data);
        pos += block_size;
    }
    free_wide_match_index(&index);
    return output;
}
