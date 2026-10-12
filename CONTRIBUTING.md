# Contributing

A hobby reverse-engineering / de-cloud project. PRs and issues welcome. A few rules are specific
to how it is built and tested.

## What's welcome
- Bug fixes in the ESPHome component, and host-test coverage for them.
- Reverse-engineering corrections (with capture evidence): the protocol tables are sniff-validated;
  if you find a discrepancy on your unit, include the frames.
- Support for **other Hisense A/C models** on the same bus (different byte offsets, etc.).
- Hardware reports: another board, the stock module's other flash layouts, a longer run.
- Docs.

The Matter firmwares are retired (issue #182, tag `matter-final`). Changes to them are not taken.

## Ground rules

### 1. Never commit secrets or personal data
- `dumps/` (raw flash, Wi-Fi creds + the device RSA key + the vendor firmware blob) is gitignored.
  **Never** un-ignore it or paste its contents. Same for `secrets.yaml`, `.env` files and tokens.
- Use placeholders in anything tracked: `example.com`, `you@example.com`, `192.168.1.x`,
  `/home/you/…`. No real hostnames, LAN IPs, or emails.

### 2. Don't vendor third-party code
ESPHome, LibreTiny and the SDKs they download are fetched at build time. Realtek's SDK is
proprietary: a file with a "confidential property of RealTek" banner never goes into this
repository.

### 3. Test without hardware first
```bash
firmware/test/run_tests.sh     # codec goldens, the component against the reference driver, the virtual A/C
```
Protocol code in the component (`hisense_protocol.*`, `hisense_map.h`, `hisense_bus.*`) has no
ESPHome dependency, so it runs on the host. Put any new decision there as a pure function and add
a `CHECK(...)` case for it. Don't leave logic that could be tested in the ESPHome glue.

`firmware/test/reference-driver/` is frozen. The parity test compares the component's codec with
it, so a protocol change usually has to be made on both sides. Say in the commit which capture or
RE doc justifies it.

### 4. Match the surrounding code
Follow the existing style, comment density, and naming. Explain *why* for any protocol byte value
(it should trace to a capture or the RE docs); mark an unverified one `// VERIFY`.

### 5. Say what was tested on hardware
A change that touches the bus should say what it ran on: a board and an A/C, the simulator, or
nothing. "Compiles" and "ran on a unit" are different claims, and the docs keep them apart.

## Commits & PRs
- Small, focused commits with a clear message; reference the RE doc / capture that justifies a change.
- **AI assistance:** if you used an AI tool, add an `Assisted-by: AI` trailer (vendor-neutral). Do
  **not** use `Co-authored-by:` for AI.
- Open an issue for anything non-trivial before a big PR, so we can agree on the approach.
- CI is hardware-free: `.github/workflows/qa.yaml` runs the lints, `firmware/test/run_tests.sh`,
  `esphome config` on every board file and a compile for the stock module on every push and PR.
  The pre-commit hook (`git config core.hooksPath firmware/.githooks`) runs the same host tests.
- C/C++ style and lint: run `firmware/scripts/cpp-lint.sh fix` before committing C/C++ changes; CI
  runs `cpp-lint.sh check` (clang-format 13.0.1, clang-tidy 22.1.8, ESPHome-derived rules).
- No em dashes in new markdown: the pre-commit hook blocks them.

By contributing you agree your contribution is licensed under the repo's [MIT license](LICENSE).
