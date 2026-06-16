-- mod-rune-engraving: per-character state. Idempotent; safe to re-run.

-- The rune engraved in each abstract slot, per character. `slot` is the RuneSlot
-- enum value (see src/RuneEngravingMgr.h). One rune per slot per character.
CREATE TABLE IF NOT EXISTS `character_rune` (
    `guid`    INT UNSIGNED     NOT NULL,
    `slot`    TINYINT UNSIGNED NOT NULL,
    `rune_id` INT UNSIGNED     NOT NULL,
    PRIMARY KEY (`guid`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Runes a character has unlocked (created now, unused in v1: the engine treats
-- a class-legal rune as engravable). Backs the future quest-gated unlock path.
CREATE TABLE IF NOT EXISTS `character_rune_unlock` (
    `guid`    INT UNSIGNED NOT NULL,
    `rune_id` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`guid`, `rune_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
