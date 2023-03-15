/** @file
  Copyright (c) 2020, zhen-zen. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_version.hpp>
#include "BrightnessKeys.hpp"

bool ADDPR(debugEnabled) = false;
uint32_t ADDPR(debugPrintDelay) = 0;

#define kDeliverNotifications   "RM,deliverNotifications"

// Constants for keyboards
enum
{
    kPS2M_notifyKeyTime = iokit_vendor_specific_msg(110),       // notify of timestamp a non-modifier key was pressed (data is uint64_t*)
    kPS2K_notifyKeystroke = iokit_vendor_specific_msg(202),     // notify of key press (data is PS2KeyInfo*)
};

typedef struct PS2KeyInfo
{
    uint64_t time;
    UInt16  adbKeyCode;
    bool    goingDown;
    bool    eatKey;
} PS2KeyInfo;

// Constants for brightness keys

#define kBrightnessPanel                    "BrightnessPanel"
#define kBrightnessKey                      "BrightnessKeyRouted"

#define BRIGHTNESS_DOWN         0x6b
#define BRIGHTNESS_UP           0x71

#define super IOHIKeyboard

OSDefineMetaClassAndStructors(BrightnessKeys, super)

IORegistryEntry* BrightnessKeys::getDeviceByAddress(IORegistryEntry *parent, UInt64 address, UInt64 mask) {
    IORegistryEntry* child = NULL;
    auto iter = parent->getChildIterator(gIODTPlane);
    if (iter) {
        IORegistryEntry* dev;
        UInt64 addr;
        while ((dev = (IORegistryEntry*)iter->getNextObject())) {
            auto location = dev->getLocation();
            // The device need to be present in ACPI scope and follow the naming convention ('A'-'Z', '_')
            auto name = dev->getName();
            if (location && name && name [0] <= '_' &&
                sscanf(dev->getLocation(), "%llx", &addr) == 1 &&
                (addr & mask) == address) {
                child = dev;
                break;
            }
        }
    }
    OSSafeReleaseNULL(iter);
    return child;
}

void BrightnessKeys::getBrightnessPanel() {
    auto info = DeviceInfo::create();
    
    auto getAcpiDevice = [](IORegistryEntry *dev) -> IOACPIPlatformDevice * {
        if (dev == nullptr)
            return nullptr;
        
        auto path = OSDynamicCast(OSString, dev->getProperty("acpi-path"));
        if (path != nullptr) {
            auto p = IORegistryEntry::fromPath(path->getCStringNoCopy());
            auto r = OSDynamicCast(IOACPIPlatformDevice, p);
            if (r) 
                return r;
            OSSafeReleaseNULL(p);
        }
        return nullptr;
    };
    
    if (!info)
        return;
    
    if (info->videoBuiltin != nullptr) {
        //
        // ACPI Spec B.5.1 _ADR (Return the Unique ID for this Device)
        //
        // This method returns a unique ID representing the display
        // output device. All output devices must have a unique hardware
        // ID. This method is required for all The IDs returned by this
        // method will appear in the list of hardware IDs returned by the
        // _DOD method.
        //
        _panel = getAcpiDevice(getDeviceByAddress(info->videoBuiltin, kIOACPILCDPanel, kIOACPIDisplayTypeMask));
        
        //
        // On some laptops, like AMD laptops, the panel can be of legacy type.
        //
        if (_panel == nullptr) { _panel = getAcpiDevice(getDeviceByAddress(info->videoBuiltin, kIOACPILegacyPanel)); }

        //
        // On some newer laptops, address of Display Output Device (DOD)
        // may not export panel information. We can verify it by whether
        // a DOD of CRT type present, which should present when types are
        // initialized correctly. If not, use DD1F instead.
        //
        if (_panel == nullptr && !getDeviceByAddress(info->videoBuiltin, kIOACPICRTMonitor)) {
            auto defaultPanel = info->videoBuiltin->childFromPath("DD1F", gIODTPlane);
            if (defaultPanel != nullptr) {
                _panel = getAcpiDevice(defaultPanel);
                defaultPanel->release();
            }
        }
        
        //
        // Some vendors just won't follow the specs and update their code
        //
        if (_panel == nullptr || strncmp(_panel->getName(), "DD02", strlen("DD02"))) {
            auto fallbackPanel = info->videoBuiltin->childFromPath("DD02", gIODTPlane);
            if (fallbackPanel != nullptr) {
                _panelFallback = getAcpiDevice(fallbackPanel);
                fallbackPanel->release();
            }
        }
    }
    
    for (size_t i = 0; i < info->videoExternal.size(); ++i)
    if ((_panelDiscrete = getAcpiDevice(getDeviceByAddress(info->videoExternal[i].video, kIOACPILCDPanel, kIOACPIDisplayTypeMask))) ||
        (_panelDiscrete = getAcpiDevice(getDeviceByAddress(info->videoExternal[i].video, kIOACPILegacyPanel))))
        break;
    
    DeviceInfo::deleter(info);
}

bool BrightnessKeys::start(IOService *provider) {
    if (!super::start(provider))
        return false;

    setProperty("VersionInfo", kextVersion);

    ADDPR(debugEnabled) = checkKernelArgument("-brkeysdbg") || checkKernelArgument("-liludbgall");
    PE_parse_boot_argn("liludelay", &ADDPR(debugPrintDelay), sizeof(ADDPR(debugPrintDelay)));

    workLoop = IOWorkLoop::workLoop();
    commandGate = IOCommandGate::commandGate(this);
    if (!workLoop || !commandGate || (workLoop->addEventSource(commandGate) != kIOReturnSuccess)) {
        SYSLOG("brkeys", "failed to add commandGate");
        return false;
    }

    _notificationServices = OSSet::withCapacity(1);
    _deliverNotification = OSSymbol::withCString(kDeliverNotifications);
    if (!_notificationServices || !_deliverNotification) {
        SYSLOG("brkeys", "failed to add notification service");
        return false;
    }

    OSDictionary * propertyMatch = propertyMatching(_deliverNotification, kOSBooleanTrue);
    if (propertyMatch) {
        IOServiceMatchingNotificationHandler notificationHandler = OSMemberFunctionCast(IOServiceMatchingNotificationHandler, this, &BrightnessKeys::notificationHandler);

        //
        // Register notifications for availability of any IOService objects wanting to consume our message events
        //
        _publishNotify = addMatchingNotification(gIOFirstPublishNotification, propertyMatch, notificationHandler, this, 0, 10000);
        _terminateNotify = addMatchingNotification(gIOTerminatedNotification, propertyMatch, notificationHandler, this, 0, 10000);

        propertyMatch->release();
    }

    // get IOACPIPlatformDevice for built-in panel
    getBrightnessPanel();
    
    if (_panel != NULL)
        _panelNotifiers = _panel->registerInterest(gIOGeneralInterest, _panelNotification, this);
    
    if (_panelFallback != NULL)
        _panelNotifiersFallback = _panelFallback->registerInterest(gIOGeneralInterest, _panelNotification, this);
    
    if (_panelDiscrete != NULL)
        _panelNotifiersDiscrete = _panelDiscrete->registerInterest(gIOGeneralInterest, _panelNotification, this);
    
    if (_panelNotifiers == NULL && _panelNotifiersFallback == NULL && _panelNotifiersDiscrete == NULL) {
        SYSLOG("brkeys", "unable to register any interests for GFX notifications");
        return false;
    }
    
    registerService();

    return true;
}

void BrightnessKeys::stop(IOService *provider) {
    //
    // Release ACPI provider for panel and PS2K ACPI device
    //
    if (_panel && _panelNotifiers)
        _panelNotifiers->remove();
    
    if (_panelFallback && _panelNotifiersFallback)
        _panelNotifiersFallback->remove();
    
    if (_panelDiscrete && _panelNotifiersDiscrete)
        _panelNotifiersDiscrete->remove();
    
    OSSafeReleaseNULL(_panel);
    OSSafeReleaseNULL(_panelFallback);
    OSSafeReleaseNULL(_panelDiscrete);

    _publishNotify->remove();
    _terminateNotify->remove();
    _notificationServices->flushCollection();
    OSSafeReleaseNULL(_notificationServices);
    OSSafeReleaseNULL(_deliverNotification);

    workLoop->removeEventSource(commandGate);
    OSSafeReleaseNULL(commandGate);
    OSSafeReleaseNULL(workLoop);

    super::stop(provider);
}

IOReturn BrightnessKeys::_panelNotification(void *target, void *refCon, UInt32 messageType, IOService *provider, void *messageArgument, vm_size_t argSize) {
    if (messageType == kIOACPIMessageDeviceNotification) {
        if (NULL == target) {
            DBGLOG("brkeys", "%s kIOACPIMessageDeviceNotification target is null", provider->getName());
            return kIOReturnError;
        }
        
        auto self = OSDynamicCast(BrightnessKeys, reinterpret_cast<OSMetaClassBase*>(target));
        if (NULL == self) {
            DBGLOG("brkeys", "%s kIOACPIMessageDeviceNotification target is not a ApplePS2Keyboard", provider->getName());
            return kIOReturnError;
        }
        
        if (NULL != messageArgument) {
            PS2KeyInfo info;
            UInt32 arg = *static_cast<UInt32*>(messageArgument);
            switch (arg) {
                case kIOACPIMessageBrightnessUp:
                    info.adbKeyCode = BRIGHTNESS_UP;
                    info.eatKey = false;
                    info.goingDown = true;
                    clock_get_uptime(&info.time);
                    self->dispatchMessage(kPS2M_notifyKeyTime, &info.time);
                    self->dispatchMessage(kPS2K_notifyKeystroke, &info);
                    // keyboard consume the message
                    if (info.eatKey) {
                        info.eatKey = false;
                        info.goingDown = false;
                        clock_get_uptime(&info.time);
                        self->dispatchMessage(kPS2K_notifyKeystroke, &info);
                    } else {
                        clock_get_uptime(&info.time);
                        self->dispatchKeyboardEventX(BRIGHTNESS_UP, true, info.time);
                        clock_get_uptime(&info.time);
                        self->dispatchKeyboardEventX(BRIGHTNESS_UP, false, info.time);
                    }
                    DBGLOG("brkeys", "%s ACPI brightness up", provider->getName());
                    break;
                    
                case kIOACPIMessageBrightnessDown:
                    info.adbKeyCode = BRIGHTNESS_DOWN;
                    info.eatKey = false;
                    info.goingDown = true;
                    clock_get_uptime(&info.time);
                    self->dispatchMessage(kPS2M_notifyKeyTime, &info.time);
                    self->dispatchMessage(kPS2K_notifyKeystroke, &info);
                    // keyboard consume the message
                    if (info.eatKey) {
                        info.eatKey = false;
                        info.goingDown = false;
                        clock_get_uptime(&info.time);
                        self->dispatchMessage(kPS2K_notifyKeystroke, &info);
                    } else {
                        clock_get_uptime(&info.time);
                        self->dispatchKeyboardEventX(BRIGHTNESS_DOWN, true, info.time);
                        clock_get_uptime(&info.time);
                        self->dispatchKeyboardEventX(BRIGHTNESS_DOWN, false, info.time);
                    }
                    DBGLOG("brkeys", "%s ACPI brightness down", provider->getName());
                    break;
                    
                case kIOACPIMessageBrightnessCycle:
                case kIOACPIMessageBrightnessZero:
                case kIOACPIMessageBrightnessOff:
                    DBGLOG("brkeys", "%s ACPI brightness operation 0x%02x not implemented", provider->getName(), *((UInt32 *) messageArgument));
                    return kIOReturnSuccess;
                    
                default:
                    DBGLOG("brkeys", "%s unknown ACPI notification 0x%04x", provider->getName(), *((UInt32 *) messageArgument));
                    return kIOReturnSuccess;
            }
            if (!self->_panelNotified) {
                self->_panelNotified = true;
                self->setProperty(kBrightnessPanel, safeString(provider->getName()));
                if (arg == kIOACPIMessageBrightnessUp || arg == kIOACPIMessageBrightnessDown)
                    self->setProperty(kBrightnessKey, info.eatKey);
            }
        } else {
            DBGLOG("brkeys", "%s received unknown kIOACPIMessageDeviceNotification", provider->getName());
        }
    } else {
        DBGLOG("brkeys", "%s received %08X", provider->getName(), messageType);
    }
    return kIOReturnSuccess;
}

void BrightnessKeys::dispatchMessageGated(int* message, void* data)
{
    OSCollectionIterator* i = OSCollectionIterator::withCollection(_notificationServices);

    if (i) {
        while (IOService* service = OSDynamicCast(IOService, i->getNextObject())) {
            service->message(*message, this, data);
        }
        i->release();
    }
}

void BrightnessKeys::dispatchMessage(int message, void* data)
{
    if (_notificationServices->getCount() == 0) {
        SYSLOG("brkeys", "No available notification consumer");
        return;
    }
    commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &BrightnessKeys::dispatchMessageGated), &message, data);
}

void BrightnessKeys::notificationHandlerGated(IOService *newService, IONotifier *notifier)
{
    if (notifier == _publishNotify) {
        DBGLOG("brkeys", "Notification consumer published: %s", safeString(newService->getName()));
        _notificationServices->setObject(newService);
    }

    if (notifier == _terminateNotify) {
        DBGLOG("brkeys", "Notification consumer terminated: %s", safeString(newService->getName()));
        _notificationServices->removeObject(newService);
    }
}

bool BrightnessKeys::notificationHandler(void *refCon, IOService *newService, IONotifier *notifier)
{
    commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &BrightnessKeys::notificationHandlerGated), newService, notifier);
    return true;
}

UInt32 BrightnessKeys::interfaceID() { return NX_EVS_DEVICE_INTERFACE_ADB; }

UInt32 BrightnessKeys::maxKeyCodes() { return NX_NUMKEYCODES; }

UInt32 BrightnessKeys::deviceType() { return 3; }

const unsigned char * BrightnessKeys::defaultKeymapOfLength(UInt32 * length)
{
    //
    // Keymap data borrowed and modified from IOHIDFamily/IOHIDKeyboard.cpp
    // references  http://www.xfree.org/current/dumpkeymap.1.html
    //             http://www.tamasoft.co.jp/en/general-info/unicode.html
    //
    static const unsigned char brightnessMap[] = {
        0x00,0x00, // use byte unit.
        
        // modifier definition
        0x00,   //Number of modifier keys.
        
        // ADB virtual key definitions
        0x2, // number of key definitions
        // ( modifier mask           , generated character{char_set,char_code}...         )
        0x00,0xfe,0x33, //6b F14
        0x00,0xfe,0x34, //71 F15
        
        // key sequence definition
        0x0, // number of of sequence definitions
        // ( num of keys, generated sequence characters(char_set,char_code)... )
        
        // special key definition
        0x2, // number of special keys
        // ( NX_KEYTYPE,        Virtual ADB code )
        NX_KEYTYPE_BRIGHTNESS_UP, 0x90,
        NX_KEYTYPE_BRIGHTNESS_DOWN,    0x91,
    };
    
    *length = sizeof(brightnessMap);
    return brightnessMap;
}
