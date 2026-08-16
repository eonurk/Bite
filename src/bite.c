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

static Buffer compress_blocked(const uint8_t *input, size_t input_size, int lazy) {
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
        int use_lz = payload_size < decoded_size;
        append_byte(&output, (uint8_t)use_lz);
        append_u32le(&output, (uint32_t)decoded_size);
        append_u32le(&output, (uint32_t)(use_lz ? payload_size : decoded_size));
        if (use_lz)
            append_bytes(&output, trial.data + HEADER_SIZE, payload_size);
        else
            append_bytes(&output, input + pos, decoded_size);
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
        if (mode > 1 || block_decoded == 0 || block_decoded > BLOCK_SIZE ||
            block_decoded > decoded_size - output.size || block_stored > input_size - ip)
            fail("invalid block");
        if (mode == 0) {
            if (block_stored != block_decoded) fail("invalid raw block");
            append_bytes(&output, input + ip, block_stored);
        } else {
            decompress_payload(&output, input + ip, block_stored, block_decoded);
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
        fprintf(stderr, "usage: bite c|cl|cb|clb|d INPUT OUTPUT\n");
        fprintf(stderr, "  c/cl: v1 greedy/lazy; cb/clb: v2 blocked greedy/lazy; d: decompress\n");
        return 2;
    }
    int is_greedy = strcmp(argv[1], "c") == 0 || strcmp(argv[1], "cb") == 0;
    int is_lazy = strcmp(argv[1], "cl") == 0 || strcmp(argv[1], "clb") == 0;
    int is_blocked = strcmp(argv[1], "cb") == 0 || strcmp(argv[1], "clb") == 0;
    int is_decode = strcmp(argv[1], "d") == 0;
    if (!is_greedy && !is_lazy && !is_decode) {
        fprintf(stderr, "usage: bite c|cl|cb|clb|d INPUT OUTPUT\n");
        fprintf(stderr, "  c/cl: v1 greedy/lazy; cb/clb: v2 blocked greedy/lazy; d: decompress\n");
        return 2;
    }
    Buffer input = read_file(argv[2]);
    Buffer output;
    if (is_decode)
        output = decompress_buffer(input.data, input.size);
    else if (is_blocked)
        output = compress_blocked(input.data, input.size, is_lazy);
    else
        output = compress_buffer(input.data, input.size, is_lazy);
    write_file(argv[3], &output);
    free(output.data);
    free(input.data);
    return 0;
}
