#pragma once

#include <optional>
#include "GameTypes.h"

class CAutoEquip {
public:
	virtual void AutoEquipItem(SItemBuffer* pItemBuffer);
	virtual bool FindEquipType(uint32_t dItem, uint32_t* pArray);
	virtual uint32_t GetInventorySlotID(uint32_t dItemID);
	virtual void LockUnlockEquipSlots(int iIsUnlock);

	// Returns the equip slot for the given item ID, or std::nullopt if the item shouldn't be
	// auto-equipped.
	std::optional<EquipSlot> SortItem(uint32_t dItemID);
};