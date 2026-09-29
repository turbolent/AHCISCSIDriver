#ifndef PCIMSI_CORE_H
#define PCIMSI_CORE_H

#define PCIMSI_VECTOR_FIRST 0xd0U
#define PCIMSI_VECTOR_LAST  0xdfU
#define PCIMSI_VECTOR_COUNT 16U

#define PCIMSI_MSI_ADDRESS_BASE 0xfee00000U
#define PCIMSI_VECTOR_REGISTER_MASK 0xffff0000U

#define PCIMSI_SLOT_UNAVAILABLE 0U
#define PCIMSI_SLOT_FREE        1U
#define PCIMSI_SLOT_PENDING     2U

#define PCIMSI_RELEASE_TIMEOUT_MS 2000U
#define PCIMSI_RELEASE_WAIT       0U
#define PCIMSI_RELEASE_COMPLETE   1U
#define PCIMSI_RELEASE_TIMED_OUT  2U

#define PCIMSI_DISPATCH_WAKE_NONE   0U
#define PCIMSI_DISPATCH_WAKE_NOW    1U
#define PCIMSI_DISPATCH_WAKE_DEFER  2U
#define PCIMSI_DISPATCH_MAX_WAKE_IPL 3U

#define PCIMSI_DELIVERY_INTERNAL 0U
#define PCIMSI_DELIVERY_ACKNOWLEDGED_DIRECT 2U

#define PCIMSI_ROUTE_STRAY  0U
#define PCIMSI_ROUTE_DEFER  1U
#define PCIMSI_ROUTE_DIRECT 2U

typedef struct PCIMSIMessage {
    unsigned vector;
    unsigned addressLow;
    unsigned addressHigh;
    unsigned data;
} PCIMSIMessage;

typedef struct PCIMSIIDTGateInfo {
    unsigned offset;
    unsigned selector;
    unsigned type;
    unsigned dpl;
    unsigned present;
} PCIMSIIDTGateInfo;

int PCIMSISelectAlignedPage(unsigned storageBase, unsigned storageSize,
                            unsigned pageSize, unsigned *pageBase);
int PCIMSIComposeMessage(unsigned apicID, unsigned vector,
                         PCIMSIMessage *message);
int PCIMSILVTConflictsWithVectorPool(unsigned lvt);
int PCIMSIAPICVectorStateIsClear(unsigned isr, unsigned irr, unsigned tmr);
unsigned PCIMSIClassifyAllocationSlot(int ownerPresent, unsigned releasing,
                                      unsigned pending);
unsigned PCIMSIClassifyReleaseDrain(unsigned testActive,
                                    unsigned callbackActive,
                                    unsigned pending,
                                    unsigned elapsedMilliseconds);
unsigned PCIMSIClassifyDispatcherWake(unsigned waiting,
                                      unsigned threadPresent,
                                      unsigned fullKernelStack,
                                      unsigned currentIPL);
unsigned PCIMSIClassifyInterruptRoute(unsigned deliveryKind,
                                      unsigned ownerPresent,
                                      unsigned targetPresent,
                                      unsigned fullKernelStack,
                                      unsigned currentIPL);
int PCIMSIValidNotificationMessageID(int messageID, int exitMessageID);
int PCIMSICanAcknowledgeDirect(unsigned deliveryKind, int ownerMatches,
                               unsigned outstanding, unsigned releasing);
int PCIMSIDispatchWaitMustCancel(unsigned stopping, unsigned waiting,
                                 unsigned pendingMask);
unsigned PCIMSISelectPendingIndex(unsigned pendingMask, unsigned startIndex);
unsigned PCIMSIReconcilePendingMask(
    const volatile unsigned *pendingCounts, unsigned count);
unsigned PCIMSISaturatingIncrement(unsigned value);
unsigned PCIMSISaturatingAdd(unsigned value, unsigned amount);
void PCIMSIDecodeIDTGate(const unsigned char gate[8],
                         PCIMSIIDTGateInfo *info);
void PCIMSIEncodeInterruptGate(unsigned char gate[8], unsigned offset,
                               unsigned selector);

#endif
