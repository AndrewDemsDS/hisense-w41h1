# Fixing attestation (err 604) properly

> **HISTORICAL / CONDITIONAL.** This doc's central claim, that err 604 requires custom
> self-signed VID/PID certs before commissioning will work, is **contradicted** by
> [`10-firmware-ota-procedure.md`](10-firmware-ota-procedure.md)'s stock-recovery-image note: the **stock built-in
> test DAC/PAI/CD (VID `0xFFF1`)** commissioned fine on stock Home Assistant with no cert
> surgery, just an "uncertified device" warning. The recipe below only matters for a **real
> VID/PID** (production/retail cert chain) or a controller enforcing non-test-net DCL policy,
> not normal HIL/dev use. Kept for reference, not a required step.

Stock firmware ships test certs whose **Certification Declaration VID/PID doesn't cross-
reference** the DAC/PAI/Basic-Information → commissioners fail with
`err 604 (kCertificationDeclarationInvalidVendorId)`. The clean fix (no controller patch):
provision **self-consistent** attestation credentials.

## Generate matching DAC / PAI / CD with `chip-cert`

Pick a VID/PID and use it EVERYWHERE (CD, PAI, DAC subject, Basic Information cluster).
For local/dev use, the CSA **test** VID `0xFFF1` + a PID (e.g. `0x8004`) works because the
Matter **test** PAA/CD-signing keys are trusted by controllers running with test-net DCL,
but for zero-config commissioning on a *stock* controller, sign a CD with the test CD key
and keep VID/PID identical across all three.

```
# 1) Certification Declaration (must list the same VID + PID)
chip-cert gen-cd --key credentials/test/certification-declaration/Chip-Test-CD-Signing-Key.pem \
  --cert credentials/test/certification-declaration/Chip-Test-CD-Signing-Cert.pem \
  --out cd.der --format-version 1 --vendor-id 0xFFF1 --product-id 0x8004 \
  --device-type-id 0x0301 --certificate-id ZIG20142ZB330003-24 \
  --security-level 0 --security-info 0 --version-number 0x2694 --certification-type 0

# 2) PAI (from a test PAA) and 3) DAC (VID/PID in subject must match the CD)
chip-cert gen-att-cert --type i --subject-cn "Matter PAI" --subject-vid 0xFFF1 \
  --ca-key ...PAA-Key.pem --ca-cert ...PAA-Cert.pem --out-key pai.key --out pai.der
chip-cert gen-att-cert --type d --subject-cn "Matter DAC" --subject-vid 0xFFF1 \
  --subject-pid 0x8004 --ca-key pai.key --ca-cert pai.der --out-key dac.key --out dac.der
```

## Set the same VID/PID in the device

- **Basic Information cluster** VID/PID (in ZAP / the app) must equal the CD/DAC values.
- **CommissionableDataProvider / DeviceInstanceInfoProvider**: point at the new DAC/PAI/CD
  + keep the setup discriminator/passcode (or set your own).

## Flash into the factory-data partition

AmebaZ2 Matter stores factory data (DAC/PAI/CD + commissioning data) in a dedicated
partition read via the KV/`chip-factory` provider. Use the SDK's factory-data tool to pack
`dac.der/pai.der/cd.der` + discriminator/passcode into the factory image, then flash that
partition (matches what we found at flash ~`0x3da000` in the dump).

## Result

With CD/DAC/PAI/Basic-Info all sharing one VID/PID, `err 604` disappears and the module
commissions into a **stock** Home Assistant Matter server, no wheel patch, no downgrade.
