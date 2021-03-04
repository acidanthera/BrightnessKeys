BrightnessKeys
==============
[![Build Status](https://github.com/acidanthera/BrightnessKeys/workflows/CI/badge.svg?branch=master)](https://github.com/acidanthera/BrightnessKeys/actions) [![Scan Status](https://scan.coverity.com/projects/22193/badge.svg?flat=1)](https://scan.coverity.com/projects/22193)

Automatic handling of brightness keys based on ACPI Specification, Appendix B: Video Extensions.  
Requires Lilu 1.2.0 or newer.

#### Boot arguments
- `-brkeysdbg` to enable debug printing (available in DEBUG binaries).

#### Special cases
Typically no DSDT patches are required. Please remove old `_Qxx` to `XQxx` ones.  
On some models, may be required add ACPI patch `_OSI to XOSI` and `SSDT-XOSI`.  

<details>
<summary>Spoiler: On some old ThinkPad models, additional handling may be required.</summary>
<br>
Here is an example for their "brightness up" EC event.

```
Method (_Q14, 0, NotSerialized)
{
    If (^HKEY.MHKK (0x8000))
    {
        ^HKEY.MHKQ (0x1010)                // Vendor-specific event: TP_HKEY_EV_BRGHT_UP
    }

    If (NBCF) // Whether
    {
        If (VIGD)
        {
            Notify (^^^VID.LCD0, 0x86)     // Send 0x86 "Increase Brightness" to integrated graphics
        }
        Else
        {
            Notify (^^^PEG.VID.LCD0, 0x86) // Send 0x86 "Increase Brightness" to discrete graphics
        }
    }
    Else
    {
        Local0 = BRLV                      // Local variable to store current brightness level
        If ((Local0 != 0x0F))
        {
            Local0++
            BRLV = Local0
        }

        If (VIGD)
        {
            UCMS (0x16)                    // SMI access for integrated graphics
            BRNS ()
        }
        Else
        {
            VBRC (Local0)                  // SMI access for discrete graphics
        }

        ^HKEY.MHKQ (0x6050)                // Vendor-specific event: TP_HKEY_EV_BACKLIGHT_CHANGED
    }
}
```

When `NBCF` is set to zero by default, the method will not notify graphics devices and try to adjust brightness directly. To override that, set `NBCF = 0x01` in SSDT hotpatch, or just replace its declaration using a simple patch.

- For DSDT compiled with older iasl, replace `Name (NBCF, 0x00)` to `Name (NBCF, 0x01)`:  
Find: `08 4E424346 0A 00` `// NameOp "NBCF" BytePrefix "00"`  
Repl: `08 4E424346 0A 01` `// NameOp "NBCF" BytePrefix "01"`

- For DSDT compiled with newer iasl, replace `Name (NBCF, Zero)` to `Name (NBCF, One)`:  
Find: `08 4E424346 00` `// NameOp "NBCF" ZeroOp`  
Repl: `08 4E424346 01` `// NameOp "NBCF" OneOp`
</details>

#### Credits
- [Apple](https://www.apple.com) for macOS
- [usr-sse2](https://github.com/usr-sse2) for separating this driver from VoodooPS2
- [vit9696](https://github.com/vit9696) for `DeviceInfo` API from Lilu
- [zhen-zen](https://github.com/zhen-zen) for implementation
