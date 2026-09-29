#define _POSIX_SOURCE
/* Read-only, fault-injected queue deadline check on AHCISCRATCH.
 * Mask that port's IE externally before running. Two generic-SCSI minors
 * avoid serialization in one generic-device instance. No driver fault hooks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <bsd/dev/scsireg.h>
#define LENGTH 131072
/* The native libc.h prototype is hidden by the POSIX wait() header mode. */
extern int ioctl(int, long, ...);

static double stamp(void)
{
    struct timeval t;
    if(gettimeofday(&t,0)) exit(1);
    return t.tv_sec*1000000.0+t.tv_usec;
}
static int selectScratch(const char *path,int controller,int target)
{
    scsi_adr_t address;
    scsi_req_t r;
    unsigned char data[36];
    int fd=open(path,O_RDWR);
    if(fd<0) { perror(path); return -1; }
    memset(&address,0,sizeof(address));
    address.sa_target=target;
    if(ioctl(fd,SGIOCCNTR,&controller)<0 || ioctl(fd,SGIOCSTL,&address)<0) return -1;
    memset(&r,0,sizeof(r)); memset(data,0,sizeof(data));
    ((unsigned char *)&r.sr_cdb)[0]=0x12;
    ((unsigned char *)&r.sr_cdb)[1]=1;
    ((unsigned char *)&r.sr_cdb)[2]=0x80;
    ((unsigned char *)&r.sr_cdb)[4]=15;
    r.sr_cdb_length=6; r.sr_dma_dir=SR_DMA_RD;
    r.sr_addr=(caddr_t)data; r.sr_dma_max=15; r.sr_ioto=5;
    if(ioctl(fd,SGIOCREQ,&r)<0 || r.sr_io_status!=SR_IOST_GOOD ||
       r.sr_scsi_status || r.sr_dma_xfr!=15 || data[3]!=11 ||
       memcmp(data+4,"AHCISCRATCH",11)) {
        fprintf(stderr,"Refusing test: expected AHCISCRATCH on %s\n",path);
        close(fd); return -1;
    }
    return fd;
}
static int runRead(int fd,int queued)
{
    unsigned char *data=malloc(LENGTH),*c;
    scsi_req_t r;
    double begin,elapsed;
    int result,i;
    if(!data) return 1;
    memset(data,0xa5,LENGTH); memset(&r,0,sizeof(r));
    c=(unsigned char *)&r.sr_cdb;
    c[0]=queued ? 0xa8 : 0x28; c[4]=queued ? 2 : 1;
    c[queued ? 8 : 7]=1; /* 256 blocks, 512 bytes each. */
    r.sr_cdb_length=queued ? 12 : 10; r.sr_dma_dir=SR_DMA_RD;
    r.sr_addr=(caddr_t)data; r.sr_dma_max=LENGTH; r.sr_ioto=queued ? 1 : 3;
    begin=stamp(); result=ioctl(fd,SGIOCREQ,&r); elapsed=stamp()-begin;
    printf("QUEUE_RESULT kind=%s ioctl=%d status=%d bytes=%d wall_us=%.0f reported=%ld.%06ld\n",
        queued ? "queued" : "active",result,r.sr_io_status,r.sr_dma_xfr,
        elapsed,r.sr_exec_time.tv_sec,r.sr_exec_time.tv_usec);
    fflush(stdout);
    if(result<0 || r.sr_io_status!=SR_IOST_IOTO || r.sr_dma_xfr ||
       elapsed<(queued ? 1500000.0 : 2500000.0) || elapsed>8000000.0) return 1;
    sleep(2);
    for(i=0;i<LENGTH;i++) if(data[i]!=0xa5) {
        fprintf(stderr,"Failed %s read changed caller memory\n",queued ? "queued" : "active");
        return 1;
    }
    free(data); return 0;
}
int main(int argc,char **argv)
{
    int a,b,controller,target,pipefd[2],first,second,status,pid,count=0,failed=0;
    char byte;
    if(argc!=5) {
        fprintf(stderr,"usage: native_queued_timeout sg-first sg-second controller target\n");
        return 2;
    }
    controller=atoi(argv[3]); target=atoi(argv[4]);
    a=selectScratch(argv[1],controller,target);
    b=selectScratch(argv[2],controller,target);
    if(a<0 || b<0 || pipe(pipefd)) return 1;
    fflush(stdout); first=fork(); if(first<0) return 1;
    if(!first) {
        close(pipefd[0]); close(b);
        if(write(pipefd[1],"R",1)!=1) _exit(1);
        close(pipefd[1]); _exit(runRead(a,0));
    }
    close(pipefd[1]);
    if(read(pipefd[0],&byte,1)!=1 || byte!='R') return 1;
    close(pipefd[0]); sleep(1);
    second=fork(); if(second<0) return 1;
    if(!second) { close(a); _exit(runRead(b,1)); }
    close(a); close(b);
    while((pid=wait(&status))>0) {
        count++;
        if((pid!=first && pid!=second) || !WIFEXITED(status) || WEXITSTATUS(status)) failed=1;
    }
    if(failed || count!=2) return 1;
    puts("PASS active and queued timeouts, zero transfers, both caller buffers unchanged");
    puts("REQUIRE_DRIVER_LOG queued port op a8 expired at one-second limit");
    return 0;
}
