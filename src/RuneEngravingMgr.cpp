/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "RuneEngravingMgr.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"

RuneEngravingMgr* RuneEngravingMgr::instance()
{
    static RuneEngravingMgr instance;
    return &instance;
}

char const* RuneEngravingMgr::SlotName(uint8 slot)
{
    switch (slot)
    {
        case RUNE_SLOT_HEAD:     return "Head";
        case RUNE_SLOT_NECK:     return "Neck";
        case RUNE_SLOT_SHOULDER: return "Shoulder";
        case RUNE_SLOT_CLOAK:    return "Cloak";
        case RUNE_SLOT_CHEST:    return "Chest";
        case RUNE_SLOT_WRIST:    return "Wrist";
        case RUNE_SLOT_HANDS:    return "Hands";
        case RUNE_SLOT_WAIST:    return "Waist";
        case RUNE_SLOT_LEGS:     return "Legs";
        case RUNE_SLOT_FEET:     return "Feet";
        case RUNE_SLOT_RING:     return "Ring";
        default:                 return "Unknown";
    }
}

void RuneEngravingMgr::LoadCatalog()
{
    std::lock_guard<std::mutex> guard(_catalogMutex);
    _catalog.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT `rune_id`, `spell_id`, `class_mask`, `slot_mask`, `name`, `description` "
        "FROM `rune_template` WHERE `enabled` = 1");
    if (!result)
    {
        LOG_INFO("module", "RuneEngraving: catalog empty (no enabled rows in rune_template).");
        return;
    }

    do
    {
        Field* f = result->Fetch();
        RuneTemplate rune;
        rune.RuneId      = f[0].Get<uint32>();
        rune.SpellId     = f[1].Get<uint32>();
        rune.ClassMask   = f[2].Get<uint32>();
        rune.SlotMask    = f[3].Get<uint32>();
        rune.Name        = f[4].Get<std::string>();
        rune.Description = f[5].Get<std::string>();
        _catalog[rune.RuneId] = std::move(rune);
    } while (result->NextRow());

    LOG_INFO("module", "RuneEngraving: loaded {} rune(s) into the catalog.", _catalog.size());
}

uint32 RuneEngravingMgr::CatalogSize() const
{
    std::lock_guard<std::mutex> guard(_catalogMutex);
    return uint32(_catalog.size());
}

// The returned pointer is valid until the next LoadCatalog(): the catalog is
// only rewritten by startup load and the admin-only `.rune reload`.
RuneTemplate const* RuneEngravingMgr::GetRune(uint32 runeId) const
{
    std::lock_guard<std::mutex> guard(_catalogMutex);
    auto it = _catalog.find(runeId);
    return it != _catalog.end() ? &it->second : nullptr;
}

bool RuneEngravingMgr::RuneFitsSlot(RuneTemplate const& rune, uint8 slot) const
{
    return IsValidSlot(slot) && (rune.SlotMask & (1u << slot)) != 0;
}

bool RuneEngravingMgr::RuneAllowedForClass(Player* player, RuneTemplate const& rune) const
{
    if (!player)
        return false;
    if (rune.ClassMask == 0)
        return true;
    return (rune.ClassMask & (1u << (player->getClass() - 1))) != 0;
}

bool RuneEngravingMgr::IsUnlocked(Player* player, RuneTemplate const& rune) const
{
    // v1: a class-legal rune is considered unlocked. The character_rune_unlock
    // table and the rune_quest_unlock contract back a gated path added later.
    return RuneAllowedForClass(player, rune);
}

std::vector<RuneTemplate const*> RuneEngravingMgr::GetRunesForSlot(Player* player, uint8 slot) const
{
    std::vector<RuneTemplate const*> out;
    if (!player || !IsValidSlot(slot))
        return out;

    std::lock_guard<std::mutex> guard(_catalogMutex);
    for (auto const& [runeId, rune] : _catalog)
    {
        if (RuneFitsSlot(rune, slot) && RuneAllowedForClass(player, rune) && IsUnlocked(player, rune))
            out.push_back(&rune);
    }
    return out;
}

void RuneEngravingMgr::LoadPlayer(Player* player)
{
    if (!player)
        return;

    ObjectGuid guid = player->GetGUID();
    std::array<uint32, RUNE_SLOT_MAX> slots{};

    QueryResult result = CharacterDatabase.Query(
        "SELECT `slot`, `rune_id` FROM `character_rune` WHERE `guid` = {}",
        guid.GetCounter());
    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            uint8 slot = f[0].Get<uint8>();
            uint32 runeId = f[1].Get<uint32>();
            if (IsValidSlot(slot))
                slots[slot] = runeId;
        } while (result->NextRow());
    }

    std::lock_guard<std::mutex> guard(_stateMutex);
    _engraved[guid] = slots;
}

void RuneEngravingMgr::UnloadPlayer(ObjectGuid guid)
{
    std::lock_guard<std::mutex> guard(_stateMutex);
    _engraved.erase(guid);
}

void RuneEngravingMgr::ApplyAll(Player* player)
{
    if (!player)
        return;

    std::lock_guard<std::mutex> guard(_stateMutex);
    auto it = _engraved.find(player->GetGUID());
    if (it == _engraved.end())
        return;

    for (uint8 slot = 0; slot < RUNE_SLOT_MAX; ++slot)
    {
        uint32 runeId = it->second[slot];
        if (!runeId)
            continue;

        RuneTemplate const* rune = GetRune(runeId);
        if (!rune)
            continue; // unknown right now (e.g. rune disabled) — keep the row, don't grant

        // A class mismatch can only arise if this character GUID was reused by a
        // character of a different class (real characters can't change class).
        // The stored rune isn't this character's — purge it instead of granting.
        if (!RuneAllowedForClass(player, *rune))
        {
            it->second[slot] = 0;
            CharacterDatabase.Execute(
                "DELETE FROM `character_rune` WHERE `guid` = {} AND `slot` = {}",
                player->GetGUID().GetCounter(), uint32(slot));
            continue;
        }

        if (rune->SpellId)
            player->learnSpell(rune->SpellId, /*temporary*/ true);
    }
}

bool RuneEngravingMgr::SpellGrantedByOtherSlot(ObjectGuid guid, uint32 spellId, uint8 exceptSlot) const
{
    auto it = _engraved.find(guid);
    if (it == _engraved.end())
        return false;

    for (uint8 slot = 0; slot < RUNE_SLOT_MAX; ++slot)
    {
        if (slot == exceptSlot)
            continue;
        uint32 runeId = it->second[slot];
        if (!runeId)
            continue;
        if (RuneTemplate const* rune = GetRune(runeId))
            if (rune->SpellId == spellId)
                return true;
    }
    return false;
}

bool RuneEngravingMgr::Engrave(Player* player, uint8 slot, uint32 runeId)
{
    if (!player || !IsValidSlot(slot))
        return false;

    std::lock_guard<std::mutex> guard(_stateMutex);

    RuneTemplate const* rune = GetRune(runeId);
    if (!rune || !RuneFitsSlot(*rune, slot) || !RuneAllowedForClass(player, *rune) || !IsUnlocked(player, *rune))
        return false;

    ObjectGuid guid = player->GetGUID();
    std::array<uint32, RUNE_SLOT_MAX>& slots = _engraved[guid];

    // Strip the spell from the rune currently in this slot, unless another slot
    // still grants the same spell.
    if (uint32 oldRuneId = slots[slot])
        if (RuneTemplate const* oldRune = GetRune(oldRuneId))
            if (oldRune->SpellId && oldRune->SpellId != rune->SpellId
                && !SpellGrantedByOtherSlot(guid, oldRune->SpellId, slot))
                player->removeSpell(oldRune->SpellId, SPEC_MASK_ALL, /*onlyTemporary*/ true);

    slots[slot] = runeId;
    CharacterDatabase.Execute(
        "REPLACE INTO `character_rune` (`guid`, `slot`, `rune_id`) VALUES ({}, {}, {})",
        guid.GetCounter(), uint32(slot), runeId);

    if (rune->SpellId)
        player->learnSpell(rune->SpellId, /*temporary*/ true);

    return true;
}

bool RuneEngravingMgr::RemoveRune(Player* player, uint8 slot)
{
    if (!player || !IsValidSlot(slot))
        return false;

    std::lock_guard<std::mutex> guard(_stateMutex);

    ObjectGuid guid = player->GetGUID();
    auto it = _engraved.find(guid);
    if (it == _engraved.end())
        return false;

    uint32 runeId = it->second[slot];
    if (!runeId)
        return false;

    if (RuneTemplate const* rune = GetRune(runeId))
        if (rune->SpellId && !SpellGrantedByOtherSlot(guid, rune->SpellId, slot))
            player->removeSpell(rune->SpellId, SPEC_MASK_ALL, /*onlyTemporary*/ true);

    it->second[slot] = 0;
    CharacterDatabase.Execute(
        "DELETE FROM `character_rune` WHERE `guid` = {} AND `slot` = {}",
        guid.GetCounter(), uint32(slot));

    return true;
}

uint32 RuneEngravingMgr::GetEngraved(ObjectGuid guid, uint8 slot) const
{
    if (!IsValidSlot(slot))
        return 0;

    std::lock_guard<std::mutex> guard(_stateMutex);
    auto it = _engraved.find(guid);
    return it != _engraved.end() ? it->second[slot] : 0;
}

void RuneEngravingMgr::DeleteCharacterData(CharacterDatabaseTransaction trans, uint32 guidLow)
{
    trans->Append("DELETE FROM `character_rune` WHERE `guid` = {}", guidLow);
    trans->Append("DELETE FROM `character_rune_unlock` WHERE `guid` = {}", guidLow);

    std::lock_guard<std::mutex> guard(_stateMutex);
    _engraved.erase(ObjectGuid::Create<HighGuid::Player>(guidLow));
}
