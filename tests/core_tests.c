#include "AHCICore.h"
#include "AHCIRegs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define REQUIRE(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
static AHCIB8 buffer[AHCI_MAX_TRANSFER+16];
static AHCIDevice disk;
static void putWord(AHCIB8 *d,unsigned n,unsigned v) { d[n*2]=(AHCIB8)v; d[n*2+1]=(AHCIB8)(v>>8); }
static void identifyTests(void)
{
    AHCIB8 id[512]; AHCIDevice d; unsigned i;
    memset(id,0,sizeof(id)); putWord(id,0,0x40); putWord(id,49,0x300);
    putWord(id,60,0xffff); putWord(id,61,0x0fff); putWord(id,83,0x7400);
    putWord(id,85,0x20); putWord(id,87,0x4000); putWord(id,101,0x1000);
    id[54]='O'; id[55]='M'; id[56]='E'; id[57]='D'; id[58]=' '; id[59]='L';
    REQUIRE(AHCIIdentify(id,0,&d)); REQUIRE(d.blocks==0x10000000ULL);
    REQUIRE(d.blockSize==512 && d.lba48 && d.flush && d.cache);
    REQUIRE(!strncmp(d.model,"MODEL",5));
    putWord(id,106,0x5000); putWord(id,117,2048);
    REQUIRE(AHCIIdentify(id,0,&d) && d.blockSize==4096);
    putWord(id,117,257); REQUIRE(!AHCIIdentify(id,0,&d));
    putWord(id,106,0); putWord(id,83,0x5000);
    REQUIRE(AHCIIdentify(id,0,&d) && !d.lba48 && d.blocks==0x0fffffff);
    putWord(id,0,0x8580); REQUIRE(AHCIIdentify(id,1,&d) && d.packetBytes==12);
    putWord(id,0,0x8581); REQUIRE(AHCIIdentify(id,1,&d) && d.packetBytes==16);
    putWord(id,0,0x8582); REQUIRE(!AHCIIdentify(id,1,&d));
    putWord(id,0,0x8580); id[510]=0xa5; id[511]=0;
    for(i=0;i<511;i++) id[511]-=id[i];
    REQUIRE(AHCIIdentify(id,1,&d)); id[1]^=1; REQUIRE(!AHCIIdentify(id,1,&d));
}
static AHCIPlan translate(AHCIB8 *c,unsigned len,int read,unsigned cap)
{ return AHCITranslate(&disk,c,len,read,buffer,cap,0); }
static void cacheIdentityTests(void)
{
    static const unsigned validity[] = {0,0xffff,0x8000,0xc000,0x4000,0x4123};
    static const unsigned support[] = {0x7400,0x5000,0x4400,0};
    static const unsigned opcodes[] = {0x2a,0xaa,0x8a};
    static const unsigned lengths[] = {10,12,16};
    static const unsigned countByte[] = {8,9,13};
    AHCIB8 id[512], c[16]; AHCIDevice d; AHCIPlan p;
    unsigned v,s,enabled,op,mayCache,canFlush;
    for(v=0;v<sizeof(validity)/sizeof(validity[0]);v++)
    for(s=0;s<sizeof(support)/sizeof(support[0]);s++)
    for(enabled=0;enabled<2;enabled++) {
        memset(id,0,sizeof(id)); putWord(id,0,0x40); putWord(id,49,0x300);
        putWord(id,60,1024); putWord(id,100,1024);
        putWord(id,83,support[s]); putWord(id,85,enabled ? 0x20 : 0);
        putWord(id,87,validity[v]);
        REQUIRE(AHCIIdentify(id,0,&d));
        mayCache = (validity[v]&0xc000)!=0x4000 || enabled;
        canFlush = s<2;
        REQUIRE(d.cache==(int)mayCache && d.flush==(int)canFlush);
        for(op=0;op<3;op++) {
            memset(c,0,sizeof(c)); c[0]=(AHCIB8)opcodes[op]; c[1]=8;
            /* WRITE10/12 use bytes 7..8 / 6..9; WRITE16 uses 10..13. */
            c[countByte[op]]=1;
            p=AHCITranslate(&d,c,lengths[op],0,buffer,512,0);
            if(mayCache && !canFlush) REQUIRE(p.action==AHCI_CHECK);
            else REQUIRE(p.action==AHCI_ATA && p.bytes==512 && p.fua==(int)mayCache);
            c[1]=0;
            p=AHCITranslate(&d,c,lengths[op],0,buffer,512,0);
            REQUIRE(p.action==AHCI_ATA && !p.fua && p.bytes==512);
        }
        memset(c,0,sizeof(c)); c[0]=0x35;
        p=AHCITranslate(&d,c,10,0,buffer,0,0);
        if(canFlush) REQUIRE(p.action==AHCI_ATA && p.fis[2]==(d.lba48 ? 0xea : 0xe7));
        else REQUIRE(p.action==(mayCache ? AHCI_CHECK : AHCI_DONE));
        memset(c,0,sizeof(c)); c[0]=0x1b;
        p=AHCITranslate(&d,c,6,0,buffer,0,0);
        if(mayCache && !canFlush) REQUIRE(p.action==AHCI_CHECK);
        else REQUIRE(p.action==AHCI_ATA && p.stopAfterFlush==(int)canFlush);
    }
}
static void diskTests(void)
{
    AHCIB8 c[16]; AHCIPlan p; unsigned i;
    memset(&disk,0,sizeof(disk)); disk.blocks=0x100000100ULL;
    disk.blockSize=512; disk.lba48=1; disk.flush=1; disk.cache=1;
    memcpy(disk.model,"TEST SATA DISK",15); memcpy(disk.serial,"TEST123",8); memcpy(disk.firmware,"0001",5);
    memset(c,0,16); c[0]=0x12; c[4]=36;
    p=translate(c,6,1,36); REQUIRE(p.action==AHCI_DONE && p.responseBytes==36);
    REQUIRE(buffer[0]==0 && !memcmp(buffer+8,"ATA",3));
    p=translate(c,6,0,36); REQUIRE(p.action==AHCI_CHECK);
    c[1]=1; c[2]=0x80; p=translate(c,6,1,36); REQUIRE(buffer[3]==7 && p.responseBytes==11);
    memset(c,0,16); c[0]=0x25;
    p=translate(c,10,1,8); REQUIRE(p.responseBytes==8 && AHCIReadBE(buffer,4)==0xffffffffU);
    memset(c,0,16); c[0]=0x9e; c[1]=0x10; c[13]=32;
    p=translate(c,16,1,32); REQUIRE(p.responseBytes==32 && AHCIReadBE(buffer,4)==1 && AHCIReadBE(buffer+4,4)==255);
    memset(c,0,16); c[0]=0x28; c[5]=17; c[8]=3;
    p=translate(c,10,1,1536); REQUIRE(p.action==AHCI_ATA && p.bytes==1536 && p.fis[2]==0x25 && p.fis[4]==17);
    REQUIRE(translate(c,10,1,1535).action==AHCI_CHECK);
    REQUIRE(translate(c,10,0,1536).action==AHCI_CHECK);
    c[8]=0; REQUIRE(translate(c,10,1,0).action==AHCI_DONE);
    c[8]=1; c[1]=0x80; REQUIRE(translate(c,10,1,512).action==AHCI_CHECK);
    memset(c,0,16); c[0]=0x8a; c[1]=8; c[9]=23; c[13]=2;
    p=translate(c,16,0,1024); REQUIRE(p.fua && p.fis[2]==0x35 && p.bytes==1024);
    disk.flush=0; REQUIRE(translate(c,16,0,1024).action==AHCI_CHECK); disk.flush=1;
    AHCIWriteBE(c+2,disk.blocks-1,8); p=translate(c,16,0,1024); REQUIRE(p.sense[12]==0x21);
    c[13]=1; REQUIRE(translate(c,16,0,512).action==AHCI_ATA);
    AHCIWriteBE(c+2,~0ULL,8); REQUIRE(translate(c,16,0,512).action==AHCI_CHECK);
    memset(c,0,16); c[0]=8; REQUIRE(translate(c,6,1,131072).blocks==256);
    disk.lba48=0; disk.blocks=1U<<28;
    p=translate(c,6,1,131072); REQUIRE(p.fis[2]==0xc8 && p.fis[12]==0);
    memset(c,0,16); c[0]=0x28; AHCIWriteBE(c+2,0x0fffffff,4); c[8]=1;
    p=translate(c,10,1,512); REQUIRE(p.fis[7]==0x4f && p.fis[4]==255);
    c[8]=2; p=translate(c,10,1,1024); REQUIRE(p.sense[12]==0x21);
    disk.lba48=1; disk.blocks=1ULL<<40;
    disk.blockSize=4096; memset(c,0,16); c[0]=0x28; c[8]=32;
    REQUIRE(translate(c,10,1,131072).bytes==131072); c[8]=33;
    REQUIRE(translate(c,10,1,131072).action==AHCI_CHECK); disk.blockSize=512;
    memset(c,0,16); c[0]=0x1a; c[2]=0x3f; c[4]=255;
    p=translate(c,6,1,255); REQUIRE(p.responseBytes==96 && buffer[0]==95 && buffer[3]==8);
    for(i=0;i<96;i++) {
        memset(buffer,0xa5,sizeof(buffer)); p=translate(c,6,1,i);
        REQUIRE(p.responseBytes==i && buffer[i]==0xa5);
    }
    memset(c,0,16); c[0]=0x35; p=translate(c,10,0,0);
    REQUIRE(p.action==AHCI_ATA && p.fis[2]==0xea);
    c[8]=1; p=translate(c,10,0,0); /* SYNCHRONIZE CACHE range length */
    REQUIRE(p.action==AHCI_ATA && p.fis[2]==0xea);
    c[8]=0; c[6]=0x80; REQUIRE(translate(c,10,0,0).action==AHCI_CHECK);
    c[6]=0;
    disk.flush=0; REQUIRE(translate(c,10,0,0).action==AHCI_CHECK);
    disk.cache=0; REQUIRE(translate(c,10,0,0).action==AHCI_DONE);
    disk.flush=1; disk.cache=1;
    memset(c,0,16); c[0]=0x1b; p=translate(c,6,0,0);
    REQUIRE(p.stopAfterFlush && p.fis[2]==0xea);
    memset(c,0,16); c[0]=0xff; REQUIRE(translate(c,16,0,0).action==AHCI_CHECK);
    c[0]=0x88; REQUIRE(AHCICDBLength(c,0,12)==0 && AHCICDBLength(c,0,16)==16);
    REQUIRE(AHCICDBLength(c,12,16)==0);
}
static void packetTests(void)
{
    AHCIDevice d; AHCIB8 c[16]; AHCIPlan p; unsigned n;
    memset(&d,0,sizeof(d)); d.packet=1; d.packetBytes=12; d.dma=1;
    memset(c,0,16); c[0]=0x12; c[4]=36;
    p=AHCITranslate(&d,c,6,1,buffer,36,0);
    REQUIRE(p.action==AHCI_PACKET && p.fis[2]==0xa0 && p.packet[0]==0x12 && p.packet[11]==0 && p.fis[3]==0);
    REQUIRE(AHCIPacketFinish(&p,buffer,24,&n) && n==24);
    REQUIRE(!AHCIPacketFinish(&p,buffer,37,&n));
    c[0]=0x88; REQUIRE(AHCITranslate(&d,c,16,1,buffer,2048,0).action==AHCI_CHECK);
    d.packetBytes=16; p=AHCITranslate(&d,c,16,1,buffer,2048,0);
    REQUIRE(p.action==AHCI_PACKET && p.fis[3]==1);
    memset(c,0,16); c[0]=8; c[2]=1; c[4]=0;
    p=AHCITranslate(&d,c,6,1,buffer,131072,0);
    REQUIRE(p.packet[0]==0x28 && p.packet[4]==1 && p.packet[7]==1 && p.packet[8]==0);
    memset(c,0,16); c[0]=0x1a; c[2]=0x2a; c[4]=64;
    p=AHCITranslate(&d,c,6,1,buffer,64,0);
    REQUIRE(p.bytes==68 && p.packet[0]==0x5a && p.packet[8]==68);
    memset(buffer,0,68); buffer[1]=18; buffer[2]=3; buffer[3]=0x80; buffer[8]=0x2a; buffer[9]=10;
    REQUIRE(AHCIPacketFinish(&p,buffer,20,&n) && n==16);
    REQUIRE(buffer[0]==15 && buffer[1]==3 && buffer[2]==0x80 && buffer[4]==0x2a);
    REQUIRE(!AHCIPacketFinish(&p,buffer,7,&n));
    c[4]=0; p=AHCITranslate(&d,c,6,1,buffer,0,0); REQUIRE(p.action==AHCI_DONE);
    c[4]=1; p=AHCITranslate(&d,c,6,1,buffer,1,0); REQUIRE(p.bytes==8);
    memset(buffer,0,8); buffer[1]=6;
    REQUIRE(AHCIPacketFinish(&p,buffer,8,&n) && n==1);
    c[2]=2; c[4]=64; p=AHCITranslate(&d,c,6,1,buffer,64,0);
    REQUIRE(p.action==AHCI_DONE && p.responseBytes==20 && buffer[4]==2 && buffer[5]==14);
    memset(c,0,16); c[0]=0x15; c[1]=0x10; c[4]=16;
    memset(buffer,0,32); buffer[3]=8; buffer[12]=8; buffer[13]=2; buffer[14]=4;
    p=AHCITranslate(&d,c,6,0,buffer,16,0);
    REQUIRE(p.action==AHCI_PACKET && p.packet[0]==0x55 && p.bytes==12 && buffer[8]==8 && buffer[10]==4);
    REQUIRE(AHCIPacketFinish(&p,buffer,12,&n) && n==16);
    memset(buffer,0,32); buffer[3]=255;
    REQUIRE(AHCITranslate(&d,c,6,0,buffer,16,0).action==AHCI_CHECK);
}
static void readFuaTests(void)
{
    AHCIB8 c[16]; AHCIPlan p; unsigned i;
    static const unsigned ops[3]={0x28,0xa8,0x88};
    static const unsigned lengths[3]={10,12,16};
    static const unsigned counts[3]={8,9,13};
    for(i=0;i<3;i++) {
        memset(c,0,sizeof(c)); c[0]=(AHCIB8)ops[i];
        c[1]=8; c[counts[i]]=1;
        p=translate(c,lengths[i],1,512);
        REQUIRE(p.action==AHCI_CHECK && p.sense[2]==5 && p.sense[12]==0x24);
        c[1]=0x10; /* DPO remains a permitted caching hint. */
        REQUIRE(translate(c,lengths[i],1,512).action==AHCI_ATA);
    }
}
static void deadlineCounterTests(void)
{
    AHCIU64 c[3]={185000000ULL,185000000ULL,185000000ULL};
    AHCIU64 n[3]={50000000ULL,50000000ULL,50000000ULL};
    AHCIU64 start=0x1234567800000000ULL;
    unsigned budget=AHCIDeadlineCounterBudget(c,n);
    REQUIRE(budget==3700000U);
    REQUIRE(AHCIDeadlineCounterFresh(start,start,budget));
    REQUIRE(AHCIDeadlineCounterFresh(start,start+budget-1,budget));
    REQUIRE(!AHCIDeadlineCounterFresh(start,start+budget,budget));
    REQUIRE(!AHCIDeadlineCounterFresh(start,start+3700000000ULL,budget));
    REQUIRE(!AHCIDeadlineCounterFresh(start,start-1,budget));
    REQUIRE(!AHCIDeadlineCounterFresh(~0ULL-100,5,budget));
    REQUIRE(!AHCIDeadlineCounterFresh(0,1,budget));
    REQUIRE(!AHCIDeadlineCounterFresh(start,start,0));
    REQUIRE(!AHCIDeadlineCounterFresh(start,start,100000001U));
    REQUIRE(!AHCIDeadlineExpired(1000999999ULL,1000000000ULL,1));
    REQUIRE(AHCIDeadlineExpired(2000000000ULL,1000000000ULL,1));
    c[0]=184000000ULL; REQUIRE(AHCIDeadlineCounterBudget(c,n)==3680000U);
    c[0]=150000000ULL; REQUIRE(!AHCIDeadlineCounterBudget(c,n));
    c[0]=~0ULL; REQUIRE(!AHCIDeadlineCounterBudget(c,n));
    c[0]=0; REQUIRE(!AHCIDeadlineCounterBudget(c,n));
    c[0]=185000000ULL;
    n[0]=0; REQUIRE(!AHCIDeadlineCounterBudget(c,n));
    n[0]=19999999ULL; REQUIRE(!AHCIDeadlineCounterBudget(c,n));
    n[0]=5000000001ULL; REQUIRE(!AHCIDeadlineCounterBudget(c,n));
    REQUIRE(!AHCIDeadlineCounterBudget(0,n));
    REQUIRE(!AHCIDeadlineCounterBudget(c,0));
}
static void executionCounterTests(void)
{
    AHCIU64 c[3]={720000000ULL,721000000ULL,719000000ULL};
    AHCIU64 n[3]={200000000ULL,200000000ULL,200000000ULL};
    AHCIU64 start=0x1234567800000000ULL, elapsed=~0ULL;
    unsigned rate=AHCIExecutionCounterRate(c,n);
    REQUIRE(rate==3600000U);
    REQUIRE(AHCIShortCounterElapsed(start,start+900000,rate,&elapsed));
    REQUIRE(elapsed==250000ULL);
    REQUIRE(AHCIShortCounterElapsed(start,start+rate-1,rate,&elapsed));
    REQUIRE(elapsed==999999ULL);
    elapsed=123;
    REQUIRE(!AHCIShortCounterElapsed(start,start+rate,rate,&elapsed));
    REQUIRE(!AHCIShortCounterElapsed(start,start+3600000000ULL,rate,&elapsed));
    REQUIRE(!AHCIShortCounterElapsed(start,start-1,rate,&elapsed));
    REQUIRE(!AHCIShortCounterElapsed(~0ULL-100,5,rate,&elapsed));
    REQUIRE(!AHCIShortCounterElapsed(0,1,rate,&elapsed));
    REQUIRE(!AHCIShortCounterElapsed(start,start+1,0,&elapsed));
    REQUIRE(!AHCIShortCounterElapsed(start,start+1,99999,&elapsed));
    REQUIRE(!AHCIShortCounterElapsed(start,start+1,100000001,&elapsed));
    REQUIRE(!AHCIShortCounterElapsed(start,start+1,rate,0));
    REQUIRE(elapsed==123); /* Rejection leaves the output untouched. */
    REQUIRE(AHCIShortCounterElapsed(start,start,rate,&elapsed) && !elapsed);
    REQUIRE(AHCIShortCounterElapsed(start,start+99999999,100000000,&elapsed));
    REQUIRE(elapsed==999999ULL); /* Multiply must remain 64 bit. */
    c[0]=720000001ULL; c[1]=719000000ULL; c[2]=721000000ULL;
    REQUIRE(AHCIExecutionCounterRate(c,n)==3600000U);
    c[0]=740000000ULL;
    REQUIRE(AHCIDeadlineCounterBudget(c,n)!=0);
    REQUIRE(!AHCIExecutionCounterRate(c,n)); /* Reporting requires 1%, not 10%. */
    c[0]=~0ULL; REQUIRE(!AHCIExecutionCounterRate(c,n));
    c[0]=0; REQUIRE(!AHCIExecutionCounterRate(c,n));
    c[0]=720000000ULL;
    n[0]=0; REQUIRE(!AHCIExecutionCounterRate(c,n));
    n[0]=19999999ULL; REQUIRE(!AHCIExecutionCounterRate(c,n));
    n[0]=5000000001ULL; REQUIRE(!AHCIExecutionCounterRate(c,n));
    REQUIRE(!AHCIExecutionCounterRate(0,n));
    REQUIRE(!AHCIExecutionCounterRate(c,0));
    c[0]=c[1]=c[2]=100000000000ULL;
    n[0]=n[1]=n[2]=1000000000ULL;
    REQUIRE(AHCIExecutionCounterRate(c,n)==100000000U);
    c[0]=c[1]=c[2]=100000000ULL;
    REQUIRE(AHCIExecutionCounterRate(c,n)==100000U);
    c[0]=c[1]=c[2]=500000000000ULL;
    n[0]=n[1]=n[2]=5000000000ULL;
    REQUIRE(AHCIExecutionCounterRate(c,n)==100000000U);
}
static void receivedTaskFileTests(void)
{
    unsigned tfd=~0U, i;
    static const unsigned rejected[4]={0x51,0x70,0x58,0xd0};
    REQUIRE(AHCIReceivedTaskFile(0,AHCI_DHRS,0x00504034,&tfd) && tfd==0x50);
    REQUIRE(AHCICompletion(0,tfd,AHCI_DHRS,512,512,1)==1);
    REQUIRE(AHCICompletion(0,tfd,AHCI_DHRS,511,512,1)==-1);
    for(i=0;i<4;i++) {
        REQUIRE(AHCIReceivedTaskFile(0,AHCI_DHRS,
            0x40004034U|(rejected[i]<<16),&tfd));
        REQUIRE(tfd==(0x4000|rejected[i]));
        REQUIRE(AHCICompletion(0,tfd,AHCI_DHRS,512,512,1)==-1);
    }
    tfd=0x1234;
    REQUIRE(!AHCIReceivedTaskFile(1,AHCI_DHRS,0x00504034,&tfd));
    REQUIRE(!AHCIReceivedTaskFile(~0U,AHCI_DHRS,0x00504034,&tfd));
    REQUIRE(!AHCIReceivedTaskFile(0,0,0x00504034,&tfd));
    REQUIRE(!AHCIReceivedTaskFile(0,AHCI_DHRS|AHCI_TFES,0x00504034,&tfd));
    REQUIRE(!AHCIReceivedTaskFile(0,AHCI_DHRS|0x40,0x00504034,&tfd));
    REQUIRE(!AHCIReceivedTaskFile(0,AHCI_DHRS|0x400000,0x00504034,&tfd));
    REQUIRE(!AHCIReceivedTaskFile(0,AHCI_DHRS,0x0050405f,&tfd)); /* PIO setup */
    REQUIRE(!AHCIReceivedTaskFile(0,AHCI_DHRS,0x00504134,&tfd)); /* PMP */
    REQUIRE(!AHCIReceivedTaskFile(0,AHCI_DHRS,0,&tfd));
    REQUIRE(!AHCIReceivedTaskFile(0,AHCI_DHRS,~0U,&tfd));
    REQUIRE(!AHCIReceivedTaskFile(0,AHCI_DHRS,0x00504034,0));
    REQUIRE(tfd==0x1234);
}
static void dmaTests(void)
{
    AHCIHeader h; AHCITable t; AHCIPlan p; AHCIU32 pages[33]; unsigned i,n,offset,expected;
    memset(&p,0,sizeof(p)); p.action=AHCI_ATA; p.read=1; AHCIFIS(p.fis,0x25);
    for(i=0;i<33;i++) pages[i]=0x200000+i*0x2000;
    for(offset=0;offset<4096;offset+=254) for(n=0;n<=131072;n+=1024) {
        p.bytes=n;
        REQUIRE(AHCIBuildTable(&h,&t,0x100500,&p,pages,33,offset));
        expected=0;
        for(i=0;i<h.prdtLength;i++) {
            unsigned bytes=(t.prd[i].count&0x3fffff)+1;
            REQUIRE(t.prd[i].address==pages[i]+(i ? 0 : offset));
            REQUIRE(bytes<=4096 && !(bytes&1)); expected+=bytes;
        }
        REQUIRE(expected==n && h.table==0x100500 && h.flags==5);
    }
    p.bytes=131072; REQUIRE(!AHCIBuildTable(&h,&t,0x100501,&p,pages,33,0));
    REQUIRE(!AHCIBuildTable(&h,&t,0x100500,&p,pages,32,2));
    p.bytes=1; REQUIRE(AHCIBuildTable(&h,&t,0x100500,&p,pages,1,0) && t.prd[0].count==1);
    REQUIRE(!AHCIBuildTable(&h,&t,0x100500,&p,pages,1,1));
    p.bytes=0; REQUIRE(AHCIBuildTable(&h,&t,0x100500,&p,0,0,0) && h.prdtLength==0);
    REQUIRE(AHCICompletion(1,0x50,0,0,512,1)==0);
    REQUIRE(AHCICompletion(0,0x50,1,512,512,1)==1);
    REQUIRE(AHCICompletion(0,0x50,1,511,512,1)==-1);
    REQUIRE(AHCICompletion(0,0x50,1,24,36,0)==1);
    REQUIRE(AHCICompletion(0,0x50,1,37,36,0)==-1);
    REQUIRE(AHCICompletion(1,0x51,0,0,512,1)==0); /* stale taskfile before new command */
    REQUIRE(AHCICompletion(1,0x51,AHCI_TFES,0,512,1)==-1);
    /* Overflow is fatal even if PRDBC stops exactly at the PRDT limit. */
    REQUIRE(AHCICompletion(0,0x50,0x01000001U,512,512,0)==-1);
    REQUIRE(AHCICompletion(1,0x50,0x01000000U,0,512,0)==-1);
    REQUIRE((AHCI_IRQ_MASK & 0x01000000U)!=0);
    REQUIRE(AHCICompletion(~0U,~0U,~0U,0,0,0)==-1);
    REQUIRE(AHCIDeadlineExpired(31000000000ULL,1000000000ULL,30));
    REQUIRE(!AHCIDeadlineExpired(30999999999ULL,1000000000ULL,30));
    REQUIRE(!AHCIDeadlineExpired(999,1000,30));
    AHCIATAError(buffer,0x51,0x40,0); REQUIRE(buffer[2]==3 && buffer[12]==0x11);
    AHCIATAError(buffer,0x51,0x10,0); REQUIRE(buffer[2]==5 && buffer[12]==0x21);
}
int main(void)
{
    unsigned base,size,offset;
    REQUIRE(AHCIMapWindow(0xfebf9000U,4096,8192,&base,&size,&offset));
    REQUIRE(base==0xfebf8000U && size==8192 && offset==4096);
    REQUIRE(AHCIMapWindow(0x12345,4096,8192,&base,&size,&offset));
    REQUIRE(base==0x12000 && size==8192 && offset==0x345);
    REQUIRE(AHCIMapWindow(0x13fff,4096,8192,&base,&size,&offset));
    REQUIRE(base==0x12000 && size==16384 && offset==8191);
    REQUIRE(AHCIMapWindow(0xfffff000U,4096,8192,&base,&size,&offset));
    REQUIRE(base==0xffffe000U && size==8192 && offset==4096);
    REQUIRE(!AHCIMapWindow(0xfffff000U,4097,8192,&base,&size,&offset));
    REQUIRE(!AHCIMapWindow(0x1000,4096,6144,&base,&size,&offset));
    REQUIRE(!AHCIMapWindow(0x1000,0,8192,&base,&size,&offset));
    identifyTests(); cacheIdentityTests(); diskTests(); packetTests(); readFuaTests(); dmaTests();
    receivedTaskFileTests(); deadlineCounterTests(); executionCounterTests();
    {
        AHCIB8 c[6]={0x1a,0,0,0,12,0};
        AHCIPlan p=translate(c,6,1,12);
        REQUIRE(p.action==AHCI_DONE && p.responseBytes==12 && buffer[3]==8);
        REQUIRE(AHCIReadBE(buffer+9,3)==disk.blockSize);
    }
    printf("PASS %u core checks\n",checks); return 0;
}
