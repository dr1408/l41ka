# l41ka iPad8 17.1.1 static analysis

Target: iPad11,6 / j171aap / CPID 0x8020 / iPadOS 17.1.1 21B91.
Working comparison: iPhone XR / CPID 0x8020 / iOS 18.7.10 reaches `05ac:4c41` LaikaDFU.

## Runtime symptom

- XR: after `setup-iboot`, device eventually enumerates as `05ac:4c41` with LaikaDFU EP2 OUT bulk endpoint.
- iPad8: after the same host-side writes and `SetBootLR(0x100000000)`, device enumerates as Apple DFU `05ac:1229` with serial marker:

```text
PWND:[usbliter8] L41kA:[+]
```

That marker comes from the T8020 shellcode serial patch, not from LaikaDFU stage2. So it proves pwned DFU/ROM overlay is alive, but does not prove the stage2 patchfinder/laikadfu code reached USB enumeration.

## What setup-iboot actually patches

`src/rom-patcher/iBootPatcherSetup.cpp` T8020 path writes:

- LaikaDFU combined stage2 to `0x19c388000`.
- ROM overlay patches through writable copy base `0x19c378000`.
- boot trampoline at `0x19c018000` to branch to `0x19c388000`.
- `SetBootLR(0x100000000)`.

The hardcoded ROM patch anchors exist in the local T8020 SecureROM dump (`iBoot-3865.0.0.4.7`). Because XR and iPad8 are both CPID 0x8020 and report the same BootROM string, these ROM patch anchors are probably not the device-specific failure.

## DeviceTree USB/static offset comparison

Compared:

- `DeviceTree.j171aap.parsed.txt` (iPad8)
- `DeviceTree.n841ap_18.7.9.parsed.txt` (XR)

Relevant USB/DART nodes match:

```text
usb-complex reg: 0x39000000 size 0x100
usb-device  reg: 0x00100000 size 0x10000 relative to usb-complex => 0x39100000
DART USB    reg: 0x39900000 size 0x4000
otgphyctrl  reg: 0x39000000/0x39000060
clock-gates: usb-complex 0x66 0x67 0x68 0xf5, dart-usb 0xb1
```

These correspond to l41ka physical offsets:

```text
usb_complex = 0x239000000
usb_phy     = 0x239000064
dwc2_base   = 0x239100000
dart_base   = 0x239900000
```

So current evidence does **not** point to iPad8 having different USB MMIO/DART base addresses.

## Patchfinder static result on iPad8 17.1.1 iBoot/iBEC/iBSS

Using l41ka's host patchfinder harness against:

- `iPad11,6_17.1.1_21B91_iBoot.plain`
- `iPad11,6_17.1.1_21B91_iBEC.plain`
- `iPad11,6_17.1.1_21B91_iBSS.plain`

Current unmodified harness returns:

```text
patchfinder returned -7
no patch writes recorded
```

Instrumented discovery status:

```text
PF 01 AutobootOncePatcher          required=1 found=1
PF 02 AutobootPatcher              required=1 found=1
PF 03 ForceLocalAutobootPatcher    required=0 found=0
PF 04 IgnoreBootCommandPatcher     required=0 found=0
PF 05 ClearBootdelayPatcher        required=0 found=0
PF 06 WatchdogCallsPatcher         required=1 found=1
PF 07 Stage2Patcher                required=1 found=0   <-- first fatal missing patch
PF 08 ReconfigPatcher              required=1 found=1
PF 09 AESPatcher                   required=1 found=0   <-- also missing
PF 10 RVBARPatcher                 required=1 found=1
PF 11 FuseStayPatcher              required=1 found=1
PF 12 FuseLockPatcher              required=1 found=1
PF 13 FuseDebugPatcher             required=1 found=1
PF 14 ManifestHardwarePatcher      required=0 found=0
PF 15 ManifestDigestPatcher        required=0 found=0
PF 16 SVCPatcher                   required=1 found=0   <-- also missing
```

Same result for iBoot/iBEC/iBSS because those local 17.1.1 files are effectively the same iBoot payload layout for this purpose.

## Key correction

The iPad8 failure is most likely **not** Pico/libusb/timing and not USB MMIO base mismatch.

The static failure is in the combined stage2 patchfinder for iBoot-10151.42.2:

1. `Stage2Patcher` cannot find the version-string location using its current signature.
2. `AESPatcher` cannot find its target using the iOS 18-oriented signature.
3. `SVCPatcher` cannot find the SVC handler to redirect into LaikaDFU.

Because patchfinder returns `-7`, the stage2 wrapper spins before patching/entering LaikaDFU. From host side, setup still appears successful because `SetBootLR` and DFU abort were submitted successfully; the runtime patchfinder failure is silent unless we read the mailbox or add visible stage2 diagnostics.

## Next static work

Port patchfinder signatures for iBoot-10151.42.2:

1. Stage2Patcher:
   - fallback to direct search for the early `iBoot-10151.42.2` string with enough zero padding and patch there.
   - local offset candidate: `0x280` in the raw iBoot image.

2. SVCPatcher:
   - unique `svc #7` exists at file offset `0x580cc` (assuming base `0x19c050000`, VA `0x19c0a80cc`).
   - the containing function begins around `0x580bc` but does not match the current handler signature.
   - build a 17.1.1-specific resolver around the unique `svc #7` wrapper or its caller.

3. AESPatcher:
   - current two-call `movz w0,#4<<16 ... movz w0,#8<<16` signature does not exist in 17.1.1.
   - either make AES patch optional for this path if not needed, or locate the equivalent function statically.


## Patchfinder fix implemented

Code changed in `src/payloads/iboot/patchfinder/patchfinder.cpp`:

1. `Stage2Patcher` now has a fallback that directly finds an early `iBoot-...`/`mBoot-...` version string with enough zero padding and patches that padding with ` (l41ka stage2 loader)`.
2. `AESPatcher` is now optional. If its old signature is found it still patches, but missing AES no longer aborts the entire iBoot setup path.
3. `SVCPatcher` now records the unique `svc #7` instruction. If primary/fallback handler signatures fail, it uses `functionStart()` around the unique `svc #7` as the SVC handler target.

Validation on iPad8 17.1.1 `iBoot-10151.42.2` now succeeds:

```text
patchfinder returned 0
Stage2Patcher offset=0x00000290
SVCPatcher offset=0x000580cc..0x000580ec
```

XR 18.7.10 iBoot was downloaded with `ipsw` from AppleDB URL:

```text
iPhone11,8_18.7.10_22H374_Restore.ipsw
Firmware/all_flash/iBoot.n841.RELEASE.im4p
iBoot-11881.140.96.700.4
```

It was unwrapped/decompressed to:

```text
/Project-Cascadia/xr_18.7.10_22H374/iBoot.n841.plain
```

Patchfinder still succeeds on XR 18.7.10 after the changes. XR keeps its normal stronger matches, including AESPatcher and SVC handler signature:

```text
patchfinder returned 0
Stage2Patcher offset=0x0013fbc8
AESPatcher offset=0x000381dc
SVCPatcher offset=0x000b9ac0..0x000b9ae0
```

## Correction: the unique `svc #7` wrapper is NOT the right SVC patch target

The previous iPad8 attempt used the unique `svc #7` wrapper at file offset `0x580bc` as a fallback SVC target. That was wrong.

Disassembly proves it is only a tiny wrapper/panic path, not the full EL handoff handler that l41ka expects to overwrite:

```text
0x19c0a80bc: pacibsp
0x19c0a80c0: stp x29, x30, [sp, #-0x10]!
0x19c0a80c4: mov x29, sp
0x19c0a80c8: mov w2, #1
0x19c0a80cc: svc #7
0x19c0a80d0: mov x0, #0x843
...
```

Patching at `resolved + 0x10` for this function starts at the `svc #7` itself and spills into the next function. That is not equivalent to the XR iOS 18.7.10 target.

## Correct iPad8 iOS 17.1.1 SVC handler candidate

Static analysis found the iOS17-style handler at file offset `0x79898` (`0x19c0c9898` with the harness base). This function has the expected shape for the l41ka SVC patch:

```text
0x19c0c9898: pacibsp
0x19c0c989c: sub sp, sp, #0x50
0x19c0c98a0: stp x22, x21, [sp, #0x20]
0x19c0c98a4: stp x20, x19, [sp, #0x30]
0x19c0c98a8: stp x29, x30, [sp, #0x40]
0x19c0c98ac: add x29, sp, #0x40
...
0x19c0c98e8: mov x19, x0
0x19c0c98ec: mov x20, x1
0x19c0c98f0: mov x21, x2
0x19c0c98f4: mov x22, x3
```

The current patchfinder now matches this iOS17 handler signature directly instead of using the bad `svc #7` fallback.

Validated patch writes for iPad8 17.1.1 now are:

```text
patchfinder returned 0
Stage2Patcher offset=0x00000290
SVCPatcher offset=0x000798a8..0x000798c8
```

This also validates against the matching iBEC and iBSS files, and XR 18.7.10 remains unchanged:

```text
XR iBoot-11881.140.96.700.4:
Stage2Patcher offset=0x0013fbc8
AESPatcher offset=0x000381dc
SVCPatcher offset=0x000b9ac0..0x000b9ae0
```

Meaning: XR still uses the original strong signatures. iPad8 iBoot-10151.42.2 uses the new iOS17-specific SVC handler signature.
