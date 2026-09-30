#import "AHCISCSI.h"
#import <driverkit/generalFuncs.h>
#import <driverkit/kernelDriver.h>
#import <driverkit/i386/PCI.h>
#import <mach/mach_interface.h>
#import <mach/mach_traps.h>
#import <string.h>

@implementation AHCISCSI (Controller)
- (unsigned)read:(unsigned)o
{
    return ahciMMIORead(registers,registerLength,o);
}
- (void)write:(unsigned)o value:(unsigned)v
{
    if (registers && o <= registerLength-4) {
        *(volatile unsigned *)(registers+o) = v;
        __asm__ volatile("" : : : "memory");
        (void)*(volatile unsigned *)(registers+o);
    }
}
- (void)post:(unsigned)o value:(unsigned)v
{
    /* Command-path writes target the same UC-mapped PCI function from the
     * serialized I/O thread. A subsequent synchronous write/read drains the
     * ordered batch. MSI acknowledgement explicitly drains before releasing
     * the provider gate. MSI issue drains through the global status register;
     * polling drains its doorbell on its next status read.
     * Ownership/reset/containment keep the synchronous accessor. */
    ahciMMIOPost(registers,registerLength,o,v);
}
- (unsigned)pread:(unsigned)p reg:(unsigned)r
{ return ahciMMIORead(registers,registerLength,AHCI_PORT(p)+r); }
- (void)pwrite:(unsigned)p reg:(unsigned)r value:(unsigned)v
{
    [self write:AHCI_PORT(p)+r value:v];
    if (r == AHCI_IE) {
        prefetchedInterrupts=0;
        if (v) enabledPortInterrupts |= 1U<<p;
        else enabledPortInterrupts &= ~(1U<<p);
    }
}
- (int)pciRead:(unsigned)o value:(unsigned *)v
{
    unsigned long data;
    IOReturn result;
    result=[IODirectDevice getPCIConfigData:&data atRegister:o withDeviceDescription:pciDescription];
    if (result != IO_R_SUCCESS) {
        IOLog("AHCI: PCI read %02x failed (%d)\n",o,result);
        return 0;
    }
    *v = (unsigned)data;
    if (data == ~0UL) {
        IOLog("AHCI: PCI read %02x returned all ones\n",o);
        return 0;
    }
    return 1;
}
- (int)pciWrite:(unsigned)o value:(unsigned)v mask:(unsigned)mask
{
    unsigned got;
    IOReturn result;
    result=[IODirectDevice setPCIConfigData:v atRegister:o withDeviceDescription:pciDescription];
    if (result != IO_R_SUCCESS) {
        IOLog("AHCI: PCI write %02x=%08x failed (%d)\n",o,v,result);
        return 0;
    }
    if (![self pciRead:o value:&got]) return 0;
    if ((got & mask) != (v & mask)) {
        IOLog("AHCI: PCI write %02x wanted=%08x got=%08x mask=%08x\n",o,v,got,mask);
        return 0;
    }
    return 1;
}
- (int)busMaster:(int)enabled
{
    unsigned command;
    if (![self pciRead:4 value:&command]) return 0;
    command = ((command & 0xffffU) | 0x402) & ~4U;
    if (enabled) command |= 4;
    return [self pciWrite:4 value:command mask:0x406];
}
- (int)configurePCI:description
{
    IOPCIConfigSpace config;
    unsigned bar, command, next, n=0, cap, seen[8];
    IOReturn result;
    id table=[description configTable];
    const char *location=[table valueForStringKey:"Location"];
    pciDescription = description;
    IOLog("AHCI: PCI probe Location=\"%s\", interrupt mode=%s\n",
        location ? location : "",polling ? "Polling" : "MSI");
    if (location) [table freeString:location];
    memset(&config,0,sizeof(config)); memset(seen,0,sizeof(seen));
    result=[IODirectDevice getPCIConfigSpace:&config withDeviceDescription:description];
    if (result != IO_R_SUCCESS) {
        IOLog("AHCI: cannot read PCI configuration (%d); check Auto Detect IDs, Location and PCIBus\n",result);
        return 0;
    }
    if (config.ClassCode != 0x010601) {
        IOLog("AHCI: PCI %04x:%04x class=%06x is not AHCI 010601; check Location and firmware SATA mode\n",
            config.VendorID,config.DeviceID,config.ClassCode);
        return 0;
    }
    if ((config.HeaderType & 0x7f) != 0) {
        IOLog("AHCI: unsupported PCI header type %02x\n",config.HeaderType);
        return 0;
    }
    bar = config.BaseAddress[5];
    IOLog("AHCI: PCI class=%06x BAR5=%08x command=%04x\n",config.ClassCode,bar,config.Command);
    if (!bar || (bar & 7) || (config.BaseAddress[4] & 7) == 4) {
        IOLog("AHCI: BAR5 is not an assigned 32-bit memory BAR (BAR4=%08x BAR5=%08x)\n",
            config.BaseAddress[4],bar);
        return 0;
    }
    if (![self pciRead:4 value:&command]) return 0;
    /* First claim only the generic registers, whose extent AHCI guarantees.
     * Keep the resource description alive for the lifetime of the device. */
    if (![self pciWrite:4 value:((command & 0xffff)|2) mask:0xffff]) return 0;
    pciBAR=bar; registerLength=0x100;
    /* Mach VM pages are 8 KiB on OPENSTEP/i386, independently of AHCI's
     * descriptor segment size. The mapping base may precede BAR5. */
    if (!AHCIMapWindow(bar&~15U,registerLength,vmPageSize,
        &registerRange.start,&registerRange.size,&registerOffset)) {
        IOLog("AHCI: cannot form BAR5 mapping (BAR5=%08x length=%u page=%u)\n",
            bar,registerLength,vmPageSize);
        return 0;
    }
    result=[description setMemoryRangeList:&registerRange num:1];
    if (result != IO_R_SUCCESS) {
        IOLog("AHCI: cannot claim PCI memory %08x + %u bytes (%d)\n",
            registerRange.start,registerRange.size,result);
        return 0;
    }
    result=[description setInterruptList:(unsigned *)0 num:0];
    if (result != IO_R_SUCCESS) {
        IOLog("AHCI: cannot clear legacy IRQ resources (%d)\n",result);
        return 0;
    }
    if ([description numInterrupts]) {
        IOLog("AHCI: legacy IRQ resources remain after clearing\n");
        return 0;
    }
    if (!(config.Status & 0x10)) {
        if (!polling) IOLog("AHCI: controller has no PCI capability list; MSI mode requires controller MSI support\n");
        return polling;
    }
    if (![self pciRead:0x34 value:&next]) return 0;
    next &= 255;
    while (next) {
        if (next < 0x40 || next > 0xfc || (next & 3) || n++ >= 48 || (seen[next/32] & (1U << (next%32)))) {
            IOLog("AHCI: invalid or cyclic PCI capability list at %02x\n",next);
            return 0;
        }
        seen[next/32] |= 1U << (next%32);
        if (![self pciRead:next value:&cap]) return 0;
        if ((cap & 255) == 5) {
            if (msiOffset) { IOLog("AHCI: duplicate MSI capability at %02x\n",next); return 0; }
            msiOffset = next; msiControl = cap >> 16;
        }
        if ((cap & 255) == 0x11) {
            if (msixOffset) { IOLog("AHCI: duplicate MSI-X capability at %02x\n",next); return 0; }
            msixOffset = next;
        }
        next = (cap >> 8) & 255;
    }
    if (msiOffset && msiOffset + ((msiControl & 0x80) ? 16 : 12) +
        ((msiControl & 0x100) ? 8 : 0) > 256) {
        IOLog("AHCI: MSI capability at %02x extends beyond PCI configuration space\n",msiOffset);
        return 0;
    }
    if (!polling && !msiOffset)
        IOLog("AHCI: controller has no MSI capability; PCIMSI cannot supply missing controller MSI support\n");
    return polling || msiOffset != 0;
}
- (int)claimHardware
{
    unsigned version, handoff, command, mask=0, length;
    int sized;
    version=ahciMMIORead(registers,registerLength,AHCI_VS);
    IOLog("AHCI: ownership mapping %08x, VS=%08x\n",registers,version);
    if (version < 0x10000 || version > 0x10301) return 0;
    if (version >= 0x10200 && (ahciMMIORead(registers,registerLength,AHCI_CAP2)&1)) {
        handoff=ahciMMIORead(registers,registerLength,AHCI_BOHC);
        if (handoff == ~0U) return 0;
        /* Preserve SOOE: setting OOS must notify BIOS through its SMI
         * handler before it can release BOS. Do not acknowledge OOC here. */
        [self write:AHCI_BOHC value:((handoff|2)&~8U)];
        if (![self waitRegister:AHCI_BOHC mask:0x11 value:0 milliseconds:2200]) return 0;
    }
    hardwareOwned=1;
    if (![self disableMSI] || ![self busMaster:0]) return 0;
    [self unmapMemoryRange:0 from:registerMapping]; registers=0; registerMapping=0;
    /* Firmware has relinquished ownership. Probe with decoding and DMA off. */
    if (![self pciRead:4 value:&command] ||
        ![self pciWrite:4 value:((command&0xffff)&~6U) mask:6]) return 0;
    sized=[IODirectDevice setPCIConfigData:~0U atRegister:0x24 withDeviceDescription:pciDescription] == IO_R_SUCCESS;
    if (sized) sized=[self pciRead:0x24 value:&mask];
    if (![self pciWrite:0x24 value:pciBAR mask:~0U]) return 0;
    length=~(mask&~15U)+1;
    if (!sized || length < 0x180 || (length&(length-1)) ||
        (pciBAR&~15U) > ~0U-(length-1)) return 0;
    if (![self busMaster:0]) return 0;
    registerLength=length < AHCI_PORT(32) ? length : AHCI_PORT(32);
    if (!AHCIMapWindow(pciBAR&~15U,registerLength,vmPageSize,
        &registerRange.start,&registerRange.size,&registerOffset)) return 0;
    if ([pciDescription setMemoryRangeList:&registerRange num:1] != IO_R_SUCCESS) return 0;
    if ([self mapMemoryRange:0 to:&registerMapping findSpace:YES cache:IO_CacheOff] != IO_R_SUCCESS) return 0;
    registers=registerMapping+registerOffset;
    IOLog("AHCI: BAR5 mapped at %08x, %u bytes\n",registers,registerLength);
    return 1;
}
- (int)disableMSI
{
    unsigned v;
    int ok = 1;
    if (msiOffset) {
        if (![self pciRead:msiOffset value:&v] ||
            ![self pciWrite:msiOffset value:(v & ~0x00710000U) mask:0x00710000U]) ok = 0;
    }
    if (msixOffset) {
        if (![self pciRead:msixOffset value:&v] ||
            ![self pciWrite:msixOffset value:((v | 0x40000000U) & ~0x80000000U) mask:0xc0000000U]) ok = 0;
    }
    return ok;
}
- (int)configureMSI
{
    unsigned v, dataOffset;
    IOReturn result;
    if (polling) return [self disableMSI];
    if (!msiOffset || IOGetObjectForDeviceName("PCIMSI0",&msiProvider) != IO_R_SUCCESS ||
        !msiProvider || [msiProvider msiInterfaceVersion] != PCIMSI_INTERFACE_VERSION ||
        ![msiProvider isMSIServiceActive]) return 0;
    result = [msiProvider allocateAcknowledgedMSIVectorFor:self interruptPort:[self interruptPort]
        messageID:messageID message:&msiMessage];
    if (result != IO_R_SUCCESS) return 0;
    vectorOwned = 1;
    if (msiMessage.vector < PCIMSI_VECTOR_FIRST || msiMessage.vector > PCIMSI_VECTOR_LAST ||
        msiMessage.addressHigh || (msiMessage.addressLow & 0xfff00fffU) != 0xfee00000U ||
        msiMessage.data != msiMessage.vector) return 0;
    if (promptRetry) {
        result = [msiProvider enableMSIPromptRetry:msiMessage.vector owner:self];
        if (result != IO_R_SUCCESS && !((result == IO_R_BUSY || result == IO_R_RESOURCE ||
            result == IO_R_UNSUPPORTED) && [msiProvider isMSIServiceActive])) return 0;
        IOLog("AHCI: MSI prompt retry result %d\n",result);
    }
    if (![self pciWrite:msiOffset+4 value:msiMessage.addressLow mask:~0U]) return 0;
    dataOffset = msiOffset+8;
    if (msiControl & 0x80) {
        if (![self pciWrite:dataOffset value:0 mask:~0U]) return 0;
        dataOffset += 4;
    }
    if (![self pciRead:dataOffset value:&v] ||
        ![self pciWrite:dataOffset value:((v & 0xffff0000U) | msiMessage.data) mask:0xffff]) return 0;
    if (msiControl & 0x100) {
        if (![self pciRead:dataOffset+4 value:&v] ||
            ![self pciWrite:dataOffset+4 value:(v & ~1U) mask:1]) return 0;
    }
    if (![self pciRead:msiOffset value:&v] ||
        ![self pciWrite:msiOffset value:((v & ~0x00700000U) | 0x00010000U) mask:0x00710000U]) return 0;
    return 1;
}
- (int)waitRegister:(unsigned)o mask:(unsigned)m value:(unsigned)v milliseconds:(unsigned)ms
{
    unsigned got, attempts=0;
    ns_time_t start,now;
    IOGetTimestamp(&start);
    for (;;) {
        got = ahciMMIORead(registers,registerLength,o);
        if (got == ~0U) return 0;
        if ((got & m) == v) return 1;
        IOGetTimestamp(&now);
        if ((now >= start && now-start >= (AHCIU64)ms*1000000ULL) || attempts++ >= ms) return 0;
        IOSleep(1);
    }
}
- (void)logPort:(unsigned)p failure:(const char *)reason
{
    /* Error-only snapshots: preserve the registers for containment/recovery. */
    IOLog("AHCI: port %u %s: CMD=%08x TFD=%08x SSTS=%08x\n",p,reason,
        [self pread:p reg:AHCI_CMD],[self pread:p reg:AHCI_TFD],
        [self pread:p reg:AHCI_SSTS]);
    IOLog("AHCI: port %u CI=%08x IS=%08x SERR=%08x\n",p,
        [self pread:p reg:AHCI_CI],[self pread:p reg:AHCI_PIS],
        [self pread:p reg:AHCI_SERR]);
}
- (int)stopPort:(unsigned)p
{
    unsigned cmd = ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_CMD);
    ports[p].completedIdle=0;
    if (cmd == ~0U) { [self logPort:p failure:"stop: inaccessible registers"]; return 0; }
    [self pwrite:p reg:AHCI_IE value:0];
    [self pwrite:p reg:AHCI_CMD value:(cmd & ~AHCI_ST)];
    if (![self waitRegister:AHCI_PORT(p)+AHCI_CMD mask:AHCI_CR value:0 milliseconds:500]) {
        [self logPort:p failure:"stop: command engine did not stop"]; return 0;
    }
    cmd = ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_CMD);
    [self pwrite:p reg:AHCI_CMD value:(cmd & ~AHCI_FRE)];
    if (![self waitRegister:AHCI_PORT(p)+AHCI_CMD mask:AHCI_FR value:0 milliseconds:500]) {
        [self logPort:p failure:"stop: FIS receiver did not stop"]; return 0;
    }
    ports[p].dmaOwned = 0;
    return 1;
}
- (int)allocatePort:(unsigned)p
{
    AHCIPort *port = &ports[p];
    unsigned i, phys;
    if (port->control) return 1;
    port->controlAllocation = IOMalloc(8192);
    port->dataAllocation = IOMalloc(AHCI_MAX_TRANSFER+4096);
    if (!port->controlAllocation || !port->dataAllocation) return 0;
    port->control = (AHCIB8 *)(((unsigned)port->controlAllocation+4095)&~4095U);
    port->data = (AHCIB8 *)(((unsigned)port->dataAllocation+4095)&~4095U);
    memset(port->control,0,4096); memset(port->data,0,AHCI_MAX_TRANSFER);
    if (IOPhysicalFromVirtual(IOVmTaskSelf(),(vm_address_t)port->control,&phys) != IO_R_SUCCESS || (phys&4095)) return 0;
    port->controlPhysical = phys;
    for (i=0;i<32;i++) {
        if (IOPhysicalFromVirtual(IOVmTaskSelf(),(vm_address_t)port->data+i*4096,&phys) != IO_R_SUCCESS || (phys&4095)) return 0;
        port->pages[i] = phys;
    }
    AHCISense(port->lastSense,0,0,0);
    return 1;
}
- (int)resetPort:(unsigned)p
{
    unsigned cmd, ctl, sig;
    AHCIPort *port = &ports[p];
    if (![self stopPort:p] || !port->control) return 0;
    [self pwrite:p reg:AHCI_CLB value:port->controlPhysical];
    [self pwrite:p reg:AHCI_CLBU value:0];
    [self pwrite:p reg:AHCI_FB value:port->controlPhysical+1024];
    [self pwrite:p reg:AHCI_FBU value:0];
    cmd = ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_CMD);
    /* Disable link power management, power and spin up a cold port. */
    cmd = (cmd & ~0xff8c0000U) | 6U;
    [self pwrite:p reg:AHCI_CMD value:cmd];
    ctl = ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_SCTL);
    if (ctl == ~0U) return 0;
    ctl = (ctl & ~0xf0fU) | 0x300U;
    /* The receive area must be ready for the initial D2H Register FIS. */
    port->dmaOwned = 1;
    [self pwrite:p reg:AHCI_CMD value:(cmd|AHCI_FRE)];
    [self pwrite:p reg:AHCI_SCTL value:(ctl|1)]; IOSleep(2);
    [self pwrite:p reg:AHCI_SCTL value:ctl];
    if (![self waitRegister:AHCI_PORT(p)+AHCI_SSTS mask:15 value:3 milliseconds:1000]) {
        [self logPort:p failure:"reset: no active link"]; return 0;
    }
    /* COMINIT sets DIAG.X after DET is released. AHCI 1.3.1 sections
     * 10.1.2/10.4.2 require clearing it AFTER link establishment, before
     * waiting for the initial FIS to update PxTFD. An earlier clear races
     * COMINIT and can leave real hardware permanently reporting BSY. */
    [self pwrite:p reg:AHCI_SERR value:~0U];
    [self pwrite:p reg:AHCI_PIS value:~0U];
    if (![self waitRegister:AHCI_PORT(p)+AHCI_TFD mask:0x88 value:0 milliseconds:10000]) {
        [self logPort:p failure:"reset: task file stayed busy"]; return 0;
    }
    sig = ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_SIG);
    if (sig != AHCI_SIG_ATA && sig != AHCI_SIG_ATAPI) {
        IOLog("AHCI: port %u unsupported signature %08x\n",p,sig); return 0;
    }
    port->device.packet = sig == AHCI_SIG_ATAPI;
    if (port->device.packet) cmd |= 0x01000000U;
    else cmd &= ~0x01000000U;
    [self pwrite:p reg:AHCI_CMD value:(cmd|AHCI_FRE|AHCI_ST)];
    [self pwrite:p reg:AHCI_IE value:(ready && !polling ? AHCI_IRQ_MASK : 0)];
    return 1;
}
- (int)issue:(unsigned)p plan:(AHCIPlan *)plan
{
    AHCIPort *port = &ports[p];
    AHCIHeader *header = (AHCIHeader *)port->control;
    AHCITable *table = (AHCITable *)(port->control+1280);
    unsigned global;
    /* Only this serialized driver sets CI, and it uses one slot per port.
     * A validated completion proves the preceding command released it.
     * Initialization, reset, and unsolicited causes require fresh hardware
     * checks. Never reuse a command table while a previous DMA owns it. */
    if (!port->completedIdle &&
        (ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_CI) != 0 || (ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_TFD) & 0x88))) return 0;
    if (!AHCIBuildTable(header,table,port->controlPhysical+1280,plan,port->pages,32,0)) return 0;
    port->completedIdle=0;
    /* Completion/recovery already acknowledged the preceding command.
     * Do not clear unseen asynchronous causes while issuing a new command. */
    __asm__ volatile("" : : : "memory");
    port->dmaOwned = 1;
    prefetchedInterrupts=0;
    ahciMMIOPost(registers,registerLength,AHCI_PORT(p)+AHCI_CI,1);
    if (!polling) {
        global=ahciMMIORead(registers,registerLength,AHCI_IS);
        /* Reuse a real IS snapshot only when this ATA port is the sole
         * enabled source and already has a cause. Zero, extra/stale bits,
         * ATAPI and multiport configurations retain the service-time read.
         * No other command can issue on this port before service consumes
         * this snapshot. Every IE write invalidates it, including resets. */
        if (port->active && !port->device.packet && plan->action == AHCI_ATA &&
            enabledPortInterrupts == (1U<<p) && global == enabledPortInterrupts)
            prefetchedInterrupts=global;
    }
    return 1;
}
- (int)identifyPort:(unsigned)p
{
    AHCIPlan plan;
    AHCIPort *port = &ports[p];
    ns_time_t start,now;
    int result=0;
    memset(&plan,0,sizeof(plan)); plan.action=AHCI_ATA; plan.read=1; plan.bytes=512;
    AHCIFIS(plan.fis,port->device.packet ? 0xa1 : 0xec);
    /* IDENTIFY follows COMRESET, outside the ordinary completion path. */
    [self pwrite:p reg:AHCI_PIS value:~0U];
    if (![self issue:p plan:&plan]) { [self logPort:p failure:"IDENTIFY issue failed"]; return 0; }
    IOGetTimestamp(&start);
    for (;;) {
        result=AHCICompletion(ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_CI), ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_TFD),
            ahciMMIORead(registers,registerLength,AHCI_PORT(p)+AHCI_PIS), ((AHCIHeader *)port->control)->transferred,512,1);
        if (result) break;
        IOGetTimestamp(&now);
        if (AHCIDeadlineExpired(now,start,10)) break;
        IOSleep(1);
    }
    if (result != 1) {
        IOLog("AHCI: port %u IDENTIFY result=%d transferred=%u\n",p,result,
            ((AHCIHeader *)port->control)->transferred);
        [self logPort:p failure:"IDENTIFY failed"]; return 0;
    }
    if (!AHCIIdentify(port->data,port->device.packet,&port->device)) {
        IOLog("AHCI: port %u unsupported IDENTIFY data\n",p); return 0;
    }
    IOLog("AHCI: port %u %s '%s', block size %u, %u MiB\n",p,
        port->device.packet ? "ATAPI" : "ATA",port->device.model,port->device.blockSize,
        (unsigned)((port->device.blocks*port->device.blockSize)>>20));
    port->online=1;
    return 1;
}
- (int)initializeHardware
{
    unsigned vs, p, cmd;
    capabilities=ahciMMIORead(registers,registerLength,AHCI_CAP); vs=ahciMMIORead(registers,registerLength,AHCI_VS); implemented=ahciMMIORead(registers,registerLength,AHCI_PI);
    IOLog("AHCI: CAP=%08x VS=%08x PI=%08x\n",capabilities,vs,implemented);
    if (capabilities == ~0U || vs == ~0U || vs < 0x10000 || vs > 0x10301) return 0;
    portCount=32;
    while (portCount && !(implemented & (1U << (portCount-1)))) portCount--;
    if (!portCount || registerLength < AHCI_PORT(portCount)) return 0;
    /* claimHardware already completed BIOS/OS handoff before BAR sizing. */
    [self write:AHCI_GHC value:AHCI_AE];
    IOLog("AHCI: stopping firmware engines\n");
    for (p=0;p<portCount;p++) if (implemented & (1U<<p)) {
        if (![self stopPort:p]) return 0;
    }
    [self write:AHCI_GHC value:(AHCI_AE|1)];
    if (![self waitRegister:AHCI_GHC mask:1 value:0 milliseconds:1000]) return 0;
    [self write:AHCI_GHC value:AHCI_AE];
    IOLog("AHCI: controller reset complete\n");
    /* No new DMA pointers are used until every firmware engine has stopped. */
    for (p=0;p<portCount;p++) if (implemented & (1U<<p)) {
        if (![self stopPort:p] || ![self allocatePort:p]) {
            IOLog("AHCI: port %u DMA allocation or engine stop failed\n",p); return 0;
        }
    }
    if (![self busMaster:1]) { IOLog("AHCI: enabling bus mastering failed\n"); return 0; }
    IOLog("AHCI: bus mastering enabled; probing ports\n");
    for (p=0;p<portCount;p++) if (implemented & (1U<<p)) {
        if (![self resetPort:p] || ![self identifyPort:p]) {
            ports[p].online=0;
            if (![self stopPort:p]) return 0;
            IOLog("AHCI: port %u empty or unsupported\n",p);
        }
    }
    if (![self configureMSI]) { IOLog("AHCI: interrupt setup failed\n"); return 0; }
    for (p=0;p<portCount;p++) if (implemented & (1U<<p)) {
        [self pwrite:p reg:AHCI_PIS value:~0U];
        [self pwrite:p reg:AHCI_IE value:(!polling && ports[p].online ? AHCI_IRQ_MASK : 0)];
    }
    [self write:AHCI_IS value:~0U];
    cmd=AHCI_AE | (polling ? 0 : 2);
    [self write:AHCI_GHC value:cmd];
    vs=ahciMMIORead(registers,registerLength,AHCI_GHC);
    if ((vs & (AHCI_AE|2)) != cmd) {
        IOLog("AHCI: final GHC wanted=%08x got=%08x\n",cmd,vs); return 0;
    }
    return 1;
}
- (int)quiesce
{
    unsigned p;
    int stoppedAll=1, fenced, irqOff;
    if (!hardwareOwned) return 1;
    if (registers) {
        [self write:AHCI_GHC value:AHCI_AE];
        for (p=0;p<portCount;p++) if (implemented & (1U<<p)) {
            if (![self stopPort:p]) stoppedAll=0;
        }
    }
    irqOff=[self disableMSI];
    fenced=[self busMaster:0];
    if (vectorOwned && irqOff) {
        if ([msiProvider releaseMSIVector:msiMessage.vector owner:self] == IO_R_SUCCESS) vectorOwned=0;
        else irqOff=0;
    }
    return (stoppedAll || fenced) && irqOff;
}
@end
