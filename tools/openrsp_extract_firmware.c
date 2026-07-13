/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FIRMWARE_LENGTH 6115u

static const unsigned char first_signature[] = {
    0x02,0x00,0x21,0x02,0x03,0x58,0xcc,0xcc,0xcc,0xcc,0xcc,0x02,0x03,0x95,0xcc,0xcc,
    0xcc,0xcc,0xcc,0x02,0x03,0x96,0xcc,0xcc,0xcc,0xcc,0xcc,0x02,0x12,0x74,0x02,0x00
};
static const unsigned char second_signature[] = {
    0x8f,0x83,0xed,0xf0,0x05,0x29,0xe4,0xb5,0x29,0xa8,0x05,0x2a,0x80,0xa4,0x90,0x18,
    0xe4,0xe4,0xf0,0x90,0xc0,0x00,0x74,0x0b,0xf0,0xe4,0xa3,0xf0,0xa3,0xf0,0xa3,0xf0
};

static int regular_file(const char *path)
{
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static const char *resolve_service(const char *input, char *resolved, size_t size)
{
    if (regular_file(input)) return input;
    const char *suffixes[] = {"/bin/sdrplay_apiService", "/sdrplay_apiService"};
    for (size_t index = 0; index < sizeof(suffixes) / sizeof(suffixes[0]); ++index) {
        if (snprintf(resolved, size, "%s%s", input, suffixes[index]) > 0 && regular_file(resolved))
            return resolved;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 4 || strcmp(argv[2], "--output") != 0) {
        fprintf(stderr, "Usage: %s OFFICIAL_INSTALL_OR_SERVICE --output FIRMWARE_FILE\n", argv[0]);
        return EXIT_FAILURE;
    }
    char resolved[4096];
    const char *source = resolve_service(argv[1], resolved, sizeof(resolved));
    if (!source) {
        fprintf(stderr, "Cannot find sdrplay_apiService under: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    FILE *file = fopen(source, "rb");
    if (!file) return EXIT_FAILURE;
    if (fseek(file, 0, SEEK_END) != 0) return EXIT_FAILURE;
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) return EXIT_FAILURE;
    unsigned char *binary = malloc((size_t)length);
    if (!binary || fread(binary, 1, (size_t)length, file) != (size_t)length) return EXIT_FAILURE;
    fclose(file);

    size_t match = 0;
    unsigned int matches = 0;
    for (size_t offset = 0; offset + FIRMWARE_LENGTH <= (size_t)length; ++offset) {
        if (memcmp(binary + offset, first_signature, sizeof(first_signature)) == 0 &&
            memcmp(binary + offset + 4096u, second_signature, sizeof(second_signature)) == 0) {
            if (matches == 0u) {
                match = offset;
            } else if (memcmp(binary + match, binary + offset, FIRMWARE_LENGTH) != 0) {
                fprintf(stderr, "Found conflicting validated RSPduo firmware images\n");
                free(binary);
                return EXIT_FAILURE;
            }
            ++matches;
        }
    }
    if (matches == 0u) {
        fprintf(stderr, "No validated RSPduo firmware image found\n");
        free(binary);
        return EXIT_FAILURE;
    }
    FILE *output = fopen(argv[3], "wb");
    if (!output) {
        fprintf(stderr, "Cannot create %s: %s\n", argv[3], strerror(errno));
        free(binary);
        return EXIT_FAILURE;
    }
    int result = fwrite(binary + match, 1, FIRMWARE_LENGTH, output) == FIRMWARE_LENGTH &&
                 fclose(output) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    free(binary);
    if (result == EXIT_SUCCESS)
        printf("FIRMWARE_EXTRACTED bytes=%u offset=%zu identical_copies=%u source=%s output=%s\n",
               FIRMWARE_LENGTH, match, matches, source, argv[3]);
    return result;
}
