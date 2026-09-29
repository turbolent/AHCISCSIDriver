#define _POSIX_SOURCE

#include <sys/types.h>
#include <sys/wait.h>
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

#define PAGE_SIZE_BYTES 4096U
#define MAX_TRANSFER_BYTES (2U * 1024U * 1024U)
#define MAX_WORKERS 32

typedef struct TransferCase {
    unsigned length;
    unsigned offset;
} TransferCase;

static const TransferCase transferCases[] = {
    { 512U, 0U },
    { 4096U, 1U },
    { 4608U, 4095U },
    { 8192U, 17U },
    { 8704U, 2047U },
    { 65536U, 3U },
    { 1048576U, 127U },
    { MAX_TRANSFER_BYTES, 4095U }
};

static const unsigned rawTransferLengths[] = {
    4096U,
    8192U,
    12288U,
    65536U,
    1048576U,
    MAX_TRANSFER_BYTES
};

static unsigned char patternByte(unsigned worker, unsigned round,
                                 unsigned record, unsigned position)
{
    return (unsigned char)((worker * 53U + round * 29U +
                            record * 131U + position * 17U) & 0xffU);
}

static void fillPattern(unsigned char *buffer, unsigned length,
                        unsigned worker, unsigned round, unsigned record)
{
    unsigned i;
    for (i = 0; i < length; i++)
        buffer[i] = patternByte(worker, round, record, i);
}

static int verifyPattern(const unsigned char *buffer, unsigned length,
                         unsigned worker, unsigned round, unsigned record)
{
    unsigned i;
    for (i = 0; i < length; i++) {
        if (buffer[i] != patternByte(worker, round, record, i)) {
            fprintf(stderr,
                    "worker %u round %u record %u byte %u: got %u expected %u\n",
                    worker, round, record, i, (unsigned)buffer[i],
                    (unsigned)patternByte(worker, round, record, i));
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

/* OPENSTEP's raw sd path can submit 512 KiB commands despite the
 * controller's advertised 128 KiB maximum. Keep each raw syscall within
 * that contract; retain the larger logical records and full checksums.
 * File-mode stress still issues its original large filesystem reads. */
static int readRawFull(int descriptor, unsigned char *buffer, unsigned length)
{
    unsigned completed, chunk;
    completed = 0;
    while (completed < length) {
        chunk = length - completed;
        if (chunk > 128U * 1024U) chunk = 128U * 1024U;
        if (!readFull(descriptor, buffer + completed, chunk)) return 0;
        completed += chunk;
    }
    return 1;
}

static unsigned long checksumBuffer(const unsigned char *buffer,
                                    unsigned length)
{
    unsigned long checksum;
    unsigned i;

    checksum = 2166136261UL;
    for (i = 0; i < length; i++) {
        checksum ^= buffer[i];
        checksum *= 16777619UL;
    }
    return checksum;
}

static int runWriteWorker(const char *path, unsigned rounds, unsigned worker)
{
    unsigned char *allocation;
    unsigned char *buffer;
    unsigned round;
    unsigned record;
    unsigned long bytes;
    int descriptor;

    allocation = (unsigned char *)malloc(MAX_TRANSFER_BYTES +
                                         PAGE_SIZE_BYTES * 2U);
    if (allocation == 0) {
        fprintf(stderr, "worker %u: cannot allocate transfer buffer\n", worker);
        return 0;
    }
    bytes = 0;

    for (round = 0; round < rounds; round++) {
        descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
        if (descriptor < 0) {
            perror(path);
            free(allocation);
            return 0;
        }
        for (record = 0;
             record < sizeof(transferCases) / sizeof(transferCases[0]);
             record++) {
            buffer = allocation + transferCases[record].offset;
            fillPattern(buffer, transferCases[record].length,
                        worker, round, record);
            if (!writeFull(descriptor, buffer, transferCases[record].length)) {
                perror("write");
                close(descriptor);
                free(allocation);
                return 0;
            }
            bytes += transferCases[record].length;
        }
        if (fsync(descriptor) < 0 || close(descriptor) < 0) {
            perror("sync/close");
            free(allocation);
            return 0;
        }

        descriptor = open(path, O_RDONLY | O_BINARY, 0);
        if (descriptor < 0) {
            perror(path);
            free(allocation);
            return 0;
        }
        for (record = 0;
             record < sizeof(transferCases) / sizeof(transferCases[0]);
             record++) {
            buffer = allocation + transferCases[record].offset;
            if (!readFull(descriptor, buffer, transferCases[record].length)) {
                perror("read");
                close(descriptor);
                free(allocation);
                return 0;
            }
            if (!verifyPattern(buffer, transferCases[record].length,
                               worker, round, record)) {
                close(descriptor);
                free(allocation);
                return 0;
            }
            bytes += transferCases[record].length;
        }
        if (close(descriptor) < 0) {
            perror("close");
            free(allocation);
            return 0;
        }
    }

    unlink(path);
    free(allocation);
    printf("worker %u: %u round(s), %lu verified I/O bytes\n",
           worker, rounds, bytes);
    return 1;
}

static int runReadWorker(const char *path, unsigned rounds, unsigned worker)
{
    unsigned char *allocation;
    unsigned char *aligned;
    unsigned char *buffer;
    unsigned long expected[sizeof(rawTransferLengths) /
                           sizeof(rawTransferLengths[0])];
    unsigned long checksum;
    unsigned long bytes;
    unsigned round;
    unsigned record;
    int descriptor;

    allocation = (unsigned char *)malloc(MAX_TRANSFER_BYTES +
                                         PAGE_SIZE_BYTES * 2U);
    if (allocation == 0) {
        fprintf(stderr, "worker %u: cannot allocate transfer buffer\n", worker);
        return 0;
    }
    aligned = (unsigned char *)(((size_t)allocation + PAGE_SIZE_BYTES - 1U) &
                                ~((size_t)PAGE_SIZE_BYTES - 1U));
    descriptor = open(path, O_RDONLY | O_BINARY, 0);
    if (descriptor < 0) {
        perror(path);
        free(allocation);
        return 0;
    }
    bytes = 0;

    for (round = 0; round < rounds; round++) {
        if (lseek(descriptor, 0L, SEEK_SET) < 0) {
            perror("lseek");
            close(descriptor);
            free(allocation);
            return 0;
        }
        for (record = 0;
             record < sizeof(rawTransferLengths) /
                      sizeof(rawTransferLengths[0]);
             record++) {
            buffer = aligned;
            if (!readRawFull(descriptor, buffer, rawTransferLengths[record])) {
                fprintf(stderr, "worker %u round %u record %u: ",
                        worker, round, record);
                perror("read");
                close(descriptor);
                free(allocation);
                return 0;
            }
            checksum = checksumBuffer(buffer, rawTransferLengths[record]);
            if (round == 0)
                expected[record] = checksum;
            else if (checksum != expected[record]) {
                fprintf(stderr,
                        "worker %u round %u record %u: checksum changed\n",
                        worker, round, record);
                close(descriptor);
                free(allocation);
                return 0;
            }
            bytes += rawTransferLengths[record];
        }
    }

    close(descriptor);
    free(allocation);
    printf("worker %u: %u read-only round(s), %lu stable bytes\n",
           worker, rounds, bytes);
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
    char workerPath[1024];
    const char *path;
    unsigned rounds;
    unsigned workers;
    unsigned worker;
    unsigned argument;
    int readOnly;
    int status;
    int failures;
    int child;

    readOnly = 0;
    argument = 1;
    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        readOnly = 1;
        argument++;
    }
    if ((unsigned)argc < argument + 1U ||
        (unsigned)argc > argument + 3U) {
        fprintf(stderr,
                "usage: %s [-r] path [rounds [workers]]\n", argv[0]);
        return EXIT_FAILURE;
    }
    path = argv[argument++];
    rounds = 1;
    workers = 1;
    if ((unsigned)argc > argument &&
        !parseCount(argv[argument], 100000U, &rounds)) {
        fprintf(stderr, "rounds must be between 1 and 100000\n");
        return EXIT_FAILURE;
    }
    argument++;
    if ((unsigned)argc > argument &&
        !parseCount(argv[argument], MAX_WORKERS, &workers)) {
        fprintf(stderr, "workers must be between 1 and %u\n", MAX_WORKERS);
        return EXIT_FAILURE;
    }
    if (workers == 1) {
        if (readOnly)
            return runReadWorker(path, rounds, 0) ?
                   EXIT_SUCCESS : EXIT_FAILURE;
        return runWriteWorker(path, rounds, 0) ?
               EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (strlen(path) + 12U >= sizeof(workerPath)) {
        fprintf(stderr, "path is too long\n");
        return EXIT_FAILURE;
    }

    failures = 0;
    for (worker = 0; worker < workers; worker++) {
        if (readOnly)
            strcpy(workerPath, path);
        else
            sprintf(workerPath, "%s.%u", path, worker);
        child = fork();
        if (child == 0) {
            if (readOnly)
                exit(runReadWorker(workerPath, rounds, worker) ?
                     EXIT_SUCCESS : EXIT_FAILURE);
            exit(runWriteWorker(workerPath, rounds, worker) ?
                 EXIT_SUCCESS : EXIT_FAILURE);
        }
        if (child < 0) {
            perror("fork");
            failures++;
            break;
        }
    }
    while (wait(&status) > 0) {
        if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS)
            failures++;
    }
    if (failures != 0) {
        fprintf(stderr, "%d worker(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all %u stress workers passed\n", workers);
    return EXIT_SUCCESS;
}
