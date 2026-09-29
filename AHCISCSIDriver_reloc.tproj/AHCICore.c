#include "AHCICore.h"
#include "AHCIRegs.h"
#include <string.h>

typedef char ahci_u32_size[(sizeof(AHCIU32) == 4) ? 1 : -1];
typedef char ahci_header_size[(sizeof(AHCIHeader) == 32) ? 1 : -1];
typedef char ahci_prd_size[(sizeof(AHCIPRD) == 16) ? 1 : -1];

AHCIU32 AHCIReadBE(const AHCIB8 *p, unsigned n)
{
    AHCIU32 v = 0;
    while (n--) v = (v << 8) | *p++;
    return v;
}
void AHCIWriteBE(AHCIB8 *p, AHCIU64 v, unsigned n)
{
    while (n) { p[--n] = (AHCIB8)v; v >>= 8; }
}
static unsigned word(const AHCIB8 *p, unsigned n)
{ return p[n * 2] | ((unsigned)p[n * 2 + 1] << 8); }
static void ataString(char *out, const AHCIB8 *data, unsigned n, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; i++) {
        unsigned ch = data[n * 2 + (i ^ 1U)];
        out[i] = (ch >= 32 && ch <= 126) ? (char)ch : ' ';
    }
    while (count && out[count - 1] == ' ') count--;
    out[count] = 0;
}
void AHCISense(AHCIB8 *s, unsigned key, unsigned asc, unsigned ascq)
{
    memset(s, 0, AHCI_SENSE_SIZE);
    s[0] = 0x70; s[2] = (AHCIB8)key; s[7] = 10;
    s[12] = (AHCIB8)asc; s[13] = (AHCIB8)ascq;
}
int AHCIIdentify(const AHCIB8 *data, int packet, AHCIDevice *d)
{
    unsigned w, i, checksum = 0;
    memset(d, 0, sizeof(*d));
    if (word(data, 0) == 0xffff || !!(word(data, 0) & 0x8000) != !!packet)
        return 0;
    if (data[510] == 0xa5) {
        for (i = 0; i < 512; i++) checksum += data[i];
        if (checksum & 255) return 0;
    }
    d->packet = packet; d->removable = !!(word(data, 0) & 0x80);
    d->dma = !!(word(data, 49) & 0x100);
    ataString(d->serial, data, 10, 20);
    ataString(d->firmware, data, 23, 8);
    ataString(d->model, data, 27, 40);
    if (packet) {
        w = word(data, 0) & 3;
        if (w > 1) return 0;
        d->packetBytes = w ? 16 : 12;
        return 1;
    }
    if (!(word(data, 49) & 0x200) || !d->dma) return 0;
    w = word(data, 83);
    if ((w & 0xc000) == 0x4000) {
        d->lba48 = !!(w & 0x400);
        d->flush = !!(w & (d->lba48 ? 0x2000 : 0x1000));
    }
    /* Word 87 validates the enabled-feature words 85..87. Unknown cache
     * state must not suppress FUA/flush: only a valid disabled indication
     * proves that ordinary WRITE DMA reaches nonvolatile storage. */
    d->cache = (word(data, 87) & 0xc000) != 0x4000 ||
               (word(data, 85) & 0x20) != 0;
    d->blocks = word(data, 60) | ((AHCIU64)word(data, 61) << 16);
    if (d->lba48) {
        d->blocks = 0;
        for (i = 0; i < 4; i++) d->blocks |= (AHCIU64)word(data, 100+i) << (16*i);
        if (d->blocks > (1ULL << 48)) return 0;
    } else if (d->blocks > (1U << 28)) return 0;
    d->blockSize = 512;
    w = word(data, 106);
    if ((w & 0xc000) == 0x4000 && (w & 0x1000)) {
        AHCIU64 size = (word(data, 117) | ((AHCIU64)word(data, 118) << 16)) * 2;
        if (size < 512 || size > 4096 || (size & (size - 1))) return 0;
        d->blockSize = (AHCIU32)size;
    }
    return d->blocks != 0;
}
unsigned AHCICDBLength(const AHCIB8 *cdb, unsigned supplied, unsigned available)
{
    unsigned n, group = cdb[0] >> 5;
    if (group == 0) n = 6;
    else if (group == 1 || group == 2) n = 10;
    else if (group == 4) n = 16;
    else if (group == 5) n = 12;
    else n = supplied;
    if (!n || n > available || n > 16 || (supplied && supplied != n)) return 0;
    return n;
}
void AHCIFIS(AHCIB8 *fis, unsigned command)
{
    memset(fis, 0, 20); fis[0] = 0x27; fis[1] = 0x80;
    fis[2] = (AHCIB8)command;
}
static AHCIPlan check(unsigned key, unsigned asc, unsigned ascq)
{
    AHCIPlan p;
    memset(&p, 0, sizeof(p)); p.action = AHCI_CHECK;
    AHCISense(p.sense, key, asc, ascq); return p;
}
static AHCIPlan response(AHCIB8 *buffer, unsigned capacity,
    const AHCIB8 *data, unsigned size, unsigned allocation)
{
    AHCIPlan p;
    memset(&p, 0, sizeof(p));
    if (size > allocation) size = allocation;
    if (size > capacity) size = capacity;
    if (size) memcpy(buffer, data, size);
    p.responseBytes = size; return p;
}
static void ataAddress(AHCIPlan *p, const AHCIDevice *d, unsigned op28, unsigned op48)
{
    unsigned i;
    AHCIFIS(p->fis, d->lba48 ? op48 : op28);
    p->fis[7] = 0x40;
    for (i = 0; i < 3; i++) p->fis[4+i] = (AHCIB8)(p->lba >> (i*8));
    if (d->lba48) {
        for (i = 0; i < 3; i++) p->fis[8+i] = (AHCIB8)(p->lba >> (24+i*8));
        p->fis[13] = (AHCIB8)(p->blocks >> 8);
    } else p->fis[7] |= (AHCIB8)(p->lba >> 24);
    p->fis[12] = (AHCIB8)p->blocks;
    p->action = AHCI_ATA;
}

static AHCIPlan packetPlan(const AHCIDevice *d, const AHCIB8 *c,
    unsigned len, int read, AHCIB8 *buf, unsigned cap)
{
    AHCIPlan p;
    unsigned n, hdr, bd, payload;
    memset(&p, 0, sizeof(p));
    p.action = AHCI_PACKET; p.read = read; p.bytes = cap;
    p.allocation = cap;
    if (len > (unsigned)d->packetBytes) return check(5, 0x24, 0);
    /* OPENSTEP probes the SCSI disconnect/reconnect page even on optical
     * targets. SATA packet devices have no SCSI disconnect bus to configure. */
    if ((c[0] == 0x1a || c[0] == 0x5a) && (c[2] & 63) == 2) {
        AHCIB8 mode[24];
        unsigned header = c[0] == 0x1a ? 4 : 8;
        if (!read || (c[1] & ~8U) || c[3] || (c[2] >> 6) == 3) return check(5,0x24,0);
        memset(mode,0,sizeof(mode)); mode[header]=2; mode[header+1]=14;
        if ((c[2] >> 6) != 1) mode[header+2]=1;
        AHCIWriteBE(mode,header+16-(header == 4 ? 1 : 2),header == 4 ? 1 : 2);
        return response(buf,cap,mode,header+16,c[0] == 0x1a ? c[4] : AHCIReadBE(c+7,2));
    }
    memcpy(p.packet, c, len);
    if (len == 6) p.packet[1] &= 0x1f;
    if (c[0] == 0x08 || c[0] == 0x0a) {
        memset(p.packet, 0, 16);
        p.packet[0] = c[0] == 8 ? 0x28 : 0x2a;
        p.packet[3] = c[1] & 0x1f; p.packet[4] = c[2]; p.packet[5] = c[3];
        n = c[4] ? c[4] : 256; AHCIWriteBE(p.packet + 7, n, 2);
        p.packet[9] = c[5];
    } else if (c[0] == 0x1a) {
        if (!read || (c[1] & ~8U) || c[3]) return check(5, 0x24, 0);
        memset(p.packet, 0, 16); p.packet[0] = 0x5a;
        p.packet[1] = c[1]; p.packet[2] = c[2];
        n = c[4]; if (n > cap) n = cap;
        if (!n) { p.action=AHCI_DONE; p.bytes=0; return p; }
        p.bytes = n < 4 ? 8 : n + 4; p.allocation = n;
        AHCIWriteBE(p.packet + 7, p.bytes, 2);
        p.packet[9] = c[5]; p.conversion = AHCI_CONVERT_MODE6;
    } else if (c[0] == 0x15 || c[0] == 0x55) {
        if (read) return check(5, 0x24, 0);
        hdr = c[0] == 0x15 ? 4 : 8;
        n = c[0] == 0x15 ? c[4] : AHCIReadBE(c + 7, 2);
        if (!n) { p.action = AHCI_DONE; p.bytes = 0; return p; }
        if (n > cap || n < hdr) return check(5, 0x1a, 0);
        bd = hdr == 4 ? buf[3] : AHCIReadBE(buf + 6, 2);
        if (bd > n - hdr) return check(5, 0x26, 0);
        payload = n - hdr - bd;
        if (payload > AHCI_MAX_TRANSFER - 8) return check(5, 0x1a, 0);
        memmove(buf + 8, buf + hdr + bd, payload);
        memset(buf, 0, 8);
        memset(p.packet, 0, 16); p.packet[0] = 0x55;
        p.packet[1] = c[1]; p.packet[9] = c[len-1];
        p.bytes = payload + 8; p.responseBytes = n;
        AHCIWriteBE(p.packet + 7, p.bytes, 2);
    }
    AHCIFIS(p.fis, 0xa0);
    /* Control packets use PIO protocol; AHCI still DMA-transfers their data. */
    if (d->dma && (p.packet[0] == 0x28 || p.packet[0] == 0x2a ||
        p.packet[0] == 0xa8 || p.packet[0] == 0xaa ||
        p.packet[0] == 0x88 || p.packet[0] == 0x8a)) p.fis[3] = 1;
    n = p.bytes; if (n > 0xfffe) n = 0xfffe;
    n = (n + 1) & ~1U;
    p.fis[5] = (AHCIB8)n; p.fis[6] = (AHCIB8)(n >> 8);
    return p;
}

AHCIPlan AHCITranslate(const AHCIDevice *d, const AHCIB8 *c,
    unsigned len, int read, AHCIB8 *buf, unsigned cap, const AHCIB8 *sense)
{
    AHCIPlan p;
    AHCIB8 data[256];
    unsigned op = c[0], n, allocation = 0, page, pos, hdr, start, end;
    int write = 0, verify = 0;
    memset(&p, 0, sizeof(p)); memset(data, 0, sizeof(data));
    if (!AHCICDBLength(c, len, len) || cap > AHCI_MAX_TRANSFER || (cap && !buf))
        return check(5, 0x24, 0);
    if (c[len-1] & 5) return check(5, 0x24, 0); /* linked/NACA unsupported */
    if (d->packet) return packetPlan(d, c, len, read, buf, cap);
    /* Ordinary non-NCQ READ DMA cannot honor force-unit-access reads. */
    if ((op == 0x28 || op == 0xa8 || op == 0x88) && (c[1] & 8))
        return check(5,0x24,0);
    switch (op) {
    case 0x00: return p;
    case 0x03:
        if (!read || c[1] || c[2] || c[3]) return check(5, 0x24, 0);
        if (sense) memcpy(data, sense, 18); else AHCISense(data, 0, 0, 0);
        return response(buf, cap, data, 18, c[4]);
    case 0x12:
        if (!read || (c[1] & ~1U) || c[3]) return check(5, 0x24, 0);
        if (c[1] & 1) {
            data[1] = c[2];
            if (!c[2]) { data[3] = 3; data[4] = 0; data[5] = 0x80; data[6] = 0x83; n = 7; }
            else if (c[2] == 0x80) {
                n = (unsigned)strlen(d->serial); data[3] = (AHCIB8)n;
                memcpy(data+4, d->serial, n); n += 4;
            } else if (c[2] == 0x83) {
                data[3] = 68; data[4] = 2; data[5] = 1; data[7] = 64;
                memset(data+8, ' ', 64); memcpy(data+8, "ATA     ", 8);
                memcpy(data+16, d->model, strlen(d->model));
                memcpy(data+48, d->serial, strlen(d->serial)); n = 72;
            } else return check(5, 0x24, 0);
        } else {
            if (c[2]) return check(5, 0x24, 0);
            data[1] = d->removable ? 0x80 : 0; data[2] = 2; data[3] = 2; data[4] = 31;
            memset(data+8, ' ', 28); memcpy(data+8, "ATA", 3);
            n = (unsigned)strlen(d->model); if (n > 16) n = 16;
            memcpy(data+16, d->model, n);
            n = (unsigned)strlen(d->firmware); if (n > 4) n = 4;
            memcpy(data+32, d->firmware, n); n = 36;
        }
        return response(buf, cap, data, n, c[4]);
    case 0x25:
        if (!read || c[1] || AHCIReadBE(c+2,4) || c[8]) return check(5,0x24,0);
        AHCIWriteBE(data, d->blocks - 1 > 0xffffffffULL ? 0xffffffffULL : d->blocks-1, 4);
        AHCIWriteBE(data+4, d->blockSize, 4);
        return response(buf, cap, data, 8, 8);
    case 0x9e:
        if (!read || c[1] != 0x10 || AHCIReadBE(c+2,4) || AHCIReadBE(c+6,4) || c[14])
            return check(5,0x24,0);
        AHCIWriteBE(data, d->blocks-1, 8); AHCIWriteBE(data+8, d->blockSize, 4);
        return response(buf, cap, data, 32, AHCIReadBE(c+10,4));
    case 0xa0:
        if (!read || c[2]) return check(5,0x24,0);
        data[3] = 8; return response(buf, cap, data, 16, AHCIReadBE(c+6,4));
    case 0x1a: case 0x5a:
        if (!read || (c[1] & ~8U) || c[3] || (c[2] >> 6) == 3) return check(5,0x24,0);
        hdr = op == 0x1a ? 4 : 8; pos = hdr;
        allocation = op == 0x1a ? c[4] : AHCIReadBE(c+7,2);
        if (!(c[1] & 8)) {
            AHCIWriteBE(data+pos+1, d->blocks > 0xffffff ? 0xffffff : d->blocks, 3);
            AHCIWriteBE(data+pos+5, d->blockSize, 3); pos += 8;
            data[hdr-1] = 8;
        }
        page = c[2] & 63;
        /* OPENSTEP asks for page zero to obtain only the block descriptor. */
        if (page != 0 && page != 2 && page != 3 && page != 4 && page != 8 && page != 0x3f)
            return check(5,0x24,0);
        start = page == 0x3f ? 2 : page; end = page == 0x3f ? 8 : page;
        for (n = start; n <= end; n++) {
            unsigned size = n == 2 ? 16 : n == 3 || n == 4 ? 24 : n == 8 ? 20 : 0;
            if (!size) continue;
            data[pos] = (AHCIB8)n; data[pos+1] = (AHCIB8)(size-2);
            if ((c[2] >> 6) != 1) {
                if (n == 2) data[pos+2] = 1;
                if (n == 3) { AHCIWriteBE(data+pos+10,63,2); AHCIWriteBE(data+pos+12,d->blockSize,2); }
                if (n == 4) { AHCIU64 cyl = d->blocks/(255*63); if (!cyl) cyl=1;
                    AHCIWriteBE(data+pos+2,cyl > 0xffffff ? 0xffffff : cyl,3); data[pos+5]=255; }
                if (n == 8 && d->cache) data[pos+2] = 4;
            }
            pos += size;
        }
        AHCIWriteBE(data, pos - (hdr == 4 ? 1 : 2), hdr == 4 ? 1 : 2);
        return response(buf, cap, data, pos, allocation);
    case 0x1e:
        if (c[4] > 1 || c[1] || c[2] || c[3]) return check(5,0x24,0);
        return p; /* Fixed disk has no eject mechanism. */
    case 0x35: case 0x91:
        if (c[1] || c[op == 0x35 ? 6 : 14]) return check(5,0x24,0);
        if (!d->flush) return d->cache ? check(5,0x20,0) : p;
        p.action = AHCI_ATA; AHCIFIS(p.fis, d->lba48 ? 0xea : 0xe7); return p;
    case 0x1b:
        if (c[1] || c[2] || c[3] || (c[4] & ~1U)) return check(5,0x24,0);
        if (c[4] & 1) return p; /* A subsequent read wakes an ATA disk. */
        if (!d->flush && d->cache) return check(5,0x20,0);
        p.action = AHCI_ATA; p.stopAfterFlush = d->flush;
        AHCIFIS(p.fis, d->flush ? (d->lba48 ? 0xea : 0xe7) : 0xe0); return p;
    case 0x08: case 0x0a: case 0x13:
        p.lba = ((AHCIU32)(c[1]&31)<<16) | AHCIReadBE(c+2,2);
        p.blocks = c[4] ? c[4] : 256; write = op == 0x0a; verify = op == 0x13; break;
    case 0x28: case 0x2a: case 0x2f:
        if (c[1] & ~0x18U || c[6]) return check(5,0x24,0);
        p.lba = AHCIReadBE(c+2,4); p.blocks = AHCIReadBE(c+7,2);
        write = op == 0x2a; verify = op == 0x2f; p.fua = write && (c[1]&8); break;
    case 0xa8: case 0xaa: case 0xaf:
        if (c[1] & ~0x18U || c[10]) return check(5,0x24,0);
        p.lba = AHCIReadBE(c+2,4); p.blocks = AHCIReadBE(c+6,4);
        write = op == 0xaa; verify = op == 0xaf; p.fua = write && (c[1]&8); break;
    case 0x88: case 0x8a: case 0x8f:
        if (c[1] & ~0x18U || c[14]) return check(5,0x24,0);
        p.lba = ((AHCIU64)AHCIReadBE(c+2,4)<<32) | AHCIReadBE(c+6,4);
        p.blocks = AHCIReadBE(c+10,4); write = op == 0x8a;
        verify = op == 0x8f; p.fua = write && (c[1]&8); break;
    default: return check(5,0x20,0);
    }
    if (!p.blocks) return p;
    if (p.lba >= d->blocks || p.blocks > d->blocks-p.lba) return check(5,0x21,0);
    if (!d->lba48 && (p.lba + p.blocks > (1ULL<<28) || p.blocks > 256))
        return check(5,0x24,0);
    if (p.blocks > 65536) return check(5,0x24,0);
    if (verify) {
        if (op != 0x13 && c[1]) return check(5,0x24,0);
        ataAddress(&p,d,0x40,0x42); return p;
    }
    if (!!read == !!write || p.blocks > cap/d->blockSize) return check(5,0x24,0);
    if (p.fua && d->cache && !d->flush) return check(5,0x24,0);
    if (!d->cache) p.fua = 0;
    p.bytes = p.blocks*d->blockSize; p.read = !write;
    ataAddress(&p,d,write ? 0xca : 0xc8,write ? 0x35 : 0x25);
    return p;
}

int AHCIPacketFinish(const AHCIPlan *p, AHCIB8 *data, unsigned done, unsigned *bytes)
{
    unsigned bd, mdl, count;
    if (done > p->bytes) return 0;
    *bytes = done;
    if (p->conversion == AHCI_CONVERT_MODE6) {
        if (done < 8) return 0;
        bd = AHCIReadBE(data+6,2); mdl = AHCIReadBE(data,2);
        if (bd > 255 || mdl < 6) return 0;
        count = done-4;
        data[0] = (AHCIB8)(mdl-3 > 255 ? 255 : mdl-3);
        data[1] = data[2]; data[2] = data[3]; data[3] = (AHCIB8)bd;
        memmove(data+4, data+8, done-8);
        if (count > p->allocation) count = p->allocation;
        *bytes = count;
    } else if (p->responseBytes) {
        if (done != p->bytes) return 0;
        *bytes = p->responseBytes;
    }
    return 1;
}
void AHCIATAError(AHCIB8 *s, unsigned status, unsigned error, int write)
{
    if (status & 0x20) AHCISense(s,4,0x44,0);
    else if (error & 0x80) AHCISense(s,0xb,0x47,3);
    else if (error & 0x40) AHCISense(s,3,write ? 0x0c : 0x11,0);
    else if (error & 0x10) AHCISense(s,5,0x21,0);
    else if (error & 4) AHCISense(s,0xb,0,0);
    else AHCISense(s,4,0x44,0);
}
int AHCIBuildTable(AHCIHeader *h, AHCITable *t, AHCIU32 address,
    const AHCIPlan *p, const AHCIU32 *pages, unsigned count, unsigned offset)
{
    unsigned remaining = (p->bytes+1)&~1U, i=0, n;
    if ((address & 127) || offset >= 4096 || (offset & 1) ||
        p->bytes > AHCI_MAX_TRANSFER || (remaining && !pages)) return 0;
    memset(h,0,sizeof(*h)); memset(t,0,sizeof(*t));
    h->flags = (AHCIU16)(5 | ((!p->read && p->bytes) ? 0x40 : 0) |
                 (p->action == AHCI_PACKET ? 0x20 : 0));
    h->table = address;
    memcpy(t->fis,p->fis,20); memcpy(t->packet,p->packet,16);
    while (remaining) {
        if (i >= count || i >= AHCI_MAX_PRDS || (pages[i] & 4095)) return 0;
        n = 4096-offset; if (n > remaining) n = remaining;
        t->prd[i].address = pages[i]+offset; t->prd[i].count = n-1;
        remaining -= n; offset=0; i++;
    }
    h->prdtLength = (AHCIU16)i;
    return 1;
}
int AHCICompletion(unsigned ci, unsigned tfd, unsigned is,
    unsigned done, unsigned expected, int exact)
{
    if (ci == ~0U || tfd == ~0U || is == ~0U || (is & AHCI_FATAL)) return -1;
    if (ci & 1) return 0;
    if (tfd & 0xa9 || done > expected || (exact && done != expected)) return -1;
    return 1;
}
int AHCIReceivedTaskFile(unsigned ci, unsigned status, unsigned received,
    unsigned *taskfile)
{
    /* Non-queued ATA completion only. RegFIS:Entry posts the received FIS
     * before RegFIS:ClearCI clears CI and RegFIS:SetIntr sets DHRS (AHCI
     * 1.3.1 section 5.3.8). The caller reads CI before this coherent DMA word.
     * Errors, link events, ATAPI/PIO and missing FIS evidence use live TFD. */
    if (!taskfile || ci || !(status & AHCI_DHRS) ||
        (status & (AHCI_FATAL | AHCI_LINK_CHANGE)) ||
        (received & 0x0fffU) != 0x0034U) return 0;
    *taskfile=received >> 16;
    return 1;
}
int AHCIDeadlineExpired(AHCIU64 now, AHCIU64 start, unsigned seconds)
{
    /* OPENSTEP's interpolated clock can step backwards slightly between
     * threads. A negative interval is not an expired request. */
    return now >= start && now-start >= (AHCIU64)seconds*1000000000ULL;
}
unsigned AHCIDeadlineCounterBudget(const AHCIU64 cycles[3], const AHCIU64 nanos[3])
{
    unsigned i, low=~0U, high=0, rate;
    AHCIU64 measured;
    if (!cycles || !nanos) return 0;
    for(i=0;i<3;i++) {
        if (nanos[i] < 20000000ULL || nanos[i] > 5000000000ULL) return 0;
        measured=cycles[i]/(nanos[i]/1000ULL);
        if (measured < 100 || measured > 100000) return 0;
        rate=(unsigned)measured;
        if (rate < low) low=rate;
        if (rate > high) high=rate;
    }
    if (high-low > low/10) return 0;
    /* One millisecond, versus the minimum request timeout of one second.
     * Choose the lowest sample; calibration is only an early-age guard,
     * never a replacement clock for expiration. Reported execution time uses
     * a separately validated, more precise rate below. */
    return low*1000U;
}
int AHCIDeadlineCounterFresh(AHCIU64 start, AHCIU64 now, unsigned budget)
{
    return budget && budget <= 100000000U && start && now >= start && now-start < budget;
}
unsigned AHCIExecutionCounterRate(const AHCIU64 cycles[3], const AHCIU64 nanos[3])
{
    unsigned rate[3],i,j,swap;
    /* Validate the broad frequency/interval limits before multiplication.
     * At most 100 GHz over five seconds keeps cycles * 1000000 in 64 bits. */
    if (!AHCIDeadlineCounterBudget(cycles,nanos)) return 0;
    for(i=0;i<3;i++) rate[i]=(unsigned)(cycles[i]*1000000ULL/nanos[i]);
    for(i=0;i<2;i++) for(j=i+1;j<3;j++) if (rate[j]<rate[i]) {
        swap=rate[i]; rate[i]=rate[j]; rate[j]=swap;
    }
    /* Reporting uses the median frequency, with all three samples agreeing
     * within one percent. The deadline guard deliberately has looser bounds. */
    if (rate[0]<100000U || rate[2]>100000000U ||
        rate[2]-rate[0]>rate[0]/100) return 0;
    return rate[1];
}
int AHCIShortCounterElapsed(AHCIU64 start, AHCIU64 now, unsigned rate,
    AHCIU64 *elapsed)
{
    if (!elapsed || rate<100000U || !AHCIDeadlineCounterFresh(start,now,rate)) return 0;
    /* Restrict estimates to <1 ms; longer/preempted requests use the OS
     * timestamps. The bounded delta also makes this multiplication safe. */
    *elapsed=(now-start)*1000000ULL/rate;
    return 1;
}
int AHCIMapWindow(unsigned address, unsigned length, unsigned granule,
    unsigned *base, unsigned *size, unsigned *offset)
{
    AHCIU64 extent;
    if (!length || granule < 4096 || (granule & (granule-1)) ||
        address > ~0U-(length-1)) return 0;
    *offset=address&(granule-1); *base=address-*offset;
    extent=((AHCIU64)*offset+length+granule-1)&~((AHCIU64)granule-1);
    if (extent > ~0U) return 0;
    *size=(unsigned)extent;
    return 1;
}
