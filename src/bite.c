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

static Buffer compress_buffer(const uint8_t *input, size_t input_size, int lazy) {
    if (input_size > INT32_MAX) fail("input exceeds baseline 2 GiB limit");
    Buffer output = {0};
    reserve(&output, HEADER_SIZE + input_size / 2 + 64);
    append_byte(&output, 'B'); append_byte(&output, 'I');
    append_byte(&output, 'T'); append_byte(&output, 'E');
    append_byte(&output, 1); append_byte(&output, 0);
    append_byte(&output, 0); append_byte(&output, 0);
    append_u64le(&output, (uint64_t)input_size);

    int32_t *heads = (int32_t *)malloc(HASH_SIZE * sizeof(*heads));
    int32_t *previous = input_size ? (int32_t *)malloc(input_size * sizeof(*previous)) : NULL;
    if (!heads || (input_size && !previous)) fail("out of memory");
    for (size_t i = 0; i < HASH_SIZE; ++i) heads[i] = -1;

    size_t pos = 0;
    while (pos < input_size) {
        size_t control_pos = output.size;
        append_byte(&output, 0);
        uint8_t control = 0;
        for (unsigned bit = 0; bit < 8 && pos < input_size; ++bit) {
            size_t length, offset;
            find_match(input, input_size, pos, heads, previous, &length, &offset);
            int current_inserted = 0;
            if (lazy && length && length < MAX_MATCH && pos + 1 < input_size) {
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

static Buffer huffman_decode(const uint8_t *input, size_t input_size) {
    if (input_size < 6) fail("truncated Huffman header");
    size_t decoded_size = read_u32le(input);
    size_t tree_size = read_u16le(input + 4);
    if (decoded_size > BLOCK_SIZE * 2u || tree_size == 0 || tree_size > input_size - 6)
        fail("invalid Huffman header");
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
        while (output.size < decoded_size) {
            int node = root;
            while (nodes[node].symbol < 0) {
                int bit = read_bit(&data_reader);
                if (bit < 0) fail("truncated Huffman data");
                node = bit ? nodes[node].right : nodes[node].left;
            }
            append_byte(&output, (uint8_t)nodes[node].symbol);
        }
    }
    while ((padding = read_bit(&data_reader)) >= 0)
        if (padding) fail("invalid Huffman data padding");
    return output;
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

static void decompress_payload(Buffer *output, const uint8_t *input,
                               size_t input_size, size_t decoded_size) {
    if (decoded_size > SIZE_MAX - output->size) fail("decoded size overflow");
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
                if (offset > output->size - block_start || length > target_size - output->size)
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

static Buffer decompress_buffer(const uint8_t *input, size_t input_size) {
    if (input_size < HEADER_SIZE || memcmp(input, "BITE", 4) != 0 || (input[4] != 1 && input[4] != 2))
        fail("invalid or unsupported stream");
    uint64_t decoded_u64 = read_u64le(input + 8);
    if (decoded_u64 > SIZE_MAX) fail("decoded size unsupported");
    size_t decoded_size = (size_t)decoded_u64;
    Buffer output = {0};
    reserve(&output, decoded_size ? decoded_size : 1);
    if (input[4] == 1) {
        decompress_payload(&output, input + HEADER_SIZE, input_size - HEADER_SIZE, decoded_size);
        return output;
    }

    size_t ip = HEADER_SIZE;
    while (output.size < decoded_size) {
        if (input_size - ip < BLOCK_HEADER_SIZE) fail("truncated block header");
        uint8_t mode = input[ip++];
        size_t block_decoded = read_u32le(input + ip); ip += 4;
        size_t block_stored = read_u32le(input + ip); ip += 4;
        if (mode > 2 || block_decoded == 0 || block_decoded > BLOCK_SIZE ||
            block_decoded > decoded_size - output.size || block_stored > input_size - ip)
            fail("invalid block");
        if (mode == 0) {
            if (block_stored != block_decoded) fail("invalid raw block");
            append_bytes(&output, input + ip, block_stored);
        } else if (mode == 1) {
            decompress_payload(&output, input + ip, block_stored, block_decoded);
        } else {
            Buffer token_stream = huffman_decode(input + ip, block_stored);
            decompress_payload(&output, token_stream.data, token_stream.size, block_decoded);
            free(token_stream.data);
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

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: bite c|cl|cb|clb|cbh|clbh|d INPUT OUTPUT\n");
        fprintf(stderr, "  c/cl: v1; cb/clb: v2 blocked; cbh/clbh: v2 + Huffman; d: decompress\n");
        return 2;
    }
    int is_greedy = strcmp(argv[1], "c") == 0 || strcmp(argv[1], "cb") == 0 || strcmp(argv[1], "cbh") == 0;
    int is_lazy = strcmp(argv[1], "cl") == 0 || strcmp(argv[1], "clb") == 0 || strcmp(argv[1], "clbh") == 0;
    int is_blocked = strcmp(argv[1], "cb") == 0 || strcmp(argv[1], "clb") == 0 ||
                     strcmp(argv[1], "cbh") == 0 || strcmp(argv[1], "clbh") == 0;
    int is_entropy = strcmp(argv[1], "cbh") == 0 || strcmp(argv[1], "clbh") == 0;
    int is_decode = strcmp(argv[1], "d") == 0;
    if (!is_greedy && !is_lazy && !is_decode) {
        fprintf(stderr, "usage: bite c|cl|cb|clb|cbh|clbh|d INPUT OUTPUT\n");
        fprintf(stderr, "  c/cl: v1; cb/clb: v2 blocked; cbh/clbh: v2 + Huffman; d: decompress\n");
        return 2;
    }
    Buffer input = read_file(argv[2]);
    Buffer output;
    if (is_decode)
        output = decompress_buffer(input.data, input.size);
    else if (is_blocked)
        output = compress_blocked(input.data, input.size, is_lazy, is_entropy);
    else
        output = compress_buffer(input.data, input.size, is_lazy);
    write_file(argv[3], &output);
    free(output.data);
    free(input.data);
    return 0;
}
