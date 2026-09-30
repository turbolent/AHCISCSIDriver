# AHCISCSIDriver

AHCISCSIDriver is an OPENSTEP 4.2 SCSI controller driver for SATA/AHCI storage.
It exposes ATA disks and ATAPI CD/DVD drives through OPENSTEP's SCSI interface.
Each physical AHCI port maps to the corresponding SCSI target, LUN 0.

## Requirements

- PCI class `01:06:01` AHCI controller, configured for AHCI mode in firmware
- AHCI 1.0 through 1.3.1; MSI capability is required for the default mode
- Assigned memory BAR5 and DMA memory addressable below 4 GB
- Direct-attached ATA or ATAPI drives
- ATA disks with 512 to 4096-byte logical sectors; use 512-byte sectors for boot disks
- [PCIMSI](https://github.com/turbolent/PCIMSI) 0.32 (interface 5) installed and loaded before AHCISCSIDriver for MSI mode

The driver uses MSI by default, which requires [PCIMSI](https://github.com/turbolent/PCIMSI).
Polling is available as an explicit recovery or compatibility fallback,
which does not use PCIMSI or claim a DriverKit IRQ.

| `Interrupt Mode` | Requirement |
| --- | --- |
| `MSI` (default) | PCIMSI 0.32, interface 5 |
| `Polling` (fallback) | None |

NCQ, port multipliers, hotplug, legacy INTx, and sleep/resume are not supported.

## Installation

For the default MSI mode, first install PCIMSI 0.32.

Open `AHCISCSIDriver.config`.
Configure.app should open and confirm the driver was installed.
Select the SCSI devices panel, click Add,
select `SATA AHCI SCSI Storage Controller`,
and click Add.

If the driver is not shown,
check `Show All Installed Drivers`,
select `SATA AHCI SCSI Storage Controller`,
and click Add.
Then click Expert. For a controller listed in `Auto Detect IDs`, leave
`Location` empty so PCIBus selects the matching controller.
To select a controller explicitly, set `Location` to its PCI coordinates
using this exact syntax:

```text
Dev:<device> Func:<function> Bus:<bus>
```

For example, PCI bus 0, device 31, function 2 is `Dev:31 Func:2 Bus:0`.
The bus, device, and function must match the AHCI controller reported by the PCI enumeration.

The default `Auto Detect IDs` cover Intel `8086:2922` and `8086:2829`.
For another compatible controller, add its ID in device/vendor order;
for example, Intel `8086:1c02` becomes `0x1c028086`.

Click Done, click Save, and Quit.

Now verify that `/private/Drivers/i386/AHCISCSIDriver.config/Instance0.table`
contains the expected `"Location"` and `"Interrupt Mode"` values.

Then, in `/private/Drivers/i386/System.config/Instance0.table`,
verify that `PCIMSI` appears **after** `PCIBus` and that `AHCISCSIDriver`
appears **after** `PCIMSI` in `Boot Drivers` for MSI mode.
For polling mode, AHCISCSIDriver only needs to follow PCIBus.
Remove the IDE driver for this controller if it would also claim the same hardware;
retain drivers needed by other controllers.

For an AHCI boot disk, update `/etc/fstab` and any `rootdev` kernel flag
to the resulting SCSI device, for example `sd0a` instead of `hd0a`.
Check the actual disk number: other SCSI controllers can change the numbering.
The bootloader must also be able to read the disk through firmware.

Select AHCI mode in firmware before booting with this driver.
The driver does not switch a running controller from IDE or RAID mode.
Reboot to load the driver and apply the configuration.

## Acknowledgements

AHCISCSIDriver was developed using NVMeSCSIDriver and the IDE/ATA/ATAPI driver
as references for OPENSTEP integration and SCSI behavior,
together with NeXT's DriverKit headers and examples.
Hardware programming follows Intel's AHCI specification.
