/* Read-only counter snapshot for the disposable profile-source.py build. */
#import <driverkit/IODeviceMaster.h>
#import <driverkit/return.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    IODeviceMaster *master=[IODeviceMaster new];
    IOObjectNumber object;
    IOString kind,name;
    unsigned values[37],count,i;
    IOReturn result;
    if (!master) return 1;
    for(object=0;object<256;object++) {
        result=[master lookUpByObjectNumber:object deviceKind:&kind deviceName:&name];
        if (result==IO_R_NO_DEVICE) break;
        if (result!=IO_R_SUCCESS) continue;
        count=37;
        result=[master getIntValues:values forParameter:"AHCILabProfile" objectNumber:object count:&count];
        if (result!=IO_R_SUCCESS) continue;
        if (count!=37 || values[0]!=3) return 1;
        printf("PROFILE_DEVICE %s schema=%u\n",name,values[0]);
        for(i=0;i<12;i++)
            printf("PROFILE kind=%u count=%u cycles=%.0f\n",i,values[1+i*3],
                (double)values[3+i*3]*4294967296.0+values[2+i*3]);
        [master free]; return 0;
    }
    fprintf(stderr,"AHCI profile schema 3 not found\n");
    [master free]; return 1;
}
