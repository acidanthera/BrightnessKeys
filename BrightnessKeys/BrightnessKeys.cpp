/** @file
  Copyright (c) 2020, zhen-zen. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_version.hpp>
#include "BrightnessKeys.hpp"


// Constants for brightness keys

#define kBrightnessPanel                    "BrightnessPanel"
#define kBrightnessKey                      "BrightnessKey"

#ifdef DEBUG
#define DEBUG_LOG(args...)  do { IOLog(args); } while (0)
#else
#define DEBUG_LOG(args...)  do { } while (0)
#endif

#define BRIGHTNESS_DOWN         0x6b
#define BRIGHTNESS_UP           0x71

#define super IOHIKeyboard

OSDefineMetaClassAndStructors(BrightnessKeys, super)

bool BrightnessKeys::init() {
    if (!super::init())
        return false;
    // initialize ACPI support for brightness key
    _panel = 0;
    _panelFallback = 0;
    _panelDiscrete = 0;
    _panelNotified = false;
    _panelPrompt = false;
    _panelNotifiers = 0;
    _panelNotifiersFallback = 0;
    _panelNotifiersDiscrete = 0;
    return true;
}

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
        if (strncmp(_panel->getName(), "DD02", strlen("DD02"))) {
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

    // get IOACPIPlatformDevice for built-in panel
    getBrightnessPanel();
    
    if (_panel != NULL)
        _panelNotifiers = _panel->registerInterest(gIOGeneralInterest, _panelNotification, this);
    
    if (_panelFallback != NULL)
        _panelNotifiersFallback = _panelFallback->registerInterest(gIOGeneralInterest, _panelNotification, this);
    
    if (_panelDiscrete != NULL)
        _panelNotifiersDiscrete = _panelDiscrete->registerInterest(gIOGeneralInterest, _panelNotification, this);
    
    if (_panelNotifiers == NULL && _panelNotifiersFallback == NULL && _panelNotifiersDiscrete == NULL) {
        IOLog("ps2br: unable to register any interests for GFX notifications\n");
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
    super::stop(provider);
}

IOReturn BrightnessKeys::_panelNotification(void *target, void *refCon, UInt32 messageType, IOService *provider, void *messageArgument, vm_size_t argSize) {
    if (messageType == kIOACPIMessageDeviceNotification) {
        if (NULL == target) {
            DEBUG_LOG("%s kIOACPIMessageDeviceNotification target is null\n", provider->getName());
            return kIOReturnError;
        }
        
        auto self = OSDynamicCast(BrightnessKeys, reinterpret_cast<OSMetaClassBase*>(target));
        if (NULL == self) {
            DEBUG_LOG("%s kIOACPIMessageDeviceNotification target is not a ApplePS2Keyboard\n", provider->getName());
            return kIOReturnError;
        }
        
        if (NULL != messageArgument) {
            uint64_t now_abs;
            UInt32 arg = *static_cast<UInt32*>(messageArgument);
            switch (arg) {
                case kIOACPIMessageBrightnessUp:
                    clock_get_uptime(&now_abs);
                    self->dispatchKeyboardEventX(BRIGHTNESS_UP, true, now_abs);
                    clock_get_uptime(&now_abs);
                    self->dispatchKeyboardEventX(BRIGHTNESS_UP, false, now_abs);
                    DEBUG_LOG("%s %s ACPI brightness up\n", self->getName(), provider->getName());
                    break;
                    
                case kIOACPIMessageBrightnessDown:
                    clock_get_uptime(&now_abs);
                    self->dispatchKeyboardEventX(BRIGHTNESS_DOWN, true, now_abs);
                    clock_get_uptime(&now_abs);
                    self->dispatchKeyboardEventX(BRIGHTNESS_DOWN, false, now_abs);
                    DEBUG_LOG("%s %s ACPI brightness down\n", self->getName(), provider->getName());
                    break;
                    
                case kIOACPIMessageBrightnessCycle:
                case kIOACPIMessageBrightnessZero:
                case kIOACPIMessageBrightnessOff:
                    DEBUG_LOG("%s %s ACPI brightness operation 0x%02x not implemented\n", self->getName(), provider->getName(), *((UInt32 *) messageArgument));
                    return kIOReturnSuccess;
                    
                default:
                    DEBUG_LOG("%s %s unknown ACPI notification 0x%04x\n", self->getName(), provider->getName(), *((UInt32 *) messageArgument));
                    return kIOReturnSuccess;
            }
            if (!self->_panelNotified) {
                self->_panelNotified = true;
                self->setProperty(kBrightnessPanel, safeString(provider->getName()));
            }
        } else {
            DEBUG_LOG("%s %s received unknown kIOACPIMessageDeviceNotification\n", self->getName(), provider->getName());
        }
    } else {
        DEBUG_LOG("%s received %08X\n", provider->getName(), messageType);
    }
    return kIOReturnSuccess;
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



