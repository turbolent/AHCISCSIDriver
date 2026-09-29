/* OPENSTEP 4.2: cc -O -Wall native_file_raw.c -o native_file_raw
 *
 * native_file_raw create|verify /dev/rsdNh FS_BASE /absolute/file [passes [workers]]
 * FS_BASE is an explicitly supplied byte offset within the raw h device.
 * Example for the qualified physical root: /dev/rsd0h 164864.
 * Defaults/minimums: 100 passes, 4 processes. The 8 MiB file is retained.
 *
 * Checksumming a live root's raw LBA 0 region is invalid: normal superblock,
 * free-space and inode updates change those bytes. This test instead reads
 * only the stable extents of an O_EXCL-created, known-pattern regular file.
 * Raw descriptors are ALWAYS O_RDONLY. There are no raw or disklabel writes.
 * Keep the file untouched and the filesystem mounted throughout the test.
 * Only big-endian NeXT UFS, 128-byte inodes, full blocks, direct and single
 * indirect addressing, and offsets accessible to signed 32-bit off_t work.
 * Unsupported layouts fail closed; the supplied base is never guessed.
 * Layout reference: quickstep/nextufs/src/core/{image,layout}.c.
 */
#define _POSIX_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int fsync(int descriptor);

#define FILE_BYTES (8UL * 1024UL * 1024UL)
#define MAX_IO 131072U
#define MAX_BLOCKS 1024U
#define MAX_WORKERS 16U
#define U32_MAX 0xffffffffUL
#define OFFSET_MAX 0x7fffffffUL
#define HASH_START 2166136261UL

typedef struct Geometry {
    unsigned long base, size, ncg, bsize, fsize, frag;
    unsigned long sblk, iblk, dblk, delta, mask, ipg, fpg, inopb, nindir;
    unsigned long csaddr, cssize, csend;
} Geometry;

typedef struct FileMap {
    unsigned char inode[128];
    unsigned long inode_offset, indirect, block[MAX_BLOCKS];
    unsigned count;
} FileMap;

static unsigned char *scratch, *expected;

static int fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return 0;
}

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
        ((unsigned long)p[2] << 8) | p[3];
}

static unsigned be16(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static unsigned long hash(unsigned long value, const unsigned char *p,
                          unsigned length)
{
    while (length--) value = ((value ^ *p++) * 16777619UL) & U32_MAX;
    return value;
}

static int add(unsigned long a, unsigned long b, unsigned long *result)
{
    if (a > U32_MAX || b > U32_MAX - a) return 0;
    *result = a + b;
    return 1;
}

static int multiply(unsigned long a, unsigned long b, unsigned long *result)
{
    if (a > U32_MAX || (a && b > U32_MAX / a)) return 0;
    *result = a * b;
    return 1;
}

static int power2(unsigned long n)
{
    return n && !(n & (n - 1));
}

/* One aligned raw syscall per transfer. A short raw read is an error, not
 * an invitation to retry an unaligned remainder or silently reduce stress. */
static int raw_read(int fd, unsigned long offset, unsigned length)
{
    int n;
    if (!length || length > MAX_IO || (length & 511U) || (offset & 511UL) ||
        offset > OFFSET_MAX || length - 1UL > OFFSET_MAX - offset)
        return fail("raw read outside alignment/32-bit offset bounds");
    if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
        perror("raw lseek"); return 0;
    }
    do { n = read(fd, scratch, length); } while (n < 0 && errno == EINTR);
    if (n != (int)length) {
        fprintf(stderr, "raw offset=%lu length=%u result=%d\n", offset, length, n);
        if (n < 0) perror("raw read");
        return fail("short or failed raw read");
    }
    return 1;
}

static int geometry(int fd, unsigned long base, Geometry *g)
{
    unsigned long total, shift, sector_shift;
    memset(g, 0, sizeof(*g));
    if ((base & 511UL) || base > OFFSET_MAX - 10240UL ||
        !raw_read(fd, base + 8192UL, 2048U)) return 0;
    if (be32(scratch + 1372) != 0x00011954UL)
        return fail("no big-endian NeXT UFS superblock at supplied base + 8192");
    g->base = base;
    g->sblk = be32(scratch + 8); g->iblk = be32(scratch + 16);
    g->dblk = be32(scratch + 20); g->delta = be32(scratch + 24);
    g->mask = be32(scratch + 28); g->size = be32(scratch + 36);
    g->ncg = be32(scratch + 44); g->bsize = be32(scratch + 48);
    g->fsize = be32(scratch + 52); g->frag = be32(scratch + 56);
    g->nindir = be32(scratch + 116); g->inopb = be32(scratch + 120);
    g->csaddr = be32(scratch + 152); g->cssize = be32(scratch + 156);
    g->ipg = be32(scratch + 184); g->fpg = be32(scratch + 188);
    if (!power2(g->bsize) || g->bsize < 8192UL || g->bsize > 65536UL ||
        !power2(g->fsize) || g->fsize < 512UL || g->fsize > g->bsize ||
        g->frag != g->bsize / g->fsize || g->frag > 8UL ||
        g->inopb != g->bsize / 128UL || g->nindir != g->bsize / 4UL ||
        !g->size || !g->ncg || !g->ipg || !g->fpg ||
        g->ipg % g->inopb || g->fpg % g->frag ||
        (g->size - 1UL) / g->fpg + 1UL != g->ncg ||
        g->sblk >= g->iblk || g->iblk >= g->dblk || g->dblk >= g->fpg ||
        g->iblk % g->frag || g->delta % g->frag ||
        !multiply(g->ipg / g->inopb, g->frag, &total) ||
        total > g->dblk - g->iblk ||
        !multiply(g->size, g->fsize, &total) ||
        !add(base, total, &total) ||
        FILE_BYTES / g->bsize > 12UL + g->nindir)
        return fail("unsupported or inconsistent UFS geometry");
    shift = 0; while ((1UL << shift) < g->frag) shift++;
    sector_shift = 0;
    while ((512UL << sector_shift) < g->fsize) sector_shift++;
    if (be32(scratch + 96) != shift || be32(scratch + 100) != sector_shift ||
        be32(scratch + 124) != g->fsize / 512UL)
        return fail("inconsistent UFS fragment shifts");
    if (!g->csaddr || !g->cssize || g->cssize % g->fsize ||
        !multiply(g->ncg, 16UL, &total) || g->cssize < total ||
        !add(g->csaddr, g->cssize / g->fsize, &g->csend) || g->csend > g->size)
        return fail("invalid UFS cylinder-summary range");
    printf("UFS base=%lu size_frags=%lu bsize=%lu fsize=%lu ncg=%lu ipg=%lu "
           "fpg=%lu inode_frag=%lu data_frag=%lu cg_delta=%lu cg_mask=%08lx "
           "summary_frag=%lu summary_bytes=%lu "
           "super_raw_fnv32=%08lx (mutable bytes not compared)\n",
           base, g->size, g->bsize, g->fsize, g->ncg, g->ipg, g->fpg,
           g->iblk, g->dblk, g->delta, g->mask, g->csaddr, g->cssize,
           hash(HASH_START, scratch, 2048U));
    return 1;
}

static int cg_start(const Geometry *g, unsigned long cg, unsigned long *start)
{
    unsigned long a, b;
    return cg < g->ncg && multiply(cg, g->fpg, &a) &&
        multiply(g->delta, cg & (g->mask ^ U32_MAX), &b) &&
        add(a, b, start) && *start < g->size;
}

static int block_offset(const Geometry *g, unsigned long frag, unsigned long *off)
{
    unsigned long bytes;
    return frag && frag % g->frag == 0 && frag < g->size &&
        g->frag <= g->size - frag && multiply(frag, g->fsize, &bytes) &&
        add(g->base, bytes, off) && *off <= OFFSET_MAX &&
        g->bsize - 1UL <= OFFSET_MAX - *off;
}

/* Full data blocks must lie outside their cylinder group's metadata.
 * Groups after zero may also have data before the alternate superblock. */
static int data_block(const Geometry *g, unsigned long frag, unsigned long *off)
{
    unsigned long cg, start, end, boundary, group_end;
    if (!block_offset(g, frag, off)) return 0;
    cg = frag / g->fpg;
    if (!cg_start(g, cg, &start) || !add(frag, g->frag, &end) ||
        !multiply(cg + 1UL, g->fpg, &group_end) || end > group_end)
        return 0;
    if (frag < g->csend && end > g->csaddr) return 0;
    if (cg && add(start, g->sblk, &boundary) && end <= boundary) return 1;
    return add(start, g->dblk, &boundary) && frag >= boundary;
}

static int load_map(int fd, const Geometry *g, const struct stat *st, FileMap *m)
{
    unsigned long ino, group, start, frag, a, offset, slot, unused;
    unsigned i, j;
    const unsigned char *p;
    memset(m, 0, sizeof(*m));
    ino = (unsigned long)st->st_ino;
    group = ino / g->ipg;
    slot = ino % g->ipg;
    if (ino < 2UL || ino > U32_MAX || !cg_start(g, group, &start) ||
        !add(start, g->iblk, &frag) ||
        !multiply(slot / g->inopb, g->frag, &a) || !add(frag, a, &frag) ||
        !block_offset(g, frag, &offset) ||
        !raw_read(fd, offset, (unsigned)g->bsize))
        return fail("invalid or inaccessible inode location");
    slot = (slot % g->inopb) * 128UL;
    m->inode_offset = offset + slot;
    memcpy(m->inode, scratch + slot, 128U);
    p = m->inode;
    if (be16(p) != (unsigned)st->st_mode || be16(p + 2) != 1U ||
        be16(p + 4) != (unsigned)st->st_uid ||
        be16(p + 6) != (unsigned)st->st_gid ||
        be32(p + 8) != 0 || be32(p + 12) != FILE_BYTES ||
        be32(p + 24) != (unsigned long)st->st_mtime ||
        be32(p + 32) != (unsigned long)st->st_ctime ||
        be32(p + 92) || be32(p + 96))
        return fail("raw inode does not match regular file or uses extra indirection");
    m->count = (unsigned)(FILE_BYTES / g->bsize);
    if (m->count > MAX_BLOCKS || m->count <= 12U ||
        be32(p + 104) != (m->count + 1UL) * (g->bsize / 512UL))
        return fail("unexpected file allocation size");
    for (i = 0; i < 12U; i++) m->block[i] = be32(p + 40U + i * 4U);
    m->indirect = be32(p + 88);
    if (!data_block(g, m->indirect, &offset) ||
        !raw_read(fd, offset, (unsigned)g->bsize))
        return fail("invalid single-indirect block");
    for (i = 12U; i < m->count; i++) m->block[i] = be32(scratch + (i - 12U) * 4U);
    for (unused = m->count - 12U; unused < g->nindir; unused++)
        if (be32(scratch + unused * 4UL))
            return fail("nonzero indirect pointer beyond file length");
    for (i = 0; i < m->count; i++) {
        if (!data_block(g, m->block[i], &offset) || m->block[i] == m->indirect)
            return fail("hole, metadata alias, or out-of-bounds data block");
        for (j = 0; j < i; j++)
            if (m->block[i] == m->block[j])
                return fail("duplicate/overlapping data extents");
    }
    /* Reading through the filesystem may legitimately change only atime. */
    memset(m->inode + 16, 0, 8U);
    return 1;
}

static unsigned run_blocks(const Geometry *g, const FileMap *m, unsigned i)
{
    unsigned n;
    n = 1;
    while (i + n < m->count && (n + 1UL) * g->bsize <= MAX_IO &&
           m->block[i + n] == m->block[i] + n * g->frag) n++;
    return n;
}

static void print_map(const Geometry *g, const FileMap *m)
{
    unsigned i, n, runs, max;
    unsigned long off, map_hash;
    unsigned char encoded[4];
    map_hash = HASH_START;
    for (i = 0; i < m->count; i++) {
        encoded[0] = (unsigned char)(m->block[i] >> 24);
        encoded[1] = (unsigned char)(m->block[i] >> 16);
        encoded[2] = (unsigned char)(m->block[i] >> 8);
        encoded[3] = (unsigned char)m->block[i];
        map_hash = hash(map_hash, encoded, 4U);
    }
    printf("MAP inode_byte=%lu generation=%lu indirect_frag=%lu blocks=%u "
           "map_fnv32=%08lx inode_fnv32_no_atime=%08lx\n", m->inode_offset,
           be32(m->inode + 108), m->indirect, m->count, map_hash,
           hash(HASH_START, m->inode, 128U));
    runs = max = 0;
    for (i = 0; i < m->count; i += n) {
        n = run_blocks(g, m, i);
        off = g->base + m->block[i] * g->fsize;
        printf("EXTENT file_byte=%lu raw_byte=%lu bytes=%lu first_frag=%lu\n",
               i * g->bsize, off, n * g->bsize, m->block[i]);
        runs++;
        if (n * g->bsize > max) max = (unsigned)(n * g->bsize);
    }
    printf("MAP transfers_per_pass=%u max_transfer=%u bytes\n", runs, max);
}

/* The integer mixer includes the full word offset, unlike a byte pattern
 * periodic at 256 bytes that cannot detect swapped or stale disk blocks. */
static void make_pattern(void)
{
    unsigned long i, v;
    for (i = 0; i < FILE_BYTES; i += 4UL) {
        v = (i / 4UL) ^ 0x6d5a56e9UL;
        v = ((v ^ (v >> 16)) * 0x45d9f3bUL) & U32_MAX;
        v = ((v ^ (v >> 16)) * 0x45d9f3bUL) & U32_MAX;
        v ^= v >> 16;
        expected[i] = (unsigned char)v;
        expected[i + 1] = (unsigned char)(v >> 8);
        expected[i + 2] = (unsigned char)(v >> 16);
        expected[i + 3] = (unsigned char)(v >> 24);
    }
}

static int write_file(int fd)
{
    unsigned long pos;
    int n;
    for (pos = 0; pos < FILE_BYTES; pos += (unsigned)n) {
        do { n = write(fd, expected + pos,
                       FILE_BYTES - pos < MAX_IO ? (unsigned)(FILE_BYTES - pos) : MAX_IO);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) { perror("regular file write"); return 0; }
    }
    if (fsync(fd) < 0) { perror("regular file fsync"); return 0; }
    return 1;
}

static int verify_file(int fd)
{
    unsigned long pos;
    unsigned done;
    int n;
    if (lseek(fd, 0, SEEK_SET) != 0) { perror("regular file seek"); return 0; }
    for (pos = 0; pos < FILE_BYTES; pos += MAX_IO) {
        done = 0;
        while (done < MAX_IO) {
            do { n = read(fd, scratch + done, MAX_IO - done); }
            while (n < 0 && errno == EINTR);
            if (n <= 0) return fail("short or failed regular file read");
            done += (unsigned)n;
        }
        if (memcmp(scratch, expected + pos, MAX_IO))
            return fail("regular file pattern mismatch");
    }
    return 1;
}

static int same_file(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_size == b->st_size && a->st_mode == b->st_mode &&
        a->st_uid == b->st_uid && a->st_gid == b->st_gid &&
        a->st_nlink == b->st_nlink && a->st_mtime == b->st_mtime &&
        a->st_ctime == b->st_ctime;
}

static int file_identity(int fd, const char *path, const struct stat *initial)
{
    struct stat opened, named;
    if (fstat(fd, &opened) < 0 || lstat(path, &named) < 0) {
        perror("file identity"); return 0;
    }
    if (!same_file(&opened, initial) || !same_file(&named, initial))
        return fail("file descriptor/path identity or metadata changed");
    return 1;
}

static int run_worker(const char *raw, const struct stat *device,
                      const Geometry *g, const FileMap *m,
                      unsigned passes, unsigned worker, unsigned long golden)
{
    struct stat current;
    unsigned pass, i, n, length, j;
    unsigned long sum, offset, logical;
    int fd;
    fd = open(raw, O_RDONLY);
    if (fd < 0) { perror("worker raw open"); return 0; }
    if (fstat(fd, &current) < 0 || current.st_rdev != device->st_rdev ||
        (current.st_mode & S_IFMT) != S_IFCHR) {
        close(fd); return fail("worker raw device changed");
    }
    for (pass = 0; pass < passes; pass++) {
        sum = HASH_START;
        for (i = 0; i < m->count; i += n) {
            n = run_blocks(g, m, i);
            length = (unsigned)(n * g->bsize);
            logical = i * g->bsize;
            offset = g->base + m->block[i] * g->fsize;
            if (!raw_read(fd, offset, length)) { close(fd); return 0; }
            if (memcmp(scratch, expected + logical, length)) {
                for (j = 0; j < length && scratch[j] == expected[logical + j]; j++) {}
                fprintf(stderr, "MISMATCH worker=%u pass=%u file_byte=%lu raw_byte=%lu "
                        "got=%02x expected=%02x\n", worker, pass + 1U, logical + j,
                        offset + j, (unsigned)scratch[j], (unsigned)expected[logical + j]);
                close(fd); return 0;
            }
            sum = hash(sum, scratch, length);
        }
        if (sum != golden) { close(fd); return fail("raw pass checksum mismatch"); }
        if (pass == 0 || (pass + 1U) % 25U == 0 || pass + 1U == passes) {
            printf("worker=%u pass=%u/%u raw_pattern_fnv32=%08lx PASS\n",
                   worker, pass + 1U, passes, sum);
            fflush(stdout);
        }
    }
    if (close(fd) < 0) { perror("worker raw close"); return 0; }
    return 1;
}

static int number(const char *text, unsigned long min, unsigned long max,
                  unsigned long *value)
{
    char *end;
    unsigned long n;
    if (*text < '0' || *text > '9') return 0;
    errno = 0; n = strtoul(text, &end, 0);
    if (errno || *end || n < min || n > max) return 0;
    *value = n; return 1;
}

static int raw_name(const char *path)
{
    const char *p;
    if (strncmp(path, "/dev/rsd", 8)) return 0;
    p = path + 8;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    return p[0] == 'h' && p[1] == '\0';
}

int main(int argc, char **argv)
{
    Geometry before, after;
    FileMap map, final_map;
    struct stat initial, device, named;
    unsigned char *allocation;
    unsigned long base, passes, workers, golden;
    unsigned worker, started, remaining;
    pid_t children[MAX_WORKERS], child, waited;
    int raw, file, create, status, failures, ok;

    passes = 100; workers = 4;
    if (argc < 5 || argc > 7 ||
        (strcmp(argv[1], "create") && strcmp(argv[1], "verify")) ||
        !raw_name(argv[2]) || argv[4][0] != '/' ||
        !number(argv[3], 0, OFFSET_MAX - 10240UL, &base) ||
        (argc > 5 && !number(argv[5], 100, 10000, &passes)) ||
        (argc > 6 && !number(argv[6], 4, MAX_WORKERS, &workers))) {
        fprintf(stderr, "usage: %s create|verify /dev/rsdNh FS_BASE /absolute/file "
                "[passes>=100 [workers=4..16]]\n", argv[0]);
        return 2;
    }
    allocation = (unsigned char *)malloc(MAX_IO + 8191U);
    expected = (unsigned char *)malloc((size_t)FILE_BYTES);
    if (!allocation || !expected) return !fail("buffer allocation");
    scratch = (unsigned char *)(((size_t)allocation + 8191U) & ~((size_t)8191U));
    make_pattern(); golden = hash(HASH_START, expected, (unsigned)FILE_BYTES);
    raw = open(argv[2], O_RDONLY);
    if (raw < 0) { perror("raw open"); return 1; }
    if (fstat(raw, &device) < 0 || (device.st_mode & S_IFMT) != S_IFCHR ||
        !geometry(raw, base, &before)) return !fail("invalid raw device/filesystem");
    create = !strcmp(argv[1], "create");
    if (!create && (lstat(argv[4], &named) < 0 ||
                   (named.st_mode & S_IFMT) != S_IFREG))
        return !fail("verify requires an existing regular file, not a symlink");
    file = open(argv[4], create ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY, 0600);
    if (file < 0) { perror("regular file open"); return 1; }
    if (fstat(file, &initial) < 0 || (initial.st_mode & S_IFMT) != S_IFREG ||
        initial.st_nlink != 1 || (create && initial.st_size != 0))
        return !fail("unexpected file type/link count/initial size");
    if (create && !write_file(file)) return 1;
    if (fstat(file, &initial) < 0 || initial.st_size != (off_t)FILE_BYTES ||
        !file_identity(file, argv[4], &initial) || !verify_file(file) ||
        !load_map(raw, &before, &initial, &map)) return 1;
    printf("FILE path=%s dev=%lu inode=%lu size=%lu nlink=%lu mtime=%lu ctime=%lu "
           "raw_rdev=%lu pattern=offset-mixer-v1 fnv32=%08lx\n", argv[4],
           (unsigned long)initial.st_dev, (unsigned long)initial.st_ino,
           (unsigned long)initial.st_size, (unsigned long)initial.st_nlink,
           (unsigned long)initial.st_mtime, (unsigned long)initial.st_ctime,
           (unsigned long)device.st_rdev, golden);
    print_map(&before, &map);
    /* OPENSTEP's old libc dereferences NULL in fflush(NULL). */
    fflush(stdout); fflush(stderr);
    started = 0; failures = 0;
    for (worker = 0; worker < (unsigned)workers; worker++) {
        child = fork();
        if (child < 0) { perror("fork"); failures++; break; }
        if (child == 0) {
            /* Each child opens its own raw fd: lseek positions must not be shared. */
            close(raw); close(file);
            ok = run_worker(argv[2], &device, &before, &map,
                            (unsigned)passes, worker, golden);
            fflush(stdout); fflush(stderr); _exit(ok ? 0 : 1);
        }
        children[started++] = child;
    }
    remaining = started;
    while (remaining) {
        /* OPENSTEP's libc supplies wait(), but not waitpid(). Account for
         * each PID exactly once and never treat ECHILD as successful work. */
        do { waited = wait(&status); }
        while (waited < 0 && errno == EINTR);
        if (waited < 0) { perror("wait"); failures++; break; }
        for (worker = 0; worker < started && children[worker] != waited; worker++) {}
        if (worker == started) {
            fail("unexpected or duplicate child exit"); failures++; continue;
        }
        children[worker] = 0;
        remaining--;
        if (!WIFEXITED(status) || WEXITSTATUS(status)) failures++;
    }
    if (!file_identity(file, argv[4], &initial) || !verify_file(file) ||
        !file_identity(file, argv[4], &initial) ||
        !geometry(raw, base, &after) || memcmp(&before, &after, sizeof(before)) ||
        !load_map(raw, &after, &initial, &final_map) ||
        memcmp(&map, &final_map, sizeof(map))) {
        fail("post-stress file identity, filesystem geometry, inode or map changed");
        failures++;
    }
    if (close(file) < 0 || close(raw) < 0) { perror("close"); failures++; }
    free(expected); free(allocation);
    if (failures) {
        fprintf(stderr, "FAIL: %d failure(s); test file retained at %s\n", failures, argv[4]);
        return 1;
    }
    printf("PASS workers=%lu passes=%lu verified_raw_MiB=%lu file_bytes=%lu "
           "expected_fnv32=%08lx stable_inode_and_map file_retained=%s\n",
           workers, passes, workers * passes * 8UL, FILE_BYTES, golden, argv[4]);
    return 0;
}
