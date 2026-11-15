# Vendored agent skills

These skills are copied **unmodified** from upstream, pinned to the commits below, and were read in full before being added (2026-11-15). Each folder carries the upstream `LICENSE`, all MIT.

| Skill | Upstream | Pinned commit | Files taken |
|---|---|---|---|
| `ponytail`, `ponytail-review`, `ponytail-audit`, `ponytail-debt` | [DietrichGebert/ponytail](https://github.com/DietrichGebert/ponytail) v5.3.3 | `7801d061243d7b753bad0e0b3c057cd250b11755` | `skills/<name>/SKILL.md` |
| `test-driven-development` | [obra/superpowers](https://github.com/obra/superpowers) | `22f0b463e7a5e579904dd6211a520321035526f7` | `SKILL.md`, `writing-good-tests.md` |
| `systematic-debugging` | obra/superpowers | same | `SKILL.md`, `root-cause-tracing.md`, `defense-in-depth.md`, `condition-based-waiting.md` |
| `verification-before-completion` | obra/superpowers | same | `SKILL.md` |
| `bitbake-debug` | [Higangssh/yocto-agent-skills](https://github.com/Higangssh/yocto-agent-skills) | `53643acdbfecdc3e2186d478c7ae5b19fe0adf12` | `SKILL.md`, `references/{yocto-field-guide,qa-errors,variables-core}.md` |
| `yocto-recipe-review` | Higangssh/yocto-agent-skills | same | `SKILL.md`, `references/{variables-core,classes-core,qa-errors}.md` |
| `yocto-bsp-kernel` | Higangssh/yocto-agent-skills | same | `SKILL.md`, `references/{bsp-kernel,classes-core,migration}.md` |

## Left out on purpose

- **ponytail:**
  - `ponytail-gain` (benchmark scoreboard) and `ponytail-help`, which aren't needed.
  - The Node.js session hooks. Activation happens through AGENTS.md instead, and no third-party code runs at session start.
- **superpowers `systematic-debugging`:** `find-polluter.sh` and `condition-based-waiting-example.ts`, which only work for npm/TypeScript projects. The Markdown files that mention them still apply.
- **yocto-agent-skills `yocto-recipe-review`:** `references/official-doc-map.md`, a list of documentation URLs that wasn't reviewed. Use https://docs.yoctoproject.org for scarthgap directly.

## Name mapping

The superpowers skills refer to each other as `superpowers:<name>`. Here they are installed under the plain name, e.g. `test-driven-development`.

## Updating

1. Download the new upstream revision into an empty scratch directory. Don't execute anything from it.
2. Read the changed files in full.
3. Copy them over, and update the pinned commit in this table.
