# Architecture

## The goal: optional coupling

The engine and its content (runes) must stay **optionally** connected. Installing
a content module like `mod-sod-mage` on its own should still work (its spells are
real, learnable by GM command); installing this engine *as well* should make those
same spells acquirable as engravable runes — with **no hard dependency** in either
direction.

AzerothCore modules are statically linked into `worldserver`, so any C++ call from
a content module into this engine would be a compile-time dependency — exactly what
we must avoid. The coupling is therefore **through the database, not symbols**.

| Side | Owns | Role |
|------|------|------|
| **Engine** (this module) | `rune_template`, `rune_quest_unlock`, `rune_contract` (world); `character_rune`, `character_rune_unlock` (characters); all C++ | the mechanism; seeds **no** runes |
| **Content** (e.g. `mod-sod-mage`) | its own spells + rune rows | inserts into `rune_template`, guarded so it's a no-op without the engine |

Result:

- **Engine absent** — the content module's rune SQL is a clean no-op; its spells
  still exist and are GM-`.learn`-able.
- **Engine present** — the content's runes appear at the engraver NPC and grant
  their spells when engraved.

The full content-side contract is in [Integrating content](integrating-content.md).

## Abstract engraving slots

Engraving slots (Head, Neck, Shoulder, Cloak, Chest, Wrist, Hands, Waist, Legs,
Feet, Ring) are an engine concept **independent of equipped gear** — engraving is a
per-character choice, one rune per slot. They are *not* `EquipmentSlots`; a rune's
spell applies regardless of what the character is wearing. A rune's legal slots are
a `slot_mask` bitmask of `1 << RuneSlot`, and the `RuneSlot` enum's numeric values
are the contract (see `src/RuneEngravingMgr.h`).

## How a spell is granted

Rune spells are granted as **temporary** spells:

- engrave → `Player::learnSpell(spellId, /*temporary*/ true)`
- un-engrave / swap → `Player::removeSpell(spellId, SPEC_MASK_ALL, /*onlyTemporary*/ true)`

`onlyTemporary` means the engine never strips a spell the player learned by other
means. Temporary spells are **not persisted** to `character_spell`, so they vanish
on logout — which is intentional: the engine **re-applies** every engraved rune's
spell on login (`OnPlayerLogin`). The source of truth is the `character_rune` table.

## Self-healing edge cases

- **GUID reuse.** AzerothCore can reseed the character-GUID counter after a restart,
  so a deleted character's GUID can be reused by a new (possibly different-class)
  character. On login, `ApplyAll` skips and purges any stored rune whose `class_mask`
  doesn't match the current character. A rune merely *missing* from the catalog
  (e.g. temporarily `enabled = 0`) is kept, not purged.
- **Character deletion.** `OnPlayerDeleteFromDB` appends `DELETE`s for the
  character's rune rows to the deletion transaction, so nothing is left to inherit.

## Concurrency

`RuneEngravingMgr` separates two concerns under two locks: the **catalog** (loaded
once at startup, rewritten only by the admin-only `.rune reload`) and the
**per-character state**. Lock ordering is one-way (state → catalog, never the
reverse) to avoid deadlock. See [Gotchas](gotchas.md) for the reload caveat.

## UI: gossip first

The 3.3.5a client has no engraving panel, and there's no clean core hook for
receiving client addon messages — so the v1 UI is a **gossip NPC** (entry `700000`,
`ScriptName npc_rune_engraver`): pick a slot → pick a rune → engrave/remove. The
engine logic lives in `RuneEngravingMgr`; the NPC and the `.rune` GM command are
thin front-ends, so a future addon UI can reuse the same API.

## No core edits

Everything lives under `modules/mod-rune-engraving/`. Behavior is driven through
script hooks and DB rows — never by patching AzerothCore itself.

See also: [Integrating content](integrating-content.md) · [Deploy & verify](deploy-and-verify.md) · [Gotchas](gotchas.md)
