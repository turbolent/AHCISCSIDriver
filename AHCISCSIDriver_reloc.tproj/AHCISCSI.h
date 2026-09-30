#ifndef AHCI_SCSI_H
#define AHCI_SCSI_H
#define AHCI_DRIVER_VERSION "0.4"
#import <driverkit/IOSCSIController.h>
#import <driverkit/i386/IOPCIDirectDevice.h>
#import <driverkit/i386/IOPCIDeviceDescription.h>
#import <machkit/NXLock.h>
#import "PCIMSIClient.h"
#include "AHCICore.h"
#include "AHCIRegs.h"

/* Inline the CPU access only; preserve UC mapping, bounds checks,
 * compiler barriers, posted writes, and every explicit hardware readback. */
static __inline__ unsigned ahciMMIORead(vm_address_t base, unsigned length, unsigned offset)
{
    unsigned value;
    if (!base || length<4 || offset>length-4) return ~0U;
    /* Inlining removes the opaque Objective-C call's compiler barrier.
     * Keep DMA descriptor/FIS reads after the hardware completion read and
     * keep ordinary stores before it. UC x86 ordering is otherwise unchanged. */
    __asm__ volatile("" : : : "memory");
    value=*(volatile unsigned *)(base+offset);
    __asm__ volatile("" : : : "memory");
    return value;
}
static __inline__ void ahciMMIOPost(vm_address_t base, unsigned length,
    unsigned offset, unsigned value)
{
    if (base && length>=4 && offset<=length-4) {
        __asm__ volatile("" : : : "memory");
        *(volatile unsigned *)(base+offset)=value;
        __asm__ volatile("" : : : "memory");
    }
}

typedef struct AHCIRequest {
    struct AHCIRequest *next;
    NXConditionLock *done;
    unsigned target, length, capacity, seconds;
    AHCIB8 cdb[16];
    int read, noSense, operation;
    void *buffer;
    vm_task_t client;
    sc_status_t status;
    unsigned char scsiStatus;
    unsigned transferred;
    AHCIB8 sense[18];
    ns_time_t started;
    AHCIU64 startedCounter;
} AHCIRequest;

typedef struct AHCIPort {
    void *controlAllocation, *dataAllocation;
    AHCIB8 *control, *data;
    AHCIU32 controlPhysical, pages[32];
    AHCIDevice device;
    AHCIRequest *active;
    AHCIPlan plan;
    ns_time_t started;
    unsigned seconds, savedBytes;
    int online, phase, dmaOwned, completedIdle;
    AHCIB8 lastSense[18];
} AHCIPort;

@interface AHCISCSI : IOSCSIController
{
    id pciDescription, msiProvider;
    vm_address_t registers, registerMapping;
    IORange registerRange;
    unsigned pciBAR, vmPageSize, registerOffset;
    int hardwareOwned;
    unsigned registerLength, implemented, capabilities, portCount;
    unsigned msiOffset, msixOffset, msiControl, messageID;
    PCIMSIMessage msiMessage;
    int polling, promptRetry, vectorOwned, superStarted, registered;
    volatile int ready, stopped, workerRunning, workerExited, tickPending;
    unsigned activeCount;
    unsigned enabledPortInterrupts, prefetchedInterrupts;
    volatile unsigned deadlineCounterBudget;
    volatile unsigned executionCounterRate;
    port_t kernelPort;
    NXLock *queueLock;
    AHCIRequest *queueFirst, *queueLast;
    AHCIPort ports[32];
}
+ (BOOL)probe:description;
- initialize:description;
- free;
- (sc_status_t)executeRequest:(IOSCSIRequest *)request buffer:(void *)buffer client:(vm_task_t)client;
- (sc_status_t)executeSCSI3Request:(IOSCSI3Request *)request buffer:(void *)buffer client:(vm_task_t)client;
- (sc_status_t)resetSCSIBus;
- (unsigned)maxTransfer;
- (int)numberOfTargets;
- (void)getDMAAlignment:(IODMAAlignment *)alignment;
- (void)commandRequestOccurred;
- (void)timeoutOccurred;
- (void)otherOccurred:(int)message;
- (void)interruptOccurred;
- (void)worker;
- (void)submit:(AHCIRequest *)request;
- (void)dispatch;
- (void)initializeClockGuard;
- (int)requestIsFresh:(AHCIRequest *)request;
- (void)pollBriefly;
- (void)finish:(unsigned)port status:(sc_status_t)status bytes:(unsigned)bytes;
- (void)service:(int)checkTimeout;
- (void)failController;
- (int)copyClient:(AHCIRequest *)r data:(void *)data count:(unsigned)count toClient:(int)toClient;
@end

@interface AHCISCSI (Controller)
- (unsigned)read:(unsigned)offset;
- (void)write:(unsigned)offset value:(unsigned)value;
- (void)post:(unsigned)offset value:(unsigned)value;
- (unsigned)pread:(unsigned)p reg:(unsigned)reg;
- (void)pwrite:(unsigned)p reg:(unsigned)reg value:(unsigned)value;
- (int)pciRead:(unsigned)offset value:(unsigned *)value;
- (int)pciWrite:(unsigned)offset value:(unsigned)value mask:(unsigned)mask;
- (int)busMaster:(int)enabled;
- (int)configurePCI:description;
- (int)claimHardware;
- (int)initializeHardware;
- (int)configureMSI;
- (int)disableMSI;
- (void)logPort:(unsigned)p failure:(const char *)reason;
- (int)stopPort:(unsigned)p;
- (int)resetPort:(unsigned)p;
- (int)allocatePort:(unsigned)p;
- (int)identifyPort:(unsigned)p;
- (int)issue:(unsigned)p plan:(AHCIPlan *)plan;
- (int)waitRegister:(unsigned)offset mask:(unsigned)mask value:(unsigned)value milliseconds:(unsigned)ms;
- (int)quiesce;
@end
#endif
