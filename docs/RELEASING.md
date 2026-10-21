# Releasing VIOS

A release is a tag `vX.Y.Z` on `main`. GitHub Actions
([`.github/workflows/release.yml`](../.github/workflows/release.yml)) builds it
and publishes it on the repository's Releases page.

## Where the version lives

The [`VERSION`](../VERSION) file at the top of the repository holds the
version as `MAJOR.MINOR.PATCH`, for example `0.8.0`. Everything else reads it:

| Reader | What it does with it |
|---|---|
| `make vios-resident` | compiles it into the resident: the boot and console banner shows `VIOS v0.8`, then `(c) 2026 Alessandro Iob` |
| the ESP32 build (`scripts/embed_vios.py`) | compiles it into the firmware's boot event and `build-manifest.json` |
| `tools/vios_release.py` | names the package and picks this version's notes from `CHANGELOG.md` |
| the release workflow | refuses a tag that does not equal `v` + `VERSION` |

The banner shows `MAJOR.MINOR`. The full version is in `build-manifest.json`
and in the release name.

Versions follow [semantic versioning](https://semver.org): a fix that changes
no behaviour bumps PATCH; new words, devices or tools bump MINOR; a change
that breaks saved packages, the SD layout or the wire protocol between the CPU
and the ESP32 bumps MAJOR (MINOR while VIOS is `0.x`).

## The changelog

[`CHANGELOG.md`](../CHANGELOG.md) lists what changed, newest first, in the
[Keep a Changelog](https://keepachangelog.com) style. Keep it current as you
work: every change a user would notice adds a line under `## [Unreleased]`,
in the section it belongs to (System, Forth, Compiler, GUI and display server,
Devices and files, Tools and host harness, Audio, Boot, firmware and recovery,
Documentation, Known limits).

## Making a release

1. Make sure `main` is what you want to ship. Every session branch meant for
   the release is merged, and the board passes on the build from `main`:

   ```bash
   make test
   ```

   ```bash
   .venv/bin/python tools/vios_board_suite.py docs/bring-up/vios-board-suite-<build>
   ```

2. Set the version. Edit `VERSION` (for example `0.8.0` → `0.9.0`).

3. In `CHANGELOG.md`, rename `## [Unreleased]` to `## [0.9.0] - YYYY-MM-DD`
   and add a fresh, empty `## [Unreleased]` above it. Check the notes read
   well on their own:

   ```bash
   .venv/bin/python tools/vios_release.py notes
   ```

4. Commit both on `main` and push:

   ```bash
   git commit -am "Release 0.9.0"
   ```

   ```bash
   git push origin main
   ```

5. Tag the commit and push the tag. The tag starts the release build:

   ```bash
   git tag -a v0.9.0 -m "VIOS 0.9.0"
   ```

   ```bash
   git push origin v0.9.0
   ```

6. Watch the run under the repository's **Actions** tab, or:

   ```bash
   gh run watch
   ```

   When it finishes, the release appears under **Releases** with three files:
   `vios-0.9.0.zip`, `vios-0.9.0-SHA256SUMS` and `vios-0.9.0-manual.zip`.

   The same tag also runs the **Manual** workflow, which deploys the manual
   to https://vios.iob.dev ([docs/manual/HOSTING.md](manual/HOSTING.md)).

If the build fails, fix the problem on `main`, then move the tag to the fixed
commit before anyone downloads it:

```bash
git tag -d v0.9.0
```

```bash
git push origin :refs/tags/v0.9.0
```

and repeat step 5. Once a release is public, don't move its tag: release a
new PATCH version instead.

### Trying the build without releasing

On the **Actions** tab, choose **Release**, then **Run workflow** on `main`.
The run does everything except publish; the package is attached to the run as
an artifact. The same checks run locally:

```bash
make test vios-resident viosboot
```

```bash
.venv/bin/pio run -d firmware/esp32-vios -e esp32-vios
```

```bash
.venv/bin/python tools/vios_release.py package
```

## What the workflow does

1. Checks that `VERSION`, the tag and `CHANGELOG.md` agree, and that the
   tagged commit is on `main`.
2. Installs Python, PlatformIO and AgonDev v0.22 (the Linux x86-64 build,
   checked against its published SHA-256).
3. Runs the host tests (`make test`).
4. Builds the eZ80 resident and the boot stub (`make vios-resident viosboot`)
   and the ESP32 firmware, with the internal UART at 1,152,000 baud and the GUI
   on, the same pairing as the board.
5. Packages the release (`tools/vios_release.py package`) and builds the
   manual site.
6. Publishes the GitHub release with this version's changelog section as its
   notes.

## What a release contains

`vios-X.Y.Z.zip` unpacks to `vios-X.Y.Z/`:

| Path | Contents |
|---|---|
| `esp32/firmware.bin` | the ESP32 application, with the CPU image embedded |
| `esp32/build-manifest.json` | the build's identity: version, image CRCs, baud, GUI |
| `sd/` | the SD card tree: `mos/viosboot.bin`, `vios/vios.bin`, `vios/vios.man`, `autoexec.txt` |
| `tools/` | `vios_firmware_ab.py` and `vios_sd_install.py` |
| `CHANGELOG.md` | this version's notes |
| `LICENSE` | the MIT License |
| `SHA256SUMS` | a checksum for every file above |

## Installing a release

Check the download first:

```bash
cd vios-0.8.0 && shasum -a 256 -c SHA256SUMS
```

Install the ESP32 firmware into the spare A/B slot. The tool backs up the
current flash first, and the previous firmware stays bootable:

```bash
python tools/vios_firmware_ab.py --port /dev/cu.usbserial-XXXX install --backup-dir backups esp32/firmware.bin
```

Put the SD files on the card. Mount it on the computer and run the installer,
which keeps your other `autoexec.txt` lines and your saved modules:

```bash
python tools/vios_sd_install.py install /Volumes/AGON --resident sd/vios/vios.bin --stub sd/mos/viosboot.bin
```

Or copy the `sd/` tree onto the card by hand. That replaces `autoexec.txt`, so
merge it by hand if the card already has one. Then reset the board; the first
lines on the screen and on the USB console are:

```text
VIOS v0.8
(c) 2026 Alessandro Iob
```

The manual's installation chapter covers first-time setup in full.
