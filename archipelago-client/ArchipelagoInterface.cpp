#include <sstream>
#include <spdlog/spdlog.h>

#include "ArchipelagoInterface.h"
#include "ItemRandomiser.h"
#include "subprojects/apclientpp/apuuid.hpp"
#include "er_version_check.h"
#include "er_gamehook.h"   // er_ap::game::SetAutoUpgrade (auto_upgrade)

// Client's implemented slot_data CONTRACT version (beta.N lockstep with the apworld range and the
// randomizer). Checked (advisory) against slot_data["versions"] at connect. Flips to a release at
// freeze. See BRIEF-contract-map-reveal / er_version_check.h.
static constexpr const char* ER_CLIENT_CONTRACT_VERSION = "0.1.0-beta.4";

#ifdef __EMSCRIPTEN__
#define DATAPACKAGE_CACHE "/settings/datapackage.json"
#define UUID_FILE "/settings/uuid"
#else
#define DATAPACKAGE_CACHE "datapackage.json" // TODO: place in %appdata%
#define UUID_FILE "uuid" // TODO: place in %appdata%
#endif

extern CCore* Core;
extern CItemRandomiser* ItemRandomiser;
extern CGameHook* GameHook;

bool ap_sync_queued = false;
APClient* ap;

BOOL CArchipelago::Initialise(std::string URI) {
	
	spdlog::debug("CArchipelago::Initialise");

	// read or generate uuid, required by AP
	std::string uuid = ap_get_uuid(UUID_FILE);
	if (ap != nullptr) {
		ap->reset();
	}

	ap = new APClient(uuid, "EldenRing", URI);

	ap_sync_queued = false;
	ap->set_socket_connected_handler([]() {
		});
	ap->set_socket_disconnected_handler([]() {
		Core->connected = false;
		});
	ap->set_slot_connected_handler([](const json& data) {
		spdlog::info("Slot connected successfully, reading slot data ... ");
		// read the archipelago slot data

		//Mandatory values
		if (!data.contains("apIdsToItemIds") || !data.contains("itemCounts") || !data.contains("seed") || !data.contains("slot")) {
			Core->Panic("Please check the following values : [apItemsToItemIds], [seed] and [slot]", "One of the mandatory values is missing in the slot data", AP_MissingValue, 1);
		}

		// Contract version (beta.3): WARN (don't refuse) if our implemented version isn't in the
		// server's range -- advisory lockstep, see BRIEF-contract-map-reveal. The randomizer also
		// checks at bake; hardening this to a refusal is a follow-up once the client build is tested.
		if (data.contains("versions")) {
			std::string verRange = data.at("versions").get<std::string>();
			if (!er_ap::versionSatisfies(ER_CLIENT_CONTRACT_VERSION, verRange)) {
				spdlog::warn("Contract mismatch: client {} not in server range {} -- update one side.",
					ER_CLIENT_CONTRACT_VERSION, verRange);
				GameHook->showBanner("Archipelago contract version mismatch -- see log");
			}
		}

		std::map<std::string, DWORD> map;
		data.at("apIdsToItemIds").get_to(map);
		for (std::map<std::string, DWORD>::iterator it = map.begin(); it != map.end(); ++it) {
			ItemRandomiser->pApItemsToItemIds[std::stoll(it->first)] = it->second;
		}

		map.clear();
		data.at("itemCounts").get_to(map);
		for (std::map<std::string, DWORD>::iterator it = map.begin(); it != map.end(); ++it) {
			ItemRandomiser->pItemCounts[std::stoll(it->first)] = it->second;
		}
		
		data.at("slot").get_to(Core->pSlotName);

		if (data.contains("options")) {
			(data.at("options").contains("auto_equip")) ? (data.at("options").at("auto_equip").get_to(ItemRandomiser->dIsAutoEquip)) : ItemRandomiser->dIsAutoEquip = false;
			(data.at("options").contains("lock_equip")) ? (data.at("options").at("lock_equip").get_to(GameHook->dLockEquipSlots)) : GameHook->dLockEquipSlots = false;
			(data.at("options").contains("death_link")) ? (data.at("options").at("death_link").get_to(GameHook->dIsDeathLink)) : GameHook->dIsDeathLink = false;
			(data.at("options").contains("enable_dlc")) ? (data.at("options").at("enable_dlc").get_to(GameHook->dEnableDLC)) : GameHook->dEnableDLC = false;
			// ER goal detection: which ending completes the slot (see isSoulOfCinderDefeated).
			(data.at("options").contains("ending_condition")) ? (data.at("options").at("ending_condition").get_to(GameHook->dEndingCondition)) : GameHook->dEndingCondition = 1;
			// Auto-upgrade (slot_data auto_upgrade; patch_client_autoupgrade.py): upgrade any weapon
			// you acquire to your current highest reinforce level on the same smithing track.
			if (data.at("options").contains("auto_upgrade"))
				er_ap::game::SetAutoUpgrade(data.at("options").at("auto_upgrade").get<int>());
			else
				er_ap::game::SetAutoUpgrade(0);
			// Global Scadutree Blessing (default off; patch_client_global_scadu_blessing.py).
			if (data.at("options").contains("global_scadutree_blessing"))
				er_ap::game::SetGlobalScaduBlessing(data.at("options").at("global_scadutree_blessing").get<int>());
			else
				er_ap::game::SetGlobalScaduBlessing(0);
		}

		// Goal locations (optional; nonempty for ending_condition 2/3): goal = all checked.
		Core->goalLocations.clear();
		if (data.contains("goalLocations")) {
			data.at("goalLocations").get_to(Core->goalLocations);
			if (!Core->goalLocations.empty()) {
				spdlog::info("Goal: complete all {} goal locations (ending_condition {})",
					Core->goalLocations.size(), GameHook->dEndingCondition);
			}
		}

		// Dungeon sweep map (optional; present when the apworld's dungeon_sweep option is on):
		// trigger location id -> all location ids in that dungeon. See CCore::PollLocationFlags.
		Core->dungeonSweeps.clear();
		if (data.contains("dungeonSweeps")) {
			std::map<std::string, std::vector<int64_t>> sweepMap;
			data.at("dungeonSweeps").get_to(sweepMap);
			for (const auto& entry : sweepMap) {
				Core->dungeonSweeps[std::stoll(entry.first)] = entry.second;
			}
			if (!Core->dungeonSweeps.empty()) {
				spdlog::info("Dungeon sweep enabled for {} dungeon(s)", Core->dungeonSweeps.size());
			}
		}

		// Region-fusion grace bundle (optional; present only when region gating is active —
		// apworld world_logic < 3): lock-item name -> grace warp-unlock flags. The received-items
		// handler queues these flags when the matching lock item arrives, and
		// CCore::FlushPendingGraceFlags sets them on an in-game tick so the region's Sites of
		// Grace become fast-travelable. See SPEC-region-chain.md.
		Core->regionGraces.clear();
		if (data.contains("regionGraces")) {
			data.at("regionGraces").get_to(Core->regionGraces);
			if (!Core->regionGraces.empty()) {
				spdlog::info("Region grace bundle: {} region(s) will unlock graces on lock-item receipt",
					Core->regionGraces.size());
			}
		}

		// Region-open flags (physical enforcement; SPEC-region-fog-gates.md): lock-item name -> one
		// flag set on receipt, gated on by baked border fog gates. Parsed alongside regionGraces;
		// set through the same pendingGraceFlags drain.
		Core->regionOpenFlags.clear();
		if (data.contains("regionOpenFlags")) {
			data.at("regionOpenFlags").get_to(Core->regionOpenFlags);
			if (!Core->regionOpenFlags.empty()) {
				spdlog::info("Region-open flags: {} region(s) will open on lock-item receipt",
					Core->regionOpenFlags.size());
			}
		}

		// Natural-key triggers (SPEC-natural-locks.md): tolerant parse -- older seeds omit the key.
		// LockName -> { "anyOf": [ {"items":[...],"flags":[...]}, ... ] }. A clause is satisfied
		// when all items were received AND all flags are set; ANY clause blooms the region. Reuses
		// the existing regionGraces/regionOpenFlags/lockRevealFlags/lockNotifyItems tables for the
		// bloom effect (see CCore::EvaluateNaturalKeyTriggers).
		Core->naturalKeyTriggers.clear();
		if (data.contains("naturalKeyTriggers")) {
			for (auto& kv : data.at("naturalKeyTriggers").items()) {
				std::vector<CCore::NKClause> clauses;
				if (kv.value().contains("anyOf")) {
					for (auto& c : kv.value().at("anyOf")) {
						CCore::NKClause cl;
						if (c.contains("items")) for (auto& it : c.at("items")) cl.items.push_back(it.get<std::string>());
						if (c.contains("flags")) for (auto& fl : c.at("flags")) cl.flags.push_back(fl.get<uint32_t>());
						clauses.push_back(std::move(cl));
					}
				}
				Core->naturalKeyTriggers[kv.key()] = std::move(clauses);
			}
			if (!Core->naturalKeyTriggers.empty())
				spdlog::info("Natural-key triggers: {} region(s) bloom on vanilla trigger disjunction",
					Core->naturalKeyTriggers.size());
		}
		Core->areaLockFlags.clear();
		if (data.contains("areaLockFlags")) {
			data.at("areaLockFlags").get_to(Core->areaLockFlags);
			if (!Core->areaLockFlags.empty())
				spdlog::info("Region-lock: {} area range(s) enforced (generalized detection)", Core->areaLockFlags.size());
		}
		// DLC-only auto-entry: flag set when the player is detected in the start area (Chapel of Anticipation).
		Core->dlcEntryWarpFlag = data.value("dlcEntryWarpFlag", 0);
		Core->dlcStartAreaId   = data.value("dlcStartAreaId", 0);
		if (Core->dlcEntryWarpFlag)
			spdlog::info("DLC auto-entry: flag {} fires when player enters area {}", Core->dlcEntryWarpFlag, Core->dlcStartAreaId);
		// Random starting region: same latch pattern as DLC auto-entry, for the rolled start region.
		Core->randomStartWarpFlag = data.value("randomStartWarpFlag", 0);
		Core->randomStartAreaId   = data.value("randomStartAreaId", 0);
		Core->randomStartDoneFlag = data.value("randomStartDoneFlag", 0);
		if (Core->randomStartWarpFlag)
			spdlog::info("Random start: flag {} fires when player enters area {}", Core->randomStartWarpFlag, Core->randomStartAreaId);
		Core->lockRevealFlags.clear();
		if (data.contains("lockRevealFlags")) {
			data.at("lockRevealFlags").get_to(Core->lockRevealFlags);
			if (!Core->lockRevealFlags.empty())
				spdlog::info("Region-lock: {} lock(s) reveal/open on receipt", Core->lockRevealFlags.size());
		}
		Core->lockNotifyItems.clear();
		if (data.contains("lockNotifyItems")) {
			data.at("lockNotifyItems").get_to(Core->lockNotifyItems);
			if (!Core->lockNotifyItems.empty())
				spdlog::info("Region-lock: {} lock(s) grant an unlock-notify item", Core->lockNotifyItems.size());
		}
		Core->progressiveGrants.clear();
		if (data.contains("progressiveGrants")) {
			for (auto& kv : data.at("progressiveGrants").items()) {
				std::vector<std::pair<std::vector<uint32_t>, std::vector<uint32_t>>> tiers;
				for (auto& t : kv.value()) {
					std::vector<uint32_t> flags;
					for (auto& f : t.at("flags")) flags.push_back(f.get<uint32_t>());
					// Goods arrive as a single "goods" (bells/consumables) OR a "goodsList" array
					// (progressive_physick = a whole tear family per step). Tolerate either; a tier with
					// NEITHER is skipped, never thrown -- a throw here aborts the whole slot-data handler
					// and silently kills ALL item grants (the 'key goods not found' bug).
					std::vector<uint32_t> goods;
					if (t.contains("goodsList")) { for (auto& g : t.at("goodsList")) goods.push_back(g.get<uint32_t>()); }
					else if (t.contains("goods")) { goods.push_back(t.at("goods").get<uint32_t>()); }
					tiers.emplace_back(std::move(goods), std::move(flags));
				}
				Core->progressiveGrants[kv.key()] = std::move(tiers);
			}
			if (!Core->progressiveGrants.empty())
				spdlog::info("Progressive: {} progressive item(s) loaded", Core->progressiveGrants.size());
		}
		// Limgrave start graces: queue them at connect so the free starting region is fully fast-
		// travelable from load-in (drained by FlushPendingGraceFlags like the on-receipt grace bundle).
		if (data.contains("startGraces")) {
			std::vector<uint32_t> _sg;
			data.at("startGraces").get_to(_sg);
			for (uint32_t _f : _sg) Core->pendingGraceFlags.push_back(_f);
			if (!_sg.empty())
				spdlog::info("Region-lock: queued {} Limgrave start grace(s)", _sg.size());
		}
		// Start items (e.g. Spectral Steed Whistle = Torrent): grant once at load-in via the same
		// in-world grant queue as the unlock-notify items.
		if (data.contains("startItems") && !Core->startItemsGranted) {
			// Each entry is either a bare FullID (count 1) or a [FullID, count] pair (beta.4):
			// the pair form lets a single rune stack (Quick Start = 71 Lord's Runes) grant in one call.
			const auto& _si = data.at("startItems");
			for (const auto& _e : _si) {
				if (_e.is_array()) {
					int32_t _id = _e.at(0).get<int32_t>();
					int32_t _ct = _e.size() > 1 ? _e.at(1).get<int32_t>() : 1;
					if (_ct < 1) _ct = 1;
					Core->pendingStartItems.push_back({_id, _ct});
				} else {
					Core->pendingStartItems.push_back({_e.get<int32_t>(), 1});
				}
			}
			if (!_si.empty())
				spdlog::info("Region-lock: queued {} start item(s)", _si.size());
		}

		// Map reveal (slot_data "reveal_all_maps"; beta.3): under map_option=give the apworld grants
		// no map fragment items -- the client sets every region's map-reveal flag directly. One-shot
		// per connect, drained on a loaded tick in CCore::Run (retried until the flag holder is
		// ready). See BRIEF-contract-map-reveal / TODO #5.
		Core->revealAllMapsPending = false;
		if (data.contains("reveal_all_maps") && data.at("reveal_all_maps").get<bool>()) {
			Core->revealAllMapsPending = true;
			spdlog::info("reveal_all_maps: will reveal all region maps (no map items granted)");
		}

		std::list<std::string> tags;
		if (GameHook->dIsDeathLink) {
			tags.push_back("DeathLink");
			ap->ConnectUpdate(false, 1, true, tags);
		}

		Core->SetSeed(data.at("seed"), false /* fromSave */);
		Core->InitSavePath();
		Core->connected = true;

		});
	ap->set_slot_disconnected_handler([]() {
		spdlog::info("Slot disconnected");
		Core->connected = false;
		GameHook->showBanner(L"Archipelago disconnected! Don't pick up any items until it reconnects");

		});
	ap->set_slot_refused_handler([](const std::list<std::string>& errors){
		for (const auto& error : errors) {
			spdlog::warn("Connection refused: {}", error);
		}
		GameHook->showBanner(L"Archipelago connection refused!");
		});

	ap->set_room_info_handler([]() {
		std::list<std::string> tags;
		if (GameHook->dIsDeathLink) { tags.push_back("DeathLink"); }
		// items_handling 0b111: remote items + OWN-WORLD items + starting inventory. Own-world
		// echo is essential for ER: acquisitions that bypass the AddItemFunc detour (shop
		// purchases, detected via flag polling) can't grant locally, so ALL items — including
		// self-found ones — are granted through the server's ReceivedItems stream, deduped by
		// the persisted last_received_index. (Was 5 = no own-world echo: a shop-bought
		// self-item sent the check but the item never arrived.)
		ap->ConnectSlot(Core->pSlotName, Core->pPassword, 7, tags, { 0,6,6 });
		});

	ap->set_items_received_handler([](const std::list<APClient::NetworkItem>& items) {

		if (!ap->is_data_package_valid()) {
			// NOTE: this should not happen since we ask for data package before connecting
			if (!ap_sync_queued) ap->Sync();
			ap_sync_queued = true;
			return;
		}

		for (const auto& item : items) {
			std::string itemname = ap->get_item_name(item.item, ap->get_player_game(item.player));
			std::string sender = ap->get_player_alias(item.player);
			std::string location = ap->get_location_name(item.location, ap->get_player_game(item.player));

			spdlog::info("#{}: {} from {} - {}", item.index, itemname, sender, location);

			// Maintain the received-NAME set for natural-key trigger evaluation. Rebuilt from the
			// items_received replay on reconnect (items_handling 0b111), so it needs no persistence.
			Core->receivedItemNames.insert(itemname);

			// Region-fusion: if this item is a region lock key, queue its grace warp-unlock flags
			// for CCore::FlushPendingGraceFlags to set on the next in-game tick. Done HERE, not in
			// the grant path, because every region-lock key shares sentinel er_code 99999 and is
			// indistinguishable by GiveNextItem — the item NAME (region identity) only exists here.
			// SetEventFlag is idempotent + save-persisted, so re-queuing on reconnect is harmless.
			auto graceIt = Core->regionGraces.find(itemname);
			if (graceIt != Core->regionGraces.end()) {
				for (uint32_t flag : graceIt->second) {
					Core->pendingGraceFlags.push_back(flag);
				}
				spdlog::info("Region lock '{}' received: queued {} grace flag(s)",
					itemname, graceIt->second.size());
			}

			// Region-fusion (physical): also queue the region-open flag so baked border fog gates
			// drop. Same queue/drain as the grace flags (SetEventFlag, idempotent + save-persisted).
			auto openIt = Core->regionOpenFlags.find(itemname);
			if (openIt != Core->regionOpenFlags.end()) {
				Core->pendingGraceFlags.push_back(openIt->second);
				spdlog::info("Region lock '{}' received: queued region-open flag {}",
					itemname, openIt->second);
			}
			// Generalized open-state: setting the region's map-reveal flags both reveals the map and
			// flips the enforcement open-flag (reveal_flags[0]) so the kick check stops -> region opens.
			auto revealIt = Core->lockRevealFlags.find(itemname);
			if (revealIt != Core->lockRevealFlags.end()) {
				for (int32_t flag : revealIt->second)
					Core->pendingGraceFlags.push_back((uint32_t)flag);
				spdlog::info("Region lock '{}' received: queued {} map-reveal/open flag(s)",
					itemname, revealIt->second.size());
			}
			// Unlock NOTIFICATION: queue the region's map/token grant so the native ticker fires on a
			// loaded tick (in-world) and tells the player WHICH region just opened.
			auto notifyIt = Core->lockNotifyItems.find(itemname);
			if (notifyIt != Core->lockNotifyItems.end())
				Core->pendingNotifyGrants.push_back(notifyIt->second);

			// Companion acquisition flags: itemname -> vanilla "obtained" event flag(s) that gate NPC/
			// shop/tutorial behavior a raw client grant never trips. Setting them fires the item's
			// tutorial and stops the Twin Maiden Husks re-selling it (their ESD checks these flags).
			// Same drain as the region flags above (FlushPendingGraceFlags -> SetEventFlag, idempotent
			// + save-persisted). Add an entry here for any future possession-gated companion item.
			static const std::unordered_map<std::string, std::vector<uint32_t>> kCompanionAcquireFlags = {
				{ "Spirit Calling Bell", { 60110u } },  // summon tutorial + Twin Maiden dup-sale
				{ "Whetstone Knife",     { 60130u } },  // Ashes of War tutorial + Twin Maiden dup-sale
				{ "Iron Whetblade",      { 65610u } },  // unlocks affinities (aux flags) + dup-sale
				{ "Red-Hot Whetblade",   { 65640u } },
				{ "Sanctified Whetblade",{ 65660u } },
				{ "Glintstone Whetblade",{ 65680u } },
				{ "Black Whetblade",     { 65720u } },
			};
			auto compIt = kCompanionAcquireFlags.find(itemname);
			if (compIt != kCompanionAcquireFlags.end()) {
				for (uint32_t flag : compIt->second)
					Core->pendingGraceFlags.push_back(flag);
				spdlog::info("Companion item '{}' received: queued {} acquisition flag(s)",
					itemname, compIt->second.size());
			}

			// Key-item acquisition flags: vanilla KEY ITEMS whose progression gate reads an
			// "obtained" EVENT FLAG, not inventory. A raw client goods-grant leaves the gate
			// sealed, so set the flag here. Same NAME->flags shape + same pendingGraceFlags drain
			// (FlushPendingGraceFlags -> SetEventFlag, idempotent + save-persisted) as the
			// companion map above. Kept separate from kCompanionAcquireFlags so each map's intent
			// stays clear. Add an entry for any future flag-gated key item.
			static const std::unordered_map<std::string, std::vector<uint32_t>> kKeyItemAcquireFlags = {
				{ "Rold Medallion",   { 400001u } },  // Grand Lift of Rold gates on 400001 (was sealed)
				{ "Drawing-Room Key", { 400072u } },  // Volcano Manor drawing-room transition
			};
			auto keyIt = kKeyItemAcquireFlags.find(itemname);
			if (keyIt != kKeyItemAcquireFlags.end()) {
				for (uint32_t flag : keyIt->second)
					Core->pendingGraceFlags.push_back(flag);
				spdlog::info("Key item '{}' received: queued {} obtained-flag(s)",
					itemname, keyIt->second.size());
			}

			// Great-rune restore-on-receipt: under num_regions_rune_source=pool the great runes are
			// INJECTED into the pool (patch_apworld_num_regions_pool_runes.py) and arrive as the raw,
			// UNRESTORED goods row, which the game won't let you equip until you activate it at the
			// matching Divine Tower (which the seal may have removed). ER ships a second 'restored'
			// EquipParamGoods row per rune (191-196; Paramdex EquipParamGoods 33-38, named "(Restored)"
			// in the apworld GOODS.txt). Granting that restored row = the same state the tower confers,
			// so the player can equip + Rune-Arc it immediately. We push it through the normal grant
			// queue as a GOODS-packed FullID (id | 0x40000000) -- identical mechanism to the Lord's Rune
			// overflow grant a few lines below. ADDITIVE: no `continue`, the raw rune still grants too.
			// The Great Rune of the Unborn (Rennala, 10080) has no tower / no restored row and is usable
			// as received, so it is intentionally absent.
			static const std::unordered_map<std::string, uint32_t> kGreatRuneRestoreGoods = {
				{ "Godrick's Great Rune",  191u },  // unrestored 8148 -> restored 191
				{ "Radahn's Great Rune",   192u },  // unrestored 8149 -> restored 192
				{ "Morgott's Great Rune",  193u },  // unrestored 8150 -> restored 193
				{ "Rykard's Great Rune",   194u },  // unrestored 8151 -> restored 194
				{ "Mohg's Great Rune",     195u },  // unrestored 8152 -> restored 195
				{ "Malenia's Great Rune",  196u },  // unrestored 8153 -> restored 196
			};
			auto runeIt = kGreatRuneRestoreGoods.find(itemname);
			if (runeIt != kGreatRuneRestoreGoods.end()) {
				ItemRandomiser->receivedItemsQueue.push_front({
					(DWORD)(runeIt->second | 0x40000000), 1, sender, itemname,
					item.player == ap->get_player_number()
				});
				spdlog::info("Great rune '{}' received: also granting restored goods {} (usable now)",
					itemname, runeIt->second);
			}

			// Progressive stone bells: resolve the tier by receipt order. The Kth copy sets its Twin
			// Maiden shop flags (idempotent) AND grants the cosmetic bell goods via the normal queue
			// (which respects last_received_index -> no dup on reconnect). Counter resets on the queue
			// rebuild, so the full replay recomputes tiers from 0.
			auto progIt = Core->progressiveGrants.find(itemname);
			if (progIt != Core->progressiveGrants.end()) {
				// Persisted + index-deduped tier (Core.h progressiveHighIndex). Only a copy with a NEW
				// AP item.index (> the highest already applied) advances the tier; replayed copies on a
				// reconnect / new session are skipped, so the persisted counter stays correct across
				// sessions. Shop-unlock flags are ER event flags that persist in the game save, so a
				// skipped replay needs no re-application.
				if ((long long)item.index > Core->progressiveHighIndex) {
					int k = Core->progressiveCounter[itemname]++;
					const auto& tiers = progIt->second;
					if (k < (int)tiers.size()) {
						for (uint32_t f : tiers[k].second) Core->pendingGraceFlags.push_back(f);
						for (uint32_t g : tiers[k].first) {
							ItemRandomiser->receivedItemsQueue.push_front({
								g, 1, sender, itemname,
								item.player == ap->get_player_number()
							});
						}
						spdlog::info("Progressive '{}' #{}: {} goods + {} shop flag(s)", itemname, k + 1, tiers[k].first.size(), tiers[k].second.size());
					} else {
						// Already at max tier: every shop rung this bell unlocks is live, so this overflow copy
						// (the pool ships more copies than tiers, stone_bells.py PROGRESSIVE_BELL_POOL_COUNT)
						// would otherwise be a dead no-op. Convert it into one Lord's Rune (EquipParamGoods 2919,
						// 50,000 runes) granted through the normal queue -- one acquisition popup, like the
						// cosmetic bell above. GOODS-packed FullID, same form as Quick Start. The persisted
						// index-dedup (progressiveHighIndex) means each real overflow copy grants exactly once.
						ItemRandomiser->receivedItemsQueue.push_front({
							(DWORD)(2919 | 0x40000000), 1, sender, std::string("Lord's Rune"),
							item.player == ap->get_player_number()
						});
						spdlog::info("Progressive '{}' #{}: max tier reached -> granted 1 Lord's Rune", itemname, k + 1);
					}
					Core->progressiveHighIndex = (long long)item.index;
					Core->writeSaveFileNextTick = true;
				}
				continue;  // handled; skip the normal sentinel(99998) grant path
			}

			//Determine the item address
			auto ds3IdSearch = ItemRandomiser->pApItemsToItemIds.find(item.item);
			if (ds3IdSearch == ItemRandomiser->pApItemsToItemIds.end()) {
				spdlog::warn("Item '{}' was not found in the item pool. Please check your seed options.", itemname);
				continue;
			}

			auto countSearch = ItemRandomiser->pItemCounts.find(item.item);
			ItemRandomiser->receivedItemsQueue.push_front({
				ds3IdSearch->second,
				countSearch == ItemRandomiser->pItemCounts.end() ? 1 : countSearch->second,
				sender,                                  // Notify v2: source shown at grant time
				itemname,                                // AP canonical item name (verbatim)
				item.player == ap->get_player_number()   // self-found -> notification drops the "from"
			});
		}
		});

	//TODO :   * you can still use `set_data_package` or `set_data_package_from_file` during migration to make use of the old cache

	ap->set_print_handler([](const std::string& msg) {
		spdlog::info(msg);
		GameHook->showBanner(msg);
		});

	ap->set_print_json_handler([](const APClient::PrintJSONArgs& args) {
		// Log every server message (the AP item/chat/hint feed), as before.
		spdlog::info(ap->render_json(args.data, APClient::RenderFormat::TEXT));

		// OUTGOING notification (notify v2): when WE checked a location whose item belongs to
		// ANOTHER player, surface "Sent <item> to <player>". In an ItemSend, item->player is the
		// finder (us) and 'receiving' is the recipient; the item lives in the recipient's world, so
		// resolve its name with the recipient's game. Self-found and incoming are handled elsewhere
		// (the items_received 'X from Y' path), so exclude receiving == us.
		if (args.type == "ItemSend" && args.item && args.receiving) {
			int me = ap->get_player_number();
			if (args.item->player == me && *args.receiving != me) {
				std::string itemName  = ap->get_item_name(args.item->item, ap->get_player_game(*args.receiving));
				std::string recipient = ap->get_player_alias(*args.receiving);
				spdlog::info("Sent {} to {}", itemName, recipient);
				// TODO(banner): also show this on-screen. Fine as a banner for outgoing (not mid-fight
				// when sending). Needs the dynamic-text mechanism (runtime FMG) -- see SPEC-notify-banner.md.
			}
		}
		});

	ap->set_bounced_handler([](const json& cmd) {
		if (GameHook->dIsDeathLink) {
			spdlog::debug("Received DeathLink");
			auto tagsIt = cmd.find("tags");
			auto dataIt = cmd.find("data");
			if (tagsIt != cmd.end() && tagsIt->is_array()
				&& std::find(tagsIt->begin(), tagsIt->end(), "DeathLink") != tagsIt->end())
			{
				if (dataIt != cmd.end() && dataIt->is_object()) {
					json data = *dataIt;
					if (data["source"].get<std::string>() != Core->pSlotName) {
						
						std::string source = data["source"].is_string() ? data["source"].get<std::string>().c_str() : "???";
						std::string cause = data["cause"].is_string() ? data["cause"].get<std::string>().c_str() : "???";
						std::string message = "Died by the hands of " + source + " : " + cause;
						spdlog::info(message);
						GameHook->showBanner(message);

						GameHook->deathLinkData = true;
					}
				}
				else {
					spdlog::debug("Bad deathlink packet!");
				}
			}
		}
		});
	
	return true;
}


VOID CArchipelago::say(std::string message) {
	if (ap && ap->get_state() == APClient::State::SLOT_CONNECTED) {
		ap->Say(message);
	}
}

// Tell the server the goal is complete (CLIENT_GOAL), with logging.
VOID CArchipelago::gameFinished() {
	if (ap && ap->get_state() == APClient::State::SLOT_CONNECTED) {
		if (ap->StatusUpdate(APClient::ClientStatus::GOAL)) {
			spdlog::info("GOAL COMPLETE! Sent CLIENT_GOAL to the server");
		}
		else {
			spdlog::warn("Failed to send CLIENT_GOAL status update");
		}
	}
}


BOOLEAN CArchipelago::isConnected() {
	return ap && ap->get_state() == APClient::State::SLOT_CONNECTED;
}

// ending_condition 2/3 goal: every goal location checked (server-side truth, so this
// also completes retroactively on reconnect). False whenever goalLocations is empty
// (ec 0/1 use boss defeat flags via CGameHook::isSoulOfCinderDefeated instead).
BOOLEAN CArchipelago::isGoalComplete() {
	if (!ap || ap->get_state() != APClient::State::SLOT_CONNECTED) return false;
	if (Core->goalLocations.empty()) return false;
	const std::set<int64_t> checked = ap->get_checked_locations();
	for (int64_t loc : Core->goalLocations) {
		if (!checked.count(loc)) return false;
	}
	return true;
}

VOID CArchipelago::update() {

	if (ap) ap->poll();

	int size = ItemRandomiser->checkedLocationsList.size();
	if (ap && size > 0) {
		if (ap->LocationChecks(ItemRandomiser->checkedLocationsList)) {
			spdlog::debug("{} checks sent successfully", size);
			ItemRandomiser->checkedLocationsList.clear();
		}
		else {
			spdlog::debug("{} checks have not been sent and will be kept in queue", size);
		}
	}
}

VOID CArchipelago::sendDeathLink() {
	if (!ap || !GameHook->dIsDeathLink) return;

	spdlog::info("Sending deathlink");

	json data{
		{"time", ap->get_server_time()},
		{"cause", "Dark Souls III."},
		{"source", ap->get_slot()},
	};
	ap->Bounce(data, {}, {}, { "DeathLink" });
}
