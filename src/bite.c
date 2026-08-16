#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HASH_BITS 16u
#define HASH_SIZE (1u << HASH_BITS)
#define WINDOW 65535u
#define MIN_MATCH 4u
#define MAX_MATCH 259u
#define MAX_CHAIN 64u
#define HEADER_SIZE 16u
#define BLOCK_SIZE 65536u
#define BLOCK_HEADER_SIZE 9u
#define DELTA_SAMPLE_SEGMENT 1365u

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
    if (decoded_size > BLOCK_SIZE * 2u || tree_size > input_size - 6)
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

static Buffer split_huffman_encode(const uint8_t *input, size_t input_size,
                                   size_t decoded_size) {
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
    for (unsigned i = 0; i < 5; ++i) encoded[i] = huffman_encode(streams[i].data, streams[i].size);
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

static void split_huffman_decode(Buffer *output, const uint8_t *input,
                                 size_t input_size, size_t decoded_size,
                                 size_t match_base, int fast_lookup) {
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
        streams[i] = huffman_decode(input + offset, sizes[i], fast_lookup);
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

static Buffer compress_entropy_parsed_range(const uint8_t *input, size_t input_size,
                                            size_t start, size_t decoded_size,
                                            const uint8_t *model_tokens,
                                            size_t model_size) {
    if (input_size > INT32_MAX) fail("input exceeds entropy parser 2 GiB limit");
    if (start > input_size || decoded_size != input_size - start)
        fail("invalid entropy parsing range");
    uint16_t literal_costs[256], distance_low_costs[256];
    uint16_t distance_high_costs[256], length_costs[256];
    split_cost_model(model_tokens, model_size, decoded_size, literal_costs,
                     distance_low_costs, distance_high_costs, length_costs);

    uint16_t *match_lengths = decoded_size
        ? (uint16_t *)malloc(decoded_size * sizeof(*match_lengths)) : NULL;
    uint16_t *match_offsets = decoded_size
        ? (uint16_t *)malloc(decoded_size * sizeof(*match_offsets)) : NULL;
    uint16_t *choices = decoded_size
        ? (uint16_t *)malloc(decoded_size * sizeof(*choices)) : NULL;
    uint64_t *costs = (uint64_t *)malloc((decoded_size + 1u) * sizeof(*costs));
    int32_t *heads = (int32_t *)malloc(HASH_SIZE * sizeof(*heads));
    int32_t *previous = input_size ? (int32_t *)malloc(input_size * sizeof(*previous)) : NULL;
    if ((decoded_size && (!match_lengths || !match_offsets || !choices)) || !costs || !heads ||
        (input_size && !previous))
        fail("out of memory");
    for (size_t i = 0; i < HASH_SIZE; ++i) heads[i] = -1;
    for (size_t i = 0; i < start; ++i) insert_position(input, input_size, i, heads, previous);
    for (size_t pos = start; pos < input_size; ++pos) {
        size_t length, offset;
        find_match(input, input_size, pos, heads, previous, &length, &offset);
        match_lengths[pos - start] = (uint16_t)length;
        match_offsets[pos - start] = (uint16_t)offset;
        insert_position(input, input_size, pos, heads, previous);
    }

    costs[decoded_size] = 0;
    for (size_t reverse = decoded_size; reverse > 0; --reverse) {
        size_t pos = reverse - 1;
        costs[pos] = 1u + literal_costs[input[start + pos]] + costs[pos + 1];
        choices[pos] = 0;
        size_t maximum = match_lengths[pos];
        size_t minimum = maximum > 7u ? maximum - 7u : MIN_MATCH;
        if (minimum < MIN_MATCH) minimum = MIN_MATCH;
        for (size_t length = minimum; length <= maximum; ++length) {
            uint16_t offset = (uint16_t)(match_offsets[pos] - 1u);
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
                size_t offset = match_offsets[pos] - 1u;
                append_byte(&output, (uint8_t)offset);
                append_byte(&output, (uint8_t)(offset >> 8));
                append_byte(&output, (uint8_t)(length - MIN_MATCH));
                pos += length;
            }
        }
        output.data[control_pos] = control;
    }
    free(previous);
    free(heads);
    free(costs);
    free(choices);
    free(match_offsets);
    free(match_lengths);
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
        size_t history_start = pos > WINDOW ? pos - WINDOW : 0;
        size_t history_size = pos - history_start;
        Buffer trial = compress_range(input + history_start, history_size + decoded_size,
                                      history_size, decoded_size, lazy_limit);
        size_t payload_size = trial.size - HEADER_SIZE;
        Buffer huffman = {0};
        if (entropy) huffman = huffman_encode(trial.data + HEADER_SIZE, payload_size);
        Buffer split = {0};
        if (split_entropy)
            split = split_huffman_encode(trial.data + HEADER_SIZE, payload_size, decoded_size);
        Buffer parsed_split = {0};
        if (entropy_parse) {
            Buffer parsed_trial = compress_entropy_parsed_range(
                input + history_start, history_size + decoded_size, history_size,
                decoded_size, trial.data + HEADER_SIZE, payload_size);
            parsed_split = split_huffman_encode(parsed_trial.data + HEADER_SIZE,
                                                parsed_trial.size - HEADER_SIZE,
                                                decoded_size);
            if (entropy_parse > 1) {
                Buffer iterated_trial = compress_entropy_parsed_range(
                    input + history_start, history_size + decoded_size, history_size,
                    decoded_size, parsed_trial.data + HEADER_SIZE,
                    parsed_trial.size - HEADER_SIZE);
                Buffer iterated_split = split_huffman_encode(
                    iterated_trial.data + HEADER_SIZE,
                    iterated_trial.size - HEADER_SIZE, decoded_size);
                if (iterated_split.size < parsed_split.size) {
                    free(parsed_split.data);
                    parsed_split = iterated_split;
                } else {
                    free(iterated_split.data);
                }
                free(iterated_trial.data);
            }
            free(parsed_trial.data);
        }
        Buffer transformed_split = {0};
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
                                                         decoded_size);
                free(transformed_trial.data);
                free(transformed);
            }
        }
        uint8_t mode = 0;
        size_t stored_size = decoded_size;
        if (payload_size < stored_size) { mode = 4; stored_size = payload_size; }
        if (entropy && huffman.size < stored_size) { mode = 5; stored_size = huffman.size; }
        if (split_entropy && split.size < stored_size) { mode = 6; stored_size = split.size; }
        if (entropy_parse && parsed_split.size < stored_size) {
            mode = 6;
            stored_size = parsed_split.size;
        }
        if (transformed_stride && transformed_split.size + 1 < stored_size) {
            mode = 7;
            stored_size = transformed_split.size + 1;
        }
        append_byte(&output, mode);
        append_u32le(&output, (uint32_t)decoded_size);
        append_u32le(&output, (uint32_t)stored_size);
        if (mode == 4)
            append_bytes(&output, trial.data + HEADER_SIZE, payload_size);
        else if (mode == 5)
            append_bytes(&output, huffman.data, huffman.size);
        else if (mode == 6) {
            const Buffer *selected_split = entropy_parse && parsed_split.size == stored_size
                ? &parsed_split : &split;
            append_bytes(&output, selected_split->data, selected_split->size);
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
        free(trial.data);
        pos += decoded_size;
    }
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
        if ((mode > 2 && mode != 4 && mode != 5 && mode != 6 && mode != 7) ||
            block_decoded == 0 || block_decoded > BLOCK_SIZE ||
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
                                 match_base, fast_huffman);
        } else {
            if (block_stored < 1) fail("truncated delta block");
            size_t block_start = output.size;
            size_t stride = input[ip];
            split_huffman_decode(&output, input + ip + 1, block_stored - 1,
                                 block_decoded, block_start, fast_huffman);
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
        fprintf(stderr, "usage: bite c|cl|cb|clb|cbh|clbh|cblink|clblink|cblinkh|clblinkh|cblinks|clblinks|clblinksg|clblinksd|clblinksdf|clblinksopt|clblinksopt2|d|dwork|dslow INPUT OUTPUT\n");
        fprintf(stderr, "  c/cl: v1; cb/clb: v2; h: Huffman; link: linked history; d: decode\n");
        return 2;
    }
    int is_greedy = strcmp(argv[1], "c") == 0 || strcmp(argv[1], "cb") == 0 ||
                    strcmp(argv[1], "cbh") == 0 || strcmp(argv[1], "cblink") == 0 ||
                    strcmp(argv[1], "cblinkh") == 0 || strcmp(argv[1], "cblinks") == 0;
    int is_lazy = strcmp(argv[1], "cl") == 0 || strcmp(argv[1], "clb") == 0 ||
                  strcmp(argv[1], "clbh") == 0 || strcmp(argv[1], "clblink") == 0 ||
                  strcmp(argv[1], "clblinkh") == 0 || strcmp(argv[1], "clblinks") == 0 ||
                  strcmp(argv[1], "clblinksg") == 0 || strcmp(argv[1], "clblinksd") == 0 ||
                  strcmp(argv[1], "clblinksdf") == 0 || strcmp(argv[1], "clblinksopt") == 0 ||
                  strcmp(argv[1], "clblinksopt2") == 0;
    int is_linked = strcmp(argv[1], "cblink") == 0 || strcmp(argv[1], "clblink") == 0 ||
                    strcmp(argv[1], "cblinkh") == 0 || strcmp(argv[1], "clblinkh") == 0;
    is_linked = is_linked || strcmp(argv[1], "cblinks") == 0 || strcmp(argv[1], "clblinks") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksg") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksd") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksdf") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt") == 0;
    is_linked = is_linked || strcmp(argv[1], "clblinksopt2") == 0;
    int is_blocked = strcmp(argv[1], "cb") == 0 || strcmp(argv[1], "clb") == 0 ||
                     strcmp(argv[1], "cbh") == 0 || strcmp(argv[1], "clbh") == 0 || is_linked;
    int is_entropy = strcmp(argv[1], "cbh") == 0 || strcmp(argv[1], "clbh") == 0 ||
                     strcmp(argv[1], "cblinkh") == 0 || strcmp(argv[1], "clblinkh") == 0 ||
                     strcmp(argv[1], "cblinks") == 0 || strcmp(argv[1], "clblinks") == 0 ||
                     strcmp(argv[1], "clblinksg") == 0 || strcmp(argv[1], "clblinksd") == 0 ||
                     strcmp(argv[1], "clblinksdf") == 0 || strcmp(argv[1], "clblinksopt") == 0 ||
                     strcmp(argv[1], "clblinksopt2") == 0;
    int is_split_entropy = strcmp(argv[1], "cblinks") == 0 || strcmp(argv[1], "clblinks") == 0 ||
                           strcmp(argv[1], "clblinksg") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksd") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksdf") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt") == 0;
    is_split_entropy = is_split_entropy || strcmp(argv[1], "clblinksopt2") == 0;
    int is_gated_lazy = strcmp(argv[1], "clblinksg") == 0;
    int is_delta_stride = strcmp(argv[1], "clblinksd") == 0 ||
                          strcmp(argv[1], "clblinksdf") == 0 ||
                          strcmp(argv[1], "clblinksopt") == 0 ||
                          strcmp(argv[1], "clblinksopt2") == 0;
    int is_sampled_delta = strcmp(argv[1], "clblinksdf") == 0 ||
                           strcmp(argv[1], "clblinksopt") == 0 ||
                           strcmp(argv[1], "clblinksopt2") == 0;
    int is_entropy_parse = strcmp(argv[1], "clblinksopt") == 0 ? 1 :
                           (strcmp(argv[1], "clblinksopt2") == 0 ? 2 : 0);
    int is_decode = strcmp(argv[1], "d") == 0 || strcmp(argv[1], "dwork") == 0 ||
                    strcmp(argv[1], "dslow") == 0;
    if (!is_greedy && !is_lazy && !is_decode) {
        fprintf(stderr, "usage: bite c|cl|cb|clb|cbh|clbh|cblink|clblink|cblinkh|clblinkh|cblinks|clblinks|clblinksg|clblinksd|clblinksdf|clblinksopt|clblinksopt2|d|dwork|dslow INPUT OUTPUT\n");
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
