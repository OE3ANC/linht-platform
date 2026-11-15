# Review log: milestone 1

Milestone: D19, architecture §13 phase 1. The review process is defined in AGENTS.md ("Milestone review").
Scope: the whole working tree. Nothing is committed yet, so there is no base commit.

## Preparation (2026-11-15)

**On the PC**, all in `nix develop`:
- `cmake --build build`: 0 warnings; `ctest`: 7/7 passed.
- With `-fsanitize=address,undefined`: 0 warnings, 7/7 passed.
- `-DLINHT_SIM=ON`: the launcher and Settings app build. A headless smoke test with `SDL_VIDEODRIVER=offscreen` passed: launcher as foreground, Launch Settings, Home, `UnknownApp`/`LaunchFailed` errors.

**On the radio** (Rev C, image `linht-image-mcm-imx93.rootfs-20261115122731`, flashed by the user):
- Boot: settingsd (user `linht-settings`), shelld, gui_test and zmq-proxy active; no failed units; `/dev/input/linht-keys` present, and gui_test reads it.
- The done criterion was tested by the user and passed:
  - boot into gui_test;
  - long-press ESC opens the launcher;
  - Settings → Display → Brightness changes the backlight live;
  - ESC leads back, and long-press ESC returns to the radio.
- The recorded key events show standard values (1 = press, 0 = release). shelld's journal shows the matching radio → launcher → settings → launcher → radio switches.

**`ponytail-debt` ledger** (2 markers, both with a trigger):
- `shelld/main.c:470`: without settingsd the hardware is left as it is and nothing times out. Upgrade: settingsd appears (NameOwnerChanged).
- `ui/linht-ui.c:99`: the status bar shows the time only. Upgrade: battery, GNSS and TX state with hwd and radiod (phase 2).

**Open observation, not a finding yet:** one ENTER+LEFT pair was reported at the same millisecond (1 in 46 presses). These two keys share a column, and the driver doesn't wait after switching rows (`udelay(1)` is commented out), so this could be ghosting. It could also have been two keys pressed together. Needs a targeted test.

## Round 1 (2026-11-15)

Fresh `milestone-reviewer`, model Fable 5.2, whole working tree. **Result: 13 findings** (1 Must fix, 6 Should fix, 6 Nice to have). The reviewer ran the host, ASan and simulator builds (7/7) and 16 mutations (11 killed). It did not run anything on the radio.

All 13 are fixed in the working tree, which has no commits yet, so the resolution column names the change instead of a commit.

| # | Finding | Resolution |
|---|---|---|
| 1 | **Must fix.** If shelld stops or crashes while PTT is held, the transmitter stays keyed: `/dev/input/linht-keys` disappears and the kernel drops the queued releases, and `read()` fails with `ENODEV`, which gui_test ignored. | `BindsTo=linht-shelld.service` in `shelld/linht-gui-test-after-shelld.conf`, and gui_test patch 0001 leaves the main loop on `ENODEV`, so its cleanup runs. D16 and AGENTS.md updated. |
| 2 | Every new bus connection made shelld re-read all settings with blocking calls. | The `NameOwnerChanged` match is filtered on `arg0` (settingsd's name) by the bus. |
| 3 | U+0085, U+2028 and U+2029 were written raw and read back folded. | Escaped as `\uXXXX` (`yaml_printable`), added to `test_values_round_trip`, `settings-schema.md` updated. |
| 4 | No test failed when an access level broke. | `test_bus.py`: `AccessAsDeviceUser`, `AccessAsWebUser`, `AccessAsOtherUser` with one key per level. |
| 5 | The PTT check on Launch/Home was untested. | `fg_request(f, app, ptt)` returns `FG_REFUSED` while PTT is held and an app is in front. Every caller goes through it, so the duplicate check in `keys.c` is gone. `test_no_switch_while_ptt_is_held` added. |
| 6 | `deploy.sh` installed stale binaries after a failed build (`grep` set the exit status). | The build writes `build.log` and the script exits with the build's status. |
| 7 | The docs claimed things milestone 1 does not do. | Corrected: autorepeat, the data partition (D7), mock `radiod`, screen-off PTT, idle policy without `hwd`, `.bak`, D3 raw D-Bus, the home-app fallback comment, and shelld's user. D19 now lists what phase 1 still owes. `shortcut:` markers added for shelld as root and the apps' direct sd-bus use. |
| 8 | A settings file that can't be opened (`EACCES`, `EIO`) was replaced by the next write. | Moved to `.broken` like an unparsable file. `test_unopenable_file_is_kept` added. |
| 9 | The image had dangling `patches/` links to a build-host path. | `do_install:append` in `linht-utils.bbappend` removes `/root/LinHT-utils/patches`. |
| 10 | `linht-platform` didn't declare its runtime dependencies. | `RDEPENDS:${PN} += "linht-input-rules linht-utils"`. |
| 11 | UI apps opened `/dev/dri/card0`, which depends on probe order. | `ui/70-linht-lcd.rules` creates `/dev/dri/linht-lcd` for the st7735r card, and `linht-ui` opens that. §5 uses `DeviceAllow=char-drm`. |
| 12 | `sim/lv_conf.h` had an invalid SPDX expression and an unlisted licence exception. | Header reduced to `SPDX-License-Identifier: MIT`. The exception is listed in AGENTS.md. |
| 13 | `shelld/main.c` had a `shortcut:` with no upgrade trigger. | It is designed fallback behaviour, so the tag is dropped and the comment stays. |

**Found while verifying finding 1, and fixed.** With `BindsTo=` (and `After=`), `systemctl stop linht-shelld` stops gui_test first. shelld took that for a crash and started gui_test again. That start pulled shelld back in, and systemd reported `Job for linht-shelld.service canceled.`. The same race applied at shutdown. Fix: before starting an app again, shelld checks whether a job is queued for its own unit (`/org/freedesktop/systemd1/unit/self`, property `Job`). If one is, shelld is being stopped or restarted, and it doesn't start the app again.

**Decision D30** went into the same change: one repo, upstream changes as patches, the gui_test patches marked `Upstream-Status: Inappropriate`, and the keypad fix kept local until the Radio app replaces gui_test.

**Verification after the fixes:**
- **PC:** 7/7 tests with 0 warnings, both normal and ASan/UBSan. The simulator builds.
- **Mutations against the new tests:** 6 mutations, 6 killed, and the unmutated copy passes 7/7. The mutations were: web user may write device-only, everyone may write admin, device user may write factory, no PTT check, PTT check even when idle, and U+2028 written raw.
- **Radio, via `deploy.sh`:** 6/6 tests.
  - `/dev/dri/linht-lcd -> card0` is present.
  - **`systemctl stop linht-shelld`:** gui_test runs its cleanup (`Cleanup done`), and both units end up inactive.
  - **`systemctl start`:** both units come back, with the Radio app in front.
  - **`systemctl restart`:** both units come back.
  - **`systemctl kill -s KILL`:** gui_test is cleaned up 6 ms later through `BindsTo=` (systemd 255), then shelld restarts and starts gui_test again.
  - **Kernel check:** a reader on `/dev/input/linht-keys` gets `ENODEV` when shelld restarts.
  - **Apps:** Settings and the launcher start through `Shell1.Launch` on the new display path, and `Home` returns to gui_test.
- **Image:** `linht-image-mcm-imx93.rootfs-20261115131744` built.
  - The one warning (`do_rootfs_wicenv: failed to chown … linht-image.env`) also appears in both earlier builds. It comes from the build container.
  - The rootfs has no `/root/LinHT-utils/patches`, and it contains `70-linht-lcd.rules` and the drop-in with `BindsTo=`.
  - gui_test is built from the source with the `ENODEV` exit (`test.c:1088`).
  - The image is not flashed yet.

**`ponytail-debt` ledger after round 1** (4 markers, each with a trigger):
- `ui/linht-ui.c:99`: the status bar shows only the time. Upgrade: battery, GNSS and TX with hwd and radiod (phase 2).
- `shelld/linht-shelld.service.in:10`: shelld runs as root. Upgrade: a `linht-shell` user with udev and polkit rules (phase 1, D19).
- `apps/launcher/main.c:46`, `apps/settings/main.c:12`: Shell1 and Settings1 through sd-bus directly. Upgrade: `liblinht` (phase 1, D3, D19).

## Round 2

_(pending)_
