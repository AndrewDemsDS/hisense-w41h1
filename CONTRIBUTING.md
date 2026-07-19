# Contributing

A hobby reverse-engineering / de-cloud project. PRs and issues welcome. A few rules are specific
to how it builds.

## What's welcome
- Bug fixes in the RS-485 driver / Matter glue, and host-test coverage for them.
- Reverse-engineering corrections (with capture evidence): the protocol tables are sniff-validated;
  if you find a discrepancy on your unit, include the frames.
- Support for **other Hisense A/C models** on the same AmebaZ2 module (different byte offsets, etc.).
- Docs, the ESP32-replacement path, and the fleet-OTA research.

## Ground rules

### 1. Never commit secrets or personal data
- `dumps/` (raw flash, Wi-Fi creds + the device RSA key + the vendor firmware blob) is gitignored.
  **Never** un-ignore it or paste its contents. Same for `.env` files, tokens, and pairing/verifier
  material.
- Use placeholders in anything tracked: `example.com`, `you@example.com`, `192.168.1.x`,
  `/home/you/…`. No real hostnames, LAN IPs, or emails.

### 2. Don't vendor the proprietary SDK
The Realtek AmebaZ2 SDK (and its Matter component) is **proprietary, not redistributable**. Changes
to SDK files ship as **patches**, never as copied vendor source:
- Base-SDK / connectedhomeip file edits → regenerate the patch: `git -C $SDK diff > patches/<sdk>.patch`.
- Realtek **Matter-overlay** files (untracked, no git baseline) → add an idempotent step to
  `scripts/apply-matter-edits.sh` and document the exact edit in `firmware/src/sdk-edits/README.md`.
- Files that are *our* original code (`firmware/src/rs485-driver/*`, the `.zap`, the mfg-cluster def)
  stay as source under MIT.
If you're unsure whether a file is ours or Realtek's, check its header, a "confidential property of
RealTek" banner means **patch/document it, don't commit it**.

### 3. Test without hardware first
```bash
firmware/test/run_tests.sh     # host codec + Matter-map tests + the virtual-A/C round-trip
```
The mapping logic lives in `firmware/src/rs485-driver/matter_aircon_map.h` (pure, host-compilable),
add a `CHECK(...)` case in `firmware/test/test_matter_map.cpp` for any new byte mapping. Keep the
protocol layer host-testable; don't move logic that could be tested into the CHIP-only glue.

### 4. Match the surrounding code
Follow the existing style, comment density, and naming. The driver is C++11, no exceptions/RTTI.
Explain *why* for any protocol byte value (it should trace to a capture or the RE docs).

### 5. Credentials stay test-only
This is a DIY device on CSA **test** attestation (VID `0xFFF1`). Don't add real DACs / a CSA VID,
that's out of scope (and can't be redistributed). `gen-creds.sh` handles per-unit codes.

## Commits & PRs
- Small, focused commits with a clear message; reference the RE doc / capture that justifies a change.
- **AI assistance:** if you used an AI tool, add an `Assisted-by: AI` trailer (vendor-neutral). Do
  **not** use `Co-authored-by:` for AI.
- Open an issue for anything non-trivial before a big PR, so we can agree on the approach.
- CI is host-only (no hardware): `.github/workflows/qa.yaml` runs `ota-release.sh lint` (the same
  gate as the pre-commit hook) on every push/PR, make sure it passes.
- Bumping the firmware version = edit **`firmware/src/version.txt`** (the git-tracked source of
  truth) and commit it; on a PR, CI requires it to strictly increase vs the target branch.

By contributing you agree your contribution is licensed under the repo's [MIT license](LICENSE).
