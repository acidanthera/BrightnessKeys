/** @file
  Copyright (c) 2020, zhen-zen. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <IOKit/IOService.h>
#include <IOKit/hidsystem/IOHIKeyboard.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IODeviceTreeSupport.h>


//
// ACPI message and device type for brightness keys.
// See ACPI Specification, Appendix B: Video Extensions for details
//

#define kIOACPIMessageBrightnessCycle   0x85    // Cycle Brightness
#define kIOACPIMessageBrightnessUp      0x86    // Increase Brightness
#define kIOACPIMessageBrightnessDown    0x87    // Decrease Brightness
#define kIOACPIMessageBrightnessZero    0x88    // Zero Brightness
#define kIOACPIMessageBrightnessOff     0x89    // Display Device Off

#define kIOACPIDisplayTypeMask          0x0F00

#define kIOACPICRTMonitor               0x0100  // VGA* CRT or VESA* Compatible Analog Monitor
#define kIOACPILCDPanel                 0x0400  // Internal/Integrated Digital Flat Panel

#define kIOACPILegacyPanel              0x0110  // Integrated LCD Panel #1 using a common, backwards compatible ID

class BrightnessKeys : public IOHIKeyboard {
    OSDeclareDefaultStructors(BrightnessKeys)
private:
    // ACPI support for panel brightness
    IOACPIPlatformDevice *      _panel {nullptr};
    IOACPIPlatformDevice *      _panelFallback {nullptr};
    IOACPIPlatformDevice *      _panelDiscrete {nullptr};
    bool                        _panelNotified {false};
    IONotifier *                _panelNotifiers {nullptr};
    IONotifier *                _panelNotifiersFallback {nullptr};
    IONotifier *                _panelNotifiersDiscrete {nullptr};

    IOWorkLoop *workLoop {nullptr};
    IOCommandGate *commandGate {nullptr};

    IONotifier *_publishNotify {nullptr};
    IONotifier *_terminateNotify {nullptr};
    OSSet *_notificationServices {nullptr};
    const OSSymbol *_deliverNotification {nullptr};

    void dispatchMessageGated(int* message, void* data);
    bool notificationHandler(void * refCon, IOService * newService, IONotifier * notifier);
    void notificationHandlerGated(IOService * newService, IONotifier * notifier);

public:
    IORegistryEntry* getDeviceByAddress(IORegistryEntry *parent, UInt64 address, UInt64 mask = 0xFFFFFFFF);
    void getBrightnessPanel();
    static IOReturn _panelNotification(void *target, void *refCon, UInt32 messageType, IOService *provider, void *messageArgument, vm_size_t argSize);
    inline void dispatchKeyboardEventX(unsigned int keyCode, bool goingDown, uint64_t time)
    { dispatchKeyboardEvent(keyCode, goingDown, *(AbsoluteTime*)&time); }

    void dispatchMessage(int message, void* data);

    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    
    UInt32 deviceType() override;
    UInt32 interfaceID() override;
    UInt32 maxKeyCodes() override;
    const unsigned char * defaultKeymapOfLength(UInt32 * length) override;
};

