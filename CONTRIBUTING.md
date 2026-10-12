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
- CI is hardware-free: `.github/workflows/qa.yaml` runs the lints, `firmware/test/run_tests.sh`
  and `esphome config` on every board file on every push and PR, and a compile for the ESP32
  boards and the stock module when the firmware changed. The pre-commit hook (`git config core.hooksPath firmware/.githooks`) runs
  the same host tests.
- **Changelog:** a change under `firmware/esphome/components/` or `firmware/esphome/packages/`
  needs an entry under `## Unreleased` in `CHANGELOG.md`, and CI fails the pull request without
  one. Say what a node's owner will notice. Do not bump the version: that happens at release.
- C/C++ style and lint: run `firmware/scripts/cpp-lint.sh fix` before committing C/C++ changes; CI
  runs `cpp-lint.sh check` (clang-format 13.0.1, clang-tidy 22.1.8, ESPHome-derived rules).
- No em dashes in new markdown: the pre-commit hook blocks them.

## Releasing

For the maintainer. The firmware version lives in one file,
`firmware/esphome/packages/version.yaml`, and a release is the tag `v<version>` on `main`.

1. **Pick the number** (semver). Patch for fixes. Minor for something new that leaves existing
   nodes as they are. Major when a node's owner has to act: an entity renamed or removed, a new
   required secret or substitution, a changed default that alters behaviour.
2. **Bump and date it**, in one commit on `main` (through a pull request like any change): set
   `version:` in `firmware/esphome/packages/version.yaml`, and in `CHANGELOG.md` rename
   `## Unreleased` to `## X.Y.Z - YYYY-MM-DD` and put a new, empty `## Unreleased` above it.
   If the tree already has the number that is being released (as for 2.0.0), only the changelog
   changes.
3. **Check it:** `python3 firmware/scripts/version-lint.py --tag vX.Y.Z` has to pass on that
   commit. It is the check the release workflow starts with. For the whole workflow without a
   release, `gh workflow run esphome-release.yaml --ref main` is a dry run: every gate and every
   compile, and the run's summary shows the release text. It creates no tag and no release.
4. **Tag and push the tag:** `git tag -s vX.Y.Z -m "ESPHome firmware X.Y.Z"` on the merged commit,
   then `git push origin vX.Y.Z`.

The push starts `.github/workflows/esphome-release.yaml`. It refuses a tag that is not the
version or not on `main`, runs the host tests, compiles every board file, and creates the GitHub
release with the changelog section as its text. No firmware image is attached, because an image
built in CI holds the placeholder secrets. If a gate fails, nothing was published: fix `main`,
delete the tag (`git push --delete origin vX.Y.Z`), and tag the fixed commit.

The tags `amebaz2-v*`, `esp32-v*` and `matter-final` belong to the retired Matter firmwares and
stay as history.

By contributing you agree your contribution is licensed under the repo's [MIT license](LICENSE).
