#import "AHCISCSI.h"
#import <driverkit/generalFuncs.h>
#import <driverkit/kernelDriver.h>
#import <driverkit/interruptMsg.h>
#import <kernserv/prototypes.h>
#import <mach/message.h>
#import <mach/mach_interface.h>
#import <mach/vm_param.h>
#import <mach/machine.h>
#import <string.h>

#define AHCI_TICK_MSG (IO_FIRST_UNRESERVED_INTERRUPT_MSG+2)
#define AHCI_MSI_MSG (IO_FIRST_UNRESERVED_INTERRUPT_MSG+0x100)

static void workerEntry(void *arg) { [(AHCISCSI *)arg worker]; IOExitThread(); }
static void signalDone(AHCIRequest *r)
{ [r->done lock]; [r->done unlockWith:1]; }
static int counterCPUIDAvailable(void)
{
    unsigned original, changed;
    __asm__ volatile("pushfl; popl %0; movl %0,%1; xorl $0x200000,%1; "
        "pushl %1; popfl; pushfl; popl %1; pushl %0; popfl"
        : "=&r"(original), "=&r"(changed) : : "memory","cc");
    return ((original^changed)&0x200000U) != 0;
}
static void counterCPUID(unsigned leaf, unsigned *a, unsigned *d)
{
    unsigned ax,b,c,dx;
    /* Keep the loadable object's i386 subtype; these instructions are
     * executed only after their capability checks, including on old CPUs. */
    /* The OPENSTEP compiler miscompiles two pointer-dereference asm outputs
     * at -O2. Keep register outputs local and copy them out explicitly. */
    __asm__ volatile(".byte 0x0f,0xa2" : "=a"(ax), "=b"(b), "=c"(c), "=d"(dx)
        : "0"(leaf), "2"(0) : "memory");
    *a=ax; *d=dx;
}
static AHCIU64 readDeadlineCounter(void)
{
    unsigned lo,hi;
    __asm__ volatile(".byte 0x0f,0x31" : "=a"(lo), "=d"(hi) : : "memory");
    return ((AHCIU64)hi<<32)|lo;
}
static int sendMessage(port_t port, int code)
{
    msg_header_t msg;
    memset(&msg,0,sizeof(msg)); msg.msg_simple=TRUE; msg.msg_size=sizeof(msg);
    msg.msg_type=MSG_TYPE_NORMAL; msg.msg_remote_port=port; msg.msg_id=code;
    return msg_send_from_kernel(&msg,MSG_OPTION_NONE,0) == KERN_SUCCESS;
}

@implementation AHCISCSI
+ (BOOL)probe:description
{
    AHCISCSI *instance = [self alloc];
    return instance && [instance initialize:description] != nil;
}
- initialize:description
{
    id table=[description configTable];
    const char *value;
    unsigned lun, physical;
    IORange *ranges;
    IOReturn mapResult;
    int automaticClock=1, automaticElapsed=1;
    polling=0; promptRetry=1; messageID=AHCI_MSI_MSG;
    vmPageSize=PAGE_SIZE;
    value=[table valueForStringKey:"Interrupt Mode"];
    if (value) {
        if (!strcmp(value,"Polling")) polling=1;
        else if (strcmp(value,"MSI")) { [table freeString:value]; [super free]; return nil; }
        [table freeString:value];
    }
    value=[table valueForStringKey:"MSI Prompt Retry"];
    if (value) {
        if (!strcmp(value,"NO")) promptRetry=0;
        else if (strcmp(value,"YES")) { [table freeString:value]; [super free]; return nil; }
        [table freeString:value];
    }
    value=[table valueForStringKey:"Deadline Fast Path"];
    if (value) {
        if (!strcmp(value,"Off")) automaticClock=0;
        else if (strcmp(value,"Auto")) { [table freeString:value]; [super free]; return nil; }
        [table freeString:value];
    }
    value=[table valueForStringKey:"Execution Time Clock"];
    if (value) {
        if (!strcmp(value,"OS")) automaticElapsed=0;
        else if (strcmp(value,"Auto")) { [table freeString:value]; [super free]; return nil; }
        [table freeString:value];
    }
    if (![self configurePCI:description]) {
        IOLog("AHCI: PCI resources or requested interrupt mode unavailable\n");
        [super free]; return nil;
    }
    queueLock=[[NXLock alloc] init];
    if (!queueLock || ![super initFromDeviceDescription:description]) {
        [queueLock free]; queueLock=nil; [super free]; return nil;
    }
    superStarted=1;
    ranges=[description memoryRangeList];
    IOLog("AHCI: resource %08x + %u bytes\n",ranges ? ranges[0].start : 0,ranges ? ranges[0].size : 0);
    kernelPort=IOConvertPort([self interruptPort],IO_KernelIOTask,IO_Kernel);
    if (!kernelPort) { IOLog("AHCI: cannot convert interrupt port\n"); goto failed; }
    mapResult=[self mapMemoryRange:0 to:&registerMapping findSpace:YES cache:IO_CacheOff];
    if (mapResult != IO_R_SUCCESS) { IOLog("AHCI: BAR5 mapping failed (%d)\n",mapResult); goto failed; }
    registers=registerMapping+registerOffset;
    physical=0;
    mapResult=IOPhysicalFromVirtual(IOVmTaskSelf(),registerMapping,&physical);
    IOLog("AHCI: mapping physical=%08x result=%d\n",physical,mapResult);
    if (mapResult != IO_R_SUCCESS || physical != registerRange.start) goto failed;
    if (automaticClock) [self initializeClockGuard];
    if (!automaticElapsed) executionCounterRate=0;
    if (![self claimHardware] || ![self initializeHardware]) goto failed;
    workerRunning=1;
    if (!IOForkThread(workerEntry,self)) { workerRunning=0; goto failed; }
    for (lun=0;lun<SCSI_NLUNS;lun++) [self reserveSCSI3Target:32 lun:lun forOwner:self];
    ready=1;
    IOLog("AHCISCSIDriver " AHCI_DRIVER_VERSION ": %u ports, %s, 128 KiB requests\n",portCount,polling ? "polling" : "PCIMSI MSI");
    registered=1;
    [self registerDevice];
    return self;
failed:
    ready=0; stopped=1;
    (void)[self quiesce];
    /* A DriverKit thread or provider message may still reference this object.
     * Keep this failed instance resident; do not unload Objective-C code. */
    IOLog("AHCI: initialization failed; inert instance retained until reboot\n");
    return self;
}
- free
{
    AHCIRequest r;
    if (!superStarted) return [super free];
    if (ready) {
        memset(&r,0,sizeof(r)); r.operation=2; [self submit:&r];
    }
    IOLog("AHCI: live unloading unsupported; instance retained until reboot\n");
    return self;
}
- (unsigned)maxTransfer { return AHCI_MAX_TRANSFER; }
- (int)numberOfTargets { return portCount; }
- (void)getDMAAlignment:(IODMAAlignment *)a
{ a->readStart=1; a->readLength=1; a->writeStart=1; a->writeLength=1; }

/* Both SDK request layouts have the same named fields, but different CDB and
 * target sizes. Never cast one layout to the other or copy beyond its CDB.
 * Short elapsed reports measure from the counter sample before the arrival
 * timestamp, including clock acquisition and all queue/transport latency.
 * They do not alter the OS-clock arrival time used for actual deadlines. */
#define EXECUTE_BODY(NO_SENSE) \
    AHCIRequest r; ns_time_t now; AHCIU64 counterNow, elapsed; unsigned rate; \
    if (!request) return SR_IOST_INVALID; \
    request->bytesTransferred=0; request->scsiStatus=STAT_GOOD; \
    request->driverStatus=SR_IOST_INVALID; request->totalTime=0; request->latentTime=0; \
    memset(&request->senseData,0,sizeof(request->senseData)); \
    memset(&r,0,sizeof(r)); \
    if (request->target >= portCount || request->lun) { request->driverStatus=SR_IOST_SELTO; return SR_IOST_SELTO; } \
    if (request->maxTransfer < 0 || request->maxTransfer > AHCI_MAX_TRANSFER || \
        (request->maxTransfer && (!buffer || (unsigned)buffer > ~0U-(unsigned)request->maxTransfer))) return SR_IOST_INVALID; \
    r.length=AHCICDBLength((const AHCIB8 *)&request->cdb,request->cdbLength,sizeof(request->cdb)); \
    if (!r.length) return SR_IOST_INVALID; \
    memcpy(r.cdb,&request->cdb,r.length); r.target=(unsigned)request->target; \
    r.capacity=request->maxTransfer; r.read=request->read; r.noSense=(NO_SENSE); \
    r.seconds=request->timeoutLength > 0 ? request->timeoutLength : 30; \
    r.buffer=buffer; r.client=client; \
    r.startedCounter=deadlineCounterBudget ? readDeadlineCounter() : 0; \
    IOGetTimestamp(&r.started); \
    [self submit:&r]; request->driverStatus=r.status; request->scsiStatus=r.scsiStatus; \
    request->bytesTransferred=r.transferred; memcpy(&request->senseData,r.sense,sizeof(r.sense)); \
    rate=executionCounterRate; counterNow=rate ? readDeadlineCounter() : 0; \
    if (rate && counterNow < r.startedCounter) { \
        executionCounterRate=deadlineCounterBudget=0; rate=0; \
    } \
    if (AHCIShortCounterElapsed(r.startedCounter,counterNow,rate,&elapsed)) \
        request->totalTime=elapsed; \
    else { IOGetTimestamp(&now); request->totalTime=now >= r.started ? now-r.started : 0; } \
    return r.status;

- (sc_status_t)executeRequest:(IOSCSIRequest *)request buffer:(void *)buffer client:(vm_task_t)client
{ EXECUTE_BODY(request->ignoreChkcond) }
- (sc_status_t)executeSCSI3Request:(IOSCSI3Request *)request buffer:(void *)buffer client:(vm_task_t)client
{ EXECUTE_BODY(0) }
#undef EXECUTE_BODY
- (sc_status_t)resetSCSIBus
{
    AHCIRequest r;
    memset(&r,0,sizeof(r)); r.operation=1; [self submit:&r]; return r.status;
}
- (void)submit:(AHCIRequest *)r
{
    r->status=SR_IOST_INT;
    r->done=[[NXConditionLock alloc] initWith:0];
    if (!r->done) return;
    [queueLock lock];
    if (!ready || stopped) { [queueLock unlock]; [r->done free]; return; }
    r->next=0;
    if (queueLast) queueLast->next=r; else queueFirst=r;
    queueLast=r;
    [queueLock unlock];
    /* Never hold queueLock across a Mach send: the receiving I/O thread
     * needs that lock to drain requests if the message port is full. */
    (void)sendMessage(kernelPort,IO_COMMAND_MSG);
    [r->done lockWhen:1]; [r->done unlock]; [r->done free];
}
- (int)copyClient:(AHCIRequest *)r data:(void *)data count:(unsigned)count toClient:(int)toClient
{
    unsigned address=(unsigned)r->buffer, offset=0, physical, n, base, size, displacement;
    vm_address_t mapped;
    if (count > r->capacity || (count && (!r->buffer || address > ~0U-count))) return 0;
    if (!count) return 1;
    /* Kernel buffers already have a valid IOTask mapping. Keep the DMA
     * staging buffer: this shortcut changes CPU copying, not DMA ownership. */
    if (r->client == IOVmTaskSelf()) {
        if (toClient) IOCopyMemory(data,r->buffer,count,4);
        else IOCopyMemory(r->buffer,data,count,4);
        return 1;
    }
    while (offset < count) {
        n=4096-(address&4095); if (n > count-offset) n=count-offset;
        if (IOPhysicalFromVirtual(r->client,address,&physical) != IO_R_SUCCESS ||
            !AHCIMapWindow(physical,n,vmPageSize,&base,&size,&displacement) ||
            IOMapPhysicalIntoIOTask(base,size,&mapped) != IO_R_SUCCESS) return 0;
        if (toClient) IOCopyMemory((AHCIB8 *)data+offset,(void *)(mapped+displacement),n,4);
        else IOCopyMemory((void *)(mapped+displacement),(AHCIB8 *)data+offset,n,4);
        IOUnmapPhysicalFromIOTask(mapped,size); offset+=n; address+=n;
    }
    return 1;
}
- (void)finish:(unsigned)p status:(sc_status_t)status bytes:(unsigned)bytes
{
    AHCIPort *port=&ports[p];
    AHCIRequest *r=port->active;
    if (!r) return;
    port->active=0; if (activeCount) activeCount--;
    r->status=status; r->transferred=0;
    if (status == SR_IOST_GOOD) {
        if (r->read && ![self copyClient:r data:port->data count:bytes toClient:1]) r->status=SR_IOST_MEMF;
        else r->transferred=bytes;
    }
    if (status == SR_IOST_CHKSV || status == SR_IOST_CHKSNV) r->scsiStatus=STAT_CHECK;
    else r->scsiStatus=STAT_GOOD;
    signalDone(r);
}
- (void)initializeClockGuard
{
    unsigned a,d,i;
    AHCIU64 first,last,cycles[3],nanos[3];
    ns_time_t begin,end;
    deadlineCounterBudget=executionCounterRate=0;
    /* A uniprocessor kernel cannot migrate a thread to an unsynchronized
     * TSC or bring another CPU online later. Unsupported CPUs keep the
     * original clock path and never execute RDTSC. */
    if (machine_info.max_cpus != 1 || machine_info.avail_cpus != 1) {
        IOLog("AHCI: deadline guard off: CPUs maximum=%d available=%d\n",
            machine_info.max_cpus,machine_info.avail_cpus); return;
    }
    if (!counterCPUIDAvailable()) {
        IOLog("AHCI: deadline guard off: no CPUID\n"); return;
    }
    counterCPUID(0,&a,&d); if (a < 1) {
        IOLog("AHCI: deadline guard off: no basic CPU features\n"); return;
    }
    counterCPUID(1,&a,&d); if (!(d & 0x10U)) {
        IOLog("AHCI: deadline guard off: no TSC\n"); return;
    }
    counterCPUID(0x80000000U,&a,&d); if (a < 0x80000007U) {
        IOLog("AHCI: deadline guard off: no extended CPU features\n"); return;
    }
    counterCPUID(0x80000007U,&a,&d); if (!(d & 0x100U)) {
        IOLog("AHCI: deadline guard off: TSC not invariant\n"); return;
    }
    for(i=0;i<3;i++) {
        IOGetTimestamp(&begin); first=readDeadlineCounter();
        /* The old PIT-interpolated OS clock can repeat/step by a tick.
         * Long intervals reduce that error before the strict 1% reporting
         * calibration test. This one-time sleep never runs on the I/O path. */
        IOSleep(2000);
        last=readDeadlineCounter(); IOGetTimestamp(&end);
        if (last <= first || end <= begin) {
            IOLog("AHCI: deadline guard off: calibration clock reversed\n"); return;
        }
        cycles[i]=last-first; nanos[i]=end-begin;
    }
    deadlineCounterBudget=AHCIDeadlineCounterBudget(cycles,nanos);
    executionCounterRate=AHCIExecutionCounterRate(cycles,nanos);
    IOLog("AHCI: short execution-time counter rate %u cycles/ms (0 means OS clock)\n",
        executionCounterRate);
    if (!executionCounterRate && deadlineCounterBudget) {
        for(i=0;i<3;i++)
            IOLog("AHCI: execution calibration %u: %u:%08x cycles, %u:%08x ns\n",i,
                (unsigned)(cycles[i]>>32),(unsigned)cycles[i],
                (unsigned)(nanos[i]>>32),(unsigned)nanos[i]);
    }
    if (deadlineCounterBudget)
        IOLog("AHCI: deadline early-age guard %u cycles (1 ms, invariant TSC, one CPU)\n",
            deadlineCounterBudget);
    else {
        IOLog("AHCI: deadline guard off: calibration rejected\n");
        for(i=0;i<3;i++)
            IOLog("AHCI: calibration %u: %u:%08x cycles, %u:%08x ns\n",i,
                (unsigned)(cycles[i]>>32),(unsigned)cycles[i],
                (unsigned)(nanos[i]>>32),(unsigned)nanos[i]);
    }
}
- (int)requestIsFresh:(AHCIRequest *)r
{
    unsigned budget=deadlineCounterBudget;
    AHCIU64 now;
    if (!budget || !r->startedCounter || !r->seconds) return 0;
    now=readDeadlineCounter();
    if (now < r->startedCounter) {
        deadlineCounterBudget=executionCounterRate=0;
        IOLog("AHCI: backward TSC; counter timing disabled\n");
        return 0;
    }
    /* The counter sample precedes the real arrival timestamp, including any
     * intervening preemption. A calibrated age below 1 ms is far below the
     * minimum one-second timeout. Older requests and all watchdog passes
     * still use IOGetTimestamp; the counter never declares expiration. */
    return AHCIDeadlineCounterFresh(r->startedCounter,now,budget);
}
- (void)dispatch
{
    AHCIRequest *r, *previous, *next;
    unsigned p;
    ns_time_t now;
    for (;;) {
        [queueLock lock];
        previous=0; r=queueFirst;
        while (r && !r->operation && r->target < portCount && ports[r->target].active) {
            previous=r; r=r->next;
        }
        if (!r) { [queueLock unlock]; break; }
        next=r->next;
        if (previous) previous->next=next; else queueFirst=next;
        if (queueLast == r) queueLast=previous;
        [queueLock unlock];
        if (!ready || stopped) { r->status=SR_IOST_INT; signalDone(r); continue; }
        if (r->operation) {
            if (r->operation == 2) { [self failController]; r->status=SR_IOST_GOOD; }
            else {
                r->status=SR_IOST_GOOD;
                for (p=0;p<portCount;p++) if (ports[p].online) {
                    if (![self stopPort:p]) { [self failController]; r->status=SR_IOST_HW; break; }
                    [self finish:p status:SR_IOST_RESET bytes:0];
                    if (![self resetPort:p] || ![self identifyPort:p]) {
                        ports[p].online=0; r->status=SR_IOST_HW;
                        if (![self stopPort:p]) [self failController];
                    }
                }
            }
            signalDone(r); continue;
        }
        p=r->target;
        if (!ports[p].online) { r->status=SR_IOST_SELTO; signalDone(r); continue; }
        if (![self requestIsFresh:r]) {
          IOGetTimestamp(&now);
          if (AHCIDeadlineExpired(now,r->started,r->seconds)) {
            IOLog("AHCI: queued port %u op %02x expired after %u ms (limit %u s)\n",
                p,r->cdb[0],(unsigned)((now-r->started)/1000000ULL),r->seconds);
            r->status=SR_IOST_IOTO; signalDone(r); continue;
          }
        }
        ports[p].active=r; activeCount++;
        ports[p].phase=0; ports[p].savedBytes=0;
        ports[p].started=r->started; ports[p].seconds=r->seconds;
        /* ATA reads require exact DMA completion before any bytes are copied
         * out, and locally generated responses initialize all returned bytes.
         * Packet commands retain clearing, including their conversion padding. */
        if (ports[p].device.packet) memset(ports[p].data,0,AHCI_MAX_TRANSFER);
        if (!r->read && ![self copyClient:r data:ports[p].data count:r->capacity toClient:0]) {
            [self finish:p status:SR_IOST_MEMF bytes:0]; continue;
        }
        ports[p].plan=AHCITranslate(&ports[p].device,r->cdb,r->length,r->read,
            ports[p].data,r->capacity,ports[p].lastSense);
        if (ports[p].plan.action == AHCI_CHECK) {
            memcpy(ports[p].lastSense,ports[p].plan.sense,18);
            memcpy(r->sense,ports[p].lastSense,18);
            /* Sense is already known; no autosense command is needed. An
             * ATAPI device cannot report a rejection it never received. */
            [self finish:p status:SR_IOST_CHKSV bytes:0];
        } else if (ports[p].plan.action == AHCI_DONE) {
            if (r->cdb[0] == 3) AHCISense(ports[p].lastSense,0,0,0);
            [self finish:p status:SR_IOST_GOOD bytes:ports[p].plan.responseBytes];
        } else if (![self issue:p plan:&ports[p].plan]) {
            if (![self stopPort:p]) [self failController];
            else {
                [self finish:p status:SR_IOST_HW bytes:0];
                if (![self resetPort:p] || ![self identifyPort:p]) ports[p].online=0;
            }
        }
    }
}
- (void)failController
{
    unsigned p;
    AHCIRequest *r;
    [queueLock lock]; ready=0; stopped=1; workerRunning=0; [queueLock unlock];
    if (![self quiesce]) IOLog("AHCI: containment incomplete; DMA allocations and instance retained\n");
    /* Only driver-owned pages are ever DMA targets. Failed caller requests can
     * be released even when a broken HBA requires permanent page quarantine. */
    for (p=0;p<portCount;p++) { ports[p].online=0; [self finish:p status:SR_IOST_HW bytes:0]; }
    for (;;) {
        [queueLock lock]; r=queueFirst;
        if (r) { queueFirst=r->next; if (!queueFirst) queueLast=0; }
        [queueLock unlock];
        if (!r) break;
        r->status=SR_IOST_HW; signalDone(r);
    }
}
- (void)service:(int)watchdog
{
    unsigned p, is, tfd, ci, bytes, cmd, global;
    int result, timedOut, deviceError, noReset, receivedTaskfile, linkReady, haveTime=0;
    ns_time_t now;
    AHCIPlan continuation;
    if (!ready || stopped) return;
    if (watchdog) { IOGetTimestamp(&now); haveTime=1; }
    global=prefetchedInterrupts; prefetchedInterrupts=0;
    if (watchdog || polling || !global || global != enabledPortInterrupts ||
        (global & (global-1))) global=ahciMMIORead(registers,registerLength,AHCI_IS);
    if (global == ~0U) { [self failController]; return; }
    for (p=0;p<portCount && ready;p++) {
        AHCIPort *port=&ports[p];
        AHCIRequest *r=port->active;
        if (!r) {
            if (global & (1U<<p)) {
                is=ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_PIS);
                if (is == ~0U) { [self failController]; return; }
                if (is) port->completedIdle=0;
                [self pwrite:p reg:AHCI_PIS value:is];
                [self write:AHCI_IS value:(1U<<p)];
                if (is && (ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_SSTS)&15) != 3) port->online=0;
            }
            continue;
        }
        if (!watchdog && [self requestIsFresh:r]) timedOut=0;
        else {
            if (!haveTime) { IOGetTimestamp(&now); haveTime=1; }
            timedOut=AHCIDeadlineExpired(now,port->started,port->seconds);
        }
        if (watchdog && !timedOut) continue;
        /* Only consume causes present in the MSI snapshot. A different port
         * may complete while this loop is running and needs its own wakeup. */
        if (!watchdog && !polling && !(global & (1U<<p))) continue;
        is=ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_PIS);
        if (is == ~0U) { [self failController]; return; }
        /* AHCI 5.5.3: snapshot the causes, acknowledge PxIS then IS, and
         * inspect CI. That status read also drains both ordered W1C writes.
         * A completion arriving after the acknowledgement remains pending;
         * never acknowledge the old snapshot again after reading CI. */
        ahciMMIOPost(registers,registerLength,AHCI_PORT(p)+AHCI_PIS,is);
        ahciMMIOPost(registers,registerLength,AHCI_IS,(1U<<p));
        receivedTaskfile=0;
        if (!port->device.packet && port->plan.action == AHCI_ATA) {
            ci=ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_CI);
            receivedTaskfile=AHCIReceivedTaskFile(ci,is,
                *(volatile unsigned *)(port->control+1024+0x40),&tfd);
            if (!receivedTaskfile)
                tfd=ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_TFD);
        } else {
            tfd=ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_TFD); ci=ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_CI);
        }
        bytes=((AHCIHeader *)port->control)->transferred;
        result=AHCICompletion(ci,tfd,is,bytes,port->plan.bytes,port->plan.action == AHCI_ATA);
        /* A clean ATA D2H completion with exact DMA length proves this
         * command completed on the link. PRCS/PCS are latched link-change
         * evidence and already exclude the received-taskfile fast path.
         * Later removal does not invalidate completed data; it remains a
         * pending link interrupt. Read live SSTS for every other case. */
        linkReady=1;
        if (!receivedTaskfile || result != 1 || timedOut)
            linkReady=(ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_SSTS)&15) == 3;
        if (!result && !timedOut && linkReady) {
            /* The intermediate notification is acknowledged; a later
             * completion can now generate another MSI. */
            continue;
        }
        if (timedOut || !linkReady) result=-1;
        if (result < 0) {
            deviceError=!timedOut && (is & AHCI_TFES) &&
                !(is & (AHCI_FATAL & ~AHCI_TFES)) && !(tfd & 0x88);
            noReset=deviceError && port->device.packet && port->phase != 3;
            IOLog("AHCI: port %u op %02x failed IS=%08x TFD=%08x CI=%08x bytes=%u%s\n",
                p,r->cdb[0],is,tfd,ci,bytes,timedOut ? " timeout" : "");
            if (![self stopPort:p]) { [self failController]; return; }
            [self pwrite:p reg:AHCI_PIS value:~0U];
            [self write:AHCI_IS value:(1U<<p)];
            if (noReset) {
                [self pwrite:p reg:AHCI_SERR value:~0U];
                [self pwrite:p reg:AHCI_PIS value:~0U];
                cmd=ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_CMD);
                [self pwrite:p reg:AHCI_CMD value:(cmd|AHCI_FRE|AHCI_ST)];
                port->dmaOwned=1;
                [self pwrite:p reg:AHCI_IE value:(polling ? 0 : AHCI_IRQ_MASK)];
                if (r->noSense || r->cdb[0] == 3) {
                    [self finish:p status:SR_IOST_CHKSNV bytes:0]; continue;
                }
                memset(&continuation,0,sizeof(continuation));
                continuation.action=AHCI_PACKET; continuation.read=1; continuation.bytes=18;
                continuation.packet[0]=3; continuation.packet[4]=18;
                AHCIFIS(continuation.fis,0xa0); continuation.fis[5]=18;
                port->phase=3; port->plan=continuation;
                memset(port->data,0,18);
                if ([self issue:p plan:&port->plan]) continue;
            }
            if (deviceError && !port->device.packet) {
                AHCIATAError(r->sense,tfd,tfd>>8,!r->read);
                memcpy(port->lastSense,r->sense,18);
            }
            [self finish:p status:(timedOut ? SR_IOST_IOTO :
                deviceError && !port->device.packet ? (r->noSense ? SR_IOST_CHKSNV : SR_IOST_CHKSV) : SR_IOST_HW) bytes:0];
            if (![self resetPort:p] || ![self identifyPort:p]) {
                port->online=0;
                if (![self stopPort:p]) { [self failController]; return; }
            }
            continue;
        }
        /* The old aggregate cause was drained before CI was sampled, so
         * follow-up commands cannot complete under its acknowledgement. */
        port->completedIdle=(ci == 0 && !(is & AHCI_LINK_CHANGE));
        if (port->phase == 3) {
            if (bytes >= 14 && ((port->data[0]&0x7f) == 0x70 || (port->data[0]&0x7f) == 0x71)) {
                memcpy(r->sense,port->data,18); memcpy(port->lastSense,r->sense,18);
                [self finish:p status:SR_IOST_CHKSV bytes:0];
            } else if (bytes >= 8 && ((port->data[0]&0x7e) == 0x72)) {
                AHCISense(r->sense,port->data[1]&15,port->data[2],port->data[3]);
                memcpy(port->lastSense,r->sense,18); [self finish:p status:SR_IOST_CHKSV bytes:0];
            } else [self finish:p status:SR_IOST_CHKSNV bytes:0];
            continue;
        }
        if (port->plan.fua || port->plan.stopAfterFlush) {
            memset(&continuation,0,sizeof(continuation)); continuation.action=AHCI_ATA;
            if (port->plan.fua) {
                port->savedBytes=bytes; AHCIFIS(continuation.fis,port->device.lba48 ? 0xea : 0xe7);
            } else AHCIFIS(continuation.fis,0xe0);
            port->phase=1; port->plan=continuation;
            if (![self issue:p plan:&port->plan]) { [self failController]; return; }
            continue;
        }
        if (port->phase == 1) bytes=port->savedBytes;
        else if (port->plan.action == AHCI_PACKET && !AHCIPacketFinish(&port->plan,port->data,bytes,&bytes)) {
            [self finish:p status:SR_IOST_HW bytes:0]; continue;
        }
        [self finish:p status:SR_IOST_GOOD bytes:bytes];
    }
}
- (void)pollBriefly
{
    ns_time_t start, now;
    unsigned pass, p, ci;
    int completed;
    if (!polling || !activeCount || !ready) return;
    IOGetTimestamp(&start);
    /* Fast completions should not wait for the next scheduler tick. Run only
     * on the serialized I/O thread, with interrupts enabled and a bounded
     * budget. Slow commands still use the existing worker and timeout path.
     * The iteration limit also bounds the loop if the clock steps backward. */
    for (pass=0;pass<64 && ready && activeCount;pass++) {
        /* CI is only a hint here, never proof of successful completion.
         * Avoid repeatedly reading/acknowledging every status register while
         * the HBA is busy. service still checks taskfile, errors, byte count,
         * link state and deadline before completing anything. Fatal errors
         * with CI still set remain serviced by the regular timer path. */
        completed=0;
        for (p=0;p<portCount;p++) if (ports[p].active) {
            ci=ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_CI);
            if (!(ci&1) || ci == ~0U) { completed=1; break; }
        }
        if (completed) { [self service:0]; [self dispatch]; }
        IOGetTimestamp(&now);
        if (now < start || now-start >= 250000ULL) break;
        if (activeCount) IODelay(2);
    }
}
- (void)commandRequestOccurred { [self dispatch]; [self pollBriefly]; }
- (void)timeoutOccurred { [self service:1]; [self dispatch]; [self pollBriefly]; }
- (void)interruptOccurred { /* No DriverKit legacy IRQ is registered. */ }
- (void)otherOccurred:(int)msg
{
    unsigned pass, pending=0, flags;
    IOReturn result=IO_R_SUCCESS;
    if (msg == AHCI_TICK_MSG) {
        [queueLock lock]; tickPending=0; [queueLock unlock];
        [self service:!polling]; [self dispatch]; [self pollBriefly]; return;
    }
    if (msg != (int)messageID || !vectorOwned || !ready) return;
    for (pass=0;pass<8 && ready;pass++) {
        [self service:0]; [self dispatch];
        if (!ready) return;
        __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory","cc");
        if (pass == 7) result=[msiProvider acknowledgeMSIVector:msiMessage.vector owner:self];
        else result=[msiProvider consumeAcknowledgedMSIVector:msiMessage.vector owner:self pending:&pending];
        __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory","cc");
        if (result != IO_R_SUCCESS) { [self failController]; return; }
        if (pass == 7 || !pending) break;
    }
}
- (void)worker
{
    int send;
    while (workerRunning) {
        send=0;
        [queueLock lock];
        if (ready && !tickPending) { tickPending=1; send=1; }
        [queueLock unlock];
        if (send && !sendMessage(kernelPort,AHCI_TICK_MSG)) {
            [queueLock lock]; tickPending=0; [queueLock unlock];
        }
        IOSleep(polling ? (activeCount ? 1 : 10) : 100);
    }
    workerExited=1;
}
@end
