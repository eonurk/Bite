#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HASH_BITS 16u
#define HASH_SIZE (1u << HASH_BITS)
#define WINDOW 65535u
#define MIN_MATCH 4u
#define MAX_MATCH 259u
#define MAX_CHAIN 64u
#define HEADER_SIZE 16u
#define BLOCK_SIZE 65536u
#define LARGE_BLOCK_SIZE 1048576u
#define WHOLE_BLOCK_SIZE 16777216u
#define BLOCK_HEADER_SIZE 9u
#define DELTA_SAMPLE_SEGMENT 1365u
#define ENTROPY_LENGTH_CHOICES 8u
#define WIDE_HASH_BITS 19u
#define WIDE_HASH_SIZE (1u << WIDE_HASH_BITS)
#define WIDE_WINDOW 16777216u
#define WIDE_MAX_CHAIN 256u
#define WIDE_COST_CHAIN 32u
#define DEEP_WIDE_MAX_CHAIN 512u
#define DEEP_WIDE_COST_CHAIN 64u
#define EXTREME_WIDE_MAX_CHAIN 2048u
#define EXTREME_WIDE_COST_CHAIN 128u
#define WIDE_MAX_MATCH 65535u

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} Buffer;

static void fail(const char *message) {
    fprintf(stderr, "bite: %s\n", message);
    exit(1);
}

static void fail_errno(const char *path) {
    fprintf(stderr, "bite: %s: %s\n", path, strerror(errno));
    exit(1);
}

static void reserve(Buffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->size) fail("output size overflow");
    size_t needed = buffer->size + extra;
    if (needed <= buffer->capacity) return;
    size_t capacity = buffer->capacity ? buffer->capacity : 4096;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
        capacity *= 2;
    }
    uint8_t *data = (uint8_t *)realloc(buffer->data, capacity);
    if (!data) fail("out of memory");
    buffer->data = data;
    buffer->capacity = capacity;
}

static void append_byte(Buffer *buffer, uint8_t byte) {
    reserve(buffer, 1);
    buffer->data[buffer->size++] = byte;
}

static void append_bytes(Buffer *buffer, const uint8_t *data, size_t size) {
    reserve(buffer, size);
    if (size) memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
}

static void append_u32le(Buffer *buffer, uint32_t value) {
    reserve(buffer, 4);
    for (unsigned i = 0; i < 4; ++i) buffer->data[buffer->size++] = (uint8_t)(value >> (8u * i));
}

static void append_u16le(Buffer *buffer, uint16_t value) {
    append_byte(buffer, (uint8_t)value);
    append_byte(buffer, (uint8_t)(value >> 8));
}

static void append_u64le(Buffer *buffer, uint64_t value) {
    reserve(buffer, 8);
    for (unsigned i = 0; i < 8; ++i) buffer->data[buffer->size++] = (uint8_t)(value >> (8u * i));
}

static uint64_t read_u64le(const uint8_t *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8u * i);
    return value;
}

static uint32_t read_u32le(const uint8_t *p) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)p[i] << (8u * i);
    return value;
}

static uint16_t read_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t hash3(const uint8_t *p) {
    uint32_t x = (uint32_t)p[0] * 0x1e35a7bdu;
    x ^= (uint32_t)p[1] * 0x9e3779b1u;
    x ^= (uint32_t)p[2] * 0x85ebca77u;
    return (x ^ (x >> 16)) & (HASH_SIZE - 1u);
}

static uint32_t wide_hash4(const uint8_t *p) {
    uint32_t value = (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
                     ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
    value *= 0x9e3779b1u;
    value ^= value >> 15;
    value *= 0x85ebca6bu;
    return (value ^ (value >> 13)) & (WIDE_HASH_SIZE - 1u);
}

static void insert_position(const uint8_t *input, size_t input_size, size_t pos,
                            int32_t *heads, int32_t *previous) {
    if (pos + 2 >= input_size) return;
    uint32_t hash = hash3(input + pos);
    previous[pos] = heads[hash];
    heads[hash] = (int32_t)pos;
}

static void find_match(const uint8_t *input, size_t input_size, size_t pos,
                       const int32_t *heads, const int32_t *previous,
                       size_t *best_length, size_t *best_offset) {
    *best_length = 0;
    *best_offset = 0;
    if (pos + MIN_MATCH > input_size || pos + 2 >= input_size) return;
    size_t limit = input_size - pos;
    if (limit > MAX_MATCH) limit = MAX_MATCH;
    int32_t candidate = heads[hash3(input + pos)];
    unsigned depth = 0;
    while (candidate >= 0 && depth++ < MAX_CHAIN) {
        size_t c = (size_t)candidate;
        size_t offset = pos - c;
        if (offset > WINDOW) break;
        if (input[c + *best_length] == input[pos + *best_length]) {
            size_t length = 0;
            while (length < limit && input[c + length] == input[pos + length]) ++length;
            if (length > *best_length) {
                *best_length = length;
                *best_offset = offset;
                if (length == limit) break;
            }
        }
        candidate = previous[c];
    }
    if (*best_length < MIN_MATCH) {
        *best_length = 0;
        *best_offset = 0;
    }
}

static void find_costed_matches(const uint8_t *input, size_t input_size, size_t pos,
                                int32_t candidate, const int32_t *previous,
                                const uint16_t distance_low_costs[256],
                                const uint16_t distance_high_costs[256],
                                size_t *best_length, size_t *best_offset,
                                uint16_t offsets[ENTROPY_LENGTH_CHOICES],
                                int bucketed_costs) {
    *best_length = 0;
    *best_offset = 0;
    memset(offsets, 0, ENTROPY_LENGTH_CHOICES * sizeof(*offsets));
    if (pos + MIN_MATCH > input_size || pos + 2 >= input_size) return;
    size_t limit = input_size - pos;
    if (limit > MAX_MATCH) limit = MAX_MATCH;
    size_t candidate_lengths[MAX_CHAIN];
    size_t candidate_offsets[MAX_CHAIN];
    size_t candidate_count = 0;
    unsigned depth = 0;
    while (candidate >= 0 && depth++ < MAX_CHAIN) {
        size_t c = (size_t)candidate;
        size_t offset = pos - c;
        if (offset > WINDOW) break;
        size_t length = 0;
        while (length < limit && input[c + length] == input[pos + length]) ++length;
        if (length >= MIN_MATCH) {
            candidate_lengths[candidate_count] = length;
            candidate_offsets[candidate_count++] = offset;
            if (length > *best_length) *best_length = length;
        }
        candidate = previous[c];
    }
    if (*best_length < MIN_MATCH) {
        *best_length = 0;
        return;
    }
    size_t minimum = *best_length > ENTROPY_LENGTH_CHOICES - 1u
        ? *best_length - (ENTROPY_LENGTH_CHOICES - 1u) : MIN_MATCH;
    if (minimum < MIN_MATCH) minimum = MIN_MATCH;
    if (bucketed_costs) {
        size_t choice_count = *best_length - minimum + 1u;
        uint32_t bucket_costs[ENTROPY_LENGTH_CHOICES];
        uint16_t bucket_offsets[ENTROPY_LENGTH_CHOICES] = {0};
        for (size_t i = 0; i < choice_count; ++i) bucket_costs[i] = UINT32_MAX;
        for (size_t i = 0; i < candidate_count; ++i) {
            if (candidate_lengths[i] < minimum) continue;
            size_t bucket = candidate_lengths[i] - minimum;
            if (bucket >= choice_count) bucket = choice_count - 1u;
            uint16_t encoded = (uint16_t)(candidate_offsets[i] - 1u);
            uint32_t cost = distance_low_costs[(uint8_t)encoded] +
                            distance_high_costs[(uint8_t)(encoded >> 8)];
            if (cost < bucket_costs[bucket] ||
                (cost == bucket_costs[bucket] &&
                 candidate_offsets[i] < bucket_offsets[bucket])) {
                bucket_costs[bucket] = cost;
                bucket_offsets[bucket] = (uint16_t)candidate_offsets[i];
            }
        }
        uint32_t lowest_cost = UINT32_MAX;
        uint16_t selected_offset = 0;
        for (size_t reverse = choice_count; reverse > 0; --reverse) {
            size_t bucket = reverse - 1u;
            if (bucket_costs[bucket] < lowest_cost ||
                (bucket_costs[bucket] == lowest_cost &&
                 bucket_offsets[bucket] < selected_offset)) {
                lowest_cost = bucket_costs[bucket];
                selected_offset = bucket_offsets[bucket];
            }
            offsets[bucket] = selected_offset;
        }
        *best_offset = offsets[choice_count - 1u];
        return;
    }
    for (size_t length = minimum; length <= *best_length; ++length) {
        uint32_t lowest_cost = UINT32_MAX;
        size_t selected_offset = 0;
        for (size_t i = 0; i < candidate_count; ++i) {
            if (candidate_lengths[i] < length) continue;
            uint16_t encoded = (uint16_t)(candidate_offsets[i] - 1u);
            uint32_t cost = distance_low_costs[(uint8_t)encoded] +
                            distance_high_costs[(uint8_t)(encoded >> 8)];
            if (cost < lowest_cost ||
                (cost == lowest_cost && candidate_offsets[i] < selected_offset)) {
                lowest_cost = cost;
                selected_offset = candidate_offsets[i];
            }
        }
        offsets[length - minimum] = (uint16_t)selected_offset;
    }
    *best_offset = offsets[*best_length - minimum];
}

static Buffer compress_range(const uint8_t *input, size_t input_size,
                             size_t start, size_t decoded_size, size_t lazy_limit) {
    if (input_size > INT32_MAX) fail("input exceeds baseline 2 GiB limit");
    if (start > input_size || decoded_size != input_size - start) fail("invalid compression range");
    Buffer output = {0};
    reserve(&output, HEADER_SIZE + decoded_size / 2 + 64);
    append_byte(&output, 'B'); append_byte(&output, 'I');
    append_byte(&output, 'T'); append_byte(&output, 'E');
    append_byte(&output, 1); append_byte(&output, 0);
    append_byte(&output, 0); append_byte(&output, 0);
    append_u64le(&output, (uint64_t)decoded_size);

    int32_t *heads = (int32_t *)malloc(HASH_SIZE * sizeof(*heads));
    int32_t *previous = input_size ? (int32_t *)malloc(input_size * sizeof(*previous)) : NULL;
    if (!heads || (input_size && !previous)) fail("out of memory");
    for (size_t i = 0; i < HASH_SIZE; ++i) heads[i] = -1;
    for (size_t i = 0; i < start; ++i) insert_position(input, input_size, i, heads, previous);

    size_t pos = start;
    while (pos < input_size) {
        size_t control_pos = output.size;
        append_byte(&output, 0);
        uint8_t control = 0;
        for (unsigned bit = 0; bit < 8 && pos < input_size; ++bit) {
            size_t length, offset;
            find_match(input, input_size, pos, heads, previous, &length, &offset);
            int current_inserted = 0;
            if (length && length < lazy_limit && pos + 1 < input_size) {
                size_t next_length, next_offset;
                insert_position(input, input_size, pos, heads, previous);
                current_inserted = 1;
                find_match(input, input_size, pos + 1, heads, previous,
                           &next_length, &next_offset);
                (void)next_offset;
                if (next_length > length + 1) length = 0;
            }
            if (length) {
                control |= (uint8_t)(1u << bit);
                append_byte(&output, (uint8_t)((offset - 1) & 0xffu));
                append_byte(&output, (uint8_t)((offset - 1) >> 8));
                append_byte(&output, (uint8_t)(length - MIN_MATCH));
                for (size_t i = (size_t)current_inserted; i < length; ++i)
                    insert_position(input, input_size, pos + i, heads, previous);
                pos += length;
            } else {
                append_byte(&output, input[pos]);
                if (!current_inserted) insert_position(input, input_size, pos, heads, previous);
                ++pos;
            }
        }
        output.data[control_pos] = control;
    }
    free(previous);
    free(heads);
    return output;
}

static Buffer compress_buffer(const uint8_t *input, size_t input_size, int lazy) {
    return compress_range(input, input_size, 0, input_size, lazy ? MAX_MATCH : 0);
}

typedef struct {
    uint32_t frequency;
    int left;
    int right;
    int symbol;
    int min_symbol;
} HuffNode;

typedef struct {
    Buffer *output;
    uint8_t byte;
    unsigned used;
} BitWriter;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte;
    unsigned bit;
} BitReader;

typedef struct {
    int16_t symbol;
    int16_t node;
    uint8_t bits;
} HuffLookup;

#define HUFF_LOOKUP_BITS 8u
#define HUFF_LOOKUP_SIZE (1u << HUFF_LOOKUP_BITS)

static void write_bit(BitWriter *writer, unsigned bit) {
    writer->byte = (uint8_t)((writer->byte << 1) | (bit & 1u));
    if (++writer->used == 8) {
        append_byte(writer->output, writer->byte);
        writer->byte = 0;
        writer->used = 0;
    }
}

static void finish_bits(BitWriter *writer) {
    if (writer->used) {
        writer->byte = (uint8_t)(writer->byte << (8u - writer->used));
        append_byte(writer->output, writer->byte);
        writer->byte = 0;
        writer->used = 0;
    }
}

static int read_bit(BitReader *reader) {
    if (reader->byte >= reader->size) return -1;
    int bit = (reader->data[reader->byte] >> (7u - reader->bit)) & 1u;
    if (++reader->bit == 8) {
        reader->bit = 0;
        ++reader->byte;
    }
    return bit;
}

static int peek_bits(const BitReader *reader, unsigned count, uint32_t *value) {
    size_t remaining = (reader->size - reader->byte) * 8u - reader->bit;
    if (remaining < count || count > 16) return 0;
    uint32_t window = 0;
    for (unsigned i = 0; i < 3; ++i) {
        window <<= 8;
        if (reader->byte + i < reader->size) window |= reader->data[reader->byte + i];
    }
    *value = (window >> (24u - reader->bit - count)) & ((1u << count) - 1u);
    return 1;
}

static void skip_bits(BitReader *reader, unsigned count) {
    unsigned total = reader->bit + count;
    reader->byte += total / 8u;
    reader->bit = total % 8u;
}

static int huff_less(const HuffNode *nodes, int a, int b) {
    if (nodes[a].frequency != nodes[b].frequency)
        return nodes[a].frequency < nodes[b].frequency;
    if (nodes[a].min_symbol != nodes[b].min_symbol)
        return nodes[a].min_symbol < nodes[b].min_symbol;
    return a < b;
}

static void heap_push(int *heap, int *size, int node, const HuffNode *nodes) {
    int pos = (*size)++;
    while (pos > 0) {
        int parent = (pos - 1) / 2;
        if (!huff_less(nodes, node, heap[parent])) break;
        heap[pos] = heap[parent];
        pos = parent;
    }
    heap[pos] = node;
}

static int heap_pop(int *heap, int *size, const HuffNode *nodes) {
    int result = heap[0];
    int replacement = heap[--(*size)];
    int pos = 0;
    while (pos * 2 + 1 < *size) {
        int child = pos * 2 + 1;
        if (child + 1 < *size && huff_less(nodes, heap[child + 1], heap[child])) ++child;
        if (!huff_less(nodes, heap[child], replacement)) break;
        heap[pos] = heap[child];
        pos = child;
    }
    if (*size) heap[pos] = replacement;
    return result;
}

static int build_huffman_tree(const uint8_t *input, size_t input_size,
                              HuffNode *nodes, int *node_count) {
    uint32_t frequencies[256] = {0};
    for (size_t i = 0; i < input_size; ++i) ++frequencies[input[i]];
    int heap[512];
    int heap_size = 0;
    *node_count = 0;
    for (int symbol = 0; symbol < 256; ++symbol) {
        if (!frequencies[symbol]) continue;
        int index = (*node_count)++;
        nodes[index] = (HuffNode){frequencies[symbol], -1, -1, symbol, symbol};
        heap_push(heap, &heap_size, index, nodes);
    }
    if (heap_size == 0) return -1;
    while (heap_size > 1) {
        int left = heap_pop(heap, &heap_size, nodes);
        int right = heap_pop(heap, &heap_size, nodes);
        int index = (*node_count)++;
        int min_symbol = nodes[left].min_symbol < nodes[right].min_symbol
            ? nodes[left].min_symbol : nodes[right].min_symbol;
        nodes[index] = (HuffNode){nodes[left].frequency + nodes[right].frequency,
                                 left, right, -1, min_symbol};
        heap_push(heap, &heap_size, index, nodes);
    }
    return heap_pop(heap, &heap_size, nodes);
}

static void serialize_tree(const HuffNode *nodes, int node, BitWriter *writer) {
    if (nodes[node].symbol >= 0) {
        write_bit(writer, 1);
        for (int bit = 7; bit >= 0; --bit) write_bit(writer, (unsigned)nodes[node].symbol >> bit);
        return;
    }
    write_bit(writer, 0);
    serialize_tree(nodes, nodes[node].left, writer);
    serialize_tree(nodes, nodes[node].right, writer);
}

static void build_huffman_codes(const HuffNode *nodes, int node, uint8_t *path,
                                uint16_t depth, uint8_t codes[256][256],
                                uint16_t lengths[256]) {
    if (nodes[node].symbol >= 0) {
        int symbol = nodes[node].symbol;
        lengths[symbol] = depth;
        if (depth) memcpy(codes[symbol], path, depth);
        return;
    }
    path[depth] = 0;
    build_huffman_codes(nodes, nodes[node].left, path, (uint16_t)(depth + 1), codes, lengths);
    path[depth] = 1;
    build_huffman_codes(nodes, nodes[node].right, path, (uint16_t)(depth + 1), codes, lengths);
}

static Buffer huffman_encode(const uint8_t *input, size_t input_size) {
    if (input_size > UINT32_MAX) fail("Huffman input too large");
    Buffer output = {0};
    append_u32le(&output, (uint32_t)input_size);
    if (!input_size) {
        append_u16le(&output, 0);
        return output;
    }
    HuffNode nodes[511];
    int node_count;
    int root = build_huffman_tree(input, input_size, nodes, &node_count);
    (void)node_count;
    Buffer tree = {0};
    BitWriter tree_writer = {&tree, 0, 0};
    serialize_tree(nodes, root, &tree_writer);
    finish_bits(&tree_writer);
    if (tree.size > UINT16_MAX) fail("Huffman tree too large");
    append_u16le(&output, (uint16_t)tree.size);
    append_bytes(&output, tree.data, tree.size);

    uint8_t codes[256][256] = {{0}};
    uint16_t lengths[256] = {0};
    uint8_t path[256];
    build_huffman_codes(nodes, root, path, 0, codes, lengths);
    BitWriter data_writer = {&output, 0, 0};
    if (nodes[root].symbol < 0) {
        for (size_t i = 0; i < input_size; ++i) {
            uint8_t symbol = input[i];
            for (uint16_t bit = 0; bit < lengths[symbol]; ++bit)
                write_bit(&data_writer, codes[symbol][bit]);
        }
    }
    finish_bits(&data_writer);
    free(tree.data);
    return output;
}

static int deserialize_tree(BitReader *reader, HuffNode *nodes, int *node_count,
                            uint8_t seen[256]) {
    if (*node_count >= 511) return -1;
    int marker = read_bit(reader);
    if (marker < 0) return -1;
    int index = (*node_count)++;
    if (marker) {
        int symbol = 0;
        for (int i = 0; i < 8; ++i) {
            int bit = read_bit(reader);
            if (bit < 0) return -1;
            symbol = (symbol << 1) | bit;
        }
        if (seen[symbol]) return -1;
        seen[symbol] = 1;
        nodes[index] = (HuffNode){0, -1, -1, symbol, symbol};
        return index;
    }
    int left = deserialize_tree(reader, nodes, node_count, seen);
    int right = deserialize_tree(reader, nodes, node_count, seen);
    if (left < 0 || right < 0) return -1;
    nodes[index] = (HuffNode){0, left, right, -1, 0};
    return index;
}

static void build_huffman_lookup(const HuffNode *nodes, int root,
                                 HuffLookup table[HUFF_LOOKUP_SIZE]) {
    for (unsigned prefix = 0; prefix < HUFF_LOOKUP_SIZE; ++prefix) {
        int node = root;
        unsigned used = 0;
        while (used < HUFF_LOOKUP_BITS && nodes[node].symbol < 0) {
            unsigned bit = (prefix >> (HUFF_LOOKUP_BITS - 1u - used)) & 1u;
            node = bit ? nodes[node].right : nodes[node].left;
            ++used;
        }
        if (nodes[node].symbol >= 0)
            table[prefix] = (HuffLookup){(int16_t)nodes[node].symbol, -1, (uint8_t)used};
        else
            table[prefix] = (HuffLookup){-1, (int16_t)node, HUFF_LOOKUP_BITS};
    }
}

static int decode_huffman_symbol(BitReader *reader, const HuffNode *nodes, int root,
                                 const HuffLookup table[HUFF_LOOKUP_SIZE], int use_lookup) {
    int node = root;
    if (use_lookup) {
        uint32_t prefix;
        if (peek_bits(reader, HUFF_LOOKUP_BITS, &prefix)) {
            HuffLookup entry = table[prefix];
            skip_bits(reader, entry.bits);
            if (entry.symbol >= 0) return entry.symbol;
            node = entry.node;
        }
    }
    while (nodes[node].symbol < 0) {
        int bit = read_bit(reader);
        if (bit < 0) return -1;
        node = bit ? nodes[node].right : nodes[node].left;
    }
    return nodes[node].symbol;
}

static Buffer huffman_decode(const uint8_t *input, size_t input_size, int fast_lookup) {
    if (input_size < 6) fail("truncated Huffman header");
    size_t decoded_size = read_u32le(input);
    size_t tree_size = read_u16le(input + 4);
    if (decoded_size > WHOLE_BLOCK_SIZE * 2u || tree_size > input_size - 6)
        fail("invalid Huffman header");
    if (decoded_size == 0) {
        if (tree_size != 0 || input_size != 6) fail("invalid empty Huffman stream");
        Buffer empty = {0};
        return empty;
    }
    if (tree_size == 0) fail("invalid Huffman header");
    HuffNode nodes[511];
    int node_count = 0;
    uint8_t seen[256] = {0};
    BitReader tree_reader = {input + 6, tree_size, 0, 0};
    int root = deserialize_tree(&tree_reader, nodes, &node_count, seen);
    if (root < 0) fail("invalid Huffman tree");
    int padding;
    while ((padding = read_bit(&tree_reader)) >= 0)
        if (padding) fail("invalid Huffman tree padding");

    Buffer output = {0};
    reserve(&output, decoded_size ? decoded_size : 1);
    BitReader data_reader = {input + 6 + tree_size, input_size - 6 - tree_size, 0, 0};
    if (nodes[root].symbol >= 0) {
        while (output.size < decoded_size) append_byte(&output, (uint8_t)nodes[root].symbol);
    } else {
        HuffLookup table[HUFF_LOOKUP_SIZE];
        int use_lookup = fast_lookup && decoded_size >= HUFF_LOOKUP_SIZE;
        if (use_lookup && fast_lookup == 2) {
            size_t data_size = input_size - 6 - tree_size;
            uint64_t estimated_tree_edges = (uint64_t)data_size * 8u;
            uint64_t lookup_overhead = (uint64_t)decoded_size +
                                       HUFF_LOOKUP_SIZE * HUFF_LOOKUP_BITS;
            use_lookup = estimated_tree_edges > lookup_overhead;
        }
        if (use_lookup) build_huffman_lookup(nodes, root, table);
        while (output.size < decoded_size) {
            int symbol = decode_huffman_symbol(&data_reader, nodes, root, table, use_lookup);
            if (symbol < 0) fail("truncated Huffman data");
            append_byte(&output, (uint8_t)symbol);
        }
    }
    while ((padding = read_bit(&data_reader)) >= 0)
        if (padding) fail("invalid Huffman data padding");
    return output;
}

static size_t build_canonical_codes(const uint16_t lengths[256],
                                    uint8_t codes[256][256],
                                    uint8_t order[256]) {
    size_t count = 0;
    for (unsigned symbol = 0; symbol < 256; ++symbol)
        if (lengths[symbol]) order[count++] = (uint8_t)symbol;
    for (size_t i = 1; i < count; ++i) {
        uint8_t key = order[i];
        size_t j = i;
        while (j && (lengths[order[j - 1u]] > lengths[key] ||
                     (lengths[order[j - 1u]] == lengths[key] && order[j - 1u] > key))) {
            order[j] = order[j - 1u];
            --j;
        }
        order[j] = key;
    }
    if (!count) return 0;
    uint8_t current[256] = {0};
    uint16_t current_length = lengths[order[0]];
    memcpy(codes[order[0]], current, current_length);
    for (size_t i = 1; i < count; ++i) {
        int bit = (int)current_length - 1;
        while (bit >= 0 && current[bit]) current[bit--] = 0;
        if (bit < 0) fail("invalid canonical Huffman lengths");
        current[bit] = 1;
        uint16_t next_length = lengths[order[i]];
        if (next_length < current_length) fail("unsorted canonical Huffman lengths");
        memset(current + current_length, 0, next_length - current_length);
        current_length = next_length;
        memcpy(codes[order[i]], current, current_length);
    }
    return count;
}

static Buffer canonical_length_model(const uint16_t lengths[256]) {
    Buffer packed = {0};
    append_byte(&packed, 0);
    for (unsigned pair = 0; pair < 128; ++pair) {
        uint16_t first = lengths[pair * 2u];
        uint16_t second = lengths[pair * 2u + 1u];
        unsigned high = first >= 15 ? 15u : first;
        unsigned low = second >= 15 ? 15u : second;
        append_byte(&packed, (uint8_t)((high << 4) | low));
    }
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        if (lengths[symbol] >= 15) {
            if (lengths[symbol] > UINT8_MAX) fail("canonical Huffman code too long");
            append_byte(&packed, (uint8_t)lengths[symbol]);
        }
    }

    Buffer sparse = {0};
    append_byte(&sparse, 1);
    unsigned count = 0;
    for (unsigned symbol = 0; symbol < 256; ++symbol) count += lengths[symbol] != 0;
    append_u16le(&sparse, (uint16_t)count);
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        if (!lengths[symbol]) continue;
        if (lengths[symbol] > UINT8_MAX) fail("canonical Huffman code too long");
        append_byte(&sparse, (uint8_t)symbol);
        append_byte(&sparse, (uint8_t)lengths[symbol]);
    }
    if (sparse.size < packed.size) {
        free(packed.data);
        return sparse;
    }
    free(sparse.data);
    return packed;
}

static Buffer canonical_huffman_encode(const uint8_t *input, size_t input_size) {
    if (!input_size || input_size > UINT32_MAX) fail("invalid canonical Huffman input size");
    HuffNode nodes[511];
    int node_count;
    int root = build_huffman_tree(input, input_size, nodes, &node_count);
    (void)node_count;
    uint8_t codes[256][256] = {{0}};
    uint16_t lengths[256] = {0};
    uint8_t path[256];
    build_huffman_codes(nodes, root, path, 0, codes, lengths);
    if (nodes[root].symbol >= 0) lengths[nodes[root].symbol] = 1;
    uint8_t order[256];
    size_t symbol_count = build_canonical_codes(lengths, codes, order);
    if (!symbol_count) fail("empty canonical Huffman model");
    Buffer model = canonical_length_model(lengths);
    if (model.size > UINT16_MAX) fail("canonical Huffman model too large");

    Buffer output = {0};
    append_u32le(&output, (uint32_t)input_size);
    append_u16le(&output, (uint16_t)model.size);
    append_bytes(&output, model.data, model.size);
    if (symbol_count > 1) {
        BitWriter writer = {&output, 0, 0};
        for (size_t i = 0; i < input_size; ++i) {
            uint8_t symbol = input[i];
            for (uint16_t bit = 0; bit < lengths[symbol]; ++bit)
                write_bit(&writer, codes[symbol][bit]);
        }
        finish_bits(&writer);
    }
    free(model.data);
    return output;
}

static size_t parse_canonical_length_model(const uint8_t *input, size_t input_size,
                                           uint16_t lengths[256]) {
    if (!input_size) fail("empty canonical Huffman model");
    size_t count = 0;
    if (input[0] == 0) {
        if (input_size < 129) fail("truncated packed canonical Huffman model");
        for (unsigned pair = 0; pair < 128; ++pair) {
            lengths[pair * 2u] = input[1u + pair] >> 4;
            lengths[pair * 2u + 1u] = input[1u + pair] & 15u;
        }
        size_t offset = 129;
        for (unsigned symbol = 0; symbol < 256; ++symbol) {
            if (lengths[symbol] == 15) {
                if (offset >= input_size || input[offset] < 15)
                    fail("invalid packed canonical Huffman escape");
                lengths[symbol] = input[offset++];
            }
            count += lengths[symbol] != 0;
        }
        if (offset != input_size) fail("trailing packed canonical Huffman model");
    } else if (input[0] == 1) {
        if (input_size < 3) fail("truncated sparse canonical Huffman model");
        size_t stored_count = read_u16le(input + 1);
        if (!stored_count || stored_count > 256 || input_size != 3u + 2u * stored_count)
            fail("invalid sparse canonical Huffman model");
        for (size_t i = 0; i < stored_count; ++i) {
            uint8_t symbol = input[3u + 2u * i];
            uint8_t length = input[4u + 2u * i];
            if (!length || lengths[symbol]) fail("invalid sparse canonical Huffman symbol");
            lengths[symbol] = length;
        }
        count = stored_count;
    } else {
        fail("unknown canonical Huffman model");
    }
    if (!count) fail("empty canonical Huffman alphabet");
    return count;
}

static int validate_canonical_tree(const HuffNode *nodes, int node) {
    if (nodes[node].symbol >= 0) return 1;
    if (nodes[node].left < 0 || nodes[node].right < 0) return 0;
    return validate_canonical_tree(nodes, nodes[node].left) &&
           validate_canonical_tree(nodes, nodes[node].right);
}

static Buffer canonical_huffman_decode(const uint8_t *input, size_t input_size,
                                       int fast_lookup) {
    if (input_size < 7) fail("truncated canonical Huffman header");
    size_t decoded_size = read_u32le(input);
    size_t model_size = read_u16le(input + 4);
    if (!decoded_size || decoded_size > WHOLE_BLOCK_SIZE * 2u || model_size > input_size - 6u)
        fail("invalid canonical Huffman header");
    uint16_t lengths[256] = {0};
    size_t symbol_count = parse_canonical_length_model(input + 6, model_size, lengths);
    uint8_t codes[256][256] = {{0}};
    uint8_t order[256];
    if (build_canonical_codes(lengths, codes, order) != symbol_count)
        fail("inconsistent canonical Huffman model");

    HuffNode nodes[511];
    int node_count = 0;
    int root;
    if (symbol_count == 1) {
        root = node_count++;
        nodes[root] = (HuffNode){0, -1, -1, order[0], order[0]};
    } else {
        root = node_count++;
        nodes[root] = (HuffNode){0, -1, -1, -1, 0};
        for (size_t i = 0; i < symbol_count; ++i) {
            uint8_t symbol = order[i];
            int node = root;
            for (uint16_t bit = 0; bit < lengths[symbol]; ++bit) {
                int *child = codes[symbol][bit] ? &nodes[node].right : &nodes[node].left;
                int final = bit + 1u == lengths[symbol];
                if (*child < 0) {
                    if (node_count >= 511) fail("canonical Huffman tree too large");
                    *child = node_count++;
                    nodes[*child] = final
                        ? (HuffNode){0, -1, -1, symbol, symbol}
                        : (HuffNode){0, -1, -1, -1, 0};
                } else if (final || nodes[*child].symbol >= 0) {
                    fail("non-prefix-free canonical Huffman model");
                }
                node = *child;
            }
        }
        if (!validate_canonical_tree(nodes, root)) fail("incomplete canonical Huffman model");
    }

    Buffer output = {0};
    reserve(&output, decoded_size);
    BitReader reader = {input + 6 + model_size, input_size - 6 - model_size, 0, 0};
    if (symbol_count == 1) {
        if (reader.size) fail("unexpected single-symbol canonical Huffman data");
        while (output.size < decoded_size) append_byte(&output, (uint8_t)nodes[root].symbol);
        return output;
    }
    HuffLookup table[HUFF_LOOKUP_SIZE];
    int use_lookup = fast_lookup && decoded_size >= HUFF_LOOKUP_SIZE;
    if (use_lookup && fast_lookup == 2) {
        uint64_t estimated_tree_edges = (uint64_t)reader.size * 8u;
        uint64_t lookup_overhead = (uint64_t)decoded_size +
                                   HUFF_LOOKUP_SIZE * HUFF_LOOKUP_BITS;
        use_lookup = estimated_tree_edges > lookup_overhead;
    }
    if (use_lookup) build_huffman_lookup(nodes, root, table);
    while (output.size < decoded_size) {
        int symbol = decode_huffman_symbol(&reader, nodes, root, table, use_lookup);
        if (symbol < 0) fail("truncated canonical Huffman data");
        append_byte(&output, (uint8_t)symbol);
    }
    int padding;
    while ((padding = read_bit(&reader)) >= 0)
        if (padding) fail("invalid canonical Huffman data padding");
    return output;
}

static Buffer compact_huffman_encode(const uint8_t *input, size_t input_size) {
    Buffer ordinary = huffman_encode(input, input_size);
    Buffer output = {0};
    if (!input_size) {
        append_byte(&output, 0);
        append_bytes(&output, ordinary.data, ordinary.size);
        free(ordinary.data);
        return output;
    }
    Buffer canonical = canonical_huffman_encode(input, input_size);
    if (canonical.size < ordinary.size) {
        append_byte(&output, 1);
        append_bytes(&output, canonical.data, canonical.size);
    } else {
        append_byte(&output, 0);
        append_bytes(&output, ordinary.data, ordinary.size);
    }
    free(canonical.data);
    free(ordinary.data);
    return output;
}

static Buffer compact_huffman_decode(const uint8_t *input, size_t input_size,
                                     int fast_lookup) {
    if (!input_size) fail("truncated compact Huffman stream");
    if (input[0] == 0) return huffman_decode(input + 1, input_size - 1, fast_lookup);
    if (input[0] == 1) return canonical_huffman_decode(input + 1, input_size - 1, fast_lookup);
    fail("unknown compact Huffman stream");
    return (Buffer){0};
}

#define ARITHMETIC_HALF 0x80000000u
#define ARITHMETIC_QUARTER 0x40000000u
#define ARITHMETIC_THREE_QUARTERS 0xc0000000u
#define ARITHMETIC_MAX_TOTAL 16384u

typedef struct {
    uint16_t frequencies[256];
    uint32_t tree[257];
    uint32_t total;
} ArithmeticModel;

static void arithmetic_model_rebuild(ArithmeticModel *model) {
    memset(model->tree, 0, sizeof(model->tree));
    model->total = 0;
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        uint32_t frequency = model->frequencies[symbol];
        model->total += frequency;
        for (unsigned index = symbol + 1u; index <= 256u; index += index & -index)
            model->tree[index] += frequency;
    }
}

static void arithmetic_model_init(ArithmeticModel *model) {
    for (unsigned symbol = 0; symbol < 256; ++symbol) model->frequencies[symbol] = 1;
    arithmetic_model_rebuild(model);
}

static void arithmetic_small_model_init(ArithmeticModel *model,
                                        unsigned alphabet_size) {
    memset(model->frequencies, 0, sizeof(model->frequencies));
    for (unsigned symbol = 0; symbol < alphabet_size; ++symbol)
        model->frequencies[symbol] = 1;
    arithmetic_model_rebuild(model);
}

static uint32_t arithmetic_model_prefix(const ArithmeticModel *model, unsigned symbol) {
    uint32_t result = 0;
    for (unsigned index = symbol; index; index -= index & -index)
        result += model->tree[index];
    return result;
}

static unsigned arithmetic_model_symbol(const ArithmeticModel *model, uint32_t target,
                                        uint32_t *cumulative) {
    unsigned index = 0;
    uint32_t sum = 0;
    for (unsigned step = 256u; step; step >>= 1u) {
        unsigned next = index + step;
        if (next <= 256u && sum + model->tree[next] <= target) {
            index = next;
            sum += model->tree[next];
        }
    }
    if (index >= 256u) fail("invalid arithmetic symbol");
    *cumulative = sum;
    return index;
}

static void arithmetic_model_update(ArithmeticModel *model, unsigned symbol) {
    ++model->frequencies[symbol];
    ++model->total;
    for (unsigned index = symbol + 1u; index <= 256u; index += index & -index)
        ++model->tree[index];
    if (model->total < ARITHMETIC_MAX_TOTAL) return;
    for (unsigned i = 0; i < 256; ++i)
        model->frequencies[i] = (uint16_t)((model->frequencies[i] + 1u) / 2u);
    arithmetic_model_rebuild(model);
}

static void arithmetic_write_follow(BitWriter *writer, unsigned bit, size_t *pending) {
    write_bit(writer, bit);
    while (*pending) {
        write_bit(writer, bit ^ 1u);
        --*pending;
    }
}

static Buffer adaptive_arithmetic_encode(const uint8_t *input, size_t input_size) {
    if (input_size > UINT32_MAX) fail("arithmetic stream too large");
    Buffer output = {0};
    append_u32le(&output, (uint32_t)input_size);
    BitWriter writer = {&output, 0, 0};
    ArithmeticModel model;
    arithmetic_model_init(&model);
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    size_t pending = 0;
    for (size_t i = 0; i < input_size; ++i) {
        unsigned symbol = input[i];
        uint32_t cumulative = arithmetic_model_prefix(&model, symbol);
        uint64_t range = (uint64_t)high - low + 1u;
        high = low + (uint32_t)(range * (cumulative + model.frequencies[symbol]) /
                                model.total - 1u);
        low = low + (uint32_t)(range * cumulative / model.total);
        for (;;) {
            if (high < ARITHMETIC_HALF) {
                arithmetic_write_follow(&writer, 0, &pending);
            } else if (low >= ARITHMETIC_HALF) {
                arithmetic_write_follow(&writer, 1, &pending);
                low -= ARITHMETIC_HALF;
                high -= ARITHMETIC_HALF;
            } else if (low >= ARITHMETIC_QUARTER && high < ARITHMETIC_THREE_QUARTERS) {
                ++pending;
                low -= ARITHMETIC_QUARTER;
                high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            low <<= 1u;
            high = (high << 1u) | 1u;
        }
        arithmetic_model_update(&model, symbol);
    }
    ++pending;
    arithmetic_write_follow(&writer, low < ARITHMETIC_QUARTER ? 0u : 1u, &pending);
    finish_bits(&writer);
    append_u32le(&output, 0);
    return output;
}

static Buffer adaptive_arithmetic_decode(const uint8_t *input, size_t input_size) {
    if (input_size < 8) fail("truncated arithmetic stream");
    size_t decoded_size = read_u32le(input);
    if (decoded_size > WHOLE_BLOCK_SIZE * 2u) fail("invalid arithmetic stream size");
    Buffer output = {0};
    reserve(&output, decoded_size ? decoded_size : 1);
    BitReader reader = {input + 4, input_size - 4, 0, 0};
    uint32_t code = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        int value = read_bit(&reader);
        if (value < 0) fail("truncated arithmetic code");
        code = (code << 1u) | (unsigned)value;
    }
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    ArithmeticModel model;
    arithmetic_model_init(&model);
    while (output.size < decoded_size) {
        uint64_t range = (uint64_t)high - low + 1u;
        uint32_t target = (uint32_t)((((uint64_t)(code - low) + 1u) * model.total - 1u) /
                                     range);
        if (target >= model.total) fail("invalid arithmetic code");
        uint32_t cumulative;
        unsigned symbol = arithmetic_model_symbol(&model, target, &cumulative);
        high = low + (uint32_t)(range * (cumulative + model.frequencies[symbol]) /
                                model.total - 1u);
        low = low + (uint32_t)(range * cumulative / model.total);
        for (;;) {
            if (high < ARITHMETIC_HALF) {
            } else if (low >= ARITHMETIC_HALF) {
                code -= ARITHMETIC_HALF;
                low -= ARITHMETIC_HALF;
                high -= ARITHMETIC_HALF;
            } else if (low >= ARITHMETIC_QUARTER && high < ARITHMETIC_THREE_QUARTERS) {
                code -= ARITHMETIC_QUARTER;
                low -= ARITHMETIC_QUARTER;
                high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            low <<= 1u;
            high = (high << 1u) | 1u;
            int bit = read_bit(&reader);
            if (bit < 0) fail("truncated arithmetic code");
            code = (code << 1u) | (unsigned)bit;
        }
        append_byte(&output, (uint8_t)symbol);
        arithmetic_model_update(&model, symbol);
    }
    return output;
}

static ArithmeticModel *arithmetic_context_model(ArithmeticModel *models,
                                                 uint8_t initialized[256],
                                                 unsigned context) {
    if (!initialized[context]) {
        arithmetic_model_init(&models[context]);
        initialized[context] = 1;
    }
    return &models[context];
}

static Buffer adaptive_order1_arithmetic_encode(const uint8_t *input, size_t input_size) {
    if (input_size > UINT32_MAX) fail("order-1 arithmetic stream too large");
    Buffer output = {0};
    append_u32le(&output, (uint32_t)input_size);
    BitWriter writer = {&output, 0, 0};
    ArithmeticModel *models = (ArithmeticModel *)calloc(256, sizeof(*models));
    uint8_t initialized[256] = {0};
    if (!models) fail("out of memory");
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    size_t pending = 0;
    unsigned context = 0;
    for (size_t i = 0; i < input_size; ++i) {
        unsigned symbol = input[i];
        ArithmeticModel *model = arithmetic_context_model(models, initialized, context);
        uint32_t cumulative = arithmetic_model_prefix(model, symbol);
        uint64_t range = (uint64_t)high - low + 1u;
        high = low + (uint32_t)(range * (cumulative + model->frequencies[symbol]) /
                                model->total - 1u);
        low = low + (uint32_t)(range * cumulative / model->total);
        for (;;) {
            if (high < ARITHMETIC_HALF) {
                arithmetic_write_follow(&writer, 0, &pending);
            } else if (low >= ARITHMETIC_HALF) {
                arithmetic_write_follow(&writer, 1, &pending);
                low -= ARITHMETIC_HALF;
                high -= ARITHMETIC_HALF;
            } else if (low >= ARITHMETIC_QUARTER && high < ARITHMETIC_THREE_QUARTERS) {
                ++pending;
                low -= ARITHMETIC_QUARTER;
                high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            low <<= 1u;
            high = (high << 1u) | 1u;
        }
        arithmetic_model_update(model, symbol);
        context = symbol;
    }
    ++pending;
    arithmetic_write_follow(&writer, low < ARITHMETIC_QUARTER ? 0u : 1u, &pending);
    finish_bits(&writer);
    append_u32le(&output, 0);
    free(models);
    return output;
}

static Buffer adaptive_order1_arithmetic_decode(const uint8_t *input, size_t input_size) {
    if (input_size < 8) fail("truncated order-1 arithmetic stream");
    size_t decoded_size = read_u32le(input);
    if (decoded_size > WHOLE_BLOCK_SIZE * 2u) fail("invalid order-1 arithmetic stream size");
    Buffer output = {0};
    reserve(&output, decoded_size ? decoded_size : 1);
    BitReader reader = {input + 4, input_size - 4, 0, 0};
    uint32_t code = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        int value = read_bit(&reader);
        if (value < 0) fail("truncated order-1 arithmetic code");
        code = (code << 1u) | (unsigned)value;
    }
    ArithmeticModel *models = (ArithmeticModel *)calloc(256, sizeof(*models));
    uint8_t initialized[256] = {0};
    if (!models) fail("out of memory");
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    unsigned context = 0;
    while (output.size < decoded_size) {
        ArithmeticModel *model = arithmetic_context_model(models, initialized, context);
        uint64_t range = (uint64_t)high - low + 1u;
        uint32_t target = (uint32_t)((((uint64_t)(code - low) + 1u) * model->total - 1u) /
                                     range);
        if (target >= model->total) fail("invalid order-1 arithmetic code");
        uint32_t cumulative;
        unsigned symbol = arithmetic_model_symbol(model, target, &cumulative);
        high = low + (uint32_t)(range * (cumulative + model->frequencies[symbol]) /
                                model->total - 1u);
        low = low + (uint32_t)(range * cumulative / model->total);
        for (;;) {
            if (high < ARITHMETIC_HALF) {
            } else if (low >= ARITHMETIC_HALF) {
                code -= ARITHMETIC_HALF;
                low -= ARITHMETIC_HALF;
                high -= ARITHMETIC_HALF;
            } else if (low >= ARITHMETIC_QUARTER && high < ARITHMETIC_THREE_QUARTERS) {
                code -= ARITHMETIC_QUARTER;
                low -= ARITHMETIC_QUARTER;
                high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            low <<= 1u;
            high = (high << 1u) | 1u;
            int bit = read_bit(&reader);
            if (bit < 0) fail("truncated order-1 arithmetic code");
            code = (code << 1u) | (unsigned)bit;
        }
        append_byte(&output, (uint8_t)symbol);
        arithmetic_model_update(model, symbol);
        context = symbol;
    }
    free(models);
    return output;
}

static size_t small_alphabet_context_count(unsigned alphabet_size) {
    size_t count = 1;
    for (unsigned i = 0; i < 4; ++i) count *= alphabet_size;
    return count;
}

static Buffer small_alphabet_order4_encode(const uint8_t *input, size_t input_size) {
    Buffer output = {0};
    if (input_size < 1024u) return output;
    uint8_t present[256] = {0};
    unsigned alphabet_size = 0;
    for (size_t i = 0; i < input_size; ++i)
        if (!present[input[i]]) {
            present[input[i]] = 1;
            if (++alphabet_size > 8u) return output;
        }
    if (!alphabet_size) return output;
    uint8_t alphabet[8];
    uint8_t ranks[256] = {0};
    unsigned rank = 0;
    for (unsigned symbol = 0; symbol < 256; ++symbol)
        if (present[symbol]) {
            alphabet[rank] = (uint8_t)symbol;
            ranks[symbol] = (uint8_t)rank++;
        }
    append_byte(&output, (uint8_t)alphabet_size);
    append_bytes(&output, alphabet, alphabet_size);
    if (alphabet_size == 1u) return output;

    size_t context_count = small_alphabet_context_count(alphabet_size);
    ArithmeticModel *models = (ArithmeticModel *)calloc(context_count, sizeof(*models));
    uint8_t *initialized = (uint8_t *)calloc(context_count, 1);
    if (!models || !initialized) fail("out of memory");
    BitWriter writer = {&output, 0, 0};
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    size_t pending = 0;
    size_t context = 0;
    for (size_t i = 0; i < input_size; ++i) {
        unsigned symbol = ranks[input[i]];
        ArithmeticModel *model = &models[context];
        if (!initialized[context]) {
            arithmetic_small_model_init(model, alphabet_size);
            initialized[context] = 1;
        }
        uint32_t cumulative = arithmetic_model_prefix(model, symbol);
        uint64_t range = (uint64_t)high - low + 1u;
        high = low + (uint32_t)(range *
            (cumulative + model->frequencies[symbol]) / model->total - 1u);
        low += (uint32_t)(range * cumulative / model->total);
        for (;;) {
            if (high < ARITHMETIC_HALF) {
                arithmetic_write_follow(&writer, 0, &pending);
            } else if (low >= ARITHMETIC_HALF) {
                arithmetic_write_follow(&writer, 1, &pending);
                low -= ARITHMETIC_HALF;
                high -= ARITHMETIC_HALF;
            } else if (low >= ARITHMETIC_QUARTER &&
                       high < ARITHMETIC_THREE_QUARTERS) {
                ++pending;
                low -= ARITHMETIC_QUARTER;
                high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            low <<= 1u;
            high = (high << 1u) | 1u;
        }
        arithmetic_model_update(model, symbol);
        context = (context * alphabet_size + symbol) % context_count;
    }
    ++pending;
    arithmetic_write_follow(&writer, low < ARITHMETIC_QUARTER ? 0u : 1u, &pending);
    finish_bits(&writer);
    append_u32le(&output, 0);
    free(initialized);
    free(models);
    return output;
}

static Buffer small_alphabet_order4_decode(const uint8_t *input, size_t input_size,
                                           size_t decoded_size) {
    if (!input_size) fail("truncated small-alphabet stream");
    unsigned alphabet_size = input[0];
    if (!alphabet_size || alphabet_size > 8u || input_size < 1u + alphabet_size)
        fail("invalid small-alphabet stream");
    const uint8_t *alphabet = input + 1u;
    for (unsigned i = 0; i < alphabet_size; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (alphabet[i] == alphabet[j]) fail("duplicate small-alphabet symbol");
    Buffer output = {0};
    reserve(&output, decoded_size ? decoded_size : 1);
    if (alphabet_size == 1u) {
        if (input_size != 2u) fail("invalid single-symbol stream");
        while (output.size < decoded_size) append_byte(&output, alphabet[0]);
        return output;
    }
    size_t header_size = 1u + alphabet_size;
    if (input_size - header_size < 8u) fail("truncated small-alphabet code");
    BitReader reader = {input + header_size, input_size - header_size, 0, 0};
    uint32_t code = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        int value = read_bit(&reader);
        if (value < 0) fail("truncated small-alphabet code");
        code = (code << 1u) | (unsigned)value;
    }
    size_t context_count = small_alphabet_context_count(alphabet_size);
    ArithmeticModel *models = (ArithmeticModel *)calloc(context_count, sizeof(*models));
    uint8_t *initialized = (uint8_t *)calloc(context_count, 1);
    if (!models || !initialized) fail("out of memory");
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    size_t context = 0;
    while (output.size < decoded_size) {
        ArithmeticModel *model = &models[context];
        if (!initialized[context]) {
            arithmetic_small_model_init(model, alphabet_size);
            initialized[context] = 1;
        }
        uint64_t range = (uint64_t)high - low + 1u;
        uint32_t target = (uint32_t)((((uint64_t)(code - low) + 1u) *
                                      model->total - 1u) / range);
        if (target >= model->total) fail("invalid small-alphabet code");
        uint32_t cumulative;
        unsigned symbol = arithmetic_model_symbol(model, target, &cumulative);
        if (symbol >= alphabet_size) fail("invalid small-alphabet symbol");
        high = low + (uint32_t)(range *
            (cumulative + model->frequencies[symbol]) / model->total - 1u);
        low += (uint32_t)(range * cumulative / model->total);
        for (;;) {
            if (high < ARITHMETIC_HALF) {
            } else if (low >= ARITHMETIC_HALF) {
                code -= ARITHMETIC_HALF;
                low -= ARITHMETIC_HALF;
                high -= ARITHMETIC_HALF;
            } else if (low >= ARITHMETIC_QUARTER &&
                       high < ARITHMETIC_THREE_QUARTERS) {
                code -= ARITHMETIC_QUARTER;
                low -= ARITHMETIC_QUARTER;
                high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            low <<= 1u;
            high = (high << 1u) | 1u;
            int bit = read_bit(&reader);
            if (bit < 0) fail("truncated small-alphabet code");
            code = (code << 1u) | (unsigned)bit;
        }
        append_byte(&output, alphabet[symbol]);
        arithmetic_model_update(model, symbol);
        context = (context * alphabet_size + symbol) % context_count;
    }
    free(initialized);
    free(models);
    return output;
}

static unsigned literal_arithmetic_context(uint8_t previous) {
    if (previous == ' ' || previous == '\n' || previous == '\r' || previous == '\t')
        return 0;
    if ((previous >= 'a' && previous <= 'z') ||
        (previous >= 'A' && previous <= 'Z'))
        return 1;
    if (previous >= '0' && previous <= '9') return 2;
    return 3;
}

static Buffer adaptive_context_arithmetic_encode(const uint8_t *input,
                                                 const uint8_t *contexts,
                                                 size_t input_size) {
    if (input_size > UINT32_MAX) fail("context arithmetic stream too large");
    Buffer output = {0};
    append_u32le(&output, (uint32_t)input_size);
    BitWriter writer = {&output, 0, 0};
    ArithmeticModel models[4];
    uint8_t initialized[4] = {0};
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    size_t pending = 0;
    for (size_t i = 0; i < input_size; ++i) {
        unsigned context = contexts[i];
        if (context >= 4) fail("invalid literal arithmetic context");
        if (!initialized[context]) {
            arithmetic_model_init(&models[context]);
            initialized[context] = 1;
        }
        ArithmeticModel *model = &models[context];
        unsigned symbol = input[i];
        uint32_t cumulative = arithmetic_model_prefix(model, symbol);
        uint64_t range = (uint64_t)high - low + 1u;
        high = low + (uint32_t)(range * (cumulative + model->frequencies[symbol]) /
                                model->total - 1u);
        low = low + (uint32_t)(range * cumulative / model->total);
        for (;;) {
            if (high < ARITHMETIC_HALF) {
                arithmetic_write_follow(&writer, 0, &pending);
            } else if (low >= ARITHMETIC_HALF) {
                arithmetic_write_follow(&writer, 1, &pending);
                low -= ARITHMETIC_HALF;
                high -= ARITHMETIC_HALF;
            } else if (low >= ARITHMETIC_QUARTER && high < ARITHMETIC_THREE_QUARTERS) {
                ++pending;
                low -= ARITHMETIC_QUARTER;
                high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            low <<= 1u;
            high = (high << 1u) | 1u;
        }
        arithmetic_model_update(model, symbol);
    }
    ++pending;
    arithmetic_write_follow(&writer, low < ARITHMETIC_QUARTER ? 0u : 1u, &pending);
    finish_bits(&writer);
    append_u32le(&output, 0);
    return output;
}

typedef struct {
    BitReader reader;
    ArithmeticModel models[4];
    uint8_t initialized[4];
    uint32_t low;
    uint32_t high;
    uint32_t code;
    size_t remaining;
} ContextArithmeticDecoder;

static ContextArithmeticDecoder context_arithmetic_decoder_init(const uint8_t *input,
                                                                 size_t input_size) {
    if (input_size < 8) fail("truncated context arithmetic stream");
    ContextArithmeticDecoder decoder = {0};
    decoder.remaining = read_u32le(input);
    if (decoder.remaining > WHOLE_BLOCK_SIZE * 2u) fail("invalid context arithmetic size");
    decoder.reader = (BitReader){input + 4, input_size - 4, 0, 0};
    decoder.high = UINT32_MAX;
    for (unsigned bit = 0; bit < 32; ++bit) {
        int value = read_bit(&decoder.reader);
        if (value < 0) fail("truncated context arithmetic code");
        decoder.code = (decoder.code << 1u) | (unsigned)value;
    }
    return decoder;
}

static uint8_t context_arithmetic_decode_symbol(ContextArithmeticDecoder *decoder,
                                                unsigned context) {
    if (!decoder->remaining || context >= 4) fail("invalid context arithmetic symbol");
    if (!decoder->initialized[context]) {
        arithmetic_model_init(&decoder->models[context]);
        decoder->initialized[context] = 1;
    }
    ArithmeticModel *model = &decoder->models[context];
    uint64_t range = (uint64_t)decoder->high - decoder->low + 1u;
    uint32_t target = (uint32_t)((((uint64_t)(decoder->code - decoder->low) + 1u) *
                                  model->total - 1u) / range);
    if (target >= model->total) fail("invalid context arithmetic code");
    uint32_t cumulative;
    unsigned symbol = arithmetic_model_symbol(model, target, &cumulative);
    decoder->high = decoder->low +
        (uint32_t)(range * (cumulative + model->frequencies[symbol]) /
                   model->total - 1u);
    decoder->low = decoder->low + (uint32_t)(range * cumulative / model->total);
    for (;;) {
        if (decoder->high < ARITHMETIC_HALF) {
        } else if (decoder->low >= ARITHMETIC_HALF) {
            decoder->code -= ARITHMETIC_HALF;
            decoder->low -= ARITHMETIC_HALF;
            decoder->high -= ARITHMETIC_HALF;
        } else if (decoder->low >= ARITHMETIC_QUARTER &&
                   decoder->high < ARITHMETIC_THREE_QUARTERS) {
            decoder->code -= ARITHMETIC_QUARTER;
            decoder->low -= ARITHMETIC_QUARTER;
            decoder->high -= ARITHMETIC_QUARTER;
        } else {
            break;
        }
        decoder->low <<= 1u;
        decoder->high = (decoder->high << 1u) | 1u;
        int bit = read_bit(&decoder->reader);
        if (bit < 0) fail("truncated context arithmetic code");
        decoder->code = (decoder->code << 1u) | (unsigned)bit;
    }
    arithmetic_model_update(model, symbol);
    --decoder->remaining;
    return (uint8_t)symbol;
}

static Buffer adaptive_context_arithmetic_decode(const uint8_t *input, size_t input_size,
                                                 const uint8_t *contexts,
                                                 size_t context_count) {
    ContextArithmeticDecoder decoder = context_arithmetic_decoder_init(input, input_size);
    if (decoder.remaining != context_count)
        fail("inconsistent context arithmetic size");
    Buffer output = {0};
    reserve(&output, context_count ? context_count : 1);
    for (size_t i = 0; i < context_count; ++i)
        append_byte(&output, context_arithmetic_decode_symbol(&decoder, contexts[i]));
    if (decoder.remaining) fail("unused context arithmetic symbols");
    return output;
}

typedef struct {
    uint16_t zero;
    uint16_t one;
} BinaryProbability;

#define HASHED_LITERAL_BITS 21u
#define HASHED_LITERAL_SIZE (1u << HASHED_LITERAL_BITS)
#define HASHED_LITERAL_MASK (HASHED_LITERAL_SIZE - 1u)
#define HASHED_WORD_BITS 21u
#define HASHED_WORD_SIZE (1u << HASHED_WORD_BITS)
#define HASHED_WORD_MASK (HASHED_WORD_SIZE - 1u)
#define HASHED_LITERAL_THRESHOLD 10u
#define HASHED_WORD_THRESHOLD 2u

typedef struct {
    uint32_t tag;
    BinaryProbability probability;
} HashedBinaryProbability;

typedef struct {
    uint64_t tag;
    BinaryProbability probability;
} HashedWordProbability;

static BinaryProbability *literal_bit_probability(BinaryProbability *models,
                                                  unsigned context,
                                                  unsigned node) {
    BinaryProbability *model = &models[context * 256u + node];
    if (!model->zero && !model->one) {
        model->zero = 1;
        model->one = 1;
    }
    return model;
}

static void binary_probability_update(BinaryProbability *model, unsigned bit) {
    if (bit) ++model->one;
    else ++model->zero;
    if ((uint32_t)model->zero + model->one >= ARITHMETIC_MAX_TOTAL) {
        model->zero = (uint16_t)((model->zero + 1u) / 2u);
        model->one = (uint16_t)((model->one + 1u) / 2u);
    }
}

static BinaryProbability *hashed_literal_probability(
        HashedBinaryProbability *models, unsigned previous,
        unsigned previous2, unsigned previous3, unsigned node) {
    uint32_t tag = (uint32_t)previous | ((uint32_t)previous2 << 8u) |
                   ((uint32_t)previous3 << 16u) | ((uint32_t)node << 24u);
    uint32_t mixed = tag * 0x9e3779b1u;
    mixed ^= mixed >> 16u;
    HashedBinaryProbability *entry = &models[mixed & HASHED_LITERAL_MASK];
    if (entry->tag != tag ||
        (!entry->probability.zero && !entry->probability.one)) {
        entry->tag = tag;
        entry->probability = (BinaryProbability){1, 1};
    }
    return &entry->probability;
}

static uint32_t literal_word_context(const uint8_t *input, size_t pos) {
    uint32_t hash = 2166136261u;
    unsigned length = 0;
    while (length < 4u && (size_t)length < pos) {
        unsigned byte = input[pos - 1u - length];
        int word_byte = (byte >= 'a' && byte <= 'z') ||
                        (byte >= 'A' && byte <= 'Z') ||
                        (byte >= '0' && byte <= '9') || byte == '_';
        if (!word_byte) break;
        if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
        hash = (hash ^ byte) * 16777619u;
        ++length;
    }
    return length ? hash ^ (uint32_t)length * 0x9e3779b1u : 0u;
}

static BinaryProbability *hashed_word_probability(
        HashedWordProbability *models, uint32_t word_context, unsigned node) {
    uint64_t tag = (uint64_t)word_context | ((uint64_t)node << 32u);
    uint64_t mixed = tag * UINT64_C(0x9e3779b97f4a7c15);
    mixed ^= mixed >> 29u;
    HashedWordProbability *entry = &models[mixed & HASHED_WORD_MASK];
    if (entry->tag != tag ||
        (!entry->probability.zero && !entry->probability.one)) {
        entry->tag = tag;
        entry->probability = (BinaryProbability){1, 1};
    }
    return &entry->probability;
}

static void mix_additional_binary_frequency(uint32_t *zero, uint32_t *total,
                                            const BinaryProbability *rich,
                                            uint32_t threshold) {
    uint32_t rich_total = (uint32_t)rich->zero + rich->one;
    uint32_t base_zero = *zero * 4096u / *total;
    uint32_t rich_zero = (uint32_t)rich->zero * 4096u / rich_total;
    uint32_t weight = rich_total > threshold ? rich_total - threshold : 0u;
    if (weight > 32u) weight = 32u;
    *zero = (base_zero * 16u + rich_zero * weight + (16u + weight) / 2u) /
            (16u + weight);
    if (!*zero) *zero = 1;
    if (*zero >= 4096u) *zero = 4095u;
    *total = 4096u;
}

static Buffer adaptive_literal_bit_encode(const uint8_t *input,
                                          const uint8_t *previous_bytes,
                                          size_t input_size) {
    if (input_size > UINT32_MAX) fail("literal-bit arithmetic stream too large");
    Buffer output = {0};
    append_u32le(&output, (uint32_t)input_size);
    BitWriter writer = {&output, 0, 0};
    BinaryProbability *models = (BinaryProbability *)calloc(
        256u * 256u, sizeof(*models));
    if (!models) fail("out of memory");
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    size_t pending = 0;
    for (size_t i = 0; i < input_size; ++i) {
        unsigned context = previous_bytes[i];
        unsigned node = 1;
        for (int shift = 7; shift >= 0; --shift) {
            unsigned bit = (input[i] >> shift) & 1u;
            BinaryProbability *model = literal_bit_probability(models, context, node);
            uint32_t total = (uint32_t)model->zero + model->one;
            uint32_t cumulative = bit ? model->zero : 0;
            uint32_t frequency = bit ? model->one : model->zero;
            uint64_t range = (uint64_t)high - low + 1u;
            high = low + (uint32_t)(range * (cumulative + frequency) / total - 1u);
            low = low + (uint32_t)(range * cumulative / total);
            for (;;) {
                if (high < ARITHMETIC_HALF) {
                    arithmetic_write_follow(&writer, 0, &pending);
                } else if (low >= ARITHMETIC_HALF) {
                    arithmetic_write_follow(&writer, 1, &pending);
                    low -= ARITHMETIC_HALF;
                    high -= ARITHMETIC_HALF;
                } else if (low >= ARITHMETIC_QUARTER &&
                           high < ARITHMETIC_THREE_QUARTERS) {
                    ++pending;
                    low -= ARITHMETIC_QUARTER;
                    high -= ARITHMETIC_QUARTER;
                } else {
                    break;
                }
                low <<= 1u;
                high = (high << 1u) | 1u;
            }
            binary_probability_update(model, bit);
            node = node * 2u + bit;
        }
    }
    ++pending;
    arithmetic_write_follow(&writer, low < ARITHMETIC_QUARTER ? 0u : 1u, &pending);
    finish_bits(&writer);
    append_u32le(&output, 0);
    free(models);
    return output;
}

typedef struct {
    BitReader reader;
    BinaryProbability *models;
    uint32_t low;
    uint32_t high;
    uint32_t code;
    size_t remaining;
} LiteralBitDecoder;

static LiteralBitDecoder literal_bit_decoder_init(const uint8_t *input,
                                                  size_t input_size) {
    if (input_size < 8) fail("truncated literal-bit arithmetic stream");
    LiteralBitDecoder decoder = {0};
    decoder.remaining = read_u32le(input);
    if (decoder.remaining > WHOLE_BLOCK_SIZE * 2u)
        fail("invalid literal-bit arithmetic size");
    decoder.reader = (BitReader){input + 4, input_size - 4, 0, 0};
    decoder.high = UINT32_MAX;
    decoder.models = (BinaryProbability *)calloc(256u * 256u,
                                                  sizeof(*decoder.models));
    if (!decoder.models) fail("out of memory");
    for (unsigned bit = 0; bit < 32; ++bit) {
        int value = read_bit(&decoder.reader);
        if (value < 0) fail("truncated literal-bit arithmetic code");
        decoder.code = (decoder.code << 1u) | (unsigned)value;
    }
    return decoder;
}

static uint8_t literal_bit_decode_symbol(LiteralBitDecoder *decoder,
                                         unsigned context) {
    if (!decoder->remaining || context >= 256)
        fail("invalid literal-bit arithmetic symbol");
    unsigned symbol = 0;
    unsigned node = 1;
    for (unsigned decoded_bits = 0; decoded_bits < 8; ++decoded_bits) {
        BinaryProbability *model = literal_bit_probability(decoder->models,
                                                           context, node);
        uint32_t total = (uint32_t)model->zero + model->one;
        uint64_t range = (uint64_t)decoder->high - decoder->low + 1u;
        uint32_t target = (uint32_t)((((uint64_t)(decoder->code - decoder->low) + 1u) *
                                      total - 1u) / range);
        unsigned bit = target >= model->zero;
        uint32_t cumulative = bit ? model->zero : 0;
        uint32_t frequency = bit ? model->one : model->zero;
        decoder->high = decoder->low +
            (uint32_t)(range * (cumulative + frequency) / total - 1u);
        decoder->low = decoder->low + (uint32_t)(range * cumulative / total);
        for (;;) {
            if (decoder->high < ARITHMETIC_HALF) {
            } else if (decoder->low >= ARITHMETIC_HALF) {
                decoder->code -= ARITHMETIC_HALF;
                decoder->low -= ARITHMETIC_HALF;
                decoder->high -= ARITHMETIC_HALF;
            } else if (decoder->low >= ARITHMETIC_QUARTER &&
                       decoder->high < ARITHMETIC_THREE_QUARTERS) {
                decoder->code -= ARITHMETIC_QUARTER;
                decoder->low -= ARITHMETIC_QUARTER;
                decoder->high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            decoder->low <<= 1u;
            decoder->high = (decoder->high << 1u) | 1u;
            int next = read_bit(&decoder->reader);
            if (next < 0) fail("truncated literal-bit arithmetic code");
            decoder->code = (decoder->code << 1u) | (unsigned)next;
        }
        binary_probability_update(model, bit);
        symbol = (symbol << 1u) | bit;
        node = node * 2u + bit;
    }
    --decoder->remaining;
    return (uint8_t)symbol;
}

static BinaryProbability *matched_literal_probability(BinaryProbability *models,
                                                      unsigned context,
                                                      unsigned node,
                                                      int matched,
                                                      unsigned expected_bit) {
    size_t offset = (size_t)context * 768u + node;
    if (matched) offset += 256u + expected_bit * 256u;
    BinaryProbability *model = &models[offset];
    if (!model->zero && !model->one) {
        model->zero = 1;
        model->one = 1;
    }
    return model;
}

static void mixed_matched_literal_frequencies(BinaryProbability *base,
                                              BinaryProbability *rich,
                                              uint32_t *zero,
                                              uint32_t *total) {
    uint32_t base_total = (uint32_t)base->zero + base->one;
    uint32_t rich_total = (uint32_t)rich->zero + rich->one;
    uint32_t base_zero = (uint32_t)base->zero * 4096u / base_total;
    uint32_t rich_zero = (uint32_t)rich->zero * 4096u / rich_total;
    uint32_t weight = rich_total - 2u;
    if (weight > 64u) weight = 64u;
    *zero = (base_zero * 8u + rich_zero * weight + (8u + weight) / 2u) /
            (8u + weight);
    if (!*zero) *zero = 1;
    if (*zero >= 4096u) *zero = 4095u;
    *total = 4096u;
}

static Buffer adaptive_control_bit_encode(const uint8_t *controls,
                                          size_t control_count) {
    if (control_count > UINT32_MAX) fail("control-bit stream too large");
    Buffer output = {0};
    append_u32le(&output, (uint32_t)control_count);
    BitWriter writer = {&output, 0, 0};
    BinaryProbability base = {1, 1};
    BinaryProbability histories[256] = {{0}};
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    size_t pending = 0;
    unsigned history = 0;
    for (size_t i = 0; i < control_count; ++i) {
        unsigned bit = controls[i] != 0;
        BinaryProbability *model = &histories[history];
        if (!model->zero && !model->one) *model = (BinaryProbability){1, 1};
        uint32_t zero, total;
        mixed_matched_literal_frequencies(&base, model, &zero, &total);
        uint32_t cumulative = bit ? zero : 0;
        uint32_t frequency = bit ? total - zero : zero;
        uint64_t range = (uint64_t)high - low + 1u;
        high = low + (uint32_t)(range * (cumulative + frequency) / total - 1u);
        low = low + (uint32_t)(range * cumulative / total);
        for (;;) {
            if (high < ARITHMETIC_HALF) {
                arithmetic_write_follow(&writer, 0, &pending);
            } else if (low >= ARITHMETIC_HALF) {
                arithmetic_write_follow(&writer, 1, &pending);
                low -= ARITHMETIC_HALF;
                high -= ARITHMETIC_HALF;
            } else if (low >= ARITHMETIC_QUARTER &&
                       high < ARITHMETIC_THREE_QUARTERS) {
                ++pending;
                low -= ARITHMETIC_QUARTER;
                high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            low <<= 1u;
            high = (high << 1u) | 1u;
        }
        binary_probability_update(&base, bit);
        binary_probability_update(model, bit);
        history = ((history << 1u) | bit) & 255u;
    }
    ++pending;
    arithmetic_write_follow(&writer, low < ARITHMETIC_QUARTER ? 0u : 1u, &pending);
    finish_bits(&writer);
    append_u32le(&output, 0);
    return output;
}

static Buffer adaptive_control_bit_decode(const uint8_t *input, size_t input_size) {
    if (input_size < 8u) fail("truncated control-bit stream");
    size_t control_count = read_u32le(input);
    if (control_count > WHOLE_BLOCK_SIZE) fail("invalid control-bit count");
    BitReader reader = {input + 4u, input_size - 4u, 0, 0};
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    uint32_t code = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        int value = read_bit(&reader);
        if (value < 0) fail("truncated control-bit arithmetic code");
        code = (code << 1u) | (unsigned)value;
    }
    BinaryProbability base = {1, 1};
    BinaryProbability histories[256] = {{0}};
    unsigned history = 0;
    Buffer packed = {0};
    reserve(&packed, (control_count + 7u) / 8u);
    for (size_t i = 0; i < control_count; ++i) {
        BinaryProbability *model = &histories[history];
        if (!model->zero && !model->one) *model = (BinaryProbability){1, 1};
        uint32_t zero, total;
        mixed_matched_literal_frequencies(&base, model, &zero, &total);
        uint64_t range = (uint64_t)high - low + 1u;
        uint32_t target = (uint32_t)((((uint64_t)(code - low) + 1u) * total - 1u) /
                                     range);
        unsigned bit = target >= zero;
        uint32_t cumulative = bit ? zero : 0;
        uint32_t frequency = bit ? total - zero : zero;
        high = low + (uint32_t)(range * (cumulative + frequency) / total - 1u);
        low = low + (uint32_t)(range * cumulative / total);
        for (;;) {
            if (high < ARITHMETIC_HALF) {
            } else if (low >= ARITHMETIC_HALF) {
                code -= ARITHMETIC_HALF;
                low -= ARITHMETIC_HALF;
                high -= ARITHMETIC_HALF;
            } else if (low >= ARITHMETIC_QUARTER &&
                       high < ARITHMETIC_THREE_QUARTERS) {
                code -= ARITHMETIC_QUARTER;
                low -= ARITHMETIC_QUARTER;
                high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            low <<= 1u;
            high = (high << 1u) | 1u;
            int next = read_bit(&reader);
            if (next < 0) fail("truncated control-bit arithmetic code");
            code = (code << 1u) | (unsigned)next;
        }
        if ((i & 7u) == 0) append_byte(&packed, 0);
        if (bit) packed.data[i >> 3u] |= (uint8_t)(1u << (i & 7u));
        binary_probability_update(&base, bit);
        binary_probability_update(model, bit);
        history = ((history << 1u) | bit) & 255u;
    }
    return packed;
}

static Buffer adaptive_matched_literal_bit_encode(const uint8_t *input,
                                                  const uint8_t *previous_bytes,
                                                  const uint8_t *previous2_bytes,
                                                  const uint8_t *previous3_bytes,
                                                  const uint8_t *word_context_bytes,
                                                  const uint8_t *match_bytes,
                                                  const uint8_t *matched_flags,
                                                  size_t input_size,
                                                  int mixed_context) {
    if (input_size > UINT32_MAX) fail("matched-literal stream too large");
    Buffer output = {0};
    append_u32le(&output, (uint32_t)input_size);
    BitWriter writer = {&output, 0, 0};
    BinaryProbability *models = (BinaryProbability *)calloc(
        256u * 768u, sizeof(*models));
    if (!models) fail("out of memory");
    size_t rich_model_count = mixed_context == 1 ? 4096u * 768u :
                              ((mixed_context == 2 || mixed_context == 3)
                                  ? 65536u * 256u : 0);
    BinaryProbability *rich_models = rich_model_count
        ? (BinaryProbability *)calloc(rich_model_count, sizeof(*rich_models)) : NULL;
    if (mixed_context && !rich_models) fail("out of memory");
    HashedBinaryProbability *hashed_models = mixed_context == 3
        ? (HashedBinaryProbability *)calloc(HASHED_LITERAL_SIZE,
                                             sizeof(*hashed_models)) : NULL;
    if (mixed_context == 3 && !hashed_models) fail("out of memory");
    HashedWordProbability *word_models = mixed_context == 3
        ? (HashedWordProbability *)calloc(HASHED_WORD_SIZE,
                                           sizeof(*word_models)) : NULL;
    if (mixed_context == 3 && !word_models) fail("out of memory");
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    size_t pending = 0;
    for (size_t i = 0; i < input_size; ++i) {
        unsigned context = previous_bytes[i];
        unsigned node = 1;
        int matched = matched_flags[i] != 0;
        for (int shift = 7; shift >= 0; --shift) {
            unsigned bit = (input[i] >> shift) & 1u;
            unsigned expected_bit = (match_bytes[i] >> shift) & 1u;
            BinaryProbability *model = matched_literal_probability(
                models, context, node, matched, expected_bit);
            BinaryProbability *rich_model = NULL;
            BinaryProbability *hashed_model = NULL;
            BinaryProbability *word_model = NULL;
            uint32_t zero = model->zero;
            uint32_t total = (uint32_t)model->zero + model->one;
            if (mixed_context == 1) {
                unsigned rich_context = context |
                    ((unsigned)(previous2_bytes[i] >> 4u) << 8u);
                rich_model = matched_literal_probability(
                    rich_models, rich_context, node, matched, expected_bit);
                mixed_matched_literal_frequencies(model, rich_model, &zero, &total);
            } else if ((mixed_context == 2 || mixed_context == 3) && !matched) {
                unsigned rich_context = context |
                    ((unsigned)previous2_bytes[i] << 8u);
                rich_model = literal_bit_probability(rich_models, rich_context, node);
                mixed_matched_literal_frequencies(model, rich_model, &zero, &total);
                if (mixed_context == 3) {
                    hashed_model = hashed_literal_probability(
                        hashed_models, context, previous2_bytes[i],
                        previous3_bytes[i], node);
                    mix_additional_binary_frequency(
                        &zero, &total, hashed_model, HASHED_LITERAL_THRESHOLD);
                    uint32_t word_context = read_u32le(word_context_bytes + 4u * i);
                    if (word_context) {
                        word_model = hashed_word_probability(
                            word_models, word_context, node);
                        mix_additional_binary_frequency(
                            &zero, &total, word_model, HASHED_WORD_THRESHOLD);
                    }
                }
            }
            uint32_t cumulative = bit ? zero : 0;
            uint32_t frequency = bit ? total - zero : zero;
            uint64_t range = (uint64_t)high - low + 1u;
            high = low + (uint32_t)(range * (cumulative + frequency) / total - 1u);
            low = low + (uint32_t)(range * cumulative / total);
            for (;;) {
                if (high < ARITHMETIC_HALF) {
                    arithmetic_write_follow(&writer, 0, &pending);
                } else if (low >= ARITHMETIC_HALF) {
                    arithmetic_write_follow(&writer, 1, &pending);
                    low -= ARITHMETIC_HALF;
                    high -= ARITHMETIC_HALF;
                } else if (low >= ARITHMETIC_QUARTER &&
                           high < ARITHMETIC_THREE_QUARTERS) {
                    ++pending;
                    low -= ARITHMETIC_QUARTER;
                    high -= ARITHMETIC_QUARTER;
                } else {
                    break;
                }
                low <<= 1u;
                high = (high << 1u) | 1u;
            }
            binary_probability_update(model, bit);
            if (rich_model) binary_probability_update(rich_model, bit);
            if (hashed_model) binary_probability_update(hashed_model, bit);
            if (word_model) binary_probability_update(word_model, bit);
            if (matched && bit != expected_bit) matched = 0;
            node = node * 2u + bit;
        }
    }
    ++pending;
    arithmetic_write_follow(&writer, low < ARITHMETIC_QUARTER ? 0u : 1u, &pending);
    finish_bits(&writer);
    append_u32le(&output, 0);
    free(word_models);
    free(hashed_models);
    free(rich_models);
    free(models);
    return output;
}

typedef struct {
    BitReader reader;
    BinaryProbability *models;
    BinaryProbability *rich_models;
    HashedBinaryProbability *hashed_models;
    HashedWordProbability *word_models;
    int mixed_context;
    uint32_t low;
    uint32_t high;
    uint32_t code;
    size_t remaining;
} MatchedLiteralDecoder;

static MatchedLiteralDecoder matched_literal_decoder_init(const uint8_t *input,
                                                          size_t input_size,
                                                          int mixed_context) {
    if (input_size < 8) fail("truncated matched-literal stream");
    MatchedLiteralDecoder decoder = {0};
    decoder.remaining = read_u32le(input);
    if (decoder.remaining > WHOLE_BLOCK_SIZE * 2u)
        fail("invalid matched-literal size");
    decoder.reader = (BitReader){input + 4, input_size - 4, 0, 0};
    decoder.high = UINT32_MAX;
    decoder.models = (BinaryProbability *)calloc(256u * 768u,
                                                  sizeof(*decoder.models));
    if (!decoder.models) fail("out of memory");
    decoder.mixed_context = mixed_context;
    size_t rich_model_count = mixed_context == 1 ? 4096u * 768u :
                              ((mixed_context == 2 || mixed_context == 3)
                                  ? 65536u * 256u : 0);
    decoder.rich_models = rich_model_count
        ? (BinaryProbability *)calloc(rich_model_count,
                                      sizeof(*decoder.rich_models)) : NULL;
    if (mixed_context && !decoder.rich_models) fail("out of memory");
    decoder.hashed_models = mixed_context == 3
        ? (HashedBinaryProbability *)calloc(HASHED_LITERAL_SIZE,
                                             sizeof(*decoder.hashed_models)) : NULL;
    if (mixed_context == 3 && !decoder.hashed_models) fail("out of memory");
    decoder.word_models = mixed_context == 3
        ? (HashedWordProbability *)calloc(HASHED_WORD_SIZE,
                                           sizeof(*decoder.word_models)) : NULL;
    if (mixed_context == 3 && !decoder.word_models) fail("out of memory");
    for (unsigned bit = 0; bit < 32; ++bit) {
        int value = read_bit(&decoder.reader);
        if (value < 0) fail("truncated matched-literal code");
        decoder.code = (decoder.code << 1u) | (unsigned)value;
    }
    return decoder;
}

static uint8_t matched_literal_decode_symbol(MatchedLiteralDecoder *decoder,
                                             unsigned context,
                                             unsigned previous2,
                                             unsigned previous3,
                                             uint32_t word_context,
                                             unsigned match_byte,
                                             int matched) {
    if (!decoder->remaining || context >= 256 || previous2 >= 256 ||
        previous3 >= 256 ||
        match_byte >= 256)
        fail("invalid matched-literal symbol");
    unsigned symbol = 0;
    unsigned node = 1;
    for (unsigned decoded_bits = 0; decoded_bits < 8; ++decoded_bits) {
        unsigned shift = 7u - decoded_bits;
        unsigned expected_bit = (match_byte >> shift) & 1u;
        BinaryProbability *model = matched_literal_probability(
            decoder->models, context, node, matched, expected_bit);
        BinaryProbability *rich_model = NULL;
        BinaryProbability *hashed_model = NULL;
        BinaryProbability *word_model = NULL;
        uint32_t zero = model->zero;
        uint32_t total = (uint32_t)model->zero + model->one;
        if (decoder->mixed_context == 1) {
            unsigned rich_context = context | ((previous2 >> 4u) << 8u);
            rich_model = matched_literal_probability(
                decoder->rich_models, rich_context, node, matched, expected_bit);
            mixed_matched_literal_frequencies(model, rich_model, &zero, &total);
        } else if ((decoder->mixed_context == 2 || decoder->mixed_context == 3) &&
                   !matched) {
            unsigned rich_context = context | (previous2 << 8u);
            rich_model = literal_bit_probability(
                decoder->rich_models, rich_context, node);
            mixed_matched_literal_frequencies(model, rich_model, &zero, &total);
            if (decoder->mixed_context == 3) {
                hashed_model = hashed_literal_probability(
                    decoder->hashed_models, context, previous2, previous3, node);
                mix_additional_binary_frequency(
                    &zero, &total, hashed_model, HASHED_LITERAL_THRESHOLD);
                if (word_context) {
                    word_model = hashed_word_probability(
                        decoder->word_models, word_context, node);
                    mix_additional_binary_frequency(
                        &zero, &total, word_model, HASHED_WORD_THRESHOLD);
                }
            }
        }
        uint64_t range = (uint64_t)decoder->high - decoder->low + 1u;
        uint32_t target = (uint32_t)((((uint64_t)(decoder->code - decoder->low) + 1u) *
                                      total - 1u) / range);
        unsigned bit = target >= zero;
        uint32_t cumulative = bit ? zero : 0;
        uint32_t frequency = bit ? total - zero : zero;
        decoder->high = decoder->low +
            (uint32_t)(range * (cumulative + frequency) / total - 1u);
        decoder->low = decoder->low + (uint32_t)(range * cumulative / total);
        for (;;) {
            if (decoder->high < ARITHMETIC_HALF) {
            } else if (decoder->low >= ARITHMETIC_HALF) {
                decoder->code -= ARITHMETIC_HALF;
                decoder->low -= ARITHMETIC_HALF;
                decoder->high -= ARITHMETIC_HALF;
            } else if (decoder->low >= ARITHMETIC_QUARTER &&
                       decoder->high < ARITHMETIC_THREE_QUARTERS) {
                decoder->code -= ARITHMETIC_QUARTER;
                decoder->low -= ARITHMETIC_QUARTER;
                decoder->high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            decoder->low <<= 1u;
            decoder->high = (decoder->high << 1u) | 1u;
            int next = read_bit(&decoder->reader);
            if (next < 0) fail("truncated matched-literal code");
            decoder->code = (decoder->code << 1u) | (unsigned)next;
        }
        binary_probability_update(model, bit);
        if (rich_model) binary_probability_update(rich_model, bit);
        if (hashed_model) binary_probability_update(hashed_model, bit);
        if (word_model) binary_probability_update(word_model, bit);
        if (matched && bit != expected_bit) matched = 0;
        symbol = (symbol << 1u) | bit;
        node = node * 2u + bit;
    }
    --decoder->remaining;
    return (uint8_t)symbol;
}

static unsigned distance_arithmetic_context(size_t length) {
    if (length < MIN_MATCH) fail("invalid distance arithmetic length");
    size_t state = length - MIN_MATCH;
    return state < 3u ? (unsigned)state : 3u;
}

static void static_arithmetic_frequencies(const uint8_t *input, size_t input_size,
                                          uint16_t frequencies[256]) {
    uint32_t counts[256] = {0};
    for (size_t i = 0; i < input_size; ++i) ++counts[input[i]];
    if (!input_size) return;
    uint32_t total = 0;
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        if (!counts[symbol]) continue;
        uint64_t scaled = (uint64_t)counts[symbol] * ARITHMETIC_MAX_TOTAL / input_size;
        frequencies[symbol] = (uint16_t)(scaled ? scaled : 1u);
        total += frequencies[symbol];
    }
    while (total > ARITHMETIC_MAX_TOTAL) {
        unsigned selected = 0;
        for (unsigned symbol = 1; symbol < 256; ++symbol)
            if (frequencies[symbol] > frequencies[selected]) selected = symbol;
        if (frequencies[selected] <= 1) fail("cannot scale arithmetic frequencies");
        --frequencies[selected];
        --total;
    }
}

static uint16_t log2_q8(uint32_t value) {
    if (!value) fail("log2 of zero");
    unsigned exponent = 0;
    uint32_t probe = value;
    while (probe >>= 1u) ++exponent;
    uint64_t normalized = (uint64_t)value << (31u - exponent);
    unsigned fraction = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
        normalized = normalized * normalized >> 31u;
        if (normalized >= (UINT64_C(2) << 31u)) {
            normalized >>= 1u;
            fraction |= 1u << (7u - bit);
        }
    }
    return (uint16_t)(exponent * 256u + fraction);
}

static void static_arithmetic_symbol_costs(const Buffer *stream, uint16_t costs[256]) {
    uint16_t frequencies[256] = {0};
    static_arithmetic_frequencies(stream->data, stream->size, frequencies);
    uint32_t total = 0;
    for (unsigned symbol = 0; symbol < 256; ++symbol) total += frequencies[symbol];
    if (!total) {
        for (unsigned symbol = 0; symbol < 256; ++symbol) costs[symbol] = 8u * 256u;
        return;
    }
    uint16_t total_log = log2_q8(total);
    uint16_t absent_cost = (uint16_t)(total_log + 8u * 256u);
    for (unsigned symbol = 0; symbol < 256; ++symbol)
        costs[symbol] = frequencies[symbol]
            ? (uint16_t)(total_log - log2_q8(frequencies[symbol]))
            : absent_cost;
}

static void append_static_arithmetic_model(Buffer *output,
                                           const uint16_t frequencies[256]) {
    unsigned count = 0;
    for (unsigned symbol = 0; symbol < 256; ++symbol)
        count += frequencies[symbol] != 0;
    if (3u + 3u * count < 513u) {
        append_byte(output, 1);
        append_u16le(output, (uint16_t)count);
        for (unsigned symbol = 0; symbol < 256; ++symbol) {
            if (!frequencies[symbol]) continue;
            append_byte(output, (uint8_t)symbol);
            append_u16le(output, frequencies[symbol]);
        }
    } else {
        append_byte(output, 0);
        for (unsigned symbol = 0; symbol < 256; ++symbol)
            append_u16le(output, frequencies[symbol]);
    }
}

static size_t parse_static_arithmetic_model(const uint8_t *input, size_t input_size,
                                            uint16_t frequencies[256]) {
    if (!input_size) fail("truncated static arithmetic model");
    if (input[0] == 0) {
        if (input_size < 513) fail("truncated dense arithmetic model");
        for (unsigned symbol = 0; symbol < 256; ++symbol)
            frequencies[symbol] = read_u16le(input + 1u + 2u * symbol);
        return 513;
    }
    if (input[0] == 1) {
        if (input_size < 3) fail("truncated sparse arithmetic model");
        size_t count = read_u16le(input + 1);
        if (count > 256 || input_size < 3u + 3u * count)
            fail("invalid sparse arithmetic model");
        for (size_t i = 0; i < count; ++i) {
            uint8_t symbol = input[3u + 3u * i];
            uint16_t frequency = read_u16le(input + 4u + 3u * i);
            if (!frequency || frequencies[symbol]) fail("invalid sparse arithmetic symbol");
            frequencies[symbol] = frequency;
        }
        return 3u + 3u * count;
    }
    fail("unknown static arithmetic model");
    return 0;
}

static Buffer static_arithmetic_encode(const uint8_t *input, size_t input_size) {
    if (input_size > UINT32_MAX) fail("static arithmetic stream too large");
    uint16_t frequencies[256] = {0};
    static_arithmetic_frequencies(input, input_size, frequencies);
    Buffer output = {0};
    append_u32le(&output, (uint32_t)input_size);
    append_static_arithmetic_model(&output, frequencies);
    ArithmeticModel model = {0};
    memcpy(model.frequencies, frequencies, sizeof(frequencies));
    arithmetic_model_rebuild(&model);
    BitWriter writer = {&output, 0, 0};
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    size_t pending = 0;
    for (size_t i = 0; i < input_size; ++i) {
        unsigned symbol = input[i];
        uint32_t cumulative = arithmetic_model_prefix(&model, symbol);
        uint64_t range = (uint64_t)high - low + 1u;
        high = low + (uint32_t)(range * (cumulative + model.frequencies[symbol]) /
                                model.total - 1u);
        low = low + (uint32_t)(range * cumulative / model.total);
        for (;;) {
            if (high < ARITHMETIC_HALF) {
                arithmetic_write_follow(&writer, 0, &pending);
            } else if (low >= ARITHMETIC_HALF) {
                arithmetic_write_follow(&writer, 1, &pending);
                low -= ARITHMETIC_HALF;
                high -= ARITHMETIC_HALF;
            } else if (low >= ARITHMETIC_QUARTER && high < ARITHMETIC_THREE_QUARTERS) {
                ++pending;
                low -= ARITHMETIC_QUARTER;
                high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            low <<= 1u;
            high = (high << 1u) | 1u;
        }
    }
    ++pending;
    arithmetic_write_follow(&writer, low < ARITHMETIC_QUARTER ? 0u : 1u, &pending);
    finish_bits(&writer);
    append_u32le(&output, 0);
    return output;
}

static Buffer static_arithmetic_decode(const uint8_t *input, size_t input_size) {
    if (input_size < 8) fail("truncated static arithmetic stream");
    size_t decoded_size = read_u32le(input);
    if (decoded_size > WHOLE_BLOCK_SIZE * 2u) fail("invalid static arithmetic stream size");
    uint16_t frequencies[256] = {0};
    size_t model_size = parse_static_arithmetic_model(input + 4, input_size - 4,
                                                      frequencies);
    if (input_size - 4u - model_size < 4) fail("truncated static arithmetic code");
    ArithmeticModel model = {0};
    memcpy(model.frequencies, frequencies, sizeof(frequencies));
    arithmetic_model_rebuild(&model);
    if ((decoded_size && !model.total) || model.total > ARITHMETIC_MAX_TOTAL)
        fail("invalid static arithmetic frequencies");
    Buffer output = {0};
    reserve(&output, decoded_size ? decoded_size : 1);
    BitReader reader = {input + 4u + model_size, input_size - 4u - model_size, 0, 0};
    uint32_t code = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        int value = read_bit(&reader);
        if (value < 0) fail("truncated static arithmetic code");
        code = (code << 1u) | (unsigned)value;
    }
    uint32_t low = 0;
    uint32_t high = UINT32_MAX;
    while (output.size < decoded_size) {
        uint64_t range = (uint64_t)high - low + 1u;
        uint32_t target = (uint32_t)((((uint64_t)(code - low) + 1u) * model.total - 1u) /
                                     range);
        if (target >= model.total) fail("invalid static arithmetic code");
        uint32_t cumulative;
        unsigned symbol = arithmetic_model_symbol(&model, target, &cumulative);
        high = low + (uint32_t)(range * (cumulative + model.frequencies[symbol]) /
                                model.total - 1u);
        low = low + (uint32_t)(range * cumulative / model.total);
        for (;;) {
            if (high < ARITHMETIC_HALF) {
            } else if (low >= ARITHMETIC_HALF) {
                code -= ARITHMETIC_HALF;
                low -= ARITHMETIC_HALF;
                high -= ARITHMETIC_HALF;
            } else if (low >= ARITHMETIC_QUARTER && high < ARITHMETIC_THREE_QUARTERS) {
                code -= ARITHMETIC_QUARTER;
                low -= ARITHMETIC_QUARTER;
                high -= ARITHMETIC_QUARTER;
            } else {
                break;
            }
            low <<= 1u;
            high = (high << 1u) | 1u;
            int bit = read_bit(&reader);
            if (bit < 0) fail("truncated static arithmetic code");
            code = (code << 1u) | (unsigned)bit;
        }
        append_byte(&output, (uint8_t)symbol);
    }
    return output;
}

static Buffer split_huffman_encode(const uint8_t *input, size_t input_size,
                                   size_t decoded_size, int compact_models) {
    Buffer streams[5] = {{0}};
    size_t ip = 0;
    size_t produced = 0;
    while (produced < decoded_size) {
        if (ip >= input_size) fail("truncated token stream during split");
        uint8_t control = input[ip++];
        append_byte(&streams[0], control);
        for (unsigned bit = 0; bit < 8 && produced < decoded_size; ++bit) {
            if ((control & (1u << bit)) == 0) {
                if (ip >= input_size) fail("truncated literal during split");
                append_byte(&streams[1], input[ip++]);
                ++produced;
            } else {
                if (input_size - ip < 3) fail("truncated match during split");
                append_byte(&streams[2], input[ip]);
                append_byte(&streams[3], input[ip + 1]);
                append_byte(&streams[4], input[ip + 2]);
                size_t length = (size_t)input[ip + 2] + MIN_MATCH;
                ip += 3;
                if (length > decoded_size - produced) fail("invalid match during split");
                produced += length;
            }
        }
    }
    if (ip != input_size) fail("trailing token data during split");

    Buffer encoded[5] = {{0}};
    Buffer output = {0};
    for (unsigned i = 0; i < 5; ++i)
        encoded[i] = compact_models
            ? compact_huffman_encode(streams[i].data, streams[i].size)
            : huffman_encode(streams[i].data, streams[i].size);
    for (unsigned i = 0; i < 5; ++i) {
        if (encoded[i].size > UINT32_MAX) fail("split Huffman stream too large");
        append_u32le(&output, (uint32_t)encoded[i].size);
    }
    for (unsigned i = 0; i < 5; ++i) {
        append_bytes(&output, encoded[i].data, encoded[i].size);
        free(encoded[i].data);
        free(streams[i].data);
    }
    return output;
}

static Buffer split_huffman_encode_best_model(const uint8_t *input,
                                              size_t input_size,
                                              size_t decoded_size,
                                              int try_compact_models,
                                              uint8_t *selected_mode) {
    Buffer ordinary = split_huffman_encode(input, input_size, decoded_size, 0);
    *selected_mode = 6;
    if (try_compact_models) {
        Buffer compact = split_huffman_encode(input, input_size, decoded_size, 1);
        if (compact.size < ordinary.size) {
            free(ordinary.data);
            ordinary = compact;
            *selected_mode = 8;
        } else {
            free(compact.data);
        }
    }
    return ordinary;
}

static void split_huffman_decode(Buffer *output, const uint8_t *input,
                                 size_t input_size, size_t decoded_size,
                                 size_t match_base, int fast_lookup,
                                 int compact_models) {
    if (input_size < 20 || match_base > output->size) fail("invalid split Huffman header");
    uint32_t sizes[5];
    size_t offset = 20;
    for (unsigned i = 0; i < 5; ++i) {
        sizes[i] = read_u32le(input + 4 * i);
        if ((size_t)sizes[i] > input_size - offset) fail("invalid split Huffman size");
        offset += sizes[i];
    }
    if (offset != input_size) fail("trailing split Huffman data");

    Buffer streams[5] = {{0}};
    offset = 20;
    for (unsigned i = 0; i < 5; ++i) {
        streams[i] = compact_models
            ? compact_huffman_decode(input + offset, sizes[i], fast_lookup)
            : huffman_decode(input + offset, sizes[i], fast_lookup);
        offset += sizes[i];
    }
    if (streams[2].size != streams[3].size || streams[2].size != streams[4].size)
        fail("inconsistent split match streams");

    if (decoded_size > SIZE_MAX - output->size) fail("decoded size overflow");
    size_t target_size = output->size + decoded_size;
    size_t control_index = 0;
    size_t literal_index = 0;
    size_t match_index = 0;
    while (output->size < target_size) {
        if (control_index >= streams[0].size) fail("truncated split controls");
        uint8_t control = streams[0].data[control_index++];
        for (unsigned bit = 0; bit < 8 && output->size < target_size; ++bit) {
            if ((control & (1u << bit)) == 0) {
                if (literal_index >= streams[1].size) fail("truncated split literals");
                append_byte(output, streams[1].data[literal_index++]);
            } else {
                if (match_index >= streams[2].size) fail("truncated split matches");
                size_t distance = (size_t)streams[2].data[match_index] |
                                  ((size_t)streams[3].data[match_index] << 8);
                distance += 1;
                size_t length = (size_t)streams[4].data[match_index] + MIN_MATCH;
                ++match_index;
                if (distance > output->size - match_base || length > target_size - output->size)
                    fail("invalid split match");
                reserve(output, length);
                for (size_t i = 0; i < length; ++i) {
                    output->data[output->size] = output->data[output->size - distance];
                    ++output->size;
                }
            }
        }
    }
    if (control_index != streams[0].size || literal_index != streams[1].size ||
        match_index != streams[2].size)
        fail("unused split Huffman symbols");
    for (unsigned i = 0; i < 5; ++i) free(streams[i].data);
}

static void huffman_symbol_costs(const Buffer *stream, uint16_t costs[256]) {
    for (unsigned symbol = 0; symbol < 256; ++symbol) costs[symbol] = 16;
    if (!stream->size) return;
    HuffNode nodes[511];
    int node_count;
    int root = build_huffman_tree(stream->data, stream->size, nodes, &node_count);
    (void)node_count;
    uint16_t lengths[256] = {0};
    int stack_nodes[511];
    uint16_t stack_depths[511];
    size_t stack_size = 0;
    stack_nodes[stack_size] = root;
    stack_depths[stack_size++] = 0;
    while (stack_size) {
        int node = stack_nodes[--stack_size];
        uint16_t depth = stack_depths[stack_size];
        if (nodes[node].symbol >= 0) {
            lengths[nodes[node].symbol] = depth;
        } else {
            stack_nodes[stack_size] = nodes[node].left;
            stack_depths[stack_size++] = (uint16_t)(depth + 1u);
            stack_nodes[stack_size] = nodes[node].right;
            stack_depths[stack_size++] = (uint16_t)(depth + 1u);
        }
    }
    uint8_t present[256] = {0};
    uint16_t maximum = 1;
    for (size_t i = 0; i < stream->size; ++i) present[stream->data[i]] = 1;
    for (unsigned symbol = 0; symbol < 256; ++symbol)
        if (present[symbol] && lengths[symbol] > maximum) maximum = lengths[symbol];
    for (unsigned symbol = 0; symbol < 256; ++symbol)
        costs[symbol] = present[symbol] ? (lengths[symbol] ? lengths[symbol] : 1)
                                       : (uint16_t)(maximum + 8u);
}

static void split_cost_model(const uint8_t *input, size_t input_size,
                             size_t decoded_size, uint16_t literal_costs[256],
                             uint16_t distance_low_costs[256],
                             uint16_t distance_high_costs[256],
                             uint16_t length_costs[256]) {
    Buffer fields[4] = {{0}};
    size_t ip = 0;
    size_t produced = 0;
    while (produced < decoded_size) {
        if (ip >= input_size) fail("truncated token stream during cost model");
        uint8_t control = input[ip++];
        for (unsigned bit = 0; bit < 8 && produced < decoded_size; ++bit) {
            if ((control & (1u << bit)) == 0) {
                if (ip >= input_size) fail("truncated literal during cost model");
                append_byte(&fields[0], input[ip++]);
                ++produced;
            } else {
                if (input_size - ip < 3) fail("truncated match during cost model");
                append_byte(&fields[1], input[ip]);
                append_byte(&fields[2], input[ip + 1]);
                append_byte(&fields[3], input[ip + 2]);
                size_t length = (size_t)input[ip + 2] + MIN_MATCH;
                ip += 3;
                if (length > decoded_size - produced) fail("invalid match during cost model");
                produced += length;
            }
        }
    }
    if (ip != input_size) fail("trailing token data during cost model");
    huffman_symbol_costs(&fields[0], literal_costs);
    huffman_symbol_costs(&fields[1], distance_low_costs);
    huffman_symbol_costs(&fields[2], distance_high_costs);
    huffman_symbol_costs(&fields[3], length_costs);
    for (unsigned field = 0; field < 4; ++field) free(fields[field].data);
}

typedef struct {
    uint16_t *lengths;
    uint16_t *offsets;
    int32_t *previous;
} MatchIndex;

static MatchIndex build_match_index(const uint8_t *input, size_t input_size,
                                    size_t start, size_t decoded_size) {
    MatchIndex index = {0};
    index.lengths = decoded_size
        ? (uint16_t *)malloc(decoded_size * sizeof(*index.lengths)) : NULL;
    index.offsets = decoded_size
        ? (uint16_t *)malloc(decoded_size * sizeof(*index.offsets)) : NULL;
    index.previous = input_size
        ? (int32_t *)malloc(input_size * sizeof(*index.previous)) : NULL;
    int32_t *heads = (int32_t *)malloc(HASH_SIZE * sizeof(*heads));
    if ((decoded_size && (!index.lengths || !index.offsets)) ||
        (input_size && !index.previous) || !heads)
        fail("out of memory");
    for (size_t i = 0; i < HASH_SIZE; ++i) heads[i] = -1;
    for (size_t i = 0; i < start; ++i)
        insert_position(input, input_size, i, heads, index.previous);
    for (size_t pos = start; pos < input_size; ++pos) {
        size_t length, offset;
        find_match(input, input_size, pos, heads, index.previous, &length, &offset);
        index.lengths[pos - start] = (uint16_t)length;
        index.offsets[pos - start] = (uint16_t)offset;
        insert_position(input, input_size, pos, heads, index.previous);
    }
    free(heads);
    return index;
}

static void free_match_index(MatchIndex *index) {
    free(index->previous);
    free(index->offsets);
    free(index->lengths);
    *index = (MatchIndex){0};
}

typedef struct {
    uint16_t *lengths;
    uint32_t *offsets;
    int32_t *previous;
    unsigned cost_chain;
} WideMatchIndex;

static WideMatchIndex build_wide_match_index(const uint8_t *input, size_t input_size,
                                             size_t block_size, unsigned max_chain,
                                             unsigned cost_chain) {
    if (input_size > INT32_MAX) fail("input exceeds wide match-index limit");
    WideMatchIndex index = {0};
    index.cost_chain = cost_chain;
    index.lengths = input_size ? (uint16_t *)calloc(input_size, sizeof(*index.lengths)) : NULL;
    index.offsets = input_size ? (uint32_t *)calloc(input_size, sizeof(*index.offsets)) : NULL;
    index.previous = input_size ? (int32_t *)malloc(input_size * sizeof(*index.previous)) : NULL;
    int32_t *heads = (int32_t *)malloc(WIDE_HASH_SIZE * sizeof(*heads));
    if ((input_size && (!index.lengths || !index.offsets || !index.previous)) || !heads)
        fail("out of memory");
    for (size_t i = 0; i < WIDE_HASH_SIZE; ++i) heads[i] = -1;
    for (size_t pos = 0; pos < input_size; ++pos) {
        if (pos + 3u < input_size) {
            uint32_t hash = wide_hash4(input + pos);
            int32_t candidate = heads[hash];
            size_t block_end = (pos / block_size + 1u) * block_size;
            if (block_end > input_size) block_end = input_size;
            size_t limit = block_end - pos;
            if (limit > WIDE_MAX_MATCH) limit = WIDE_MAX_MATCH;
            size_t best_length = 0;
            size_t best_offset = 0;
            unsigned depth = 0;
            while (candidate >= 0 && depth++ < max_chain) {
                size_t c = (size_t)candidate;
                size_t offset = pos - c;
                if (offset > WIDE_WINDOW) break;
                if (best_length < limit && input[c + best_length] == input[pos + best_length]) {
                    size_t length = 0;
                    while (length < limit && input[c + length] == input[pos + length]) ++length;
                    if (length > best_length) {
                        best_length = length;
                        best_offset = offset;
                        if (length == limit) break;
                    }
                }
                candidate = index.previous[c];
            }
            if (best_length >= MIN_MATCH) {
                index.lengths[pos] = (uint16_t)best_length;
                index.offsets[pos] = (uint32_t)best_offset;
            }
            index.previous[pos] = heads[hash];
            heads[hash] = (int32_t)pos;
        } else {
            index.previous[pos] = -1;
        }
    }
    free(heads);
    return index;
}

static void free_wide_match_index(WideMatchIndex *index) {
    free(index->previous);
    free(index->offsets);
    free(index->lengths);
    *index = (WideMatchIndex){0};
}

typedef struct {
    Buffer streams[7];
    Buffer token_controls;
    Buffer literal_contexts;
    Buffer literal_previous;
    Buffer literal_previous2;
    Buffer literal_previous3;
    Buffer literal_word_contexts;
    Buffer literal_match_bytes;
    Buffer literal_matched_flags;
    Buffer distance_contexts;
    unsigned count;
} WideFields;

static WideFields build_wide_fields(const uint8_t *input, size_t input_size,
                                    size_t start, size_t decoded_size,
                                    const WideMatchIndex *index,
                                    const uint16_t *choices,
                                    const uint32_t *choice_offsets) {
    if (start > input_size || decoded_size > input_size - start)
        fail("invalid wide split range");
    WideFields fields = {0};
    fields.count = 7;
    unsigned distance_stream = 2;
    size_t pos = start;
    size_t end = start + decoded_size;
    size_t last_offset = 0;
    int previous_was_match = 0;
    while (pos < end) {
        size_t control_pos = fields.streams[0].size;
        append_byte(&fields.streams[0], 0);
        uint8_t control = 0;
        for (unsigned bit = 0; bit < 8 && pos < end; ++bit) {
            size_t length = choices ? choices[pos - start] : index->lengths[pos];
            size_t offset = choices ? choice_offsets[pos - start] : index->offsets[pos];
            if (!choices && length && pos + 1u < end &&
                index->lengths[pos + 1u] > length + 1u)
                length = 0;
            append_byte(&fields.token_controls, (uint8_t)(length != 0));
            if (!length) {
                uint8_t previous = pos ? input[pos - 1u] : 0;
                uint8_t previous2 = pos > 1u ? input[pos - 2u] : 0;
                uint8_t previous3 = pos > 2u ? input[pos - 3u] : 0;
                append_byte(&fields.literal_contexts,
                            (uint8_t)literal_arithmetic_context(previous));
                append_byte(&fields.literal_previous, previous);
                append_byte(&fields.literal_previous2, previous2);
                append_byte(&fields.literal_previous3, previous3);
                append_u32le(&fields.literal_word_contexts,
                             literal_word_context(input, pos));
                int matched = previous_was_match && last_offset <= pos;
                append_byte(&fields.literal_matched_flags, (uint8_t)matched);
                append_byte(&fields.literal_match_bytes,
                            matched ? input[pos - last_offset] : 0);
                append_byte(&fields.streams[1], input[pos++]);
                previous_was_match = 0;
                continue;
            }
            if (length < MIN_MATCH || (!choices && length > index->lengths[pos]) ||
                length > end - pos || offset == 0 || offset > pos ||
                offset > WIDE_WINDOW ||
                (choices && memcmp(input + pos, input + pos - offset, length) != 0))
                fail("invalid wide match");
            control |= (uint8_t)(1u << bit);
            uint32_t encoded_offset = (uint32_t)(offset - 1u);
            uint16_t encoded_length = (uint16_t)(length - MIN_MATCH);
            append_byte(&fields.distance_contexts,
                        (uint8_t)distance_arithmetic_context(length));
            append_byte(&fields.streams[distance_stream], (uint8_t)encoded_offset);
            append_byte(&fields.streams[distance_stream + 1u],
                        (uint8_t)(encoded_offset >> 8));
            append_byte(&fields.streams[distance_stream + 2u],
                        (uint8_t)(encoded_offset >> 16));
            append_byte(&fields.streams[distance_stream + 3u],
                        (uint8_t)encoded_length);
            append_byte(&fields.streams[distance_stream + 4u],
                        (uint8_t)(encoded_length >> 8));
            last_offset = offset;
            pos += length;
            previous_was_match = 1;
        }
        fields.streams[0].data[control_pos] = control;
    }
    return fields;
}

static Buffer encode_wide_fields(WideFields *fields) {
    Buffer encoded[7] = {{0}};
    Buffer output = {0};
    for (unsigned i = 0; i < fields->count; ++i)
        encoded[i] = compact_huffman_encode(fields->streams[i].data,
                                            fields->streams[i].size);
    for (unsigned i = 0; i < fields->count; ++i) {
        if (encoded[i].size > UINT32_MAX) fail("wide split stream too large");
        append_u32le(&output, (uint32_t)encoded[i].size);
    }
    for (unsigned i = 0; i < fields->count; ++i) {
        append_bytes(&output, encoded[i].data, encoded[i].size);
        free(encoded[i].data);
    }
    return output;
}

static Buffer encode_wide_fields_hybrid(WideFields *fields) {
    Buffer encoded[7] = {{0}};
    Buffer output = {0};
    uint8_t arithmetic_mask = 0;
    for (unsigned i = 0; i < fields->count; ++i) {
        Buffer huffman = compact_huffman_encode(fields->streams[i].data,
                                                 fields->streams[i].size);
        Buffer arithmetic = adaptive_arithmetic_encode(fields->streams[i].data,
                                                        fields->streams[i].size);
        if (arithmetic.size < huffman.size) {
            encoded[i] = arithmetic;
            arithmetic_mask |= (uint8_t)(1u << i);
            free(huffman.data);
        } else {
            encoded[i] = huffman;
            free(arithmetic.data);
        }
    }
    append_byte(&output, arithmetic_mask);
    for (unsigned i = 0; i < fields->count; ++i) {
        if (encoded[i].size > UINT32_MAX) fail("wide split stream too large");
        append_u32le(&output, (uint32_t)encoded[i].size);
    }
    for (unsigned i = 0; i < fields->count; ++i) {
        append_bytes(&output, encoded[i].data, encoded[i].size);
        free(encoded[i].data);
    }
    return output;
}

static Buffer encode_wide_fields_order1_hybrid(WideFields *fields) {
    Buffer encoded[7] = {{0}};
    Buffer output = {0};
    uint8_t order0_mask = 0;
    uint8_t order1_mask = 0;
    for (unsigned i = 0; i < fields->count; ++i) {
        Buffer huffman = compact_huffman_encode(fields->streams[i].data,
                                                 fields->streams[i].size);
        Buffer order0 = adaptive_arithmetic_encode(fields->streams[i].data,
                                                    fields->streams[i].size);
        Buffer order1 = adaptive_order1_arithmetic_encode(fields->streams[i].data,
                                                          fields->streams[i].size);
        if (order1.size < order0.size && order1.size < huffman.size) {
            encoded[i] = order1;
            order1_mask |= (uint8_t)(1u << i);
            free(order0.data);
            free(huffman.data);
        } else if (order0.size < huffman.size) {
            encoded[i] = order0;
            order0_mask |= (uint8_t)(1u << i);
            free(order1.data);
            free(huffman.data);
        } else {
            encoded[i] = huffman;
            free(order1.data);
            free(order0.data);
        }
    }
    append_byte(&output, order0_mask);
    append_byte(&output, order1_mask);
    for (unsigned i = 0; i < fields->count; ++i) {
        if (encoded[i].size > UINT32_MAX) fail("wide split stream too large");
        append_u32le(&output, (uint32_t)encoded[i].size);
    }
    for (unsigned i = 0; i < fields->count; ++i) {
        append_bytes(&output, encoded[i].data, encoded[i].size);
        free(encoded[i].data);
    }
    return output;
}

static Buffer encode_wide_fields_context_hybrid(WideFields *fields) {
    if (fields->literal_contexts.size != fields->streams[1].size)
        fail("inconsistent literal arithmetic contexts");
    Buffer encoded[7] = {{0}};
    Buffer output = {0};
    uint8_t order0_mask = 0;
    uint8_t context_mask = 0;
    for (unsigned i = 0; i < fields->count; ++i) {
        Buffer huffman = compact_huffman_encode(fields->streams[i].data,
                                                 fields->streams[i].size);
        Buffer order0 = adaptive_arithmetic_encode(fields->streams[i].data,
                                                    fields->streams[i].size);
        Buffer contextual = {0};
        if (i == 1)
            contextual = adaptive_context_arithmetic_encode(
                fields->streams[i].data, fields->literal_contexts.data,
                fields->streams[i].size);
        if (i == 1 && contextual.size < order0.size && contextual.size < huffman.size) {
            encoded[i] = contextual;
            context_mask |= (uint8_t)(1u << i);
            free(order0.data);
            free(huffman.data);
        } else if (order0.size < huffman.size) {
            encoded[i] = order0;
            order0_mask |= (uint8_t)(1u << i);
            free(contextual.data);
            free(huffman.data);
        } else {
            encoded[i] = huffman;
            free(contextual.data);
            free(order0.data);
        }
    }
    append_byte(&output, order0_mask);
    append_byte(&output, context_mask);
    for (unsigned i = 0; i < fields->count; ++i) {
        if (encoded[i].size > UINT32_MAX) fail("wide split stream too large");
        append_u32le(&output, (uint32_t)encoded[i].size);
    }
    for (unsigned i = 0; i < fields->count; ++i) {
        append_bytes(&output, encoded[i].data, encoded[i].size);
        free(encoded[i].data);
    }
    return output;
}

static Buffer encode_wide_fields_literal_bit_hybrid(WideFields *fields) {
    if (fields->literal_previous.size != fields->streams[1].size)
        fail("inconsistent literal-bit contexts");
    Buffer encoded[7] = {{0}};
    Buffer output = {0};
    uint8_t adaptive_mask = 0;
    uint8_t literal_bit_mask = 0;
    for (unsigned i = 0; i < fields->count; ++i) {
        Buffer huffman = compact_huffman_encode(fields->streams[i].data,
                                                 fields->streams[i].size);
        Buffer adaptive = adaptive_arithmetic_encode(fields->streams[i].data,
                                                      fields->streams[i].size);
        Buffer literal_bits = {0};
        if (i == 1)
            literal_bits = adaptive_literal_bit_encode(
                fields->streams[i].data, fields->literal_previous.data,
                fields->streams[i].size);
        if (literal_bits.data && literal_bits.size < adaptive.size &&
            literal_bits.size < huffman.size) {
            encoded[i] = literal_bits;
            literal_bit_mask |= (uint8_t)(1u << i);
            free(adaptive.data);
            free(huffman.data);
        } else if (adaptive.size < huffman.size) {
            encoded[i] = adaptive;
            adaptive_mask |= (uint8_t)(1u << i);
            free(literal_bits.data);
            free(huffman.data);
        } else {
            encoded[i] = huffman;
            free(literal_bits.data);
            free(adaptive.data);
        }
    }
    append_byte(&output, adaptive_mask);
    append_byte(&output, literal_bit_mask);
    for (unsigned i = 0; i < fields->count; ++i) {
        if (encoded[i].size > UINT32_MAX) fail("wide split stream too large");
        append_u32le(&output, (uint32_t)encoded[i].size);
    }
    for (unsigned i = 0; i < fields->count; ++i) {
        append_bytes(&output, encoded[i].data, encoded[i].size);
        free(encoded[i].data);
    }
    return output;
}

static Buffer encode_wide_fields_matched_literal_hybrid(WideFields *fields,
                                                        int mixed_context) {
    if (fields->literal_previous.size != fields->streams[1].size ||
        fields->literal_previous2.size != fields->streams[1].size ||
        fields->literal_previous3.size != fields->streams[1].size ||
        fields->literal_word_contexts.size != fields->streams[1].size * 4u ||
        fields->literal_match_bytes.size != fields->streams[1].size ||
        fields->literal_matched_flags.size != fields->streams[1].size)
        fail("inconsistent matched-literal contexts");
    Buffer encoded[7] = {{0}};
    Buffer output = {0};
    uint8_t adaptive_mask = 0;
    uint8_t matched_literal_mask = 0;
    for (unsigned i = 0; i < fields->count; ++i) {
        Buffer huffman = compact_huffman_encode(fields->streams[i].data,
                                                 fields->streams[i].size);
        Buffer adaptive = adaptive_arithmetic_encode(fields->streams[i].data,
                                                      fields->streams[i].size);
        Buffer matched_literals = {0};
        if (i == 1)
            matched_literals = adaptive_matched_literal_bit_encode(
                fields->streams[i].data, fields->literal_previous.data,
                fields->literal_previous2.data,
                fields->literal_previous3.data,
                fields->literal_word_contexts.data,
                fields->literal_match_bytes.data, fields->literal_matched_flags.data,
                fields->streams[i].size, mixed_context);
        if (matched_literals.data && matched_literals.size < adaptive.size &&
            matched_literals.size < huffman.size) {
            encoded[i] = matched_literals;
            matched_literal_mask |= (uint8_t)(1u << i);
            free(adaptive.data);
            free(huffman.data);
        } else if (adaptive.size < huffman.size) {
            encoded[i] = adaptive;
            adaptive_mask |= (uint8_t)(1u << i);
            free(matched_literals.data);
            free(huffman.data);
        } else {
            encoded[i] = huffman;
            free(matched_literals.data);
            free(adaptive.data);
        }
    }
    append_byte(&output, adaptive_mask);
    append_byte(&output, matched_literal_mask);
    for (unsigned i = 0; i < fields->count; ++i) {
        if (encoded[i].size > UINT32_MAX) fail("wide split stream too large");
        append_u32le(&output, (uint32_t)encoded[i].size);
    }
    for (unsigned i = 0; i < fields->count; ++i) {
        append_bytes(&output, encoded[i].data, encoded[i].size);
        free(encoded[i].data);
    }
    return output;
}

static Buffer encode_wide_fields_control_matched_hybrid(WideFields *fields,
                                                        int context_order) {
    if (fields->literal_previous.size != fields->streams[1].size ||
        fields->literal_previous2.size != fields->streams[1].size ||
        fields->literal_previous3.size != fields->streams[1].size ||
        fields->literal_word_contexts.size != fields->streams[1].size * 4u ||
        fields->literal_match_bytes.size != fields->streams[1].size ||
        fields->literal_matched_flags.size != fields->streams[1].size)
        fail("inconsistent control/matched-literal contexts");
    Buffer encoded[7] = {{0}};
    Buffer output = {0};
    uint8_t adaptive_mask = 0;
    uint8_t special_mask = 0;
    for (unsigned i = 0; i < fields->count; ++i) {
        Buffer huffman = compact_huffman_encode(fields->streams[i].data,
                                                 fields->streams[i].size);
        Buffer adaptive = adaptive_arithmetic_encode(fields->streams[i].data,
                                                      fields->streams[i].size);
        Buffer special = {0};
        if (i == 0)
            special = adaptive_control_bit_encode(fields->token_controls.data,
                                                  fields->token_controls.size);
        else if (i == 1)
            special = adaptive_matched_literal_bit_encode(
                fields->streams[i].data, fields->literal_previous.data,
                fields->literal_previous2.data, fields->literal_previous3.data,
                fields->literal_word_contexts.data,
                fields->literal_match_bytes.data,
                fields->literal_matched_flags.data,
                fields->streams[i].size, context_order);
        if (special.data && special.size < adaptive.size && special.size < huffman.size) {
            encoded[i] = special;
            special_mask |= (uint8_t)(1u << i);
            free(adaptive.data);
            free(huffman.data);
        } else if (adaptive.size < huffman.size) {
            encoded[i] = adaptive;
            adaptive_mask |= (uint8_t)(1u << i);
            free(special.data);
            free(huffman.data);
        } else {
            encoded[i] = huffman;
            free(special.data);
            free(adaptive.data);
        }
    }
    append_byte(&output, adaptive_mask);
    append_byte(&output, special_mask);
    for (unsigned i = 0; i < fields->count; ++i) {
        if (encoded[i].size > UINT32_MAX) fail("wide split stream too large");
        append_u32le(&output, (uint32_t)encoded[i].size);
    }
    for (unsigned i = 0; i < fields->count; ++i) {
        append_bytes(&output, encoded[i].data, encoded[i].size);
        free(encoded[i].data);
    }
    return output;
}

static Buffer encode_wide_fields_rep_hybrid(WideFields *fields,
                                             int literal_context_order) {
    size_t match_count = fields->streams[2].size;
    if (fields->streams[3].size != match_count ||
        fields->streams[4].size != match_count ||
        fields->streams[5].size != match_count ||
        fields->streams[6].size != match_count ||
        fields->literal_previous.size != fields->streams[1].size ||
        fields->literal_previous2.size != fields->streams[1].size ||
        fields->literal_previous3.size != fields->streams[1].size ||
        fields->literal_word_contexts.size != fields->streams[1].size * 4u ||
        fields->literal_match_bytes.size != fields->streams[1].size ||
        fields->literal_matched_flags.size != fields->streams[1].size)
        fail("inconsistent rep-distance fields");
    Buffer streams[8] = {{0}};
    append_bytes(&streams[0], fields->streams[0].data, fields->streams[0].size);
    append_bytes(&streams[1], fields->streams[1].data, fields->streams[1].size);
    size_t recent[4] = {0};
    for (size_t i = 0; i < match_count; ++i) {
        uint32_t encoded = (uint32_t)fields->streams[2].data[i] |
                           ((uint32_t)fields->streams[3].data[i] << 8u) |
                           ((uint32_t)fields->streams[4].data[i] << 16u);
        size_t distance = (size_t)encoded + 1u;
        unsigned code = 4;
        for (unsigned slot = 0; slot < 4; ++slot) {
            if (recent[slot] == distance) {
                code = slot;
                break;
            }
        }
        append_byte(&streams[2], (uint8_t)code);
        if (code == 4) {
            append_byte(&streams[3], (uint8_t)encoded);
            append_byte(&streams[4], (uint8_t)(encoded >> 8u));
            append_byte(&streams[5], (uint8_t)(encoded >> 16u));
            for (unsigned slot = 3; slot > 0; --slot)
                recent[slot] = recent[slot - 1u];
        } else {
            for (unsigned slot = code; slot > 0; --slot)
                recent[slot] = recent[slot - 1u];
        }
        recent[0] = distance;
        append_byte(&streams[6], fields->streams[5].data[i]);
        append_byte(&streams[7], fields->streams[6].data[i]);
    }

    Buffer encoded_streams[8] = {{0}};
    uint8_t adaptive_mask = 0;
    uint8_t static_mask = 0;
    uint8_t special_mask = 0;
    for (unsigned i = 0; i < 8; ++i) {
        Buffer huffman = compact_huffman_encode(streams[i].data, streams[i].size);
        Buffer adaptive = adaptive_arithmetic_encode(streams[i].data, streams[i].size);
        Buffer fixed = static_arithmetic_encode(streams[i].data, streams[i].size);
        Buffer special = {0};
        if (i == 0)
            special = adaptive_control_bit_encode(fields->token_controls.data,
                                                  fields->token_controls.size);
        else if (i == 1)
            special = adaptive_matched_literal_bit_encode(
                streams[i].data, fields->literal_previous.data,
                fields->literal_previous2.data, fields->literal_previous3.data,
                fields->literal_word_contexts.data,
                fields->literal_match_bytes.data,
                fields->literal_matched_flags.data,
                streams[i].size, literal_context_order);
        if (special.data && special.size < fixed.size &&
            special.size < adaptive.size && special.size < huffman.size) {
            encoded_streams[i] = special;
            special_mask |= (uint8_t)(1u << i);
            free(fixed.data);
            free(adaptive.data);
            free(huffman.data);
        } else if (fixed.size < adaptive.size && fixed.size < huffman.size) {
            encoded_streams[i] = fixed;
            static_mask |= (uint8_t)(1u << i);
            free(special.data);
            free(adaptive.data);
            free(huffman.data);
        } else if (adaptive.size < huffman.size) {
            encoded_streams[i] = adaptive;
            adaptive_mask |= (uint8_t)(1u << i);
            free(special.data);
            free(fixed.data);
            free(huffman.data);
        } else {
            encoded_streams[i] = huffman;
            free(special.data);
            free(fixed.data);
            free(adaptive.data);
        }
    }
    Buffer output = {0};
    append_byte(&output, adaptive_mask);
    append_byte(&output, static_mask);
    append_byte(&output, special_mask);
    for (unsigned i = 0; i < 8; ++i) {
        if (encoded_streams[i].size > UINT32_MAX)
            fail("rep-distance stream too large");
        append_u32le(&output, (uint32_t)encoded_streams[i].size);
    }
    for (unsigned i = 0; i < 8; ++i) {
        append_bytes(&output, encoded_streams[i].data, encoded_streams[i].size);
        free(encoded_streams[i].data);
        free(streams[i].data);
    }
    return output;
}

static Buffer encode_wide_fields_static_hybrid(WideFields *fields) {
    Buffer encoded[7] = {{0}};
    Buffer output = {0};
    uint8_t adaptive_mask = 0;
    uint8_t static_mask = 0;
    for (unsigned i = 0; i < fields->count; ++i) {
        Buffer huffman = compact_huffman_encode(fields->streams[i].data,
                                                 fields->streams[i].size);
        Buffer adaptive = adaptive_arithmetic_encode(fields->streams[i].data,
                                                      fields->streams[i].size);
        Buffer fixed = static_arithmetic_encode(fields->streams[i].data,
                                                fields->streams[i].size);
        if (fixed.size < adaptive.size && fixed.size < huffman.size) {
            encoded[i] = fixed;
            static_mask |= (uint8_t)(1u << i);
            free(adaptive.data);
            free(huffman.data);
        } else if (adaptive.size < huffman.size) {
            encoded[i] = adaptive;
            adaptive_mask |= (uint8_t)(1u << i);
            free(fixed.data);
            free(huffman.data);
        } else {
            encoded[i] = huffman;
            free(fixed.data);
            free(adaptive.data);
        }
    }
    append_byte(&output, adaptive_mask);
    append_byte(&output, static_mask);
    for (unsigned i = 0; i < fields->count; ++i) {
        if (encoded[i].size > UINT32_MAX) fail("wide split stream too large");
        append_u32le(&output, (uint32_t)encoded[i].size);
    }
    for (unsigned i = 0; i < fields->count; ++i) {
        append_bytes(&output, encoded[i].data, encoded[i].size);
        free(encoded[i].data);
    }
    return output;
}

static Buffer encode_wide_fields_distance_context_hybrid(WideFields *fields) {
    if (fields->distance_contexts.size != fields->streams[2].size ||
        fields->streams[3].size != fields->streams[2].size ||
        fields->streams[4].size != fields->streams[2].size)
        fail("inconsistent distance arithmetic contexts");
    Buffer encoded[7] = {{0}};
    Buffer output = {0};
    uint8_t adaptive_mask = 0;
    uint8_t context_mask = 0;
    for (unsigned i = 0; i < fields->count; ++i) {
        Buffer huffman = compact_huffman_encode(fields->streams[i].data,
                                                 fields->streams[i].size);
        Buffer adaptive = adaptive_arithmetic_encode(fields->streams[i].data,
                                                      fields->streams[i].size);
        Buffer contextual = {0};
        if (i >= 2 && i <= 4)
            contextual = adaptive_context_arithmetic_encode(
                fields->streams[i].data, fields->distance_contexts.data,
                fields->streams[i].size);
        if (contextual.data && contextual.size < adaptive.size &&
            contextual.size < huffman.size) {
            encoded[i] = contextual;
            context_mask |= (uint8_t)(1u << i);
            free(adaptive.data);
            free(huffman.data);
        } else if (adaptive.size < huffman.size) {
            encoded[i] = adaptive;
            adaptive_mask |= (uint8_t)(1u << i);
            free(contextual.data);
            free(huffman.data);
        } else {
            encoded[i] = huffman;
            free(contextual.data);
            free(adaptive.data);
        }
    }
    append_byte(&output, adaptive_mask);
    append_byte(&output, context_mask);
    for (unsigned i = 0; i < fields->count; ++i) {
        if (encoded[i].size > UINT32_MAX) fail("wide split stream too large");
        append_u32le(&output, (uint32_t)encoded[i].size);
    }
    for (unsigned i = 0; i < fields->count; ++i) {
        append_bytes(&output, encoded[i].data, encoded[i].size);
        free(encoded[i].data);
    }
    return output;
}

static void free_wide_fields(WideFields *fields) {
    for (unsigned i = 0; i < fields->count; ++i) free(fields->streams[i].data);
    free(fields->token_controls.data);
    free(fields->literal_contexts.data);
    free(fields->literal_previous.data);
    free(fields->literal_previous2.data);
    free(fields->literal_previous3.data);
    free(fields->literal_word_contexts.data);
    free(fields->literal_match_bytes.data);
    free(fields->literal_matched_flags.data);
    free(fields->distance_contexts.data);
    *fields = (WideFields){0};
}

static unsigned recent_distance_code(const size_t recent[4], size_t distance) {
    for (unsigned slot = 0; slot < 4; ++slot)
        if (recent[slot] == distance) return slot;
    return 4;
}

static void update_recent_distances(size_t recent[4], size_t distance,
                                    unsigned code) {
    unsigned start = code < 4 ? code : 3;
    for (unsigned slot = start; slot > 0; --slot)
        recent[slot] = recent[slot - 1u];
    recent[0] = distance;
}

static void rep_aware_wide_parse(const uint8_t *input, size_t start,
                                 size_t decoded_size,
                                 const WideFields *model,
                                 const uint16_t *base_choices,
                                 const uint32_t *base_offsets,
                                 uint16_t *choices,
                                 uint32_t *offsets) {
    if (!decoded_size) return;
    uint16_t literal_costs[256], distance_costs[3][256];
    uint16_t length_costs[2][256], rep_code_costs[256];
    static_arithmetic_symbol_costs(&model->streams[1], literal_costs);
    for (unsigned i = 0; i < 2; ++i)
        static_arithmetic_symbol_costs(&model->streams[5u + i], length_costs[i]);

    Buffer rep_codes = {0};
    Buffer raw_distances[3] = {{0}};
    size_t recent[4] = {0};
    for (size_t i = 0; i < model->streams[2].size; ++i) {
        uint32_t encoded = (uint32_t)model->streams[2].data[i] |
                           ((uint32_t)model->streams[3].data[i] << 8u) |
                           ((uint32_t)model->streams[4].data[i] << 16u);
        size_t distance = (size_t)encoded + 1u;
        unsigned code = recent_distance_code(recent, distance);
        append_byte(&rep_codes, (uint8_t)code);
        if (code == 4)
            for (unsigned stream = 0; stream < 3; ++stream)
                append_byte(&raw_distances[stream],
                            (uint8_t)(encoded >> (8u * stream)));
        update_recent_distances(recent, distance, code);
    }
    static_arithmetic_symbol_costs(&rep_codes, rep_code_costs);
    free(rep_codes.data);
    for (unsigned i = 0; i < 3; ++i) {
        static_arithmetic_symbol_costs(&raw_distances[i], distance_costs[i]);
        free(raw_distances[i].data);
    }

    uint64_t *base_costs = (uint64_t *)malloc(
        (decoded_size + 1u) * sizeof(*base_costs));
    if (!base_costs) fail("out of memory");
    base_costs[decoded_size] = 0;
    const uint64_t control_cost = 256u;
    for (size_t reverse = decoded_size; reverse > 0; --reverse) {
        size_t pos = reverse - 1u;
        size_t length = base_choices[pos];
        if (!length) {
            base_costs[pos] = control_cost +
                literal_costs[input[start + pos]] + base_costs[pos + 1u];
            continue;
        }
        if (length < MIN_MATCH || length > decoded_size - pos)
            fail("invalid base rep-aware match");
        uint32_t encoded_distance = base_offsets[pos] - 1u;
        uint16_t encoded_length = (uint16_t)(length - MIN_MATCH);
        base_costs[pos] = control_cost + rep_code_costs[4] +
            distance_costs[0][(uint8_t)encoded_distance] +
            distance_costs[1][(uint8_t)(encoded_distance >> 8u)] +
            distance_costs[2][(uint8_t)(encoded_distance >> 16u)] +
            length_costs[0][(uint8_t)encoded_length] +
            length_costs[1][(uint8_t)(encoded_length >> 8u)] +
            base_costs[pos + length];
    }

    memcpy(choices, base_choices, decoded_size * sizeof(*choices));
    memcpy(offsets, base_offsets, decoded_size * sizeof(*offsets));
    memset(recent, 0, sizeof(recent));
    size_t pos = 0;
    while (pos < decoded_size) {
        size_t best_length = 0;
        size_t best_distance = 0;
        uint64_t best_cost = control_cost + literal_costs[input[start + pos]] +
                             base_costs[pos + 1u];

        size_t base_length = base_choices[pos];
        if (base_length) {
            size_t distance = base_offsets[pos];
            unsigned code = recent_distance_code(recent, distance);
            uint16_t encoded_length = (uint16_t)(base_length - MIN_MATCH);
            uint64_t action_cost = control_cost + rep_code_costs[code] +
                length_costs[0][(uint8_t)encoded_length] +
                length_costs[1][(uint8_t)(encoded_length >> 8u)];
            if (code == 4) {
                uint32_t encoded_distance = (uint32_t)(distance - 1u);
                action_cost += distance_costs[0][(uint8_t)encoded_distance] +
                    distance_costs[1][(uint8_t)(encoded_distance >> 8u)] +
                    distance_costs[2][(uint8_t)(encoded_distance >> 16u)];
            }
            uint64_t score = action_cost + base_costs[pos + base_length];
            if (score < best_cost ||
                (score == best_cost && base_length > best_length)) {
                best_cost = score;
                best_length = base_length;
                best_distance = distance;
            }
        }

        size_t absolute = start + pos;
        for (unsigned code = 0; code < 4; ++code) {
            size_t distance = recent[code];
            if (!distance || distance > absolute) continue;
            size_t limit = decoded_size - pos;
            if (limit > WIDE_MAX_MATCH) limit = WIDE_MAX_MATCH;
            size_t maximum = 0;
            while (maximum < limit &&
                   input[absolute + maximum] ==
                   input[absolute + maximum - distance])
                ++maximum;
            if (maximum < MIN_MATCH) continue;
            size_t minimum = maximum > 15u ? maximum - 15u : MIN_MATCH;
            if (minimum < MIN_MATCH) minimum = MIN_MATCH;
            for (size_t length = minimum; length <= maximum; ++length) {
                uint16_t encoded_length = (uint16_t)(length - MIN_MATCH);
                uint64_t score = control_cost + rep_code_costs[code] +
                    length_costs[0][(uint8_t)encoded_length] +
                    length_costs[1][(uint8_t)(encoded_length >> 8u)] +
                    base_costs[pos + length];
                if (score < best_cost ||
                    (score == best_cost && length > best_length)) {
                    best_cost = score;
                    best_length = length;
                    best_distance = distance;
                }
            }
        }

        choices[pos] = (uint16_t)best_length;
        offsets[pos] = (uint32_t)best_distance;
        if (!best_length) {
            ++pos;
        } else {
            unsigned code = recent_distance_code(recent, best_distance);
            update_recent_distances(recent, best_distance, code);
            pos += best_length;
        }
    }
    free(base_costs);
}

#define REP_BEAM_WIDTH 4u
#define REP_BEAM_CHUNK 65536u

typedef struct {
    uint64_t cost;
    size_t recent[4];
    uint32_t previous_pos;
    uint32_t offset;
    uint16_t length;
    uint8_t previous_slot;
    uint8_t valid;
} RepBeamState;

static void insert_rep_beam_state(RepBeamState states[REP_BEAM_WIDTH],
                                  const RepBeamState *candidate) {
    for (unsigned slot = 0; slot < REP_BEAM_WIDTH; ++slot) {
        if (states[slot].valid &&
            memcmp(states[slot].recent, candidate->recent,
                   sizeof(candidate->recent)) == 0) {
            if (candidate->cost < states[slot].cost)
                states[slot] = *candidate;
            return;
        }
    }
    for (unsigned slot = 0; slot < REP_BEAM_WIDTH; ++slot) {
        if (!states[slot].valid) {
            states[slot] = *candidate;
            return;
        }
    }
    unsigned worst = 0;
    for (unsigned slot = 1; slot < REP_BEAM_WIDTH; ++slot)
        if (states[slot].cost > states[worst].cost) worst = slot;
    if (candidate->cost < states[worst].cost)
        states[worst] = *candidate;
}

static uint64_t rep_beam_match_cost(const uint16_t distance_costs[3][256],
                                    const uint16_t length_costs[2][256],
                                    const uint16_t rep_code_costs[256],
                                    size_t length, size_t distance,
                                    unsigned code) {
    uint16_t encoded_length = (uint16_t)(length - MIN_MATCH);
    uint64_t cost = 256u + rep_code_costs[code] +
        length_costs[0][(uint8_t)encoded_length] +
        length_costs[1][(uint8_t)(encoded_length >> 8u)];
    if (code == 4) {
        uint32_t encoded_distance = (uint32_t)(distance - 1u);
        cost += distance_costs[0][(uint8_t)encoded_distance] +
            distance_costs[1][(uint8_t)(encoded_distance >> 8u)] +
            distance_costs[2][(uint8_t)(encoded_distance >> 16u)];
    }
    return cost;
}

static void rep_beam_wide_parse(const uint8_t *input, size_t start,
                                size_t decoded_size,
                                const WideFields *model,
                                const uint16_t *base_choices,
                                const uint32_t *base_offsets,
                                uint16_t *choices,
                                uint32_t *offsets) {
    if (!decoded_size) return;
    uint16_t literal_costs[256], distance_costs[3][256];
    uint16_t length_costs[2][256], rep_code_costs[256];
    static_arithmetic_symbol_costs(&model->streams[1], literal_costs);
    for (unsigned i = 0; i < 2; ++i)
        static_arithmetic_symbol_costs(&model->streams[5u + i], length_costs[i]);
    Buffer rep_codes = {0};
    Buffer raw_distances[3] = {{0}};
    size_t model_recent[4] = {0};
    for (size_t i = 0; i < model->streams[2].size; ++i) {
        uint32_t encoded = (uint32_t)model->streams[2].data[i] |
                           ((uint32_t)model->streams[3].data[i] << 8u) |
                           ((uint32_t)model->streams[4].data[i] << 16u);
        size_t distance = (size_t)encoded + 1u;
        unsigned code = recent_distance_code(model_recent, distance);
        append_byte(&rep_codes, (uint8_t)code);
        if (code == 4)
            for (unsigned stream = 0; stream < 3; ++stream)
                append_byte(&raw_distances[stream],
                            (uint8_t)(encoded >> (8u * stream)));
        update_recent_distances(model_recent, distance, code);
    }
    static_arithmetic_symbol_costs(&rep_codes, rep_code_costs);
    free(rep_codes.data);
    for (unsigned i = 0; i < 3; ++i) {
        static_arithmetic_symbol_costs(&raw_distances[i], distance_costs[i]);
        free(raw_distances[i].data);
    }

    memcpy(choices, base_choices, decoded_size * sizeof(*choices));
    memcpy(offsets, base_offsets, decoded_size * sizeof(*offsets));
    size_t carried_recent[4] = {0};
    size_t maximum_chunk = decoded_size < REP_BEAM_CHUNK
        ? decoded_size : REP_BEAM_CHUNK;
    RepBeamState *states = (RepBeamState *)calloc(
        (maximum_chunk + 1u) * REP_BEAM_WIDTH, sizeof(*states));
    if (!states) fail("out of memory");

    for (size_t chunk_start = 0; chunk_start < decoded_size;
         chunk_start += REP_BEAM_CHUNK) {
        size_t chunk_size = decoded_size - chunk_start;
        if (chunk_size > REP_BEAM_CHUNK) chunk_size = REP_BEAM_CHUNK;
        memset(states, 0,
               (chunk_size + 1u) * REP_BEAM_WIDTH * sizeof(*states));
        RepBeamState *initial = &states[0];
        initial->valid = 1;
        for (unsigned i = 0; i < 4; ++i)
            initial->recent[i] = (uint32_t)carried_recent[i];

        for (size_t relative = 0; relative < chunk_size; ++relative) {
            size_t absolute_relative = chunk_start + relative;
            size_t absolute = start + absolute_relative;
            RepBeamState *source_states =
                &states[relative * REP_BEAM_WIDTH];
            for (unsigned source_slot = 0; source_slot < REP_BEAM_WIDTH;
                 ++source_slot) {
                const RepBeamState *source = &source_states[source_slot];
                if (!source->valid) continue;

                RepBeamState literal = *source;
                literal.valid = 1;
                literal.cost += 256u + literal_costs[input[absolute]];
                literal.previous_pos = (uint32_t)relative;
                literal.previous_slot = (uint8_t)source_slot;
                literal.length = 0;
                literal.offset = 0;
                insert_rep_beam_state(
                    &states[(relative + 1u) * REP_BEAM_WIDTH], &literal);

                size_t base_length = base_choices[absolute_relative];
                if (base_length) {
                    size_t remaining = chunk_size - relative;
                    if (base_length > remaining) base_length = remaining;
                    if (base_length >= MIN_MATCH) {
                        size_t distance = base_offsets[absolute_relative];
                        unsigned code = recent_distance_code(
                            source->recent, distance);
                        RepBeamState match = *source;
                        match.valid = 1;
                        match.cost += rep_beam_match_cost(
                            distance_costs, length_costs, rep_code_costs,
                            base_length, distance, code);
                        update_recent_distances(
                            match.recent, distance, code);
                        match.previous_pos = (uint32_t)relative;
                        match.previous_slot = (uint8_t)source_slot;
                        match.length = (uint16_t)base_length;
                        match.offset = (uint32_t)distance;
                        insert_rep_beam_state(
                            &states[(relative + base_length) * REP_BEAM_WIDTH],
                            &match);
                    }
                }

                for (unsigned code = 0; code < 4; ++code) {
                    size_t distance = source->recent[code];
                    if (!distance || distance > absolute) continue;
                    size_t limit = chunk_size - relative;
                    if (limit > WIDE_MAX_MATCH) limit = WIDE_MAX_MATCH;
                    size_t maximum = 0;
                    while (maximum < limit &&
                           input[absolute + maximum] ==
                           input[absolute + maximum - distance])
                        ++maximum;
                    if (maximum < MIN_MATCH) continue;
                    size_t minimum = maximum > 15u
                        ? maximum - 15u : MIN_MATCH;
                    if (minimum < MIN_MATCH) minimum = MIN_MATCH;
                    for (size_t length = minimum; length <= maximum; ++length) {
                        RepBeamState match = *source;
                        match.valid = 1;
                        match.cost += rep_beam_match_cost(
                            distance_costs, length_costs, rep_code_costs,
                            length, distance, code);
                        update_recent_distances(
                            match.recent, distance, code);
                        match.previous_pos = (uint32_t)relative;
                        match.previous_slot = (uint8_t)source_slot;
                        match.length = (uint16_t)length;
                        match.offset = (uint32_t)distance;
                        insert_rep_beam_state(
                            &states[(relative + length) * REP_BEAM_WIDTH],
                            &match);
                    }
                }
            }
        }

        RepBeamState *final_states = &states[chunk_size * REP_BEAM_WIDTH];
        unsigned selected_slot = REP_BEAM_WIDTH;
        for (unsigned slot = 0; slot < REP_BEAM_WIDTH; ++slot)
            if (final_states[slot].valid &&
                (selected_slot == REP_BEAM_WIDTH ||
                 final_states[slot].cost < final_states[selected_slot].cost))
                selected_slot = slot;
        if (selected_slot == REP_BEAM_WIDTH) fail("rep beam parse failed");
        for (unsigned i = 0; i < 4; ++i)
            carried_recent[i] = final_states[selected_slot].recent[i];

        size_t cursor = chunk_size;
        unsigned slot = selected_slot;
        while (cursor) {
            const RepBeamState *state =
                &states[cursor * REP_BEAM_WIDTH + slot];
            size_t previous = state->previous_pos;
            choices[chunk_start + previous] = state->length;
            offsets[chunk_start + previous] = state->offset;
            slot = state->previous_slot;
            cursor = previous;
        }
    }
    free(states);
}

static void find_wide_costed_offsets(const uint8_t *input, size_t pos,
                                     size_t block_end, const WideMatchIndex *index,
                                     const uint16_t distance_costs[3][256],
                                     size_t maximum, uint32_t offsets[16]) {
    size_t minimum = maximum > 15u ? maximum - 15u : MIN_MATCH;
    if (minimum < MIN_MATCH) minimum = MIN_MATCH;
    size_t choice_count = maximum - minimum + 1u;
    uint32_t bucket_costs[16];
    uint32_t bucket_offsets[16] = {0};
    for (size_t i = 0; i < choice_count; ++i) bucket_costs[i] = UINT32_MAX;
    uint32_t fallback_offset = index->offsets[pos];
    if (!fallback_offset) fail("missing wide match offset");
    uint32_t fallback_encoded = fallback_offset - 1u;
    bucket_costs[choice_count - 1u] =
        distance_costs[0][(uint8_t)fallback_encoded] +
        distance_costs[1][(uint8_t)(fallback_encoded >> 8)] +
        distance_costs[2][(uint8_t)(fallback_encoded >> 16)];
    bucket_offsets[choice_count - 1u] = fallback_offset;
    int32_t candidate = index->previous[pos];
    unsigned depth = 0;
    while (candidate >= 0 && depth++ < index->cost_chain) {
        size_t c = (size_t)candidate;
        size_t distance = pos - c;
        if (distance > WIDE_WINDOW) break;
        if (input[c + minimum - 1u] == input[pos + minimum - 1u]) {
            size_t limit = block_end - pos;
            if (limit > WIDE_MAX_MATCH) limit = WIDE_MAX_MATCH;
            size_t length = 0;
            while (length < limit && input[c + length] == input[pos + length]) ++length;
            if (length >= minimum) {
                size_t bucket = length >= maximum ? choice_count - 1u : length - minimum;
                uint32_t encoded = (uint32_t)(distance - 1u);
                uint32_t cost = distance_costs[0][(uint8_t)encoded] +
                                distance_costs[1][(uint8_t)(encoded >> 8)] +
                                distance_costs[2][(uint8_t)(encoded >> 16)];
                if (cost < bucket_costs[bucket] ||
                    (cost == bucket_costs[bucket] && distance < bucket_offsets[bucket])) {
                    bucket_costs[bucket] = cost;
                    bucket_offsets[bucket] = (uint32_t)distance;
                }
            }
        }
        candidate = index->previous[c];
    }
    uint32_t lowest_cost = UINT32_MAX;
    uint32_t selected_offset = 0;
    for (size_t reverse = choice_count; reverse > 0; --reverse) {
        size_t bucket = reverse - 1u;
        if (bucket_costs[bucket] < lowest_cost ||
            (bucket_costs[bucket] == lowest_cost && bucket_offsets[bucket] < selected_offset)) {
            lowest_cost = bucket_costs[bucket];
            selected_offset = bucket_offsets[bucket];
        }
        offsets[bucket] = selected_offset;
    }
    if (!selected_offset) fail("wide match index inconsistency");
}

static void parse_wide_fields(const uint8_t *input, size_t start,
                              size_t decoded_size, const WideMatchIndex *index,
                              const WideFields *model, uint16_t *choices,
                              uint32_t *choice_offsets, int static_cost_model) {
    uint16_t symbol_costs[6][256];
    for (unsigned field = 0; field + 1u < model->count; ++field)
        if (static_cost_model)
            static_arithmetic_symbol_costs(&model->streams[field + 1u],
                                           symbol_costs[field]);
        else
            huffman_symbol_costs(&model->streams[field + 1u], symbol_costs[field]);
    unsigned distance_stream = 2;
    uint64_t control_cost = static_cost_model ? 256u : 1u;
    uint64_t *costs = (uint64_t *)malloc((decoded_size + 1u) * sizeof(*costs));
    if (!costs) fail("out of memory");
    costs[decoded_size] = 0;
    for (size_t reverse = decoded_size; reverse > 0; --reverse) {
        size_t pos = reverse - 1u;
        choices[pos] = 0;
        costs[pos] = control_cost + symbol_costs[0][input[start + pos]] + costs[pos + 1u];
        size_t maximum = index->lengths[start + pos];
        if (maximum > decoded_size - pos) maximum = decoded_size - pos;
        if (maximum < MIN_MATCH) continue;
        size_t minimum = maximum > 15u ? maximum - 15u : MIN_MATCH;
        if (minimum < MIN_MATCH) minimum = MIN_MATCH;
        uint32_t offsets[16];
        find_wide_costed_offsets(input, start + pos, start + decoded_size, index,
                                 (const uint16_t (*)[256])&symbol_costs[distance_stream - 1u],
                                 maximum, offsets);
        for (size_t length = minimum; length <= maximum; ++length) {
            uint32_t selected_offset = offsets[length - minimum];
            uint32_t encoded_offset = selected_offset - 1u;
            uint64_t distance_cost =
                symbol_costs[distance_stream - 1u][(uint8_t)encoded_offset] +
                symbol_costs[distance_stream][(uint8_t)(encoded_offset >> 8)] +
                symbol_costs[distance_stream + 1u][(uint8_t)(encoded_offset >> 16)];
            uint16_t encoded_length = (uint16_t)(length - MIN_MATCH);
            uint64_t match_cost = control_cost + distance_cost +
                                  symbol_costs[distance_stream + 2u][(uint8_t)encoded_length] +
                                  symbol_costs[distance_stream + 3u]
                                              [(uint8_t)(encoded_length >> 8)] +
                                  costs[pos + length];
            if (match_cost < costs[pos]) {
                costs[pos] = match_cost;
                choices[pos] = (uint16_t)length;
                choice_offsets[pos] = selected_offset;
            }
        }
    }
    free(costs);
}

typedef struct {
    Buffer data;
    uint8_t mode;
} WideEncoding;

static void consider_wide_encoding(WideEncoding *best, Buffer candidate, uint8_t mode) {
    if (!best->data.data || candidate.size < best->data.size) {
        free(best->data.data);
        best->data = candidate;
        best->mode = mode;
    } else {
        free(candidate.data);
    }
}

static void encode_wide_candidate(WideEncoding *best, WideFields *fields,
                                  int arithmetic_level) {
    consider_wide_encoding(best, encode_wide_fields(fields), 9);
    if (arithmetic_level > 0)
        consider_wide_encoding(best, encode_wide_fields_hybrid(fields), 10);
    if (arithmetic_level > 1)
        consider_wide_encoding(best, encode_wide_fields_order1_hybrid(fields), 11);
    if (arithmetic_level > 2)
        consider_wide_encoding(best, encode_wide_fields_context_hybrid(fields), 12);
    if (arithmetic_level > 3)
        consider_wide_encoding(best, encode_wide_fields_static_hybrid(fields), 13);
    if (arithmetic_level == 12)
        consider_wide_encoding(best, encode_wide_fields_distance_context_hybrid(fields), 15);
}

static WideEncoding wide_split_huffman_encode(const uint8_t *input, size_t input_size,
                                              size_t start, size_t decoded_size,
                                              const WideMatchIndex *index,
                                              int arithmetic_level) {
    int fast_preset = arithmetic_level == 25;
    int context_mix_candidate = arithmetic_level == 27;
    if (fast_preset) arithmetic_level = 19;
    if (context_mix_candidate) arithmetic_level = 19;
    uint16_t *choices = decoded_size
        ? (uint16_t *)malloc(decoded_size * sizeof(*choices)) : NULL;
    uint32_t *choice_offsets = decoded_size
        ? (uint32_t *)malloc(decoded_size * sizeof(*choice_offsets)) : NULL;
    if (decoded_size && (!choices || !choice_offsets)) fail("out of memory");
    WideFields greedy = build_wide_fields(
        input, input_size, start, decoded_size, index, NULL, NULL);
    WideEncoding best = {0};
    if (!fast_preset) encode_wide_candidate(&best, &greedy, arithmetic_level);

    parse_wide_fields(input, start, decoded_size, index, &greedy, choices, choice_offsets, 0);
    WideFields optimized = build_wide_fields(
        input, input_size, start, decoded_size, index, choices, choice_offsets);
    if (!fast_preset) encode_wide_candidate(&best, &optimized, arithmetic_level);

    parse_wide_fields(input, start, decoded_size, index, &optimized, choices, choice_offsets, 0);
    WideFields iterated = build_wide_fields(
        input, input_size, start, decoded_size, index, choices, choice_offsets);
    if (!fast_preset) encode_wide_candidate(&best, &iterated, arithmetic_level);

    if (arithmetic_level > 4) {
        parse_wide_fields(input, start, decoded_size, index, &iterated,
                          choices, choice_offsets, 1);
        WideFields arithmetic = build_wide_fields(
            input, input_size, start, decoded_size, index, choices, choice_offsets);
        if (!fast_preset) encode_wide_candidate(&best, &arithmetic, arithmetic_level);
        if (arithmetic_level > 5) {
            parse_wide_fields(input, start, decoded_size, index, &arithmetic,
                              choices, choice_offsets, 1);
            WideFields arithmetic_iterated = build_wide_fields(
                input, input_size, start, decoded_size, index, choices, choice_offsets);
            if (!fast_preset)
                encode_wide_candidate(&best, &arithmetic_iterated, arithmetic_level);
            if (arithmetic_level > 12)
                consider_wide_encoding(
                    &best, encode_wide_fields_literal_bit_hybrid(&arithmetic_iterated), 16);
            if (arithmetic_level > 13)
                consider_wide_encoding(
                    &best, encode_wide_fields_matched_literal_hybrid(
                        &arithmetic_iterated, 0),
                    17);
            if (arithmetic_level > 14)
                consider_wide_encoding(
                    &best, encode_wide_fields_matched_literal_hybrid(
                        &arithmetic_iterated, 1),
                    18);
            if (arithmetic_level > 16)
                consider_wide_encoding(
                    &best, encode_wide_fields_control_matched_hybrid(
                        &arithmetic_iterated, 2),
                    20);
            if (arithmetic_level > 18)
                consider_wide_encoding(
                    &best, encode_wide_fields_rep_hybrid(&arithmetic_iterated, 2), 22);
            if (context_mix_candidate) {
                Buffer context_mix = encode_wide_fields_rep_hybrid(
                    &arithmetic_iterated, 3);
                if (getenv("BITE_PROFILE"))
                    fprintf(stderr,
                            "bite profile: mode24=%zu current-best=%zu\n",
                            context_mix.size, best.data.size);
                consider_wide_encoding(&best, context_mix, 24);
            }
            if (arithmetic_level > 19) {
                uint16_t *rep_choices = decoded_size
                    ? (uint16_t *)malloc(decoded_size * sizeof(*rep_choices)) : NULL;
                uint32_t *rep_offsets = decoded_size
                    ? (uint32_t *)malloc(decoded_size * sizeof(*rep_offsets)) : NULL;
                if (decoded_size && (!rep_choices || !rep_offsets))
                    fail("out of memory");
                rep_aware_wide_parse(input, start, decoded_size,
                                     &arithmetic_iterated, choices, choice_offsets,
                                     rep_choices, rep_offsets);
                WideFields rep_fields = build_wide_fields(
                    input, input_size, start, decoded_size, index,
                    rep_choices, rep_offsets);
                consider_wide_encoding(
                    &best, encode_wide_fields_rep_hybrid(&rep_fields, 2), 22);
                if (arithmetic_level == 21) {
                    uint16_t *iterated_choices = decoded_size
                        ? (uint16_t *)malloc(
                            decoded_size * sizeof(*iterated_choices)) : NULL;
                    uint32_t *iterated_offsets = decoded_size
                        ? (uint32_t *)malloc(
                            decoded_size * sizeof(*iterated_offsets)) : NULL;
                    if (decoded_size && (!iterated_choices || !iterated_offsets))
                        fail("out of memory");
                    rep_aware_wide_parse(input, start, decoded_size,
                                         &rep_fields, rep_choices, rep_offsets,
                                         iterated_choices, iterated_offsets);
                    WideFields iterated_rep_fields = build_wide_fields(
                        input, input_size, start, decoded_size, index,
                        iterated_choices, iterated_offsets);
                    consider_wide_encoding(
                        &best, encode_wide_fields_rep_hybrid(
                            &iterated_rep_fields, 2), 22);
                    free_wide_fields(&iterated_rep_fields);
                    free(iterated_offsets);
                    free(iterated_choices);
                }
                if (arithmetic_level > 21) {
                    uint16_t *beam_choices = decoded_size
                        ? (uint16_t *)malloc(
                            decoded_size * sizeof(*beam_choices)) : NULL;
                    uint32_t *beam_offsets = decoded_size
                        ? (uint32_t *)malloc(
                            decoded_size * sizeof(*beam_offsets)) : NULL;
                    if (decoded_size && (!beam_choices || !beam_offsets))
                        fail("out of memory");
                    rep_beam_wide_parse(input, start, decoded_size,
                                        &rep_fields, rep_choices, rep_offsets,
                                        beam_choices, beam_offsets);
                    WideFields beam_fields = build_wide_fields(
                        input, input_size, start, decoded_size, index,
                        beam_choices, beam_offsets);
                    consider_wide_encoding(
                        &best, encode_wide_fields_rep_hybrid(&beam_fields, 2), 22);
                    free_wide_fields(&beam_fields);
                    free(beam_offsets);
                    free(beam_choices);
                }
                free_wide_fields(&rep_fields);
                free(rep_offsets);
                free(rep_choices);
            }
            free_wide_fields(&arithmetic_iterated);
        }
        free_wide_fields(&arithmetic);
    }

    free_wide_fields(&iterated);
    free_wide_fields(&optimized);
    free_wide_fields(&greedy);
    free(choice_offsets);
    free(choices);
    return best;
}

static void wide_split_huffman_decode(Buffer *output, const uint8_t *input,
                                      size_t input_size, size_t decoded_size,
                                      size_t match_base, int fast_lookup,
                                      int arithmetic_level) {
    unsigned stream_count = 7;
    unsigned distance_stream = 2;
    uint8_t arithmetic_mask = 0;
    uint8_t order1_mask = 0;
    uint8_t context_mask = 0;
    uint8_t distance_context_mask = 0;
    uint8_t control_bit_mask = 0;
    uint8_t literal_bit_mask = 0;
    uint8_t matched_literal_mask = 0;
    uint8_t static_mask = 0;
    if (arithmetic_level > 0) {
        if (!input_size) fail("truncated hybrid wide split header");
        arithmetic_mask = *input++;
        --input_size;
        if (arithmetic_mask & 0x80u) fail("invalid hybrid wide split mask");
    }
    if (arithmetic_level > 1) {
        if (!input_size) fail("truncated order-1 wide split header");
        uint8_t secondary_mask = *input++;
        --input_size;
        if (arithmetic_level == 2) order1_mask = secondary_mask;
        else if (arithmetic_level == 3) context_mask = secondary_mask;
        else if (arithmetic_level == 4) static_mask = secondary_mask;
        else if (arithmetic_level == 5) distance_context_mask = secondary_mask;
        else if (arithmetic_level == 6) literal_bit_mask = secondary_mask;
        else if (arithmetic_level == 9) {
            control_bit_mask = secondary_mask & 1u;
            matched_literal_mask = secondary_mask & (1u << 1);
            if (secondary_mask & ~3u)
                fail("invalid control/matched wide split mask");
        } else matched_literal_mask = secondary_mask;
        if ((secondary_mask & 0x80u) || (secondary_mask & arithmetic_mask))
            fail("invalid secondary wide split mask");
        if (context_mask && context_mask != (1u << 1))
            fail("invalid literal-context wide split mask");
        if (distance_context_mask & ~((1u << 2) | (1u << 3) | (1u << 4)))
            fail("invalid distance-context wide split mask");
        if (literal_bit_mask && literal_bit_mask != (1u << 1))
            fail("invalid literal-bit wide split mask");
        if (matched_literal_mask && matched_literal_mask != (1u << 1))
            fail("invalid matched-literal wide split mask");
    }
    size_t header_size = 4u * stream_count;
    if (input_size < header_size) fail("invalid wide split header");
    uint32_t sizes[7];
    size_t offset = header_size;
    for (unsigned i = 0; i < stream_count; ++i) {
        sizes[i] = read_u32le(input + 4u * i);
        if ((size_t)sizes[i] > input_size - offset) fail("invalid wide split size");
        offset += sizes[i];
    }
    if (offset != input_size) fail("trailing wide split data");
    Buffer streams[7] = {{0}};
    size_t stream_offsets[7];
    offset = header_size;
    for (unsigned i = 0; i < stream_count; ++i) {
        stream_offsets[i] = offset;
        if ((context_mask | distance_context_mask | control_bit_mask |
             literal_bit_mask | matched_literal_mask) & (1u << i)) {
        } else if (order1_mask & (1u << i))
            streams[i] = adaptive_order1_arithmetic_decode(input + offset, sizes[i]);
        else if (static_mask & (1u << i))
            streams[i] = static_arithmetic_decode(input + offset, sizes[i]);
        else if (arithmetic_mask & (1u << i))
            streams[i] = adaptive_arithmetic_decode(input + offset, sizes[i]);
        else
            streams[i] = compact_huffman_decode(input + offset, sizes[i], fast_lookup);
        offset += sizes[i];
    }
    if (control_bit_mask)
        streams[0] = adaptive_control_bit_decode(
            input + stream_offsets[0], sizes[0]);
    if (distance_context_mask) {
        if (streams[5].size != streams[6].size)
            fail("inconsistent contextual distance lengths");
        Buffer contexts = {0};
        reserve(&contexts, streams[5].size ? streams[5].size : 1);
        for (size_t i = 0; i < streams[5].size; ++i) {
            size_t length = (size_t)streams[5].data[i] |
                            ((size_t)streams[6].data[i] << 8);
            append_byte(&contexts, (uint8_t)distance_arithmetic_context(
                length + MIN_MATCH));
        }
        for (unsigned i = 2; i <= 4; ++i)
            if (distance_context_mask & (1u << i))
                streams[i] = adaptive_context_arithmetic_decode(
                    input + stream_offsets[i], sizes[i], contexts.data, contexts.size);
        free(contexts.data);
    }
    ContextArithmeticDecoder context_decoder = {0};
    size_t context_literal_size = 0;
    if (context_mask) {
        context_decoder = context_arithmetic_decoder_init(
            input + stream_offsets[1], sizes[1]);
        context_literal_size = context_decoder.remaining;
    }
    LiteralBitDecoder literal_bit_decoder = {0};
    size_t literal_bit_size = 0;
    if (literal_bit_mask) {
        literal_bit_decoder = literal_bit_decoder_init(
            input + stream_offsets[1], sizes[1]);
        literal_bit_size = literal_bit_decoder.remaining;
    }
    MatchedLiteralDecoder matched_literal_decoder = {0};
    size_t matched_literal_size = 0;
    if (matched_literal_mask) {
        matched_literal_decoder = matched_literal_decoder_init(
            input + stream_offsets[1], sizes[1],
            arithmetic_level == 8 ? 1 : (arithmetic_level == 9 ? 2 : 0));
        matched_literal_size = matched_literal_decoder.remaining;
    }
    for (unsigned i = distance_stream + 1u; i < stream_count; ++i)
        if (streams[i].size != streams[distance_stream].size)
            fail("inconsistent wide match streams");

    if (decoded_size > SIZE_MAX - output->size) fail("decoded size overflow");
    size_t target_size = output->size + decoded_size;
    size_t control_index = 0;
    size_t literal_index = 0;
    size_t match_index = 0;
    size_t last_distance = 0;
    int previous_was_match = 0;
    while (output->size < target_size) {
        if (control_index >= streams[0].size) fail("truncated wide controls");
        uint8_t control = streams[0].data[control_index++];
        for (unsigned bit = 0; bit < 8 && output->size < target_size; ++bit) {
            if ((control & (1u << bit)) == 0) {
                if (matched_literal_mask) {
                    uint8_t previous = output->size ? output->data[output->size - 1u] : 0;
                    uint8_t previous2 = output->size > 1u
                        ? output->data[output->size - 2u] : 0;
                    uint8_t previous3 = output->size > 2u
                        ? output->data[output->size - 3u] : 0;
                    int matched = previous_was_match && match_base <= output->size &&
                                  last_distance <= output->size - match_base;
                    uint8_t match_byte = matched
                        ? output->data[output->size - last_distance] : 0;
                    uint32_t word_context = literal_word_context(
                        output->data, output->size);
                    append_byte(output, matched_literal_decode_symbol(
                        &matched_literal_decoder, previous, previous2,
                        previous3, word_context, match_byte, matched));
                    ++literal_index;
                    previous_was_match = 0;
                } else if (literal_bit_mask) {
                    uint8_t previous = output->size ? output->data[output->size - 1u] : 0;
                    append_byte(output, literal_bit_decode_symbol(
                        &literal_bit_decoder, previous));
                    ++literal_index;
                } else if (context_mask) {
                    uint8_t previous = output->size ? output->data[output->size - 1u] : 0;
                    append_byte(output, context_arithmetic_decode_symbol(
                        &context_decoder, literal_arithmetic_context(previous)));
                    ++literal_index;
                } else {
                    if (literal_index >= streams[1].size) fail("truncated wide literals");
                    append_byte(output, streams[1].data[literal_index++]);
                }
            } else {
                if (match_index >= streams[distance_stream].size)
                    fail("truncated wide matches");
                uint32_t encoded_distance =
                    (uint32_t)streams[distance_stream].data[match_index] |
                    ((uint32_t)streams[distance_stream + 1u].data[match_index] << 8u) |
                    ((uint32_t)streams[distance_stream + 2u].data[match_index] << 16u);
                size_t distance = encoded_distance;
                size_t length = (size_t)streams[distance_stream + 3u].data[match_index] |
                                ((size_t)streams[distance_stream + 4u].data[match_index] << 8);
                ++distance;
                length += MIN_MATCH;
                ++match_index;
                if (match_base > output->size || distance > output->size - match_base ||
                    distance > WIDE_WINDOW ||
                    length > target_size - output->size)
                    fail("invalid wide match");
                reserve(output, length);
                for (size_t i = 0; i < length; ++i) {
                    output->data[output->size] = output->data[output->size - distance];
                    ++output->size;
                }
                last_distance = distance;
                previous_was_match = 1;
            }
        }
    }
    if (control_index != streams[0].size ||
        literal_index != (matched_literal_mask ? matched_literal_size :
                          (literal_bit_mask ? literal_bit_size :
                          (context_mask ? context_literal_size : streams[1].size))) ||
        (context_mask && context_decoder.remaining) ||
        (literal_bit_mask && literal_bit_decoder.remaining) ||
        (matched_literal_mask && matched_literal_decoder.remaining) ||
        match_index != streams[distance_stream].size)
        fail("unused wide split symbols");
    free(literal_bit_decoder.models);
    free(matched_literal_decoder.models);
    free(matched_literal_decoder.rich_models);
    free(matched_literal_decoder.hashed_models);
    free(matched_literal_decoder.word_models);
    for (unsigned i = 0; i < stream_count; ++i) free(streams[i].data);
}

static void wide_rep_decode(Buffer *output, const uint8_t *input,
                            size_t input_size, size_t decoded_size,
                            int fast_lookup, int literal_context_order) {
    if (input_size < 3u + 8u * 4u) fail("truncated rep-distance header");
    uint8_t adaptive_mask = input[0];
    uint8_t static_mask = input[1];
    uint8_t special_mask = input[2];
    input += 3u;
    input_size -= 3u;
    if ((adaptive_mask & static_mask) || (adaptive_mask & special_mask) ||
        (static_mask & special_mask) || (special_mask & ~3u))
        fail("invalid rep-distance masks");
    size_t header_size = 8u * 4u;
    uint32_t sizes[8];
    size_t offsets[8];
    size_t offset = header_size;
    for (unsigned i = 0; i < 8; ++i) {
        sizes[i] = read_u32le(input + 4u * i);
        if ((size_t)sizes[i] > input_size - offset)
            fail("invalid rep-distance stream size");
        offsets[i] = offset;
        offset += sizes[i];
    }
    if (offset != input_size) fail("trailing rep-distance data");
    Buffer streams[8] = {{0}};
    for (unsigned i = 0; i < 8; ++i) {
        if (special_mask & (1u << i)) {
        } else if (static_mask & (1u << i)) {
            streams[i] = static_arithmetic_decode(input + offsets[i], sizes[i]);
        } else if (adaptive_mask & (1u << i)) {
            streams[i] = adaptive_arithmetic_decode(input + offsets[i], sizes[i]);
        } else {
            streams[i] = compact_huffman_decode(input + offsets[i], sizes[i],
                                                fast_lookup);
        }
    }
    if (special_mask & 1u)
        streams[0] = adaptive_control_bit_decode(input + offsets[0], sizes[0]);
    MatchedLiteralDecoder literal_decoder = {0};
    size_t literal_count = streams[1].size;
    if (special_mask & (1u << 1)) {
        literal_decoder = matched_literal_decoder_init(
            input + offsets[1], sizes[1], literal_context_order);
        literal_count = literal_decoder.remaining;
    }
    if (streams[3].size != streams[4].size ||
        streams[3].size != streams[5].size ||
        streams[2].size != streams[6].size ||
        streams[2].size != streams[7].size)
        fail("inconsistent rep-distance streams");
    if (decoded_size > SIZE_MAX - output->size) fail("decoded size overflow");
    size_t target_size = output->size + decoded_size;
    size_t control_index = 0;
    size_t literal_index = 0;
    size_t match_index = 0;
    size_t raw_index = 0;
    size_t recent[4] = {0};
    size_t last_distance = 0;
    int previous_was_match = 0;
    while (output->size < target_size) {
        if (control_index >= streams[0].size) fail("truncated rep controls");
        uint8_t control = streams[0].data[control_index++];
        for (unsigned bit_index = 0;
             bit_index < 8 && output->size < target_size; ++bit_index) {
            if ((control & (1u << bit_index)) == 0) {
                if (literal_index >= literal_count) fail("truncated rep literals");
                if (special_mask & (1u << 1)) {
                    uint8_t previous = output->size
                        ? output->data[output->size - 1u] : 0;
                    uint8_t previous2 = output->size > 1u
                        ? output->data[output->size - 2u] : 0;
                    uint8_t previous3 = output->size > 2u
                        ? output->data[output->size - 3u] : 0;
                    int matched = previous_was_match &&
                                  last_distance <= output->size;
                    uint8_t match_byte = matched
                        ? output->data[output->size - last_distance] : 0;
                    uint32_t word_context = literal_word_context(
                        output->data, output->size);
                    append_byte(output, matched_literal_decode_symbol(
                        &literal_decoder, previous, previous2,
                        previous3, word_context, match_byte, matched));
                } else {
                    append_byte(output, streams[1].data[literal_index]);
                }
                ++literal_index;
                previous_was_match = 0;
            } else {
                if (match_index >= streams[2].size)
                    fail("truncated rep matches");
                unsigned code = streams[2].data[match_index];
                size_t distance = 0;
                if (code < 4u) {
                    distance = recent[code];
                    if (!distance) fail("invalid repeated distance");
                    for (unsigned slot = code; slot > 0; --slot)
                        recent[slot] = recent[slot - 1u];
                } else if (code == 4u) {
                    if (raw_index >= streams[3].size)
                        fail("truncated raw rep distance");
                    uint32_t encoded = (uint32_t)streams[3].data[raw_index] |
                        ((uint32_t)streams[4].data[raw_index] << 8u) |
                        ((uint32_t)streams[5].data[raw_index] << 16u);
                    ++raw_index;
                    distance = (size_t)encoded + 1u;
                    for (unsigned slot = 3; slot > 0; --slot)
                        recent[slot] = recent[slot - 1u];
                } else {
                    fail("invalid repeated-distance code");
                }
                recent[0] = distance;
                size_t length = (size_t)streams[6].data[match_index] |
                    ((size_t)streams[7].data[match_index] << 8u);
                length += MIN_MATCH;
                ++match_index;
                if (distance > output->size || distance > WIDE_WINDOW ||
                    length > target_size - output->size)
                    fail("invalid rep-distance match");
                reserve(output, length);
                for (size_t i = 0; i < length; ++i) {
                    output->data[output->size] =
                        output->data[output->size - distance];
                    ++output->size;
                }
                last_distance = distance;
                previous_was_match = 1;
            }
        }
    }
    if (control_index != streams[0].size || literal_index != literal_count ||
        match_index != streams[2].size || raw_index != streams[3].size ||
        ((special_mask & (1u << 1)) && literal_decoder.remaining))
        fail("unused rep-distance symbols");
    free(literal_decoder.models);
    free(literal_decoder.rich_models);
    free(literal_decoder.hashed_models);
    free(literal_decoder.word_models);
    for (unsigned i = 0; i < 8; ++i) free(streams[i].data);
}

static Buffer compress_entropy_parsed_range(const uint8_t *input, size_t input_size,
                                            size_t start, size_t decoded_size,
                                            const uint8_t *model_tokens,
                                            size_t model_size,
                                            int alternative_distances,
                                            int bucketed_distances,
                                            const MatchIndex *shared_index) {
    if (input_size > INT32_MAX) fail("input exceeds entropy parser 2 GiB limit");
    if (start > input_size || decoded_size != input_size - start)
        fail("invalid entropy parsing range");
    uint16_t literal_costs[256], distance_low_costs[256];
    uint16_t distance_high_costs[256], length_costs[256];
    split_cost_model(model_tokens, model_size, decoded_size, literal_costs,
                     distance_low_costs, distance_high_costs, length_costs);
    if (shared_index && alternative_distances == 1)
        fail("incompatible shared match index");

    int fused_distances = alternative_distances == 2;
    uint16_t *match_lengths = shared_index ? shared_index->lengths :
        (decoded_size && !fused_distances
            ? (uint16_t *)malloc(decoded_size * sizeof(*match_lengths)) : NULL);
    uint16_t *match_offsets = shared_index ? shared_index->offsets :
        (decoded_size && !fused_distances
            ? (uint16_t *)malloc(decoded_size * sizeof(*match_offsets)) : NULL);
    uint16_t *alternative_offsets = decoded_size && alternative_distances == 1
        ? (uint16_t *)malloc(decoded_size * ENTROPY_LENGTH_CHOICES *
                             sizeof(*alternative_offsets)) : NULL;
    uint16_t *choice_offsets = decoded_size && fused_distances
        ? (uint16_t *)malloc(decoded_size * sizeof(*choice_offsets)) : NULL;
    uint16_t *choices = decoded_size
        ? (uint16_t *)malloc(decoded_size * sizeof(*choices)) : NULL;
    uint64_t *costs = (uint64_t *)malloc((decoded_size + 1u) * sizeof(*costs));
    int32_t *heads = shared_index ? NULL :
        (int32_t *)malloc(HASH_SIZE * sizeof(*heads));
    int32_t *previous = shared_index ? shared_index->previous :
        (input_size ? (int32_t *)malloc(input_size * sizeof(*previous)) : NULL);
    if ((decoded_size && (!choices ||
                          (!fused_distances && (!match_lengths || !match_offsets)) ||
                          (alternative_distances == 1 && !alternative_offsets) ||
                          (fused_distances && !choice_offsets))) || !costs ||
        (!shared_index && !heads) ||
        (input_size && !previous))
        fail("out of memory");
    if (!shared_index) {
        for (size_t i = 0; i < HASH_SIZE; ++i) heads[i] = -1;
        for (size_t i = 0; i < start; ++i)
            insert_position(input, input_size, i, heads, previous);
        for (size_t pos = start; pos < input_size; ++pos) {
            if (!fused_distances) {
                size_t length, offset;
                if (alternative_distances) {
                    int32_t candidate = pos + 2u < input_size
                        ? heads[hash3(input + pos)] : -1;
                    find_costed_matches(input, input_size, pos, candidate, previous,
                                        distance_low_costs, distance_high_costs, &length,
                                        &offset, alternative_offsets +
                                            (pos - start) * ENTROPY_LENGTH_CHOICES, 0);
                } else {
                    find_match(input, input_size, pos, heads, previous, &length, &offset);
                }
                match_lengths[pos - start] = (uint16_t)length;
                match_offsets[pos - start] = (uint16_t)offset;
            }
            insert_position(input, input_size, pos, heads, previous);
        }
    }

    costs[decoded_size] = 0;
    for (size_t reverse = decoded_size; reverse > 0; --reverse) {
        size_t pos = reverse - 1;
        costs[pos] = 1u + literal_costs[input[start + pos]] + costs[pos + 1];
        choices[pos] = 0;
        if (fused_distances) {
            uint16_t offsets[ENTROPY_LENGTH_CHOICES];
            size_t maximum, unused_offset;
            int32_t candidate = pos + start + 2u < input_size
                ? previous[pos + start] : -1;
            find_costed_matches(input, input_size, pos + start, candidate, previous,
                                distance_low_costs, distance_high_costs,
                                &maximum, &unused_offset, offsets, bucketed_distances);
            size_t minimum = maximum > ENTROPY_LENGTH_CHOICES - 1u
                ? maximum - (ENTROPY_LENGTH_CHOICES - 1u) : MIN_MATCH;
            if (minimum < MIN_MATCH) minimum = MIN_MATCH;
            for (size_t length = minimum; length <= maximum; ++length) {
                size_t selected_offset = offsets[length - minimum];
                uint16_t offset = (uint16_t)(selected_offset - 1u);
                uint64_t match_cost = 1u + distance_low_costs[(uint8_t)offset] +
                                      distance_high_costs[(uint8_t)(offset >> 8)] +
                                      length_costs[length - MIN_MATCH] + costs[pos + length];
                if (match_cost < costs[pos]) {
                    costs[pos] = match_cost;
                    choices[pos] = (uint16_t)length;
                    choice_offsets[pos] = (uint16_t)selected_offset;
                }
            }
            continue;
        }
        size_t maximum = match_lengths[pos];
        size_t minimum = maximum > 7u ? maximum - 7u : MIN_MATCH;
        if (minimum < MIN_MATCH) minimum = MIN_MATCH;
        for (size_t length = minimum; length <= maximum; ++length) {
            size_t selected_offset = alternative_distances
                ? alternative_offsets[pos * ENTROPY_LENGTH_CHOICES + length - minimum]
                : match_offsets[pos];
            uint16_t offset = (uint16_t)(selected_offset - 1u);
            uint64_t match_cost = 1u + distance_low_costs[(uint8_t)offset] +
                                  distance_high_costs[(uint8_t)(offset >> 8)] +
                                  length_costs[length - MIN_MATCH] + costs[pos + length];
            if (match_cost < costs[pos]) {
                costs[pos] = match_cost;
                choices[pos] = (uint16_t)length;
            }
        }
    }

    Buffer output = {0};
    reserve(&output, HEADER_SIZE + decoded_size / 2 + 64);
    append_byte(&output, 'B'); append_byte(&output, 'I');
    append_byte(&output, 'T'); append_byte(&output, 'E');
    append_byte(&output, 1); append_byte(&output, 0);
    append_byte(&output, 0); append_byte(&output, 0);
    append_u64le(&output, (uint64_t)decoded_size);
    size_t pos = 0;
    while (pos < decoded_size) {
        size_t control_pos = output.size;
        append_byte(&output, 0);
        uint8_t control = 0;
        for (unsigned bit = 0; bit < 8 && pos < decoded_size; ++bit) {
            size_t length = choices[pos];
            if (!length) {
                append_byte(&output, input[start + pos]);
                ++pos;
            } else {
                control |= (uint8_t)(1u << bit);
                size_t selected_offset;
                if (fused_distances) {
                    selected_offset = choice_offsets[pos];
                } else {
                    size_t maximum = match_lengths[pos];
                    size_t minimum = maximum > ENTROPY_LENGTH_CHOICES - 1u
                        ? maximum - (ENTROPY_LENGTH_CHOICES - 1u) : MIN_MATCH;
                    if (minimum < MIN_MATCH) minimum = MIN_MATCH;
                    selected_offset = alternative_distances
                        ? alternative_offsets[pos * ENTROPY_LENGTH_CHOICES + length - minimum]
                        : match_offsets[pos];
                }
                size_t offset = selected_offset - 1u;
                append_byte(&output, (uint8_t)offset);
                append_byte(&output, (uint8_t)(offset >> 8));
                append_byte(&output, (uint8_t)(length - MIN_MATCH));
                pos += length;
            }
        }
        output.data[control_pos] = control;
    }
    if (!shared_index) {
        free(previous);
        free(heads);
        free(match_offsets);
        free(match_lengths);
    }
    free(costs);
    free(choices);
    free(choice_offsets);
    free(alternative_offsets);
    return output;
}

static uint64_t byte_concentration(const uint8_t *input, size_t input_size,
                                   size_t stride) {
    uint32_t frequencies[256] = {0};
    for (size_t i = 0; i < input_size; ++i) {
        uint8_t value = input[i];
        if (stride && i >= stride) value = (uint8_t)(value - input[i - stride]);
        ++frequencies[value];
    }
    uint64_t score = 0;
    for (unsigned i = 0; i < 256; ++i) score += (uint64_t)frequencies[i] * frequencies[i];
    return score;
}

static size_t choose_delta_stride(const uint8_t *input, size_t input_size) {
    if (input_size < 32) return 0;
    uint64_t best_score = byte_concentration(input, input_size, 0);
    size_t best_stride = 0;
    for (size_t stride = 1; stride <= 8 && stride < input_size; ++stride) {
        uint64_t score = byte_concentration(input, input_size, stride);
        if (score > best_score) {
            best_score = score;
            best_stride = stride;
        }
    }
    return best_stride;
}

static size_t choose_delta_stride_sampled(const uint8_t *input, size_t input_size) {
    if (input_size < 32) return 0;
    uint32_t frequencies[9][256] = {{0}};
    size_t segment_size = input_size <= 3u * DELTA_SAMPLE_SEGMENT
        ? input_size : DELTA_SAMPLE_SEGMENT;
    size_t starts[3] = {0, 0, 0};
    size_t segment_count = 1;
    if (segment_size < input_size) {
        starts[1] = (input_size - segment_size) / 2;
        starts[2] = input_size - segment_size;
        segment_count = 3;
    }
    for (size_t segment = 0; segment < segment_count; ++segment) {
        size_t end = starts[segment] + segment_size;
        for (size_t i = starts[segment]; i < end; ++i) {
            ++frequencies[0][input[i]];
            for (size_t stride = 1; stride <= 8; ++stride) {
                uint8_t value = i < stride ? input[i]
                    : (uint8_t)(input[i] - input[i - stride]);
                ++frequencies[stride][value];
            }
        }
    }
    uint64_t scores[9] = {0};
    for (size_t stride = 0; stride <= 8; ++stride)
        for (unsigned value = 0; value < 256; ++value)
            scores[stride] += (uint64_t)frequencies[stride][value] *
                              frequencies[stride][value];
    size_t best_stride = 0;
    for (size_t stride = 1; stride <= 8; ++stride)
        if (scores[stride] > scores[best_stride]) best_stride = stride;
    if (best_stride && scores[best_stride] < scores[0] + scores[0] / 5u)
        best_stride = 0;
    return best_stride;
}

static void delta_transform(const uint8_t *input, uint8_t *output,
                            size_t input_size, size_t stride) {
    for (size_t i = 0; i < input_size; ++i)
        output[i] = i < stride ? input[i] : (uint8_t)(input[i] - input[i - stride]);
}

static void inverse_delta(Buffer *output, size_t block_start, size_t stride) {
    size_t block_size = output->size - block_start;
    if (stride == 0 || stride > 8 || stride > block_size) fail("invalid delta stride");
    for (size_t i = stride; i < block_size; ++i)
        output->data[block_start + i] = (uint8_t)(output->data[block_start + i] +
                                                  output->data[block_start + i - stride]);
}

static Buffer compress_blocked(const uint8_t *input, size_t input_size, int lazy, int entropy) {
    Buffer output = {0};
    reserve(&output, HEADER_SIZE + input_size / 2 + 64);
    append_byte(&output, 'B'); append_byte(&output, 'I');
    append_byte(&output, 'T'); append_byte(&output, 'E');
    append_byte(&output, 2); append_byte(&output, 0);
    append_byte(&output, 0); append_byte(&output, 0);
    append_u64le(&output, (uint64_t)input_size);

    size_t pos = 0;
    while (pos < input_size) {
        size_t decoded_size = input_size - pos;
        if (decoded_size > BLOCK_SIZE) decoded_size = BLOCK_SIZE;
        Buffer trial = compress_buffer(input + pos, decoded_size, lazy);
        size_t payload_size = trial.size - HEADER_SIZE;
        Buffer huffman = {0};
        if (entropy) huffman = huffman_encode(trial.data + HEADER_SIZE, payload_size);
        uint8_t mode = 0;
        size_t stored_size = decoded_size;
        if (payload_size < stored_size) { mode = 1; stored_size = payload_size; }
        if (entropy && huffman.size < stored_size) { mode = 2; stored_size = huffman.size; }
        append_byte(&output, mode);
        append_u32le(&output, (uint32_t)decoded_size);
        append_u32le(&output, (uint32_t)stored_size);
        if (mode == 1)
            append_bytes(&output, trial.data + HEADER_SIZE, payload_size);
        else if (mode == 2)
            append_bytes(&output, huffman.data, huffman.size);
        else
            append_bytes(&output, input + pos, decoded_size);
        free(huffman.data);
        free(trial.data);
        pos += decoded_size;
    }
    return output;
}

static Buffer compress_linked_blocked(const uint8_t *input, size_t input_size,
                                      size_t lazy_limit, int entropy, int split_entropy,
                                      int delta_stride, int sampled_delta,
                                      int entropy_parse) {
    int profile = getenv("BITE_PROFILE") != NULL;
    clock_t profile_total_start = clock();
    clock_t profile_phase_start = profile_total_start;
    Buffer output = {0};
    reserve(&output, HEADER_SIZE + input_size / 2 + 64);
    append_byte(&output, 'B'); append_byte(&output, 'I');
    append_byte(&output, 'T'); append_byte(&output, 'E');
    append_byte(&output, 2); append_byte(&output, 0);
    append_byte(&output, 0); append_byte(&output, 0);
    append_u64le(&output, (uint64_t)input_size);

    if (entropy_parse == 32 && input_size <= WHOLE_BLOCK_SIZE) {
        Buffer small_alphabet = small_alphabet_order4_encode(input, input_size);
        if (small_alphabet.data && small_alphabet.size < input_size) {
            append_byte(&output, 23);
            append_u32le(&output, (uint32_t)input_size);
            append_u32le(&output, (uint32_t)small_alphabet.size);
            append_bytes(&output, small_alphabet.data, small_alphabet.size);
            free(small_alphabet.data);
            if (profile)
                fprintf(stderr, "bite profile: fast-small-alphabet %.6f s\n",
                        (double)(clock() - profile_total_start) / CLOCKS_PER_SEC);
            return output;
        }
        free(small_alphabet.data);
    }

    size_t block_size = entropy_parse > 15 ? WHOLE_BLOCK_SIZE :
                        (entropy_parse > 14 ? LARGE_BLOCK_SIZE : BLOCK_SIZE);
    WideMatchIndex wide_index = {0};
    if (entropy_parse > 7) {
        unsigned max_chain = entropy_parse == 32 ? 1024u :
                             (entropy_parse > 17 ? EXTREME_WIDE_MAX_CHAIN :
                              (entropy_parse > 16 ? DEEP_WIDE_MAX_CHAIN : WIDE_MAX_CHAIN));
        unsigned cost_chain = entropy_parse == 32 ? 16u :
                              (entropy_parse > 17 ? EXTREME_WIDE_COST_CHAIN :
                               (entropy_parse > 16 ? DEEP_WIDE_COST_CHAIN : WIDE_COST_CHAIN));
        wide_index = build_wide_match_index(input, input_size, block_size,
                                            max_chain, cost_chain);
    }
    if (profile) {
        fprintf(stderr, "bite profile: wide-index %.6f s\n",
                (double)(clock() - profile_phase_start) / CLOCKS_PER_SEC);
        profile_phase_start = clock();
    }

    size_t pos = 0;
    while (pos < input_size) {
        size_t decoded_size = input_size - pos;
        if (decoded_size > block_size) decoded_size = block_size;
        size_t history_start = pos > WINDOW ? pos - WINDOW : 0;
        size_t history_size = pos - history_start;
        Buffer trial = compress_range(input + history_start, history_size + decoded_size,
                                      history_size, decoded_size, lazy_limit);
        size_t payload_size = trial.size - HEADER_SIZE;
        Buffer huffman = {0};
        if (entropy) huffman = huffman_encode(trial.data + HEADER_SIZE, payload_size);
        Buffer split = {0};
        uint8_t split_mode = 6;
        int compact_models = entropy_parse > 6;
        if (split_entropy)
            split = split_huffman_encode_best_model(
                trial.data + HEADER_SIZE, payload_size, decoded_size,
                compact_models, &split_mode);
        Buffer parsed_split = {0};
        uint8_t parsed_mode = 6;
        if (entropy_parse) {
            MatchIndex shared_index = {0};
            const MatchIndex *shared = NULL;
            if (entropy_parse > 4) {
                shared_index = build_match_index(
                    input + history_start, history_size + decoded_size,
                    history_size, decoded_size);
                shared = &shared_index;
            }
            Buffer parsed_trial = compress_entropy_parsed_range(
                input + history_start, history_size + decoded_size, history_size,
                decoded_size, trial.data + HEADER_SIZE, payload_size, 0, 0, shared);
            parsed_split = split_huffman_encode_best_model(
                parsed_trial.data + HEADER_SIZE, parsed_trial.size - HEADER_SIZE,
                decoded_size, compact_models, &parsed_mode);
            if (entropy_parse > 1) {
                Buffer iterated_trial = compress_entropy_parsed_range(
                    input + history_start, history_size + decoded_size, history_size,
                    decoded_size, parsed_trial.data + HEADER_SIZE,
                    parsed_trial.size - HEADER_SIZE, 0, 0, shared);
                uint8_t iterated_mode = 6;
                Buffer iterated_split = split_huffman_encode_best_model(
                    iterated_trial.data + HEADER_SIZE,
                    iterated_trial.size - HEADER_SIZE, decoded_size,
                    compact_models, &iterated_mode);
                if (iterated_split.size < parsed_split.size) {
                    free(parsed_split.data);
                    parsed_split = iterated_split;
                    parsed_mode = iterated_mode;
                } else {
                    free(iterated_split.data);
                }
                if (entropy_parse > 2) {
                    Buffer distance_trial = compress_entropy_parsed_range(
                        input + history_start, history_size + decoded_size, history_size,
                        decoded_size, iterated_trial.data + HEADER_SIZE,
                        iterated_trial.size - HEADER_SIZE,
                        entropy_parse > 3 ? 2 : 1,
                        entropy_parse > 5, shared);
                    uint8_t distance_mode = 6;
                    Buffer distance_split = split_huffman_encode_best_model(
                        distance_trial.data + HEADER_SIZE,
                        distance_trial.size - HEADER_SIZE, decoded_size,
                        compact_models, &distance_mode);
                    if (distance_split.size < parsed_split.size) {
                        free(parsed_split.data);
                        parsed_split = distance_split;
                        parsed_mode = distance_mode;
                    } else {
                        free(distance_split.data);
                    }
                    free(distance_trial.data);
                }
                free(iterated_trial.data);
            }
            free(parsed_trial.data);
            free_match_index(&shared_index);
        }
        Buffer transformed_split = {0};
        WideEncoding transformed_wide = {0};
        size_t transformed_stride = 0;
        if (delta_stride) {
            transformed_stride = sampled_delta
                ? choose_delta_stride_sampled(input + pos, decoded_size)
                : choose_delta_stride(input + pos, decoded_size);
            if (transformed_stride) {
                uint8_t *transformed = (uint8_t *)malloc(decoded_size);
                if (!transformed) fail("out of memory");
                delta_transform(input + pos, transformed, decoded_size, transformed_stride);
                Buffer transformed_trial = compress_range(transformed, decoded_size, 0,
                                                          decoded_size, lazy_limit);
                transformed_split = split_huffman_encode(transformed_trial.data + HEADER_SIZE,
                                                         transformed_trial.size - HEADER_SIZE,
                                                         decoded_size, 0);
                free(transformed_trial.data);
                if (entropy_parse > 18) {
                    WideMatchIndex transformed_index = build_wide_match_index(
                        transformed, decoded_size, decoded_size,
                        WIDE_MAX_CHAIN, WIDE_COST_CHAIN);
                    transformed_wide = wide_split_huffman_encode(
                        transformed, decoded_size, 0, decoded_size, &transformed_index,
                        5);
                    free_wide_match_index(&transformed_index);
                }
                free(transformed);
            }
        }
        if (profile) {
            fprintf(stderr, "bite profile: legacy-and-delta %.6f s\n",
                    (double)(clock() - profile_phase_start) / CLOCKS_PER_SEC);
            profile_phase_start = clock();
        }
        WideEncoding wide_split = {0};
        if (entropy_parse > 7) {
            int arithmetic_level = entropy_parse > 8 ? entropy_parse - 8 : 0;
            if (entropy_parse == 31) arithmetic_level = 19;
            if (entropy_parse == 32) arithmetic_level = 25;
            if (entropy_parse == 33) arithmetic_level = 27;
            wide_split = wide_split_huffman_encode(
                input, input_size, pos, decoded_size, &wide_index,
                arithmetic_level);
        }
        if (profile) {
            fprintf(stderr, "bite profile: wide-parse-and-code %.6f s\n",
                    (double)(clock() - profile_phase_start) / CLOCKS_PER_SEC);
            profile_phase_start = clock();
        }
        Buffer small_alphabet = {0};
        if (entropy_parse >= 31 && entropy_parse <= 33)
            small_alphabet = small_alphabet_order4_encode(input + pos, decoded_size);
        uint8_t mode = 0;
        size_t stored_size = decoded_size;
        const Buffer *selected_split = NULL;
        if (payload_size < stored_size) { mode = 4; stored_size = payload_size; }
        if (entropy && huffman.size < stored_size) { mode = 5; stored_size = huffman.size; }
        if (split_entropy && split.size < stored_size) {
            mode = split_mode;
            stored_size = split.size;
            selected_split = &split;
        }
        if (entropy_parse && parsed_split.size < stored_size) {
            mode = parsed_mode;
            stored_size = parsed_split.size;
            selected_split = &parsed_split;
        }
        if (transformed_stride && transformed_split.size + 1 < stored_size) {
            mode = 7;
            stored_size = transformed_split.size + 1;
        }
        if (entropy_parse > 7 && wide_split.data.size < stored_size) {
            mode = wide_split.mode;
            stored_size = wide_split.data.size;
        }
        if (entropy_parse > 18 && transformed_stride &&
            transformed_wide.data.size + 2u < stored_size) {
            mode = 14;
            stored_size = transformed_wide.data.size + 2u;
        }
        if (small_alphabet.data && small_alphabet.size < stored_size) {
            mode = 23;
            stored_size = small_alphabet.size;
        }
        append_byte(&output, mode);
        append_u32le(&output, (uint32_t)decoded_size);
        append_u32le(&output, (uint32_t)stored_size);
        if (mode == 4)
            append_bytes(&output, trial.data + HEADER_SIZE, payload_size);
        else if (mode == 5)
            append_bytes(&output, huffman.data, huffman.size);
        else if (mode == 6 || mode == 8) {
            if (!selected_split) fail("missing selected split payload");
            append_bytes(&output, selected_split->data, selected_split->size);
        }
        else if (mode == 9 || mode == 10 || mode == 11 || mode == 12 || mode == 13 ||
                 mode == 15 || mode == 16 || mode == 17 || mode == 18 || mode == 20 ||
                 mode == 22 || mode == 24)
            append_bytes(&output, wide_split.data.data, wide_split.data.size);
        else if (mode == 23)
            append_bytes(&output, small_alphabet.data, small_alphabet.size);
        else if (mode == 14) {
            append_byte(&output, (uint8_t)transformed_stride);
            append_byte(&output, transformed_wide.mode);
            append_bytes(&output, transformed_wide.data.data, transformed_wide.data.size);
        }
        else if (mode == 7) {
            append_byte(&output, (uint8_t)transformed_stride);
            append_bytes(&output, transformed_split.data, transformed_split.size);
        }
        else
            append_bytes(&output, input + pos, decoded_size);
        free(huffman.data);
        free(split.data);
        free(parsed_split.data);
        free(transformed_split.data);
        free(transformed_wide.data.data);
        free(wide_split.data.data);
        free(small_alphabet.data);
        free(trial.data);
        pos += decoded_size;
    }
    free_wide_match_index(&wide_index);
    if (profile)
        fprintf(stderr, "bite profile: total %.6f s\n",
                (double)(clock() - profile_total_start) / CLOCKS_PER_SEC);
    return output;
}

static void decompress_payload(Buffer *output, const uint8_t *input,
                               size_t input_size, size_t decoded_size,
                               size_t match_base) {
    if (decoded_size > SIZE_MAX - output->size) fail("decoded size overflow");
    if (match_base > output->size) fail("invalid match base");
    size_t block_start = output->size;
    size_t target_size = block_start + decoded_size;
    size_t ip = 0;
    while (output->size < target_size) {
        if (ip >= input_size) fail("truncated control byte");
        uint8_t control = input[ip++];
        for (unsigned bit = 0; bit < 8 && output->size < target_size; ++bit) {
            if ((control & (1u << bit)) == 0) {
                if (ip >= input_size) fail("truncated literal");
                append_byte(output, input[ip++]);
            } else {
                if (input_size - ip < 3) fail("truncated match");
                size_t offset = (size_t)input[ip] | ((size_t)input[ip + 1] << 8);
                offset += 1;
                size_t length = (size_t)input[ip + 2] + MIN_MATCH;
                ip += 3;
                if (offset > output->size - match_base || length > target_size - output->size)
                    fail("invalid match");
                reserve(output, length);
                for (size_t i = 0; i < length; ++i) {
                    output->data[output->size] = output->data[output->size - offset];
                    ++output->size;
                }
            }
        }
    }
    if (ip != input_size) fail("trailing compressed data");
}

static Buffer decompress_buffer(const uint8_t *input, size_t input_size, int fast_huffman) {
    if (input_size < HEADER_SIZE || memcmp(input, "BITE", 4) != 0 || (input[4] != 1 && input[4] != 2))
        fail("invalid or unsupported stream");
    uint64_t decoded_u64 = read_u64le(input + 8);
    if (decoded_u64 > SIZE_MAX) fail("decoded size unsupported");
    size_t decoded_size = (size_t)decoded_u64;
    Buffer output = {0};
    reserve(&output, decoded_size ? decoded_size : 1);
    if (input[4] == 1) {
        decompress_payload(&output, input + HEADER_SIZE, input_size - HEADER_SIZE, decoded_size, 0);
        return output;
    }

    size_t ip = HEADER_SIZE;
    while (output.size < decoded_size) {
        if (input_size - ip < BLOCK_HEADER_SIZE) fail("truncated block header");
        uint8_t mode = input[ip++];
        size_t block_decoded = read_u32le(input + ip); ip += 4;
        size_t block_stored = read_u32le(input + ip); ip += 4;
        if ((mode > 2 && mode != 4 && mode != 5 && mode != 6 && mode != 7 &&
             mode != 8 && mode != 9 && mode != 10 && mode != 11 && mode != 12 &&
             mode != 13 && mode != 14 && mode != 15 && mode != 16 && mode != 17 &&
             mode != 18 && mode != 20 && mode != 22 && mode != 23 && mode != 24 && mode != 25) ||
            block_decoded == 0 || block_decoded > WHOLE_BLOCK_SIZE ||
            block_decoded > decoded_size - output.size || block_stored > input_size - ip)
            fail("invalid block");
        if (mode == 0) {
            if (block_stored != block_decoded) fail("invalid raw block");
            append_bytes(&output, input + ip, block_stored);
        } else if (mode == 1) {
            size_t match_base = output.size;
            decompress_payload(&output, input + ip, block_stored, block_decoded, match_base);
        } else if (mode == 2) {
            size_t match_base = output.size;
            Buffer token_stream = huffman_decode(input + ip, block_stored, fast_huffman);
            decompress_payload(&output, token_stream.data, token_stream.size, block_decoded, match_base);
            free(token_stream.data);
        } else if (mode == 4) {
            size_t match_base = output.size > WINDOW ? output.size - WINDOW : 0;
            decompress_payload(&output, input + ip, block_stored, block_decoded, match_base);
        } else if (mode == 5) {
            size_t match_base = output.size > WINDOW ? output.size - WINDOW : 0;
            Buffer token_stream = huffman_decode(input + ip, block_stored, fast_huffman);
            decompress_payload(&output, token_stream.data, token_stream.size, block_decoded, match_base);
            free(token_stream.data);
        } else if (mode == 6) {
            size_t match_base = output.size > WINDOW ? output.size - WINDOW : 0;
            split_huffman_decode(&output, input + ip, block_stored, block_decoded,
                                 match_base, fast_huffman, 0);
        } else if (mode == 8) {
            size_t match_base = output.size > WINDOW ? output.size - WINDOW : 0;
            split_huffman_decode(&output, input + ip, block_stored, block_decoded,
                                 match_base, fast_huffman, 1);
        } else if (mode == 9) {
            wide_split_huffman_decode(&output, input + ip, block_stored,
                                      block_decoded, 0, fast_huffman, 0);
        } else if (mode == 10) {
            wide_split_huffman_decode(&output, input + ip, block_stored,
                                      block_decoded, 0, fast_huffman, 1);
        } else if (mode == 11) {
            wide_split_huffman_decode(&output, input + ip, block_stored,
                                      block_decoded, 0, fast_huffman, 2);
        } else if (mode == 12) {
            wide_split_huffman_decode(&output, input + ip, block_stored,
                                      block_decoded, 0, fast_huffman, 3);
        } else if (mode == 13) {
            wide_split_huffman_decode(&output, input + ip, block_stored,
                                      block_decoded, 0, fast_huffman, 4);
        } else if (mode == 14) {
            if (block_stored < 2) fail("truncated transformed wide block");
            size_t block_start = output.size;
            size_t stride = input[ip];
            uint8_t inner_mode = input[ip + 1u];
            if (inner_mode < 9 || inner_mode > 13)
                fail("invalid transformed wide inner mode");
            wide_split_huffman_decode(&output, input + ip + 2u, block_stored - 2u,
                                      block_decoded, block_start, fast_huffman,
                                      inner_mode - 9u);
            inverse_delta(&output, block_start, stride);
        } else if (mode == 15) {
            wide_split_huffman_decode(&output, input + ip, block_stored,
                                      block_decoded, 0, fast_huffman, 5);
        } else if (mode == 16) {
            wide_split_huffman_decode(&output, input + ip, block_stored,
                                      block_decoded, 0, fast_huffman, 6);
        } else if (mode == 17) {
            wide_split_huffman_decode(&output, input + ip, block_stored,
                                      block_decoded, 0, fast_huffman, 7);
        } else if (mode == 18) {
            wide_split_huffman_decode(&output, input + ip, block_stored,
                                      block_decoded, 0, fast_huffman, 8);
        } else if (mode == 20) {
            wide_split_huffman_decode(&output, input + ip, block_stored,
                                      block_decoded, 0, fast_huffman, 9);
        } else if (mode == 22) {
            wide_rep_decode(&output, input + ip, block_stored,
                            block_decoded, fast_huffman, 2);
        } else if (mode == 24) {
            wide_rep_decode(&output, input + ip, block_stored,
                            block_decoded, fast_huffman, 3);
        } else if (mode == 23) {
            Buffer decoded = small_alphabet_order4_decode(
                input + ip, block_stored, block_decoded);
            append_bytes(&output, decoded.data, decoded.size);
            free(decoded.data);
        } else {
            if (block_stored < 1) fail("truncated delta block");
            size_t block_start = output.size;
            size_t stride = input[ip];
            split_huffman_decode(&output, input + ip + 1, block_stored - 1,
                                 block_decoded, block_start, fast_huffman, 0);
            inverse_delta(&output, block_start, stride);
        }
        ip += block_stored;
    }
    if (ip != input_size) fail("trailing compressed data");
    return output;
}

static Buffer read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) fail_errno(path);
    if (fseek(file, 0, SEEK_END) != 0) fail_errno(path);
    long length = ftell(file);
    if (length < 0) fail_errno(path);
    if (fseek(file, 0, SEEK_SET) != 0) fail_errno(path);
    Buffer buffer = {0};
    reserve(&buffer, (size_t)length ? (size_t)length : 1);
    buffer.size = (size_t)length;
    if (buffer.size && fread(buffer.data, 1, buffer.size, file) != buffer.size) fail_errno(path);
    if (fclose(file) != 0) fail_errno(path);
    return buffer;
}

static void write_file(const char *path, const Buffer *buffer) {
    FILE *file = fopen(path, "wb");
    if (!file) fail_errno(path);
    if (buffer->size && fwrite(buffer->data, 1, buffer->size, file) != buffer->size) fail_errno(path);
    if (fclose(file) != 0) fail_errno(path);
}

#ifndef BITE_NO_MAIN
int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: bite c|cl|cb|clb|cbh|clbh|cblink|clblinkh|clblinks|clblinksopt2dfsbwasp2b16x2tdlmmdcra4|clblinksopt2dfsbwasp2b16x2tdlmmdcra4f|clblinksopt2dfsbwasp2b16x2tdlmmdcra4cm3|d|dwork|dslow INPUT OUTPUT\n");
        fprintf(stderr, "  c/cl: v1; cb/clb: v2; h: Huffman; link: linked history; d: decode\n");
        return 2;
    }
    int is_mixed_literal =
        strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlmm") == 0;
    int is_control_context =
        strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlmmdc") == 0;
    int is_rep_distance =
        strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlmmdcr") == 0;
    int is_rep_aware =
        strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlmmdcrg") == 0;
    int is_rep_aware_iterated =
        strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlmmdcrg2") == 0;
    int is_rep_beam =
        strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlmmdcrb") == 0;
    int is_fast_small_alphabet =
        strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlmmdcra4f") == 0;
    int is_context_mix =
        strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlmmdcra4cm3") == 0;
    int is_small_alphabet =
        strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlmmdcra4") == 0 ||
        is_fast_small_alphabet || is_context_mix;
    int is_greedy = strcmp(argv[1], "c") == 0 || strcmp(argv[1], "cb") == 0 ||
                    strcmp(argv[1], "cbh") == 0 || strcmp(argv[1], "cblink") == 0 ||
                    strcmp(argv[1], "cblinkh") == 0 || strcmp(argv[1], "cblinks") == 0;
    int is_lazy = strcmp(argv[1], "cl") == 0 || strcmp(argv[1], "clb") == 0 ||
                  strcmp(argv[1], "clbh") == 0 || strcmp(argv[1], "clblink") == 0 ||
                  strcmp(argv[1], "clblinkh") == 0 || strcmp(argv[1], "clblinks") == 0 ||
                  strcmp(argv[1], "clblinksg") == 0 || strcmp(argv[1], "clblinksd") == 0 ||
                  strcmp(argv[1], "clblinksdf") == 0 || strcmp(argv[1], "clblinksopt") == 0 ||
                  strcmp(argv[1], "clblinksopt2") == 0 ||
                  strcmp(argv[1], "clblinksopt2d") == 0 ||
                  strcmp(argv[1], "clblinksopt2df") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfs") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsb") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbc") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbw") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwa") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwa1") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwac") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwas") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwasp") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwasp2") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwasp2b") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwasp2b16") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwasp2b16x") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2t") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2td") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdl") == 0 ||
                  strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlm") == 0 ||
                  is_mixed_literal || is_control_context || is_rep_distance ||
                  is_rep_aware || is_rep_aware_iterated;
    is_lazy = is_lazy || is_rep_beam || is_small_alphabet;
    int is_linked = strcmp(argv[1], "cblink") == 0 || strcmp(argv[1], "clblink") == 0 ||
                    strcmp(argv[1], "cblinkh") == 0 || strcmp(argv[1], "clblinkh") == 0;
    is_linked = is_linked || strcmp(argv[1], "cblinks") == 0 || strcmp(argv[1], "clblinks") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksg") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksd") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksdf") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2d") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2df") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfs") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsb") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbc") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbw") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwa") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwa1") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwac") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwas") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwasp") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwasp2") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwasp2b") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwasp2b16") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwasp2b16x") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2t") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2td") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdl") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlm") == 0;
    is_linked = is_linked || is_mixed_literal;
    is_linked = is_linked || is_control_context;
    is_linked = is_linked || is_rep_distance;
    is_linked = is_linked || is_rep_aware;
    is_linked = is_linked || is_rep_aware_iterated;
    is_linked = is_linked || is_rep_beam;
    is_linked = is_linked || is_small_alphabet;
    int is_blocked = strcmp(argv[1], "cb") == 0 || strcmp(argv[1], "clb") == 0 ||
                     strcmp(argv[1], "cbh") == 0 || strcmp(argv[1], "clbh") == 0 || is_linked;
    int is_entropy = strcmp(argv[1], "cbh") == 0 || strcmp(argv[1], "clbh") == 0 ||
                     strcmp(argv[1], "cblinkh") == 0 || strcmp(argv[1], "clblinkh") == 0 ||
                     strcmp(argv[1], "cblinks") == 0 || strcmp(argv[1], "clblinks") == 0 ||
                     strcmp(argv[1], "clblinksg") == 0 || strcmp(argv[1], "clblinksd") == 0 ||
                     strcmp(argv[1], "clblinksdf") == 0 || strcmp(argv[1], "clblinksopt") == 0 ||
                     strcmp(argv[1], "clblinksopt2") == 0 ||
                     strcmp(argv[1], "clblinksopt2d") == 0 ||
                     strcmp(argv[1], "clblinksopt2df") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfs") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsb") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbc") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbw") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwa") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwa1") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwac") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwas") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwasp") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwasp2") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwasp2b") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwasp2b16") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwasp2b16x") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2t") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2td") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdl") == 0 ||
                     strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlm") == 0 ||
                     is_mixed_literal || is_control_context || is_rep_distance ||
                     is_rep_aware || is_rep_aware_iterated;
    is_entropy = is_entropy || is_rep_beam || is_small_alphabet;
    int is_split_entropy = strcmp(argv[1], "cblinks") == 0 || strcmp(argv[1], "clblinks") == 0 ||
                           strcmp(argv[1], "clblinksg") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksd") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksdf") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2d") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2df") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfs") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsb") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbc") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbw") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwa") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwa1") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwac") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwas") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwasp") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwasp2") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwasp2b") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwasp2b16") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwasp2b16x") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2t") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2td") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdl") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlm") == 0;
    is_split_entropy = is_split_entropy || is_mixed_literal;
    is_split_entropy = is_split_entropy || is_control_context;
    is_split_entropy = is_split_entropy || is_rep_distance;
    is_split_entropy = is_split_entropy || is_rep_aware;
    is_split_entropy = is_split_entropy || is_rep_aware_iterated;
    is_split_entropy = is_split_entropy || is_rep_beam;
    is_split_entropy = is_split_entropy || is_small_alphabet;
    int is_gated_lazy = strcmp(argv[1], "clblinksg") == 0;
    int is_delta_stride = strcmp(argv[1], "clblinksd") == 0 ||
                          strcmp(argv[1], "clblinksdf") == 0 ||
                          strcmp(argv[1], "clblinksopt") == 0 ||
                          strcmp(argv[1], "clblinksopt2") == 0 ||
                          strcmp(argv[1], "clblinksopt2d") == 0 ||
                          strcmp(argv[1], "clblinksopt2df") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfs") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsb") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbc") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbw") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwa") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwa1") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwac") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwas") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwasp") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwasp2") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwasp2b") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwasp2b16") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwasp2b16x") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2t") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2td") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdl") == 0 ||
                          strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlm") == 0 ||
                          is_mixed_literal || is_control_context || is_rep_distance ||
                          is_rep_aware || is_rep_aware_iterated;
    is_delta_stride = is_delta_stride || is_rep_beam || is_small_alphabet;
    int is_sampled_delta = strcmp(argv[1], "clblinksdf") == 0 ||
                           strcmp(argv[1], "clblinksopt") == 0 ||
                           strcmp(argv[1], "clblinksopt2") == 0 ||
                           strcmp(argv[1], "clblinksopt2d") == 0 ||
                           strcmp(argv[1], "clblinksopt2df") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfs") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsb") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbc") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbw") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwa") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwa1") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwac") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwas") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwasp") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwasp2") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwasp2b") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwasp2b16") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwasp2b16x") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2t") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2td") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdl") == 0 ||
                           strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlm") == 0 ||
                           is_mixed_literal || is_control_context || is_rep_distance ||
                           is_rep_aware || is_rep_aware_iterated;
    is_sampled_delta = is_sampled_delta || is_rep_beam || is_small_alphabet;
    int is_entropy_parse = strcmp(argv[1], "clblinksopt") == 0 ? 1 :
                           (strcmp(argv[1], "clblinksopt2") == 0 ? 2 :
                           (strcmp(argv[1], "clblinksopt2d") == 0 ? 3 :
                           (strcmp(argv[1], "clblinksopt2df") == 0 ? 4 :
                           (strcmp(argv[1], "clblinksopt2dfs") == 0 ? 5 :
                           (strcmp(argv[1], "clblinksopt2dfsb") == 0 ? 6 :
                           (strcmp(argv[1], "clblinksopt2dfsbc") == 0 ? 7 :
                           (strcmp(argv[1], "clblinksopt2dfsbw") == 0 ? 8 :
                           (strcmp(argv[1], "clblinksopt2dfsbwa") == 0 ? 9 :
                           (strcmp(argv[1], "clblinksopt2dfsbwa1") == 0 ? 10 :
                           (strcmp(argv[1], "clblinksopt2dfsbwac") == 0 ? 11 :
                           (strcmp(argv[1], "clblinksopt2dfsbwas") == 0 ? 12 :
                           (strcmp(argv[1], "clblinksopt2dfsbwasp") == 0 ? 13 :
                           (strcmp(argv[1], "clblinksopt2dfsbwasp2") == 0 ? 14 :
                           (strcmp(argv[1], "clblinksopt2dfsbwasp2b") == 0 ? 15 :
                           (strcmp(argv[1], "clblinksopt2dfsbwasp2b16") == 0 ? 16 :
                           (strcmp(argv[1], "clblinksopt2dfsbwasp2b16x") == 0 ? 17 :
                           (strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2") == 0 ? 18 :
                           (strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2t") == 0 ? 19 :
                           (strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2td") == 0 ? 20 :
                           (strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdl") == 0 ? 21 :
                           (strcmp(argv[1], "clblinksopt2dfsbwasp2b16x2tdlm") == 0 ? 22 : 0)))))))))))))))))))));
    if (is_mixed_literal) is_entropy_parse = 23;
    if (is_control_context) is_entropy_parse = 25;
    if (is_rep_distance) is_entropy_parse = 27;
    if (is_rep_aware) is_entropy_parse = 28;
    if (is_rep_aware_iterated) is_entropy_parse = 29;
    if (is_rep_beam) is_entropy_parse = 30;
    if (is_small_alphabet)
        is_entropy_parse = is_context_mix ? 33 :
                           (is_fast_small_alphabet ? 32 : 31);
    int is_decode = strcmp(argv[1], "d") == 0 || strcmp(argv[1], "dwork") == 0 ||
                    strcmp(argv[1], "dslow") == 0;
    if (!is_greedy && !is_lazy && !is_decode) {
        fprintf(stderr, "usage: bite c|cl|cb|clb|cbh|clbh|cblink|clblinkh|clblinks|clblinksopt2dfsbwasp2b16x2tdlmmdcra4|clblinksopt2dfsbwasp2b16x2tdlmmdcra4f|clblinksopt2dfsbwasp2b16x2tdlmmdcra4cm3|d|dwork|dslow INPUT OUTPUT\n");
        fprintf(stderr, "  c/cl: v1; cb/clb: v2; h: Huffman; link: linked history; d: decode\n");
        return 2;
    }
    Buffer input = read_file(argv[2]);
    Buffer output;
    if (is_decode) {
        int lookup_policy = strcmp(argv[1], "d") == 0 ? 1 :
                            (strcmp(argv[1], "dwork") == 0 ? 2 : 0);
        output = decompress_buffer(input.data, input.size, lookup_policy);
    }
    else if (is_linked)
        output = compress_linked_blocked(input.data, input.size,
                                         is_gated_lazy ? 32 : (is_lazy ? MAX_MATCH : 0),
                                         is_entropy, is_split_entropy, is_delta_stride,
                                         is_sampled_delta, is_entropy_parse);
    else if (is_blocked)
        output = compress_blocked(input.data, input.size, is_lazy, is_entropy);
    else
        output = compress_buffer(input.data, input.size, is_lazy);
    write_file(argv[3], &output);
    free(output.data);
    free(input.data);
    return 0;
}
#endif
