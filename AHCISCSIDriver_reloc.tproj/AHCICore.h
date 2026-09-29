#ifndef AHCI_CORE_H
#define AHCI_CORE_H

/* Portable wire/translation code: also compiled by the native NeXT compiler. */
typedef unsigned char AHCIB8;
typedef unsigned short AHCIU16;
typedef unsigned int AHCIU32;
typedef unsigned long long AHCIU64;
#define AHCI_MAX_PORTS 32U
#define AHCI_MAX_TRANSFER 131072U
#define AHCI_PAGE_SIZE 4096U
#define AHCI_MAX_PRDS 33U
#define AHCI_SENSE_SIZE 18U

typedef struct AHCIHeader {
    AHCIU16 flags, prdtLength;
    volatile AHCIU32 transferred;
    AHCIU32 table, tableHigh, reserved[4];
} AHCIHeader;
typedef struct AHCIPRD {
    AHCIU32 address, addressHigh, reserved, count;
} AHCIPRD;
typedef struct AHCITable {
    AHCIB8 fis[64], packet[16], reserved[48];
    AHCIPRD prd[AHCI_MAX_PRDS];
} AHCITable;

typedef struct AHCIDevice {
    /* cache means enabled OR unknown; zero requires valid disabled state. */
    int packet, packetBytes, dma, lba48, flush, cache, removable;
    AHCIU32 blockSize;
    AHCIU64 blocks;
    char serial[21], model[41], firmware[9];
} AHCIDevice;

enum { AHCI_DONE, AHCI_ATA, AHCI_PACKET, AHCI_CHECK };
enum { AHCI_CONVERT_NONE, AHCI_CONVERT_MODE6 };
typedef struct AHCIPlan {
    int action, read, fua, stopAfterFlush, conversion;
    AHCIU32 bytes, responseBytes, allocation;
    AHCIU64 lba;
    AHCIU32 blocks;
    AHCIB8 fis[20], packet[16], sense[AHCI_SENSE_SIZE];
} AHCIPlan;

AHCIU32 AHCIReadBE(const AHCIB8 *p, unsigned n);
void AHCIWriteBE(AHCIB8 *p, AHCIU64 v, unsigned n);
void AHCISense(AHCIB8 *sense, unsigned key, unsigned asc, unsigned ascq);
int AHCIIdentify(const AHCIB8 *data, int packet, AHCIDevice *device);
unsigned AHCICDBLength(const AHCIB8 *cdb, unsigned supplied, unsigned available);
AHCIPlan AHCITranslate(const AHCIDevice *dev, const AHCIB8 *cdb,
    unsigned length, int read, AHCIB8 *buffer, unsigned capacity,
    const AHCIB8 *lastSense);
int AHCIPacketFinish(const AHCIPlan *plan, AHCIB8 *data,
    unsigned transferred, unsigned *resultBytes);
void AHCIATAError(AHCIB8 *sense, unsigned status, unsigned error, int write);
void AHCIFIS(AHCIB8 *fis, unsigned command);
int AHCIBuildTable(AHCIHeader *header, AHCITable *table,
    AHCIU32 tableAddress, const AHCIPlan *plan,
    const AHCIU32 *pages, unsigned pageCount, unsigned offset);
/* Returns -1 on transport failure, 0 while outstanding, 1 on completion. */
int AHCICompletion(unsigned ci, unsigned taskfile, unsigned status,
    unsigned transferred, unsigned expected, int exact);
int AHCIReceivedTaskFile(unsigned ci, unsigned status, unsigned received,
    unsigned *taskfile);
int AHCIDeadlineExpired(AHCIU64 now, AHCIU64 start, unsigned seconds);
unsigned AHCIDeadlineCounterBudget(const AHCIU64 cycles[3], const AHCIU64 nanos[3]);
int AHCIDeadlineCounterFresh(AHCIU64 start, AHCIU64 now, unsigned budget);
unsigned AHCIExecutionCounterRate(const AHCIU64 cycles[3], const AHCIU64 nanos[3]);
int AHCIShortCounterElapsed(AHCIU64 start, AHCIU64 now, unsigned rate,
    AHCIU64 *elapsed);
int AHCIMapWindow(unsigned address, unsigned length, unsigned granule,
    unsigned *base, unsigned *size, unsigned *offset);
#endif
