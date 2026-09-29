CC = cc
CFLAGS = -Wall -I../AHCISCSIDriver_reloc.tproj
all: core_tests io_stress integrity_endurance native_scsi native_write native_timeout native_timing native_queued_timeout
core_tests: core_tests.c ../AHCISCSIDriver_reloc.tproj/AHCICore.c
	$(CC) $(CFLAGS) -o $@ core_tests.c ../AHCISCSIDriver_reloc.tproj/AHCICore.c
io_stress: io_stress.c
	$(CC) $(CFLAGS) -o $@ io_stress.c
integrity_endurance: integrity_endurance.c
	$(CC) $(CFLAGS) -o $@ integrity_endurance.c
native_scsi: native_scsi.c
	$(CC) $(CFLAGS) -o $@ native_scsi.c
native_write: native_write.c
	$(CC) $(CFLAGS) -o $@ native_write.c
native_timeout: native_timeout.c
	$(CC) $(CFLAGS) -o $@ native_timeout.c
native_timing: native_timing.c
	$(CC) $(CFLAGS) -o $@ native_timing.c
native_queued_timeout: native_queued_timeout.c
	$(CC) $(CFLAGS) -o $@ native_queued_timeout.c
test: core_tests
	./core_tests
