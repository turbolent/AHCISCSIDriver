/* Read-only OPENSTEP generic-SCSI qualification for a specified AHCI target. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <libc.h>
#include <sys/ioctl.h>
#include <bsd/dev/scsireg.h>
static unsigned char *data;
static scsi_req_t req;
static int fd;
static int noSense=1;
static unsigned be32(unsigned char *p)
{ return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }
static unsigned crc32(unsigned char *p,unsigned n)
{
    unsigned crc=~0U,i;
    while(n--) { crc^=*p++; for(i=0;i<8;i++) crc=(crc>>1)^((crc&1) ? 0xedb88320U : 0); }
    return ~crc;
}
static int submit(unsigned char *cdb,int length,int capacity)
{
    int result;
    memset(&req,0,sizeof(req)); memcpy(&req.sr_cdb,cdb,length);
    req.sr_cdb_length=length; req.sr_dma_dir=SR_DMA_RD;
    req.sr_addr=(caddr_t)data; req.sr_dma_max=capacity; req.sr_ioto=30;
    req.sr_ignore_chkcond=noSense;
    result=ioctl(fd,SGIOCREQ,&req);
    printf("op=%02x ioctl=%d driver=%d scsi=%02x bytes=%d/%d time=%ld.%06ld\n",cdb[0],result,
        req.sr_io_status,req.sr_scsi_status,req.sr_dma_xfr,capacity,
        req.sr_exec_time.tv_sec,req.sr_exec_time.tv_usec); fflush(stdout);
    return result==0 && req.sr_io_status==SR_IOST_GOOD && req.sr_scsi_status==STAT_GOOD;
}
static int sense(void)
{
    unsigned char c[6]={3,0,0,0,18,0};
    /* Some OPENSTEP generic-SCSI paths return autosense even after SGIOCDAS.
     * REQUEST SENSE has then already consumed the device's pending sense. */
    if (req.sr_io_status == SR_IOST_CHKSV) {
        memcpy(data,&req.sr_esense,18);
        printf("autosense=%02x/%02x/%02x\n",data[2]&15,data[12],data[13]);
        return 1;
    }
    if (!submit(c,6,18) || req.sr_dma_xfr < 14) return 0;
    printf("sense=%02x/%02x/%02x\n",data[2]&15,data[12],data[13]); return 1;
}
int main(int argc,char **argv)
{
    unsigned char c[16], saved[4096], *allocation;
    scsi3_req_t req3;
    scsi_adr_t address;
    unsigned block, last, i;
    int controller, type, ready=0;
    if (argc!=5) { fprintf(stderr,"usage: native_scsi sg-device controller target disk|cd|empty\n"); return 2; }
    type=!strcmp(argv[4],"disk") ? 0 : 5;
    allocation=valloc(131072+8192); if (!allocation) return 1;
    data=allocation;
    fd=open(argv[1],O_RDWR); if (fd<0) { perror("open sg"); return 1; }
    controller=atoi(argv[2]); address.sa_target=atoi(argv[3]); address.sa_lun=0;
    if (ioctl(fd,SGIOCCNTR,&controller)<0 || ioctl(fd,SGIOCSTL,&address)<0 || ioctl(fd,SGIOCDAS,0)<0) {
        perror("select target"); return 1;
    }
    memset(c,0,12); c[0]=0x12; c[4]=36;
    if (!submit(c,6,128) || req.sr_dma_xfr!=36 || (data[0]&31)!=type) return 1;
    printf("identity %.8s %.16s %.4s\n",data+8,data+16,data+32);
    for(i=0;i<5;i++) {
        memset(c,0,12);
        if (submit(c,6,0)) { ready=1; break; }
        if (!sense()) return 1;
    }
    if (!strcmp(argv[4],"empty")) {
        if (ready || (data[2]&15)!=2 || data[12]!=0x3a) return 1;
        printf("PASS empty tray and sense\n"); return 0;
    }
    if (!ready) return 1;
    memset(c,0,12); c[0]=0x25;
    if (!submit(c,10,8) || req.sr_dma_xfr!=8) return 1;
    block=be32(data+4); last=be32(data);
    printf("capacity last=%u block=%u\n",last,block);
    if (last<17 || block<512 || block>4096) return 1;
    memset(c,0,12); c[0]=0x28; c[5]=16; c[8]=1;
    if (!submit(c,10,block) || req.sr_dma_xfr!=block) return 1;
    memcpy(saved,data,block);
    if (type==0) {
        c[1]=8;
        if (submit(c,10,block) || req.sr_scsi_status!=STAT_CHECK || !sense() ||
            (data[2]&15)!=5 || data[12]!=0x24) return 1;
        printf("PASS unsupported READ FUA rejected with valid sense\n");
    }
    memset(c,0,12); c[0]=0xa8; c[5]=16; c[9]=1;
    if (!submit(c,12,block) || req.sr_dma_xfr!=block || memcmp(saved,data,block)) return 1;
    memset(c,0,16); c[0]=8; c[3]=16; c[4]=1;
    if (!submit(c,6,block) || req.sr_dma_xfr!=block || memcmp(saved,data,block)) return 1;
    /* Exercise both halves of an OPENSTEP 8 KiB VM page and page crossings. */
    for(i=4095;i<=8191;i+=4096) {
        data=allocation+i;
        memset(c,0,16); c[0]=0x28; c[5]=16; c[8]=1;
        if (!submit(c,10,block) || req.sr_dma_xfr!=block || memcmp(saved,data,block)) return 1;
    }
    data=allocation;
    memset(c,0,16); c[0]=0x28; c[5]=16;
    c[7]=(131072/block)>>8; c[8]=131072/block;
    if (!submit(c,10,131072) || req.sr_dma_xfr!=131072 || memcmp(saved,data,block)) return 1;
    printf("LBA16 128 KiB CRC32=%08x\n",crc32(data,131072));
    if (type==0) {
        memset(&req3,0,sizeof(req3)); memset(c,0,16);
        c[0]=0x88; c[9]=16; c[13]=1;
        memcpy(&req3.s3r_cdb,c,16); req3.s3r_cdb_length=16;
        req3.s3r_dma_dir=SR_DMA_RD; req3.s3r_addr=(caddr_t)data;
        req3.s3r_dma_max=block; req3.s3r_ioto=30;
        if (ioctl(fd,SGIOCREQ3,&req3)<0 || req3.s3r_io_status!=SR_IOST_GOOD ||
            req3.s3r_dma_xfr!=block || memcmp(saved,data,block)) return 1;
        memset(c,0,16); c[0]=0x9e; c[1]=0x10; c[13]=32;
        memcpy(&req3.s3r_cdb,c,16); req3.s3r_dma_max=32;
        if (ioctl(fd,SGIOCREQ3,&req3)<0 || req3.s3r_io_status!=SR_IOST_GOOD ||
            req3.s3r_dma_xfr!=32 || be32(data+4)!=last || be32(data+8)!=block) return 1;
        printf("PASS READ16 and READ CAPACITY16 through SGIOCREQ3\n");
    }
    memset(c,0,12); c[0]=0x1a; c[2]=2; c[4]=64;
    if (!submit(c,6,64) || req.sr_dma_xfr<20) return 1;
    c[1]=2; /* Rejected by the translator, never sent to the device. */
    if (submit(c,6,64) || req.sr_io_status!=SR_IOST_CHKSV ||
        req.sr_scsi_status!=STAT_CHECK || !sense() ||
        (data[2]&15)!=5 || data[12]!=0x24) return 1;
    printf("PASS local command rejection retains valid sense\n");
    memset(c,0,12); c[0]=0xff;
    if (submit(c,12,0) || req.sr_scsi_status!=STAT_CHECK || !sense() || (data[2]&15)!=5) return 1;
    memset(c,0,12); if (!submit(c,6,0)) return 1;
    if (ioctl(fd,SGIOCENAS,0)<0) return 1;
    noSense=0;
    memset(c,0,12); c[0]=0xff;
    if (submit(c,12,0) || req.sr_scsi_status!=STAT_CHECK ||
        req.sr_io_status!=SR_IOST_CHKSV || (((unsigned char *)&req.sr_esense)[2]&15)!=5) return 1;
    printf("enabled autosense=%02x/%02x/%02x\n",
        ((unsigned char *)&req.sr_esense)[2]&15,
        ((unsigned char *)&req.sr_esense)[12],((unsigned char *)&req.sr_esense)[13]);
    memset(c,0,12); if (!submit(c,6,0)) return 1;
    printf("PASS inquiry, capacity, READ6/10/12, unaligned buffers, 128 KiB, mode page, sense and recovery\n");
    close(fd); free(allocation); return 0;
}
