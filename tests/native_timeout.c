/* Read-only timeout qualification. Suppress the disposable scratch port's
 * interrupt externally, then restore it and run native_scsi again. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <libc.h>
#include <sys/ioctl.h>
#include <bsd/dev/scsireg.h>
#define LENGTH 131072
int main(int argc,char **argv)
{
    scsi_req_t r;
    scsi_adr_t address;
    unsigned char *data;
    int fd, controller, attempt, i, result;
    if(argc!=4) { fprintf(stderr,"usage: native_timeout sg-device controller target\n"); return 2; }
    data=valloc(LENGTH); if(!data) return 1;
    fd=open(argv[1],O_RDWR); if(fd<0) return 1;
    controller=atoi(argv[2]); address.sa_target=atoi(argv[3]); address.sa_lun=0;
    if(ioctl(fd,SGIOCCNTR,&controller)<0 || ioctl(fd,SGIOCSTL,&address)<0) return 1;
    memset(&r,0,sizeof(r));
    ((unsigned char *)&r.sr_cdb)[0]=0x12;
    ((unsigned char *)&r.sr_cdb)[1]=1;
    ((unsigned char *)&r.sr_cdb)[2]=0x80;
    ((unsigned char *)&r.sr_cdb)[4]=15;
    r.sr_cdb_length=6; r.sr_dma_dir=SR_DMA_RD;
    r.sr_addr=(caddr_t)data; r.sr_dma_max=15; r.sr_ioto=5;
    if(ioctl(fd,SGIOCREQ,&r)<0 || r.sr_io_status!=SR_IOST_GOOD ||
        r.sr_dma_xfr!=15 || data[3]!=11 || memcmp(data+4,"AHCISCRATCH",11)) {
        fprintf(stderr,"Expected disposable AHCISCRATCH target\n"); return 1;
    }
    for(attempt=0;attempt<4;attempt++) {
        memset(data,0xa5,LENGTH); memset(&r,0,sizeof(r));
        ((unsigned char *)&r.sr_cdb)[0]=0x28;
        ((unsigned char *)&r.sr_cdb)[4]=attempt+1;
        ((unsigned char *)&r.sr_cdb)[7]=1;
        r.sr_cdb_length=10; r.sr_dma_dir=SR_DMA_RD;
        r.sr_addr=(caddr_t)data; r.sr_dma_max=LENGTH; r.sr_ioto=1;
        result=ioctl(fd,SGIOCREQ,&r);
        printf("attempt=%d result=%d status=%d bytes=%d time=%ld.%06ld\n",
            attempt,result,r.sr_io_status,r.sr_dma_xfr,
            r.sr_exec_time.tv_sec,r.sr_exec_time.tv_usec); fflush(stdout);
        if(r.sr_io_status==SR_IOST_IOTO) {
            if(r.sr_dma_xfr) return 1;
            sleep(2);
            for(i=0;i<LENGTH;i++) if(data[i]!=0xa5) {
                fprintf(stderr,"Failed read changed caller memory\n"); return 1;
            }
            printf("PASS timed-out read, zero transfer, caller memory unchanged\n");
            close(fd); free(data); return 0;
        }
        if(result<0 || r.sr_io_status!=SR_IOST_GOOD) return 1;
    }
    fprintf(stderr,"No timeout observed; check backend throttling\n"); return 1;
}
