# Contributing

This is a hobby project, maintained in personal time. Pull requests and issues are welcome. This
page says what is useful, how to set up and test, the rules a change has to follow, and how a
release is made.

## What is welcome

- Bug fixes in the ESPHome components, with a host test that covers them.
- Corrections to the protocol, with the frames that show them. The list of what is still
  unsettled is
  [`reverse-engineering/docs/14-open-questions.md`](reverse-engineering/docs/14-open-questions.md):
  a capture from your unit can close one of them.
- Support for other Hisense A/C models on the same bus.
- Hardware reports: another board, the stock module's other flash layouts, a longer run.
- Documentation.

The Matter firmwares are retired (issue #182, tag `matter-final`). Changes to them are not taken.

Open an issue before a large change, so the approach can be agreed first.

## Set up

The host tests need `g++` and `python3` and nothing else.

```bash
git clone https://github.com/AndrewDemsDS/hisense-w41h1
cd hisense-w41h1
git config core.hooksPath firmware/.githooks     # optional: the pre-commit checks, per clone
python3 firmware/scripts/dev.py doctor           # what is installed and what is missing
```

To build firmware you also need ESPHome at the version the project pins (2026.10.0 at the time of
writing: `dev.py doctor` prints the pin, and `pipx install esphome==<pin>` installs it), and a
`firmware/esphome/secrets.yaml` copied from `secrets.yaml.example`. Building and flashing are
described in [`firmware/esphome/README.md`](firmware/esphome/README.md) and
[`docs/guide/Build-Flash-Test.md`](docs/guide/Build-Flash-Test.md).

## Test and lint

Run these before you push. They are what CI runs (`.github/workflows/qa.yaml`), and none of them
needs hardware.

```bash
firmware/test/run_tests.sh                    # every host test layer, a few seconds
python3 firmware/scripts/dev.py test          # the same, then `esphome config`
firmware/scripts/cpp-lint.sh check            # clang-format 13.0.1, the custom rules, clang-tidy 22.1.8
firmware/scripts/cpp-lint.sh fix              # applies what can be fixed, then reports the rest
ruff check .                                  # Python, rules in ruff.toml
git ls-files -z '*.sh' 'firmware/.githooks/*' | xargs -0 shellcheck --severity=warning
python3 docs/guide/lint.py                    # the guide: links, headings, sidebar, leak denylist
python3 firmware/scripts/version-lint.py      # version, changelog and the ESPHome pins agree
```

Slower checks, not part of `run_tests.sh`:

```bash
firmware/test/sanitize.sh                           # the C++ tests under ASan and UBSan, g++ and clang++, about 25 s
python3 firmware/test/host_e2e/run.py               # the real component on ESPHome's host platform, about 2 min, needs esphome
firmware/test/coverage/run.sh                       # line and branch coverage, needs gcovr (uvx fetches it)
firmware/test/fuzz/run.sh --smoke                   # replay the fuzz corpus (run_tests.sh already does this)
firmware/test/fuzz/run.sh <seconds> [harness...]    # fuzz, needs clang++ with libFuzzer
firmware/test/coverage/mutate.py --sample N         # mutation testing, minutes, needs uvx. Limits its own memory
```

What each layer covers is in [`docs/guide/Testing-and-QA.md`](docs/guide/Testing-and-QA.md).

### Cap the memory of the heavy ones

`mutate.py` limits itself. It runs at most four mutants at once (`--jobs`), gives each mutated
test binary a memory limit of its own (`--mem-limit`, 512 MB) and 20 s, keeps none of its output,
and refuses a `--jobs` that could claim more than a quarter of the machine's memory. It prints the
limits when it starts. It needs no wrapper, with one exception: `--sanitize` on a machine where
`systemd-run --user --scope` does not work has no memory limit, and says so.

A timed `fuzz/run.sh` runs every harness it is given in parallel, each a sanitizer process, and
does not limit itself. It can use up a desktop's memory and take the session with it. Run it in a
scope with a limit, so a runaway is killed alone:

```bash
systemd-run --user --scope -q -p MemoryMax=6G -p MemorySwapMax=0 -- \
    firmware/test/fuzz/run.sh 600 parsers
```

A process killed by the limit exits with 137. Name one or two harnesses per fuzz run, and run one
heavy command at a time. The same wrapper suits `host_e2e/run.py` and a full ESPHome compile on a
small machine.

### Known failures

A test that reproduces a defect which is not fixed yet stays in the suite as a known failure. It
passes while the defect is there and fails on the day the defect is fixed, with a message that
says to turn it into a plain check. The marker is `known_failure(...)` in the Python tests
(`firmware/test/edge_harness.py`, `host_e2e/scenarios.py`) and the list
`firmware/test/fuzz/regressions/KNOWN_FAILURES` for the fuzz properties. Do not delete a test
because it fails for a real defect, and do not mark a failure as known without saying which
defect it is.

## Ground rules

### 1. No secrets and no personal data

- `dumps/` is gitignored. A raw flash dump holds Wi-Fi credentials, the device's RSA key and the
  vendor's firmware. Never un-ignore it and never paste from it. The same goes for
  `secrets.yaml`, `.env` files and tokens.
- Anything tracked uses placeholders: `example.com`, `you@example.com`, `192.168.1.x`,
  `/home/you/...`. No real hostnames, LAN addresses, MAC addresses, serial numbers, Wi-Fi names,
  node numbers or emails. `docs/guide/lint.py` checks the guide for some of these. The rest of
  the tree is checked by the reviewer.

### 2. No third-party code in the tree

ESPHome, LibreTiny and the SDKs they download are fetched at build time. Realtek's SDK is
proprietary: a file with a "confidential property of RealTek" banner never goes into this
repository. [`NOTICE.md`](NOTICE.md) lists what the project depends on and what it took from
earlier community work. If a change brings in bytes or tables from another project, say so in the
pull request and add it there.

### 3. Logic goes where the host can test it

The protocol code of the component has no ESPHome dependency and runs on the host: the codec
(`hisense_protocol.*`), the bus scheduler (`hisense_bus.*`) and the decision headers
(`hisense_mapping.h`, `hisense_special.h`, `hisense_confirm.h`, `hisense_telemetry.h`,
`hisense_commands.h`, `hisense_entities.h`). `hisense_map.h` declares nothing: it only includes
them. The decisions of Wi-Fi setup mode are pure in the same way, in
`wifi_setup_mode/setup_mode_logic.h`. Put a new decision there as a pure function and add a
test for it. The ESPHome glue only runs in the host end-to-end test, where one case costs
seconds.

`firmware/test/reference-driver/` is frozen. The parity test compares the component's codec with
it, so a protocol change usually has to be made on both sides.

### 4. Every protocol byte has a source

A byte value in the codec traces to a capture, to the vendor firmware, or to a community project,
and the comment says which. A value that has not been confirmed on a unit is marked `// VERIFY`,
and gets a row in
[`reverse-engineering/docs/14-open-questions.md`](reverse-engineering/docs/14-open-questions.md).
The protocol reference is
[`reverse-engineering/docs/03-rs485-ac-protocol.md`](reverse-engineering/docs/03-rs485-ac-protocol.md):
a change to the codec that changes a fact there changes the page in the same pull request.

### 5. Say what was tested on hardware

A change that touches the bus says what it ran on: a board and an A/C, the simulator, or nothing.
"Compiles" and "ran on a unit" are different claims, and the docs keep them apart. Match the
surrounding code's style, naming and comment density.

### 6. Nothing goes to ESPHome upstream without the maintainer's decision

The components are kept in a shape that ESPHome could take
(`firmware/scripts/esphome-upstream-check.sh` runs ESPHome's own gates on them), and issue #192
tracks whether they are ready. Whether and when anything is proposed to `esphome/esphome`, to
`esphome-docs` or to LibreTiny is the maintainer's decision. Do not open a pull request or an
issue there on the project's behalf.

## Commits and pull requests

- Small, focused commits with a clear message that names the capture or the RE page behind a
  protocol change.
- If you used an AI tool, add an `Assisted-by: AI` trailer. Do not use `Co-authored-by:` for AI.
- A change under `firmware/esphome/components/` or `firmware/esphome/packages/` needs an entry
  under `## Unreleased` in `CHANGELOG.md`. CI fails the pull request without one. Write what the
  owner of a node will notice, in the group it belongs to (needs action, added, fixed, tooling,
  internals). Do not bump the version: that happens at release.
- Run `firmware/scripts/cpp-lint.sh fix` before committing C or C++.
- Prose: no em dashes in new Markdown (the pre-commit hook blocks them, see
  `firmware/scripts/stop-slop.sh`). Write a period, a comma, a colon or parentheses. Plain,
  direct sentences. State what is proven on hardware and what is not.
- The pre-commit hook runs the host tests when firmware, tests, scripts or the flasher are
  staged, the version lint when the changelog, a package or a workflow is staged, the guide
  linter for a guide page, and the C/C++ lint on staged sources. `git commit --no-verify`
  bypasses it. CI does not have a bypass.

CI on a pull request: the lints above, `run_tests.sh`, `esphome config` on every board file, a
compile of the ESP32 board files and of the stock module's three layouts when the firmware
changed, the host end-to-end test, and ESPHome's own gates at the pinned release. A nightly
workflow fuzzes, compiles the ESP8266 and RP2040 files and runs ESPHome's development-branch
gates. It does not block a pull request.

## Releasing

For the maintainer. The firmware version lives in one file,
`firmware/esphome/packages/version.yaml`, and a release is the tag `v<version>` on `main`.

The first release made this way is 2.0.0, the tag `v2.0.0` (2026-10-13).

1. **Pick the number** (semver). Patch for fixes. Minor for something new that leaves existing
   nodes as they are. Major when a node's owner has to act: an entity renamed or removed, a new
   required secret or substitution, a changed default that alters behaviour.
2. **Bump and date it**, in one commit that reaches `main` through a pull request: set `version:`
   in `firmware/esphome/packages/version.yaml`, and in `CHANGELOG.md` rename `## Unreleased` to
   `## X.Y.Z - YYYY-MM-DD` and put a new, empty `## Unreleased` above it. If the tree already has
   the number that is being released (as for 2.0.0), only the changelog changes.
3. **Check it** on that commit:
   ```bash
   python3 firmware/scripts/version-lint.py --tag vX.Y.Z
   ```
   It passes when the tag is `v` plus the version, the changelog has that version's own section
   (an Unreleased section is not enough here), the version is not below the newest `v*` tag, and
   every ESPHome pin names the same release. It is the first gate of the release workflow.
4. **Dry-run the workflow:** `gh workflow run esphome-release.yaml --ref main`. Every gate and
   every compile runs, and the run's summary shows the release text. It creates no tag and no
   release. It ends red if a real release from that commit would stop at a gate.
5. **Tag and push the tag:** `git tag -s vX.Y.Z -m "ESPHome firmware X.Y.Z"` on the merged
   commit, then `git push origin vX.Y.Z`.

The push starts `.github/workflows/esphome-release.yaml`. It refuses a tag that does not equal
the version or whose commit is not on `main`, runs the host tests and `esphome config`, compiles
every board file with the components free of warnings, and creates the GitHub release "ESPHome
firmware X.Y.Z" with the changelog section and a short "Using this release" text. No firmware
image is attached, because an image built in CI holds the placeholder secrets. If a gate fails,
nothing was published: fix `main`, delete the tag (`git push --delete origin vX.Y.Z`), and tag
the fixed commit.

The tags `amebaz2-v*`, `esp32-v*` and `matter-final` belong to the retired Matter firmwares and
stay as history.

### Pin bumps

`.github/workflows/pin-bumps.yaml` looks once a day for a newer ESPHome release on PyPI (stable
only) and a newer ESP-IDF image for the nightly `esp32-recon` job. For each one it finds, it opens a
pull request from `bot/esphome-X.Y.Z` or `bot/idf-vX.Y.Z` that moves every place the pin is
written, the install lines of the docs, and (for ESPHome) adds a `CHANGELOG.md` line. The rewrite
is `firmware/scripts/pin-bump.py`, which you can run yourself (`esphome --check`, `idf --check`).

- It never merges and deploys nothing. Merge it after the checks pass, then build and flash the
  units as for any firmware change.
- ESP-IDF stays on the v5.5 line. A move to v5.6 or v6 means a new toolchain and API changes, and
  is made by hand.
- A bump you close without merging is not opened again: the job skips a version that already has a
  pull request, open or closed.
- Its commits are made by `github-actions[bot]` and are not signed. They are the only unsigned
  commits that reach `main`, through the merge.
- A push made with `GITHUB_TOKEN` starts no workflow, so the job starts `qa.yaml` and
  `nightly.yaml` on the branch itself. If a check is missing on its pull request:
  `gh workflow run qa.yaml --ref <branch>`.
- Opening the pull request needs the repository setting "Allow GitHub Actions to create and
  approve pull requests" (Settings, Actions, General).

## Licence

By contributing you agree that your contribution is licensed under the repository's
[MIT licence](LICENSE).
