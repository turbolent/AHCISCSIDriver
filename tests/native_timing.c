/* Read-only diagnostic: reported SCSI duration vs independent syscall wall time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <libc.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <bsd/dev/scsireg.h>

static double stamp(void)
{
    struct timeval t;
    if (gettimeofday(&t,0)) exit(1);
    return t.tv_sec*1000000.0+t.tv_usec;
}
int main(int argc,char **argv)
{
    scsi_req_t r;
    scsi_adr_t address;
    unsigned char *data;
    int fd,controller,i,result,batch,over,zero,nonpositive;
    double begin,wall,reported,sumReported,maxOver,batchBegin,batchWall;
    if (argc!=4) return 2;
    fd=open(argv[1],O_RDWR); if(fd<0) { perror("open sg"); return 1; }
    controller=atoi(argv[2]); address.sa_target=atoi(argv[3]); address.sa_lun=0;
    if(ioctl(fd,SGIOCCNTR,&controller)<0 || ioctl(fd,SGIOCSTL,&address)<0) { perror("select sg"); return 1; }
    data=valloc(512); if(!data) return 1;
    for(batch=0;batch<4;batch++) {
      over=zero=nonpositive=0; sumReported=maxOver=0;
      batchBegin=stamp();
      for(i=0;i<256;i++) {
        memset(&r,0,sizeof(r));
        ((unsigned char *)&r.sr_cdb)[0]=0x28;
        ((unsigned char *)&r.sr_cdb)[5]=16;
        ((unsigned char *)&r.sr_cdb)[8]=1;
        r.sr_cdb_length=10; r.sr_dma_dir=SR_DMA_RD;
        r.sr_addr=(caddr_t)data; r.sr_dma_max=512; r.sr_ioto=5;
        begin=stamp(); result=ioctl(fd,SGIOCREQ,&r); wall=stamp()-begin;
        reported=r.sr_exec_time.tv_sec*1000000.0+r.sr_exec_time.tv_usec;
        if(result<0 || r.sr_io_status!=SR_IOST_GOOD || r.sr_scsi_status ||
           r.sr_dma_xfr!=512 || reported<0) {
            fprintf(stderr,"TIMING_FAIL i=%d ioctl=%d driver=%d scsi=%02x bytes=%d wall_us=%.3f reported_us=%.3f\n",
                i,result,r.sr_io_status,r.sr_scsi_status,r.sr_dma_xfr,wall,reported);
            return 1;
        }
        if(!reported) zero++;
        if(wall<=0) nonpositive++;
        if(reported>wall) { over++; if(reported-wall>maxOver) maxOver=reported-wall; }
        sumReported+=reported;
      }
      batchWall=stamp()-batchBegin;
      printf("TIMING batch=%d reads=%d wall_mean_us=%.3f reported_mean_us=%.3f zero=%d overshoots=%d max_over_us=%.3f nonpositive_wall=%d\n",
           batch,i,batchWall/i,sumReported/i,zero,over,maxOver,nonpositive);
      /* Individual gettimeofday samples can repeat or step backward on this
       * guest. Check aggregate duration; include all reads, do not discard
       * those samples. System-call and diagnostic overhead lies outside the
       * driver's reported interval. This is a sanity check, not a precision
       * calibration or a replacement for the frozen performance benchmark. */
      if(batchWall<=0 || sumReported>batchWall*1.05 ||
          sumReported<batchWall*0.30) { puts("TIMING_BATCH_FAIL"); return 1; }
    }
    puts("TIMING_READS_PASS");
    close(fd); free(data); return 0;
}
