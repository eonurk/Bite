#define BITE_NO_MAIN
#include "bite.c"

#include <time.h>

static double monotonic_seconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) fail("clock_gettime failed");
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static double median(double *values, size_t count) {
    qsort(values, count, sizeof(*values), compare_double);
    if (count & 1u) return values[count / 2];
    return (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

static Buffer benchmark_compress(const char *mode, const Buffer *input) {
    if (strcmp(mode, "clblinksdf") == 0)
        return compress_linked_blocked(input->data, input->size, MAX_MATCH,
                                       1, 1, 1, 1, 0);
    if (strcmp(mode, "clblinksopt") == 0)
        return compress_linked_blocked(input->data, input->size, MAX_MATCH,
                                       1, 1, 1, 1, 1);
    if (strcmp(mode, "clblinksopt2") == 0)
        return compress_linked_blocked(input->data, input->size, MAX_MATCH,
                                       1, 1, 1, 1, 2);
    if (strcmp(mode, "clblinksopt2d") == 0)
        return compress_linked_blocked(input->data, input->size, MAX_MATCH,
                                       1, 1, 1, 1, 3);
    if (strcmp(mode, "clblinksopt2df") == 0)
        return compress_linked_blocked(input->data, input->size, MAX_MATCH,
                                       1, 1, 1, 1, 4);
    if (strcmp(mode, "clblinksd") == 0)
        return compress_linked_blocked(input->data, input->size, MAX_MATCH,
                                       1, 1, 1, 0, 0);
    fail("unsupported in-process compression mode");
    return (Buffer){0};
}

static Buffer benchmark_decompress(const char *mode, const Buffer *input) {
    if (strcmp(mode, "d") == 0) return decompress_buffer(input->data, input->size, 1);
    if (strcmp(mode, "dwork") == 0) return decompress_buffer(input->data, input->size, 2);
    if (strcmp(mode, "dslow") == 0) return decompress_buffer(input->data, input->size, 0);
    fail("unsupported in-process decompression mode");
    return (Buffer){0};
}

static void verify_roundtrip(const Buffer *input, const Buffer *decoded) {
    if (input->size != decoded->size ||
        (input->size && memcmp(input->data, decoded->data, input->size) != 0))
        fail("in-process round-trip mismatch");
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: bite-bench COMPRESS_MODE DECOMPRESS_MODE REPEATS FILE [...]\n");
        return 2;
    }
    char *end = NULL;
    long parsed_repeats = strtol(argv[3], &end, 10);
    if (!end || *end || parsed_repeats < 1 || parsed_repeats > 100000)
        fail("invalid repeat count");
    size_t repeats = (size_t)parsed_repeats;
    double *encode_times = (double *)malloc(repeats * sizeof(*encode_times));
    double *decode_times = (double *)malloc(repeats * sizeof(*decode_times));
    if (!encode_times || !decode_times) fail("out of memory");

    printf("file\toriginal_size_bytes\tcompressed_size_bytes\tencode_time_seconds\tdecode_time_seconds\tcorrect\n");
    for (int argument = 4; argument < argc; ++argument) {
        Buffer input = read_file(argv[argument]);
        Buffer warm_encoded = benchmark_compress(argv[1], &input);
        Buffer warm_decoded = benchmark_decompress(argv[2], &warm_encoded);
        verify_roundtrip(&input, &warm_decoded);
        size_t compressed_size = warm_encoded.size;
        free(warm_decoded.data);
        free(warm_encoded.data);

        for (size_t repeat = 0; repeat < repeats; ++repeat) {
            double started = monotonic_seconds();
            Buffer encoded = benchmark_compress(argv[1], &input);
            encode_times[repeat] = monotonic_seconds() - started;
            if (encoded.size != compressed_size) fail("nondeterministic compressed size");

            started = monotonic_seconds();
            Buffer decoded = benchmark_decompress(argv[2], &encoded);
            decode_times[repeat] = monotonic_seconds() - started;
            verify_roundtrip(&input, &decoded);
            free(decoded.data);
            free(encoded.data);
        }
        printf("%s\t%zu\t%zu\t%.12f\t%.12f\t1\n", argv[argument], input.size,
               compressed_size, median(encode_times, repeats),
               median(decode_times, repeats));
        free(input.data);
    }
    free(decode_times);
    free(encode_times);
    return 0;
}
