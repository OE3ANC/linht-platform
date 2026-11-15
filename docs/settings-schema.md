# Settings schema format (v1)

This is the authoritative spec for settings schemas, the store and `org.m17project.LinHT.Settings1` ([interfaces/org.m17project.LinHT.Settings1.xml](../interfaces/org.m17project.LinHT.Settings1.xml)). Background is in architecture §8. Where the two differ, this document wins.

## Files

- **One schema file per namespace.**
  - System schemas: `/usr/share/linht/schemas/<namespace>.yaml`, from `schemas/` in this repo.
  - App schemas (phase 3): `settings.schema.yaml` in the bundle.
- `linht-settingsd` loads every schema at startup. Changing a schema means restarting the daemon.

```yaml
# SPDX-License-Identifier: GPL-3.0-only
namespace: system.display        # [a-z0-9_]+ segments joined by '.', unique
version: 1                       # bump when a key changes type or meaning
title: Display                   # shown by generic UIs
keys:
  brightness:                    # key name: [a-z0-9_]+
    type: int
    default: 255
    min: 32
    max: 255
    step: 32                     # UI hint only
    label: Brightness
    description: Backlight level of the screen.
    order: 1                     # UI sort order, ascending
    apply: live
    access: user
```

## Key fields

| Field | Required | Meaning |
|---|---|---|
| `type` | yes | `bool`, `int`, `float` or `string` (see below) |
| `default` | yes | must itself pass validation |
| `min`, `max` | no | inclusive bounds; `int` and `float` only |
| `choices` | no | list of allowed values; `int`, `float` or `string` |
| `step` | no | UI hint for `int` and `float` |
| `unit` | no | UI hint, e.g. `s`, `Hz` |
| `label` | yes | short UI label |
| `description` | no | one sentence for help text |
| `order` | no | UI sort order; missing keys sort last, by name |
| `apply` | no | `live` (default), `restart-app`, `restart-service`, `reboot`. This is information for UIs only. |
| `access` | no | `user` (default), `admin`, `device-only`, `factory`; see Access |

Unknown fields are a schema error: `settingsd` refuses to load that schema and logs why. This catches typos.

## Types (v1)

| Schema type | D-Bus signature | Validation |
|---|---|---|
| `bool` | `b` | — |
| `int` | `x` (int64) | `min`/`max`, `choices` |
| `float` | `d` | must be finite; `min`/`max`, `choices` |
| `string` | `s` | valid UTF-8, at most 4096 bytes; `choices` |

`settingsd` accepts exactly the listed signature; it doesn't convert a `u` to an `x`, for example. More types (`frequency`, `callsign`, `list`, `secret`) are added when the first namespace needs them, in phase 2.

## Access

`settingsd` identifies the caller by the Unix uid it gets from D-Bus.

| Level | Who may write |
|---|---|
| `user` | anyone the D-Bus policy lets call `Set` |
| `admin` | on-device UI (`linht`), `linht-web`, root |
| `device-only` | on-device UI (`linht`) and root only. **Never remote clients.** |
| `factory` | root only. A calibration flow is added in phase 2. |

Reading is never restricted, except for `secret`, which comes in phase 2.

## Store

- **Effective value** = the stored user value if present and valid, otherwise the schema `default`.
- **User layer:** `/var/lib/linht/settings/<namespace>.yaml`. It is a flat map of `key: value`, holding **only keys that differ from the default**, so setting a key to its default removes it.
- **Writes are atomic:** temp file in the same directory, `fsync`, `rename`, `fsync` of the directory.
- **Loading is forgiving.** A stored key that is unknown, of the wrong type or out of range is dropped with a warning in the journal, and the default applies. A file that can't be opened or parsed is renamed to `<namespace>.yaml.broken` for inspection, and all defaults apply. Otherwise the next write would replace it, and every stored value with it. A broken file never stops the daemon, unlike `gui_test`'s strict `settings.yaml`.
- **Strings are written double-quoted.** Anything YAML doesn't allow raw in a file is escaped (`\uXXXX`), and so are the line breaks U+0085, U+2028 and U+2029, which a parser would fold. Any valid UTF-8 value therefore reads back unchanged.
- `Describe` reports `order` as `x`.

## D-Bus behaviour

- `Set` and `SetMany` validate everything first. `SetMany` is all-or-nothing: one invalid value rejects the whole call and stores nothing.
- `Reset` removes a key from the user layer.
- `Changed(namespace, values)` is emitted once per successful call that changed something. `values` holds the **new effective values** of the keys that changed, including reset keys with their default. A call that changes nothing emits nothing.
- `Describe` returns the title and every key as `(name, properties)`, in `order`. `properties` uses the field names from the table above, with values in the key's own type (`default`, `min`, `max`) or as `av` for `choices`.
- Errors, under the `org.m17project.LinHT.Settings1.Error.` prefix:
  - `UnknownNamespace`
  - `UnknownKey`
  - `InvalidValue` (the message names the failed rule)
  - `AccessDenied`
  - `StorageFailed` (the write failed; the old value stays in effect)
