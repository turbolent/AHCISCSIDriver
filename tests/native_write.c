/* Destructive scratch-disk qualification, with explicit identity gating.
 * The final 128 KiB are saved and restored. Never use on a mounted disk. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <libc.h>
#include <sys/ioctl.h>
#include <bsd/dev/scsireg.h>
#define LENGTH 131072U
static int fd;
static unsigned be32(unsigned char *p)
{ return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }
static void put32(unsigned char *p,unsigned n)
{ p[0]=n>>24; p[1]=n>>16; p[2]=n>>8; p[3]=n; }
static int command(unsigned char *c,int len,unsigned char *data,unsigned size,int read)
{
    scsi_req_t r;
    int result;
    memset(&r,0,sizeof(r)); memcpy(&r.sr_cdb,c,len); r.sr_cdb_length=len;
    r.sr_dma_dir=read ? SR_DMA_RD : SR_DMA_WR;
    r.sr_addr=(caddr_t)data; r.sr_dma_max=size; r.sr_ioto=30;
    result=ioctl(fd,SGIOCREQ,&r);
    printf("op=%02x flags=%02x result=%d driver=%d bytes=%d/%u\n",
        c[0],c[1],result,r.sr_io_status,r.sr_dma_xfr,size); fflush(stdout);
    return result==0 && r.sr_io_status==SR_IOST_GOOD &&
        r.sr_scsi_status==STAT_GOOD && (unsigned)r.sr_dma_xfr==size;
}
static int transfer(unsigned lba,unsigned blocks,unsigned char *data,int write,int fua)
{
    unsigned char c[10];
    memset(c,0,10); c[0]=write ? 0x2a : 0x28; c[1]=fua ? 8 : 0;
    put32(c+2,lba); c[7]=blocks>>8; c[8]=blocks;
    return command(c,10,data,LENGTH,!write);
}
int main(int argc,char **argv)
{
    unsigned char c[12], *original, *patternAllocation, *readAllocation, *pattern, *readback;
    unsigned block, last, blocks, lba, i;
    scsi_adr_t address;
    int controller, passed=0;
    if (argc!=5 || strcmp(argv[4],"DISPOSABLE-AHCISCRATCH")) {
        fprintf(stderr,"usage: native_write sg-device controller target DISPOSABLE-AHCISCRATCH\n"); return 2;
    }
    original=valloc(LENGTH); patternAllocation=valloc(LENGTH+8192); readAllocation=valloc(LENGTH+8192);
    if (!original || !patternAllocation || !readAllocation) return 1;
    pattern=patternAllocation+4095; readback=readAllocation+8191;
    fd=open(argv[1],O_RDWR); if(fd<0) { perror("open sg"); return 1; }
    controller=atoi(argv[2]); address.sa_target=atoi(argv[3]); address.sa_lun=0;
    if(ioctl(fd,SGIOCCNTR,&controller)<0 || ioctl(fd,SGIOCSTL,&address)<0) return 1;
    memset(c,0,12); c[0]=0x12; c[4]=36;
    if(!command(c,6,original,36,1) || memcmp(original+8,"ATA     ",8) ||
        (memcmp(original+16,"QEMU HARDDISK   ",16) &&
         memcmp(original+16,"VBOX HARDDISK   ",16))) return 1;
    memset(c,0,12); c[0]=0x12; c[1]=1; c[2]=0x80; c[4]=15;
    if(!command(c,6,original,15,1) || original[3]!=11 || memcmp(original+4,"AHCISCRATCH",11)) {
        fprintf(stderr,"Refusing writes: disk serial is not AHCISCRATCH\n"); return 1;
    }
    memset(c,0,12); c[0]=0x25;
    if(!command(c,10,original,8,1)) return 1;
    last=be32(original); block=be32(original+4);
    if(block<512 || block>4096 || LENGTH%block || last==~0U || last<LENGTH/block) return 1;
    blocks=LENGTH/block; lba=last+1-blocks;
    if(!transfer(lba,blocks,original,0,0)) return 1;
    for(i=0;i<LENGTH;i++) pattern[i]=(unsigned char)((i*37U)^(i>>9)^0xa5U);
    if(transfer(lba,blocks,pattern,1,1) && transfer(lba,blocks,readback,0,0) &&
        !memcmp(pattern,readback,LENGTH)) passed=1;
    /* Restore even when the write or verification failed. */
    if(!transfer(lba,blocks,original,1,0)) { fprintf(stderr,"SCRATCH RESTORE FAILED\n"); return 1; }
    memset(c,0,12); c[0]=0x35;
    if(!command(c,10,original,0,0)) return 1;
    put32(c+2,lba); c[8]=1;
    if(!command(c,10,original,0,0)) return 1;
    printf("PASS SYNCHRONIZE CACHE(10) with a one-block range\n");
    if(!transfer(lba,blocks,readback,0,0) || memcmp(original,readback,LENGTH)) return 1;
    if(!passed) return 1;
    printf("PASS 128 KiB unaligned write, FUA, readback, restored bytes and flush\n");
    memset(c,0,12); c[0]=0x1b;
    if(!command(c,6,original,0,0)) return 1;
    c[4]=1; if(!command(c,6,original,0,0)) return 1;
    if(!transfer(lba,blocks,readback,0,0) || memcmp(original,readback,LENGTH)) return 1;
    close(fd); free(original); free(patternAllocation); free(readAllocation);
    if(!passed) return 1;
    printf("PASS 128 KiB unaligned write, FUA, readback, restore, flush, stop/start\n"); return 0;
}
