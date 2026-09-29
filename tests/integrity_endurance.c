#define _POSIX_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

extern int fsync(int descriptor);

#define MEBIBYTE             (1024U * 1024U)
#define MAX_TRANSFER_BYTES   (2U * MEBIBYTE)
#define BUFFER_SLACK         4096U
#define MAX_FILE_MEBIBYTES   1024U
#define MAX_PASSES           100000U

static const unsigned chunkLengths[] = {
    4096U,
    8192U,
    4608U,
    8704U,
    65536U,
    MEBIBYTE,
    MAX_TRANSFER_BYTES
};

static const unsigned bufferOffsets[] = {
    0U,
    1U,
    4095U,
    17U,
    2047U,
    3U,
    127U
};

static unsigned char patternByte(unsigned pass, unsigned long position)
{
    unsigned value;

    value = pass * 97U + (unsigned)position * 17U;
    value ^= (unsigned)(position >> 7);
    value ^= (unsigned)(position >> 19);
    return (unsigned char)(value & 0xffU);
}

static void fillPattern(unsigned char *buffer, unsigned length,
                        unsigned pass, unsigned long position)
{
    unsigned i;

    for (i = 0; i < length; i++)
        buffer[i] = patternByte(pass, position + i);
}

static int verifyPattern(const unsigned char *buffer, unsigned length,
                         unsigned pass, unsigned long position)
{
    unsigned char expected;
    unsigned i;

    for (i = 0; i < length; i++) {
        expected = patternByte(pass, position + i);
        if (buffer[i] != expected) {
            fprintf(stderr,
                    "pass %u byte %lu: got %u expected %u\n",
                    pass, position + i, (unsigned)buffer[i],
                    (unsigned)expected);
            return 0;
        }
    }
    return 1;
}

static int writeFull(int descriptor, const unsigned char *buffer,
                     unsigned length)
{
    unsigned completed;
    int result;

    completed = 0;
    while (completed < length) {
        result = write(descriptor, buffer + completed, length - completed);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return 0;
        completed += (unsigned)result;
    }
    return 1;
}

static int readFull(int descriptor, unsigned char *buffer, unsigned length)
{
    unsigned completed;
    int result;

    completed = 0;
    while (completed < length) {
        result = read(descriptor, buffer + completed, length - completed);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return 0;
        completed += (unsigned)result;
    }
    return 1;
}

static int writePass(const char *path, unsigned char *allocation,
                     unsigned long fileBytes, unsigned pass)
{
    unsigned char *buffer;
    unsigned long position;
    unsigned remaining;
    unsigned chunk;
    unsigned index;
    int descriptor;

    descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
    if (descriptor < 0) {
        perror(path);
        return 0;
    }
    position = 0;
    index = 0;
    while (position < fileBytes) {
        chunk = chunkLengths[index %
                             (sizeof(chunkLengths) / sizeof(chunkLengths[0]))];
        remaining = (unsigned)(fileBytes - position);
        if (chunk > remaining)
            chunk = remaining;
        buffer = allocation + bufferOffsets[index %
                    (sizeof(bufferOffsets) / sizeof(bufferOffsets[0]))];
        fillPattern(buffer, chunk, pass, position);
        if (!writeFull(descriptor, buffer, chunk)) {
            perror("write");
            close(descriptor);
            return 0;
        }
        position += chunk;
        index++;
    }
    if (fsync(descriptor) < 0) {
        perror("fsync");
        close(descriptor);
        return 0;
    }
    if (close(descriptor) < 0) {
        perror("close");
        return 0;
    }
    return 1;
}

static int verifyPass(const char *path, unsigned char *allocation,
                      unsigned long fileBytes, unsigned pass)
{
    unsigned char *buffer;
    unsigned long position;
    unsigned remaining;
    unsigned chunk;
    unsigned index;
    int descriptor;

    descriptor = open(path, O_RDONLY | O_BINARY, 0);
    if (descriptor < 0) {
        perror(path);
        return 0;
    }
    position = 0;
    index = 0;
    while (position < fileBytes) {
        chunk = chunkLengths[index %
                             (sizeof(chunkLengths) / sizeof(chunkLengths[0]))];
        remaining = (unsigned)(fileBytes - position);
        if (chunk > remaining)
            chunk = remaining;
        buffer = allocation + bufferOffsets[index %
                    (sizeof(bufferOffsets) / sizeof(bufferOffsets[0]))];
        if (!readFull(descriptor, buffer, chunk)) {
            fprintf(stderr, "pass %u byte %lu: short read\n", pass, position);
            close(descriptor);
            return 0;
        }
        if (!verifyPattern(buffer, chunk, pass, position)) {
            close(descriptor);
            return 0;
        }
        position += chunk;
        index++;
    }
    if (close(descriptor) < 0) {
        perror("close");
        return 0;
    }
    return 1;
}

static int parseCount(const char *text, unsigned maximum, unsigned *value)
{
    char *end;
    unsigned long parsed;

    parsed = strtoul(text, &end, 10);
    if (*text == '\0' || *end != '\0' || parsed == 0 || parsed > maximum)
        return 0;
    *value = (unsigned)parsed;
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char *allocation;
    unsigned fileMebibytes;
    unsigned passes;
    unsigned pass;
    unsigned long fileBytes;

    if (argc != 4) {
        fprintf(stderr, "usage: %s path file-MiB passes\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (!parseCount(argv[2], MAX_FILE_MEBIBYTES, &fileMebibytes)) {
        fprintf(stderr, "file-MiB must be between 1 and %u\n",
                MAX_FILE_MEBIBYTES);
        return EXIT_FAILURE;
    }
    if (!parseCount(argv[3], MAX_PASSES, &passes)) {
        fprintf(stderr, "passes must be between 1 and %u\n", MAX_PASSES);
        return EXIT_FAILURE;
    }
    fileBytes = (unsigned long)fileMebibytes * MEBIBYTE;
    allocation = (unsigned char *)malloc(MAX_TRANSFER_BYTES + BUFFER_SLACK);
    if (allocation == 0) {
        fprintf(stderr, "cannot allocate endurance buffer\n");
        return EXIT_FAILURE;
    }

    for (pass = 1; pass <= passes; pass++) {
        if (!writePass(argv[1], allocation, fileBytes, pass) ||
            !verifyPass(argv[1], allocation, fileBytes, pass)) {
            free(allocation);
            return EXIT_FAILURE;
        }
        printf("pass %u/%u: %u MiB written, synced, and verified\n",
               pass, passes, fileMebibytes);
    }

    unlink(argv[1]);
    free(allocation);
    printf("endurance complete: %u pass(es), %u MiB dataset\n",
           passes, fileMebibytes);
    return EXIT_SUCCESS;
}
