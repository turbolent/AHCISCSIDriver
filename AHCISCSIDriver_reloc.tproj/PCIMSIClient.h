#ifndef PCIMSI_CLIENT_H
#define PCIMSI_CLIENT_H

#import "PCIMSICore.h"
#import <driverkit/return.h>
#import <mach/port.h>
#import <objc/Object.h>

#define PCIMSI_INTERFACE_VERSION 5U

/* Trusted, UP kernel-driver API. Every client uses the same acknowledged
 * notification contract, whether its hardware emits MSI or MSI-X.
 * Validate interface 5 and active service before allocating. Each allocation
 * is one vector, not a conventional multi-message MSI block.
 *
 * PCIMSI preallocates the notification target. Safe full-stack/low-IPL IRQs
 * notify directly; unsafe entries defer to its fair shared dispatcher.
 * Only one notification may be outstanding. After bounded device service,
 * with raw IF exclusion, consume returns real messages accumulated behind
 * that gate: nonzero permits another bounded pass; zero rearms the gate.
 * The last pass calls acknowledge, which rearms or sends one real-message-
 * backed follow-up. No device work, waiting or allocation under raw IF off.
 *
 * Prompt retry is generic and per owned vector. Enable before device MSI.
 * Subscribers share one idle LAPIC timer and one additional vector. BUSY or
 * RESOURCE are safe unavailability only while service remains active;
 * UNSUPPORTED means calibration was safely rolled back. Other failures are
 * fatal. Timer retry never polls devices or changes acknowledged semantics.
 *
 * Before release: stop device admission, mask/disable hardware interrupts,
 * verify readbacks, and leave sleepable context free of consumer locks.
 * Release drains provider activity and detaches this timer subscription.
 * The consumer must separately drain its already-queued port messages.
 * Failed release retains state and requires reboot; never free that owner.
 */
@interface Object (PCIMSIClientAPI)
- (unsigned)msiInterfaceVersion;
- (BOOL)isMSIServiceActive;
- (IOReturn)allocateAcknowledgedMSIVectorFor:(id)owner
                             interruptPort:(port_t)interruptPort
                                 messageID:(int)messageID
                                   message:(PCIMSIMessage *)message;
- (IOReturn)consumeAcknowledgedMSIVector:(unsigned)vector
                                 owner:(id)owner pending:(unsigned *)pending;
- (IOReturn)acknowledgeMSIVector:(unsigned)vector owner:(id)owner;
- (IOReturn)enableMSIPromptRetry:(unsigned)vector owner:(id)owner;
- (IOReturn)releaseMSIVector:(unsigned)vector owner:(id)owner;
@end

#endif
