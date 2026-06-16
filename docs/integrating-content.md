# Integrating content

How another module adds runes to the engine. The contract is **pure data**: you
insert rows into the engine's `rune_template` table. No C++ linkage, no headers —
your module never references this one at build time.

## 1. Insert into `rune_template`

One row per rune. Columns:

| Column | Type | Meaning |
|--------|------|---------|
| `rune_id` | INT | unique id (use your module's documented band — see below) |
| `spell_id` | INT | the spell granted while this rune is engraved |
| `class_mask` | INT | AC classmask: `1 << (class - 1)`; `0` = any class |
| `slot_mask` | INT | bitmask of `1 << RuneSlot` for the slots this rune is legal in |
| `name` | VARCHAR | shown in the engraver NPC menu |
| `icon` | VARCHAR | optional (reserved for a future addon UI) |
| `description` | VARCHAR | optional flavor/help text |
| `source` | VARCHAR | your module tag, e.g. `'mod-sod-mage'` (greppable) |
| `enabled` | TINYINT | `1` to make it engravable; `0` hides it from the catalog |

### Slots

`slot_mask` is a bitmask over the `RuneSlot` enum (defined in
`src/RuneEngravingMgr.h`). **The numeric values are the contract** — do not assume
they'll be renumbered:

| Slot | Value | `1 << value` |
|------|------:|-------------:|
| Head | 0 | 1 |
| Neck | 1 | 2 |
| Shoulder | 2 | 4 |
| Cloak | 3 | 8 |
| Chest | 4 | 16 |
| Wrist | 5 | 32 |
| Hands | 6 | 64 |
| Waist | 7 | 128 |
| Legs | 8 | 256 |
| Feet | 9 | 512 |
| Ring | 10 | 1024 |

A rune legal only in Chest → `slot_mask = 16`. Legal in Chest **or** Legs →
`16 | 256 = 272`.

### Classes

`class_mask` uses the standard AzerothCore classmask, `1 << (classId - 1)`:

| Class | Id | Mask |
|-------|---:|-----:|
| Warrior | 1 | 1 |
| Paladin | 2 | 2 |
| Hunter | 3 | 4 |
| Rogue | 4 | 8 |
| Priest | 5 | 16 |
| Death Knight | 6 | 32 |
| Shaman | 7 | 64 |
| Mage | 8 | 128 |
| Warlock | 9 | 256 |
| Druid | 11 | 1024 |

`0` means no class restriction. In v1 a class-legal rune is immediately engravable
(no unlock gating).

## 2. Guard the SQL so it's a no-op without the engine

Your INSERT must not error when the engine isn't installed (its `rune_template`
won't exist). A plain `INSERT INTO rune_template ... WHERE EXISTS(...)` does **not**
work — MySQL errors on the missing target table *before* the `WHERE` runs. Build
the INSERT as conditional dynamic SQL instead:

```sql
SET @rune_tbl := (SELECT COUNT(*) FROM information_schema.tables
                  WHERE table_schema = DATABASE() AND table_name = 'rune_template');

SET @sql := IF(@rune_tbl > 0,
'INSERT INTO `rune_template`
    (`rune_id`, `spell_id`, `class_mask`, `slot_mask`, `name`, `icon`, `description`, `source`, `enabled`)
 VALUES
    (7000001, 401417, 128, 16, ''Regeneration'', ''spell_arcane_studentofmagic'',
     ''A channeled heal-over-time that applies Temporal Beacon.'', ''mod-sod-mage'', 1)
 ON DUPLICATE KEY UPDATE
    `spell_id` = VALUES(`spell_id`), `class_mask` = VALUES(`class_mask`),
    `slot_mask` = VALUES(`slot_mask`), `name` = VALUES(`name`),
    `icon` = VALUES(`icon`), `description` = VALUES(`description`),
    `source` = VALUES(`source`), `enabled` = VALUES(`enabled`)',
'DO 0');

PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
```

Notes:
- Use a plain single-quoted multi-line string literal (escape inner quotes as `''`)
  — it parses under any `sql_mode`, including `NO_BACKSLASH_ESCAPES`.
- `ON DUPLICATE KEY UPDATE` makes it idempotent; re-running is safe.
- When the engine is absent, `@rune_tbl = 0` and the statement runs `DO 0` (no-op).

## 3. Reserve a `rune_id` band

Pick a high, documented band so modules never collide. Known allocations:

| Module | Band |
|--------|------|
| `mod-sod-mage` | `7000000–7000999` |

## Order of application

The engine's schema must exist before your rune rows land. With manual SQL apply,
apply the engine's base SQL first, then your module's rune SQL. If both are
auto-imported and your rune SQL happens to run first, it harmlessly no-ops once
(the guard) and lands on the next apply — it's idempotent, so just re-run it.

## Future: gated unlocks (not in v1)

The engine ships two tables for a later quest/discovery-gated unlock path; **v1
ignores them** (any class-legal rune is engravable):

- `rune_quest_unlock` (world): `rune_id`, `quest_id` — map a rune to the quest(s)
  that will unlock it.
- `character_rune_unlock` (characters): `guid`, `rune_id` — per-character unlocked
  runes.

`rune_contract.version` (currently `1`) lets your SQL sanity-check compatibility.

See also: [Architecture](architecture.md) · [Deploy & verify](deploy-and-verify.md)
