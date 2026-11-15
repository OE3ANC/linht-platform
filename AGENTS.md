# AGENTS.md: LinHT launcher and platform

Instructions for coding agents working in this repo. Claude Code loads this file through `CLAUDE.md`.

## What this repo is

The app, launcher and settings platform for **LinHT**, the open-source Linux handheld transceiver from the M17 Project (Rev C board, CompuLab MCM-iMX93, Yocto scarthgap).

- **Design:** [docs/architecture.md](docs/architecture.md). Its **decisions log (D1–D30) is binding.**
- **linht-web v2:** [docs/linht-web-v2.md](docs/linht-web-v2.md).
- **Licence:** GPL-3.0-only ([LICENSE](LICENSE)).
- **Repo:** `git@github.com:OE3ANC/linht-platform.git` (remote `origin`). Everything lives here, linht-web v2 included (`web/`). Upstream components are changed only through patches in `contrib/yocto/`, never forks (D30).
- **Current milestone:** milestone 1 (D19, architecture §13 phase 1).

If the work needs a different design, stop and ask. Then update `docs/architecture.md` and add a decision entry. Never deviate silently.

## Skills: when to use which

Project skills live in [.claude/skills/](.claude/skills/); their sources are pinned in [VENDORED.md](.claude/skills/VENDORED.md).

| When | Skill |
|---|---|
| **Start of every session, before the first coding task.** Stays active all session. | `ponytail` (level `full`) |
| Implementing logic in daemons and libraries (see Testing) | `test-driven-development` |
| Any bug, test failure, build failure or unexpected device behaviour, before proposing a fix | `systematic-debugging` |
| Before saying anything works, passes or is done, including milestone completion | `verification-before-completion` |
| Writing or changing `.bb`, `.bbappend`, `.bbclass` or `layer.conf` in `contrib/yocto/` | `yocto-recipe-review` |
| Kernel config fragments, device tree, U-Boot, machine config | `yocto-bsp-kernel` |
| A BitBake task fails | `bitbake-debug` |
| End of every milestone: list deferred shortcuts | `ponytail-debt` |
| Before posting the RFC, and at phase boundaries | `ponytail-audit` |
| Network-facing code (linht-web v2, anything parsing uploads or HTTP) | built-in `security-review`, in addition to the milestone review |

ponytail decides **scope**: build the smallest complete thing. On this project it **never cuts**:

- RF and TX safety checks
- validation at D-Bus, file and HTTP boundaries
- atomic writes and other data-loss protection
- calibration handling

Mark deliberate shortcuts as `shortcut: <the limit>, <when to upgrade>`.

## Hardware and device safety

- **The device** is `ssh root@10.17.17.17`, over the `usb0` USB link, with no password.
  - Its host key changes after every reflash. Ask before you modify `~/.ssh/known_hosts`. Until then, use a throwaway file with `-o UserKnownHostsFile=<scratch>/known_hosts`.
- **Read-only inspection is always fine.** **Ask first** before any of these:
  - writing to hardware: GPIO, PWM, SPI, I²C, writes to sysfs or debugfs
  - starting, stopping or enabling services
  - changing files on the device
  - rebooting
  - flashing (`uuu`)
- **Standing permission for milestone 1**, granted by the user on 2026-11-15. Without asking each time, you may:
  - copy sources to `/root/linht-dev` on the device and build there
  - run our own test binaries
  - temporarily stop and restart `linht-gui-test` for display tests
  - write the backlight PWMs
  - install test units for `shelld` and `settingsd`

  This doesn't extend to transmitting, flashing or rebooting, which still need an explicit OK every time. Always restore the original state.
- **Before any hardware test**, record the original state. Restore it afterwards and verify the restore. Tests that need someone to watch the radio go step by step, with a question after each step.
- **Never transmit**: no PTT, no PA enable, no RF switch to TX, no TX flowgraph. The only exception is a specific test the user explicitly authorises, because transmitting is a licence matter.
- **Don't probe I²C buses** (`i2cdetect`). They are shared with the audio codec and the charger.
- **The device tree in `meta-linht-hardware` is the hardware truth.** LinHT libraries, examples and configs (`liblinht-ctrl`, `gui_test`, linht-web v1, `scripts/`) may still encode **older board revisions**. Check every pin, chip and channel against the Rev C DT, and never copy numbers from them.
  - **Already verified on Rev C** (architecture §3): LCD backlight on `pwmchip0` (TPM3) channel 3, keypad backlight on `pwmchip4` (TPM4) channel 3, both with normal polarity at 20 kHz.
  - **Key events are standard evdev** (1 = press, 0 = release) only with the keypad driver fix from `contrib/yocto`. The unpatched driver inverts every key, and gui_test patch 0003 depends on the fix. Never ship one without the other: PTT would transmit on release. Both stay local until the LVGL Radio app replaces gui_test (D30).
  - **gui_test is bound to shelld** (`BindsTo=` in `shelld/linht-gui-test-after-shelld.conf`). Keep it: without shelld, `/dev/input/linht-keys` is gone and gui_test never sees a PTT release.
- **`pgrep -f` / `pkill -f` over ssh also match the remote shell running your command**, because its command line contains the pattern. Anchor the pattern, e.g. `pkill -f "^python3 /root/linht-dev/keylog"`, or use `pgrep -x <name>`.

## Code conventions

- **Languages:**
  - C11 for daemons, `liblinht`, `linht-ui` and apps (LVGL 9.1 as shipped in the image, sd-bus from libsystemd).
  - Python 3.12 for background services and mocks.
  - Go for linht-web v2, in `web/` (its own Go module).
- **Every source file we write** starts with `SPDX-License-Identifier: GPL-3.0-only`. There are three exceptions:
  - Device-tree sources use `(GPL-2.0-or-later OR MIT)`, like the kernel device trees they extend.
  - Patches against upstream code (`*.patch`) carry no header; they inherit the upstream licence. Patches keep upstream's line endings byte for byte (`.gitattributes`: `*.patch -text`).
  - Files derived from an upstream template keep its licence: `sim/lv_conf.h` is LVGL's `MIT`.
- **No hardcoded hardware paths or numbers.** Use device-tree names and class devices: GPIO line names, `/sys/class/backlight`, `/sys/class/leds`, `/dev/input/linht-keys`, `/dev/dri/linht-lcd`.
- **`interfaces/*.xml` (D-Bus introspection XML) is the API contract.** A breaking change means a new interface version (`Radio1` → `Radio2`), never an in-place edit.
- **Persistence:**
  - Every file has exactly one owning service (D25).
  - Use YAML for configuration and SQLite for records.
  - Write atomically: temp file, `fsync`, `rename`.
  - SQLite schemas are migrated through `PRAGMA user_version`.
- **Host toolchain:** the PC runs NixOS with no compilers installed system-wide. Use the repo's dev shell, `nix develop` (`flake.nix`). For one-off commands: `nix develop -c <cmd>`. Nix only sees files tracked by git: keep `flake.nix` and `flake.lock` staged or committed. The PC's libraries are newer than the device's (libsystemd 260 vs 255, SQLite 3.53 vs 3.45). **The device is the reference**: don't use API newer than what the device has.
- **Fast device loop:** the radio builds natively (gcc 13.3, cmake 3.28, headers for sd-bus, LVGL 9.1.0, libdrm, libyaml, SQLite). Use `rsync` to `/root/linht-dev` and build there. No cross SDK is needed for milestone 1.
- **Build:** one CMake project with one target per component (D27).
  ```sh
  nix develop -c sh -c 'cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure'
  # PC simulator: launcher and Settings in an SDL window, real settingsd, mock shell (sim/mock-shell.py)
  nix develop -c sh -c 'cmake -S . -B build-sim -G Ninja -DLINHT_SIM=ON && cmake --build build-sim && sim/run.sh'
  # on the radio: rsync, native build and tests, install, restart our services
  LINHT_SSH_OPTS="-o UserKnownHostsFile=<scratch>/known_hosts" contrib/deploy.sh
  ```
  - On the PC, the UI apps only build with `-DLINHT_SIM=ON`; the normal host build runs the tests.
  - The D-Bus test needs Python dbus-next, which the radio doesn't have, so it runs on the PC only.
  - Memory and undefined-behaviour checks: `-DCMAKE_C_FLAGS="-fsanitize=address,undefined"` in a separate build directory.
- **Yocto layer in `contrib/yocto/`** (D28):
  - scarthgap, modern override syntax
  - **pinned `SRCREV`** (no `AUTOREV`)
  - a real `LICENSE` and `LIC_FILES_CHKSUM`
  - no network access in `do_compile`
  - Kernel and DT changes are carried as `bbappend`s for `linux-compulab`.
  - Every patch has an `Upstream-Status:` line (D30). gui_test patches are `Inappropriate`: gui_test is replaced, not fixed.
  - `recipes-linht/linht-platform/linht-platform.bb` builds this repo from `file://` sources, listed per directory; a new top-level source directory must be added there. It doesn't use `externalsrc`: that runs `git add` in the source tree, which the read-only mount refuses.
  - No trailing comments on BitBake assignment lines; BitBake can't parse them.
- **Build environment:** `/home/sa/Documents/compulab-yocker-revb`.
  - It is a Docker container (`yocto-build:v1`) that mounts `work/` as `/work`. User `sa` is in the `docker` group, so no sudo is needed. The container runs as uid 1000 (`compulab`), the same uid as `sa`.
  - The BSP is in `/work/in-work/compulab-nxp-bsp`, with build directory `build-mcm-imx93` and image `linht-image`.
  - The build's `conf/bblayers.conf` contains `BBLAYERS += "/linht-launcher/contrib/yocto"` (added 2026-11-15), so the container must mount this repo at `/linht-launcher`. Build the image like this:
    ```sh
    docker run --rm -v /home/sa/Documents/compulab-yocker-revb/work:/work \
      -v /home/sa/Documents/linht-launcher:/linht-launcher:ro yocto-build:v1 bash -c \
      'cd /work/in-work/compulab-nxp-bsp && source sources/poky/oe-init-build-env build-mcm-imx93 >/dev/null && bitbake linht-image'
    ```
  - The output is `build-mcm-imx93/tmp/deploy/images/mcm-imx93/linht-image-mcm-imx93.rootfs-<timestamp>.wic.zst`.
  - Image builds are long, so start them early and work on something else in parallel.

## Testing

- **Test first** (`test-driven-development`) for non-trivial logic:
  - schema validation and the settings store
  - key routing, long-press and autorepeat
  - lease and permission handling
  - D-Bus method handlers
  - SQLite migrations
  - anything touching RF state
- **No tests** for trivial code (constants, getters, plain forwarding). That is where ponytail and TDD agree.
- **UI layout** is checked in the simulator. **Hardware integration** is tested on the device, with the user's consent (see above).
- Run the whole suite before any completion claim, not just the test you wrote.

## Milestone review (mandatory)

A milestone is a phase or deliverable from architecture §13 (e.g. milestone 1, W1). It is **not done** until an independent review passes:

1. **Prepare:**
   - Run the full build and test suite and confirm both pass (`verification-before-completion`).
   - Run `ponytail-debt` and add the ledger to the review log.
2. **Review:**
   - Spawn the `milestone-reviewer` subagent ([.claude/agents/milestone-reviewer.md](.claude/agents/milestone-reviewer.md)) with **model `fable` (Claude Fable 5.2, `claude-fable-5-2`)**. Pass the model explicitly in the Agent call, even though the agent file sets it too.
   - Give it the milestone definition (decision and section), the base commit and the paths in scope. **Don't give it your own assessment.**
3. **Fix** every finding, Nice to have included.
   - If you believe a finding is wrong, don't drop it yourself. Write your reasoning into the review log; only a fresh reviewer may confirm the dismissal.
4. **Re-check** with a **new** Fable 5.2 reviewer instance (never a resumed one) over the **whole milestone scope**, not just the fixes.
5. **Repeat** steps 3–4 until a re-check ends with `RESULT: NO FINDINGS`.
6. **Log** every round in `docs/reviews/<milestone>.md`: round, findings, how each was resolved (commit or confirmed dismissal), and the final `NO FINDINGS` result.
7. Only then report the milestone as done to the user, with a link to the review log.

## Git and workflow

- **Commit only when the user asks. Never push**; the user pushes to `origin`.
- Keep `docs/` in sync with the code. A new design decision goes into the decisions log in the same change.
- Upstream LinHT sources for reference:
  - `/home/sa/Documents/compulab-yocker-revb/work/in-work/compulab-nxp-bsp/sources/meta-linht-*`
  - bare git mirrors in `…/compulab-nxp-bsp/downloads/git2/`. Read them with `git --git-dir=… show HEAD:<path>`.
