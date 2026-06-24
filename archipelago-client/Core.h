#pragma once
#define ASIO_STANDALONE
#define _WEBSOCKETPP_CPP11_INTERNAL_
// _CRT_SECURE_NO_WARNINGS is defined on the command line (vcxproj PreprocessorDefinitions);
// defining it here as well triggers C4005 macro-redefinition.
//#define WSWRAP_NO_SSL

#include <modengine/extension.h>
#include "subprojects/apclientpp/apclient.hpp"
#include <windows.h>
#include <Windows.h>
#include <iostream>
#include <vector>
#include <map>
#include <deque>
#include <string>
#include <fstream>
#include <bitset>
#include <tlhelp32.h>
#include <stdio.h>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "ArchipelagoInterface.h"

#define int3 __debugbreak();

#define FE_InitFailed 0
#define FE_AmountTooHigh 1
#define FE_NullPtr 2
#define FE_NullArray 3
#define FE_BadFunc 4
#define FE_MemError 5
#define HE_InvalidItemType 6
#define HE_InvalidInventoryEquipID 7
#define HE_Undefined 8
#define HE_NoPlayerChar 9
#define AP_InitFailed 10
#define AP_MissingFile 11
#define AP_MissingValue 12
#define FE_MissingDLC 13
#define FE_ApplySettings 14
#define FE_PatternFailed 15

#define VERSION "3.0.10"


class CCore: public modengine::ModEngineExtension {
public:
	CCore(modengine::ModEngineExtensionConnector* connector);

	static VOID Start();
	static VOID InputCommand();

	// Self-initialize without ModEngine2: construct the global CCore (null connector) and drive
	// on_attach() ourselves. Called from DllMain so the client works under any DLL loader
	// (ModEngine3, Elden Mod Loader, ...), not just ME2's modengine_ext_init. Idempotent.
	static VOID StandaloneInit();
	virtual VOID Run();
	virtual VOID Panic(const char* pMessage, const char* pSort, DWORD dError, DWORD dIsFatalError);
	virtual BOOL CheckOldApFile();

	// Initializes [savePath]. May only be called after a connection has been established. Throws
	// an exception if initialization is unsuccessful.
	//
	// TODO: We now load and save last_received_index data from the native DS3 save rather than a
	// separate JSON file. Remove this once it's no longer necessary to migrate older users onto
	// the new system (probably as part of 3.1.0).
	void InitSavePath();

	/// Sets [pSeed].
	/// 
	/// We expect this to be called twice when loading into a game: once for the seed we added to
	/// the game's save data and once for the seed from the user's connection to the Archipelago
	/// server. If all goes well, these will agree. If they don't, that probably indicates that the
	/// player loaded the wrong save by acceident, so we show them an "are you sure?" message.
	/// 
	/// The [fromSave] parameter indicates whether this seed comes from the save file, as opposed
	/// to from the Archipelago server we've connected to. It's just used for error reporting.
	void SetSeed(std::string seed, bool fromSave);

	std::string pSlotName;
	std::string pPassword;

	// The seed used to generate this multiworld. We use this to double-check that the save file
	// which was loaded is in fact connected to the correct Archipelago room. Note that this isn't
	// strictly unique�multiple distinct rooms can use the same generation, and it's possible that
	// someone may manually supply the same seed to multiple generations.
	//
	// This is set to none before the seed has been loaded from the save file, or if the save file
	// didn't contain a seed.
	std::optional<std::string> pSeed;

	BOOL writeSaveFileNextTick = false;
	BOOL sendGoalStatus = true;
	int pLastReceivedIndex = 0;

	// Whether Archipelago is currently connected or not.
	BOOL connected = false;

	// Dungeon sweep (opt-in via the apworld's dungeon_sweep option; slot_data
	// "dungeonSweeps"): trigger (mainboss drop) AP location id -> all AP location ids in
	// that dungeon. When the trigger's flag fires, the remaining members are auto-sent
	// (see PollLocationFlags). Public: populated by ArchipelagoInterface at connect.
	std::unordered_map<int64_t, std::vector<int64_t>> dungeonSweeps;

	// Boss attribution (apconfig.json "sweep_flags", SPEC-boss-attribution.md): event flag
	// (boss DefeatFlag OR Site-of-Grace lit flag) -> AP location ids attributed to it. A
	// location may appear under several flags; when ANY fires, its members are sent (first
	// trigger wins, flagSentLocations dedupes). Unlike dungeonSweeps this is keyed by the
	// event flag directly, so no locationFlags indirection. Populated in LoadConfigFile.
	std::unordered_map<uint32_t, std::vector<int64_t>> sweepFlags;

	// Goal locations for ending_condition 2/3 (slot_data "goalLocations"): the goal
	// completes when ALL of these are checked (server-side truth). Empty for ec 0/1,
	// which use boss defeat flags instead (CGameHook::isSoulOfCinderDefeated).
	// Public: populated by ArchipelagoInterface at connect, read by isGoalComplete.
	std::vector<int64_t> goalLocations;

	// Region-fusion grace bundle (slot_data "regionGraces"; SPEC-region-chain.md / TODO #13):
	// lock-item NAME -> grace warp-unlock event flags to set when that lock item is received,
	// lighting up that region's Sites of Grace (fast travel) without a Torrent slog. Present
	// only when region gating is active (apworld world_logic < 3); empty otherwise. Keyed by
	// NAME because every region-lock key shares sentinel er_code 99999 and is indistinguishable
	// by the time it reaches the grant path (GameHook.cpp GiveNextItem) — identity only exists
	// in the received-items handler. Populated by ArchipelagoInterface at connect.
	std::unordered_map<std::string, std::vector<uint32_t>> regionGraces;
	// Grace rando (slot_data "graceItems"; SPEC-grace-rando.md): AP item NAME ->
	// ONE warp-unlock event flag set when that grace item is received. Reuses the
	// regionGraces pattern (keyed by NAME, drained via pendingGraceFlags +
	// FlushPendingGraceFlags). Present only when grace rando is active; empty otherwise.
	std::unordered_map<std::string, uint32_t> graceItems;

	// Grace warp flags queued by the received-items handler (the only place a lock item's
	// identity is known) and drained on an in-game tick by FlushPendingGraceFlags (the only
	// place the event-flag setter is valid). Cross-thread like receivedItemsQueue — no lock,
	// matching the existing pattern.
	std::vector<uint32_t> pendingGraceFlags;

	// Region-open flags (slot_data "regionOpenFlags"; SPEC-region-fog-gates.md): lock-item NAME
	// -> ONE event flag set when that lock item is received, used by BAKED border fog gates as
	// their "region is open" condition. Distinct from regionGraces so the gate condition is
	// unconditional (grace bundle varies with graces_per_region). Present only under region
	// gating. Drained via the same pendingGraceFlags queue + FlushPendingGraceFlags.
	std::unordered_map<std::string, uint32_t> regionOpenFlags;

	// Natural-key triggers (slot_data "naturalKeyTriggers"; SPEC-natural-locks.md): bloom a
	// region's apparatus when a DISJUNCTION of vanilla triggers is satisfied, instead of when
	// a synthetic lock ITEM arrives. LockName -> list of clauses; a clause is satisfied when
	// ALL its items were received (by NAME, via receivedItemNames) AND ALL its flags are set
	// (GetEventFlagState). ANY satisfied clause fires the bloom (push regionGraces/
	// regionOpenFlags/lockRevealFlags + lockNotifyItems for that LockName). The open flag is
	// the save-persisted once-latch. Evaluated in CCore::EvaluateNaturalKeyTriggers on a
	// settled in-world tick. Absent/empty on seeds that don't use natural keys.
	struct NKClause { std::vector<std::string> items; std::vector<uint32_t> flags; };
	std::unordered_map<std::string, std::vector<NKClause>> naturalKeyTriggers;
	// Item NAMES received this session (maintained in the received-items handler). Rebuilt
	// from the items_received replay on reconnect (items_handling 0b111 re-delivers all),
	// so no separate persistence is needed; the region open flag is the durable latch.
	std::unordered_set<std::string> receivedItemNames;
	// Generalized region-lock detection (slot_data "areaLockFlags"): {areaNameId_lo, hi, open_flag}
	std::vector<std::vector<int32_t>> areaLockFlags;
	// DLC-only auto-entry (slot_data "dlcEntryWarpFlag" / "dlcStartAreaId"): when the poll sees the
	// player in dlcStartAreaId (Chapel of Anticipation) it sets dlcEntryWarpFlag (76999) ONCE -> baked
	// common.emevd WarpPlayer -> Gravesite Plain. 0/absent on non-dlc_only seeds. See patch_baker_dlcentry.py.
	int32_t dlcEntryWarpFlag = 0;
	int32_t dlcStartAreaId = 0;
	// Random starting region (slot_data randomStartWarpFlag/AreaId/DoneFlag): when the poll sees
	// the player in randomStartAreaId (Chapel) it sets randomStartWarpFlag (76969) ONCE -> baked
	// common.emevd WarpPlayer -> the rolled start region. randomStartDoneFlag (76968) persists so it
	// fires once per save. 0/absent on non-random-start seeds. See patch_baker_random_start_warp_v2_datadriven.py.
	int32_t randomStartWarpFlag = 0;
	int32_t randomStartAreaId = 0;
	int32_t randomStartDoneFlag = 0;
	// Lock item NAME -> map-reveal/open flags; client sets these on receipt (region opens + map reveals).
	std::unordered_map<std::string, std::vector<int32_t>> lockRevealFlags;
	// Region-lock unlock NOTIFICATION: lock item NAME -> packed GOODS address granted on receipt so the
	// native item ticker fires + names the region (locks are otherwise invisible, sentinel 99999).
	std::unordered_map<std::string, int32_t> lockNotifyItems;
	std::vector<int32_t> pendingNotifyGrants;  // drained in the in-world grant block
	// Once-per-save dedup for the notify grants above: addresses already granted this save,
	// persisted ("notify_granted") so reconnects don't re-add the region map fragment + re-fire
	// the ticker. Same eager-grant shape as the start items; lock EFFECTS stay on SetEventFlag.
	std::unordered_set<int32_t> notifyGrantedAddrs;
	// Start items (Torrent + starting flasks): queued at connect ONLY when not yet granted
	// for this save, drained once, then persisted (start_items_granted) so reconnects don't
	// re-grant. Was the endless-loop bug: startItems re-pushed to pendingNotifyGrants every
	// (re)connect, a path with no last_received_index dedup.
	// startItems entries are (FullID, count): count>1 grants a whole stack in one
	// GrantFullID call (one acquisition popup). Plain-int slot_data entries parse as count 1.
	std::vector<std::pair<int32_t, int32_t>> pendingStartItems;
	bool startItemsGranted = false;
	// Progressive stone bells (SPEC-progressive-stone-bells.md): item name -> ordered list per
	// received copy of {cosmetic goods full id, Twin Maiden eventFlag_forStock to set}. Kth copy = tier K.
	// Each tier = ({goods ids...}, {shop/unlock flags...}). goods is a LIST: progressive_physick
	// grants a whole tear family per step via a "goodsList" array; single-goods bells/consumables
	// ("goods" scalar) become a 1-element list. Parsed in ArchipelagoInterface.cpp.
	std::unordered_map<std::string, std::vector<std::pair<std::vector<uint32_t>, std::vector<uint32_t>>>> progressiveGrants;
	// Per-item receipt counter; reset on the reconnect queue-rebuild so tiers recompute from the
	// full ordered replay (idempotent flag-sets make re-application safe).
	std::unordered_map<std::string, int> progressiveCounter;
	// Cross-session tiers: progressiveCounter is now PERSISTED (was rebuilt from the connect
	// replay, which was fragile -- a late datapackage / reconnect made a 2nd-session copy recount
	// from tier 1). progressiveHighIndex = highest AP item.index already applied; replayed copies
	// (index <= it) are skipped so the persisted counter is never double-incremented.
	long long progressiveHighIndex = -1;

	// Map reveal (slot_data "reveal_all_maps"; beta.3): set by the slot-connected handler when the
	// apworld wants all region maps revealed without granting map items (map_option=give). The
	// loaded block in Run() calls GameHook->revealAllMaps until it succeeds, then clears this.
	BOOL revealAllMapsPending = false;

	static const int RUN_SLEEP = 2000;

private:
	// Whether ModEngine is running in debug mode (in which case we're sharing the console with
	// its debug output).
	BOOL modEngineDebug = false;

	// The path where the config file for the current run lives.
	std::filesystem::path configPath;

	// The path where the save data for the current run lives.
	//
	// TODO: We now load and save last_received_index data from the native DS3 save rather than a
	// separate JSON file. Remove this once it's no longer necessary to migrate older users onto
	// the new system (probably as part of 3.1.0).
	std::filesystem::path savePath;

	// The raw config data currently stored in [configPath].
	nlohmann::json configData;

	// Initializes [configPath]. Throws an exception if initialization is unsuccessful.
	void InitConfigPath();

	const char* id() override {
		return "archipelago";
	}

	void on_attach() override;
	void on_detach() override;

	// Walks the user through connecting to the Archipelago server.
	void Connect();

	// Removes items from [ItemRandomizer.ReceivedItemsQueue] that were already received according
	// to [pLastReceivedIndex].
	void SkipAlreadyReceivedItems();

	// Loads the file at [configPath] into the client's data structures. Throws an exception if the
	// config file fails to load.
	void LoadConfigFile();

	// Writes the client's updated configuration to the file at [configPath].
	void WriteConfigFile();

	// Loads the file at [savePath] into the client's data structures. Throws an exception if the
	// save file fails to load.
	//
	// TODO: We now load and save last_received_index data from the native DS3 save rather than a
	// separate JSON file. Remove this once it's no longer necessary to migrate older users onto
	// the new system (probably as part of 3.1.0).
	void LoadSaveFile();

	// ER port: writes last_received_index back to [savePath] after each grant. Upstream DS3
	// persisted this in the native game save; that hook is stubbed in the ER shim, so the JSON
	// file is the system of record here (LoadSaveFile reads + deletes it at startup).
	void WriteSaveFile();

	// ER port: AP location id -> guarding in-game event flag (from apconfig.json
	// "location_flags", emitted by the randomizer bake), polled each tick to detect checks whose
	// acquisition bypasses the AddItemFunc detour (shops, gifts, offline pickups).
	std::unordered_map<int64_t, uint32_t> locationFlags;
	std::unordered_set<int64_t> flagSentLocations;
	VOID PollLocationFlags();

	// Region-fusion (SPEC-region-chain.md): drain pendingGraceFlags, calling SetEventFlag per
	// flag so each received region's Sites of Grace become fast-travelable. SetEventFlag is
	// idempotent and the flag persists in the game save, so re-running on reconnect/replay is
	// harmless; graceFlagsSetThisSession only suppresses redundant calls / log spam. Flags that
	// can't be set yet (event-flag holder not initialized) stay queued for a later tick.
	std::unordered_set<uint32_t> graceFlagsSetThisSession;
	VOID FlushPendingGraceFlags();

	// Natural-key triggers (SPEC-natural-locks.md): on a settled in-world tick, evaluate each
	// naturalKeyTriggers entry; when ANY clause is satisfied (all items received AND all flags
	// set) and the region's open flag is not yet set, bloom that region (push graces/open/reveal
	// flags + notify item). The open flag is the once-latch. Flag reads only happen here, never
	// mid-init. Called right after FlushPendingGraceFlags in the settled poll block.
	VOID EvaluateNaturalKeyTriggers();

	// Removes (without deleting in case the user still wants it) the old save file. This should
	// only be called once the file is detected to be broken somehow.
	//
	// TODO: We now load and save last_received_index data from the native DS3 save rather than a
	// separate JSON file. Remove this once it's no longer necessary to migrate older users onto
	// the new system (probably as part of 3.1.0).
	void RemoveOldSaveFile();

	// Atomically writes the given contents to the given file. Guarantees as much as possible that 
	// the file will either not be updated or will have all of the given contents.
	//
	// Throws an exception if writing fails.
	void WriteAtomic(const std::filesystem::path& path, const std::string& contents);

	// Converts the given path to safely avoid MAX_PATH limits on Windows.
	inline std::filesystem::path WindowsLongPath(const std::filesystem::path& path)
	{
		// The \\?\ prefix that disables MAX_PATH limits also disables automatic resolution of
		// . and .. path components, so we have to normalize the path before conversion.
		return std::filesystem::path(
			L"\\\\?\\" + std::filesystem::absolute(path).lexically_normal().wstring()
		);
	}

	// Prompts the user for a yes or no response in the command line window. Returns true if they
	// respond "y", false if they respond "n". Returns [defaultResponse] if they press any other
	// key.
	bool PromptYN(std::string message, bool defaultResponse = true);

	// Prompts the user for a string response in the command line window.
	std::string PromptString(std::string prompt);

	// Prompts the user for a string response in the command line window, using the given default
	// value if they don't type anything.
	std::string PromptString(std::string prompt, std::string defaultValue);

	// Prompts the user for a string response in the command line window, using the given default
	// value if they don't type anything.
	//
	// Hides the default value rather than displaying it in plain text.
	std::string PromptStringHideDefault(std::string prompt, std::string defaultValue);

	// Returns a text description of the last Win32 error, or none if that fails.
	std::optional<std::string> GetLastWin32ErrorText();
};
