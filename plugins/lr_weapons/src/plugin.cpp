// lr_weapons — Plugify C++ port of the Pisex LR module "[LR] ExStats Weapons"
// (the original's lr_weapons.cpp + lr_weapons.h; the Metamod module ships as
// addons/lr_modules/lr_weapons with lr_weapons.vdf).
//
// What it does (identical to the original): counts kills per weapon classname into the
// `<lr_table>_weapons` table (steam, classname, kills), applies the per-weapon experience
// coefficient to the kill's experience, saves a row per kill when
// lr_db_savedata_player_mode is set and the whole slot on disconnect otherwise, zeroes the
// counters on !reset/cleanup.
//
// This plugin calls s2sdk directly - no pisexcompat, no guard library (docs/PLUGIN_STANDARD.md §7).
// The safety rules of handoff/weaponskins/PITFALLS.md therefore live right here:
//   * B1/B2 map gate: OnMapStart/OnMapEnd listeners are registered in OnPluginStart and the gate
//     g_mapReady is opened by OnMapStart (or by the first OnPluginUpdate when the plugin was loaded
//     into a running map) and dropped by OnMapEnd. Every s2sdk client query goes through
//     HasPawnAndSteamID64()/SteamIdOf(), so none can reach a torn-down client list. OnMapStart and
//     OnMapEnd only touch the gate (PITFALLS B2: no client query during a changelevel).
//   * C2 client checks: HasPawnAndSteamID64() requires the map gate, 0 <= slot < 64,
//     IsClientConnected, IsClientInGame, !IsClientSourceTV, !IsFakeClient, a non-null
//     PlayerSlotToEntPointer and GetClientPawn, and GetClientSteamID64 > 0 - the original's
//     "controller + pawn + readable steam id" test.
//   * The kill hook is the LR core's HookOnPlayerKilledPreEvent (the raw s2sdk event handle as
//     void*). The "attacker" field of player_death is a player_controller_and_pawn one
//     (core.gameevents), so its slot is read with s2sdk::GetEventPlayerSlot - the engine's
//     IGameEvent::GetInt is a different call that answers the raw event value (the legacy user id),
//     not a slot. "weapon" is a plain string field and stays s2sdk::GetEventString. Both run inside
//     try/catch (the Plugify boundary must not unwind into levels_ranks); the attacker slot is
//     bounds-checked to 0..63 and -1 (INVALID_PLAYER_SLOT_INDEX: the world, no player) is dropped
//     (C2).
//   * A7 unload: OnPluginEnd unhooks the LR callbacks, removes the disconnect and map listeners and
//     closes the gate, so `plg reload` leaves nothing behind that points into the unloaded library.
//
// Port notes (the rest is parity):
//   * The original hooked IServerGameClients::ClientDisconnect through SourceHook and read
//     pPlayerController->m_hPawn()/m_steamID(); the port registers s2sdk's OnClientDisconnect
//     (the same point the levels_ranks core uses; s2sdk 3.0 fires it before the client data is
//     reset, like the original hook) and takes the steam id cached at OnPlayerLoaded, with the
//     engine value (s2sdk::GetClientAuthId, the original's GetPlayerNetworkIDString) only as the
//     fallback. The original's "player must still have a pawn" test stays dropped - the counters
//     are what has to reach the database (the LR core documents the same deviation).
//   * exstats_weapons.ini is read at plugin start, like Load(); the file is parsed by the plugin's
//     own KV1 reader (src/weapons_core.cpp) from <GetConfigsDir()>/levels_ranks/exstats_weapons.ini,
//     with <GetConfigsDir()>/lr_weapons/exstats_weapons.ini as the fallback for a clean Plugify
//     install. The crash guards the original copy carries (slot range, empty network id) are kept.
//   * The database is its own plgdb connection with the core's settings
//     (GetDatabaseConfig(), "lr.sqlite3" for the SQLite fallback), because a Plugify plugin
//     cannot share another plugin's connection object; the table name/columns are the original's.
//   * The per-weapon SELECT of OnPlayerLoadedHook is one query for the whole row set (the original
//     issued one per weapon); the applied state is identical.
//   * No menus of its own (the original module had none; the "name"/"showtop" fields are parsed
//     and kept for config parity only).
//   * Errors go through plg::Log (with the null check every pure-s2sdk plugin uses), never
//     pisex::ErrorLog.

#include <plg/plugin.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <lr_api.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "weapons_core.hpp"

namespace {

// GetLogTag() of the original.
constexpr char kLogTag[] = "[LR-Weapons]";

// A failed row load is retried after this delay; a failed CREATE TABLE (the database may simply
// not be up yet at plugin start) after the longer one. Save errors are logged at most once per
// interval.
constexpr int64_t kLoadRetrySeconds = 5;
constexpr int64_t kDdlRetrySeconds = 30;
constexpr int64_t kErrorLogSeconds = 10;

int64_t NowSeconds() {
	return static_cast<int64_t>(std::time(nullptr));
}

// The original's g_pLRCore.
levels_ranks::LrApi* g_pLRCore = &levels_ranks::LrApi::Get();

using namespace lr_weapons;

void OnClientDisconnectListener(int32_t slot, s2sdk::NetworkDisconnectionReason reason);

// plg::Log is filled by the plugify module right before OnPluginStart; the null check keeps the
// plugin silent (not crashing) in an unusual host that never provides it.
void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogWarn(std::string_view message) {
	LogMessage(message, plg::Severity::Warning);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

std::string ToString(const plg::string& text) {
	return std::string(std::string_view(text));
}

// --- map gate (PITFALLS B1/B2) ---------------------------------------------------------------

bool g_mapReady = false;               // client queries are safe right now
bool g_mapStartSeen = false;           // an OnMapStart arrived since OnPluginStart / the last OnMapEnd
bool g_mapEndSeen = false;             // an OnMapEnd arrived since OnPluginStart
bool g_mapListenersRegistered = false; // OnMapStart/OnMapEnd are registered with s2sdk

// s2sdk listener callbacks are plain function pointers called on the game thread; nothing here may
// throw, and no client query is made - only state changes (PITFALLS B2).

void OnMapStartListener() {
	// The new map and its client list exist: readiness is granted here (and by the next
	// OnPluginUpdate for a plugin loaded into a running map).
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
}

void OnMapEndListener() {
	// From here until the next OnMapStart s2sdk's client list is torn down and any client query
	// crashes - so this handler only drops state.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

void RegisterMapListeners() {
	if (g_mapListenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		// Without the map listeners nothing could ever close the gate again, so it stays closed
		// (fail safe, never Failed - PITFALLS A1/B2).
		LogError(std::string(kLogTag) + " the s2sdk map listeners are unavailable - weapon statistics stay off");
		return;
	}
	s2sdk::OnMapStart_Register(&OnMapStartListener);
	s2sdk::OnMapEnd_Register(&OnMapEndListener);
	g_mapListenersRegistered = true;
}

void UnregisterMapListeners() {
	if (!g_mapListenersRegistered) {
		return;
	}
	g_mapListenersRegistered = false;
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		s2sdk::OnMapStart_Unregister(&OnMapStartListener);
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
	}
}

void PollMapGate() {
	if (!g_mapListenersRegistered) {
		return;
	}
	// A plugin loaded into a running map never saw an OnMapStart: the first update after
	// OnPluginStart is the proof of a live map (the host pumps updates from the server frame).
	// After an OnMapEnd only a new OnMapStart re-opens the gate: the host keeps pumping updates
	// through a changelevel, so `!g_mapEndSeen` must not be enough once one was seen.
	if (!g_mapReady && (g_mapStartSeen || !g_mapEndSeen)) {
		g_mapReady = true;
	}
}

// --- client checks (PITFALLS C2) -------------------------------------------------------------

// The original's "attacker must have a controller, a pawn and a readable steam id" test, with the
// s2sdk contract (a query on a slot without a client, a controller or a pawn is a null dereference
// inside the engine) and the map gate. Only a slot that passes this may be handed to any other
// s2sdk client call.
bool HasPawnAndSteamID64(int32_t slot) {
	if (!g_mapReady || slot < 0 || slot >= kMaxSlots) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_IsFakeClient == nullptr ||
		__s2sdk_PlayerSlotToEntPointer == nullptr || __s2sdk_GetClientPawn == nullptr ||
		__s2sdk_GetClientSteamID64 == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot) && s2sdk::IsClientInGame(slot) &&
			   !s2sdk::IsClientSourceTV(slot) && !s2sdk::IsFakeClient(slot) &&
			   s2sdk::PlayerSlotToEntPointer(slot) != nullptr &&
			   s2sdk::GetClientPawn(slot) != nullptr && s2sdk::GetClientSteamID64(slot) > 0;
	} catch (...) {
		return false;
	}
}

class LrWeaponsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// lr_weapons::OnClientDisconnect of the original (s2sdk OnClientDisconnect context: still
	// called before the client data is reset, in the same slot-preserving window as the original
	// hook).
	void OnClientDisconnect(int32_t slot);

private:
	void LoadSettings();
	void OnCoreIsReady();
	void OnPlayerLoaded(int slot, const char* steamid);
	void OnResetPlayerStats(int slot, const char* steamid);
	void OnPlayerKilledPre(void* event, int& exp_caused);
	void OnDatabaseCleanup(int type);

	// SaveDataPlayer() of the original; weapon = -1 saves every non-zero counter.
	void SaveDataPlayer(int slot, int weapon = -1);
	// One weapon's row: the INSERT/UPDATE and, on failure, the is_new flag back (an INSERT that
	// did not run must be retried as an INSERT, a duplicate-key INSERT would be silent).
	void SaveWeapon(int slot, const std::string& id, int index);
	// engine->GetPlayerNetworkIDString(slot) + ConvertSteamID, with the cached id preferred.
	std::string SteamIdOf(int slot) const;
	bool PlayerUsable(int slot) const;
	// The load of |id| into |slot|; a query error leaves the slot unloaded and schedules a retry.
	void StartLoad(int slot, const std::string& id);
	void UpdateRetries();
	void IssueCreateTable();
	void LogLoadError(const std::string& error);
	void LogSaveError(const std::string& error);

	Config m_cfg;
	State m_state{};
	std::array<std::string, kMaxSlots> m_steamids{};
	// The slot's rows are in memory: only then are kills counted and saved. A failed SELECT used
	// to look like "no rows" and the next save replaced the real kills with the ones counted
	// since the error - or inserted a duplicate key that silently never reached the table.
	std::array<bool, kMaxSlots> m_loaded{};
	std::array<bool, kMaxSlots> m_load_in_flight{};
	std::array<int64_t, kMaxSlots> m_load_retry_at{};
	std::array<uint64_t, kMaxSlots> m_load_seq{};
	std::unique_ptr<plgdb::Database> m_db;
	std::string m_table; // <lr_table>_weapons
	bool m_disconnect_registered = false;
	// The table must exist before any load/save is issued; a failed CREATE TABLE is retried so a
	// database that comes up after the core does not leave the module without a table.
	bool m_table_ready = false;
	int64_t m_ddl_requested_at = 0;
	int64_t m_last_load_error_log = 0;
	int64_t m_last_save_error_log = 0;
};

LrWeaponsPlugin g_plugin;

void OnClientDisconnectListener(int32_t slot, s2sdk::NetworkDisconnectionReason /*reason*/) {
	try {
		g_plugin.OnClientDisconnect(slot);
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnClientDisconnect failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnClientDisconnect failed with an unknown exception");
	}
}

// --- configuration -------------------------------------------------------------------------------

void LrWeaponsPlugin::LoadSettings() {
	// LoadSettings() of the original: KeyValues("LR_ExStatsWeapons")
	// -> <GetConfigsDir()>/levels_ranks/exstats_weapons.ini (the original resolved
	// addons/configs/levels_ranks/exstats_weapons.ini), with <GetConfigsDir()>/lr_weapons/
	// exstats_weapons.ini as the fallback.
	const std::string configsDir = plg::GetConfigsDir ? ToString(plg::GetConfigsDir()) : std::string();
	const ConfigSource source = lr_weapons::LoadConfig(configsDir, m_cfg);
	if (!source.loaded) {
		// The load-failure warning of the original. A failed load leaves the defaults behind
		// (no weapons: nothing is counted and no coefficient is applied), but the plugin keeps
		// running - never Failed (PITFALLS A1).
		LogError(std::string(kLogTag) + " " + source.error);
		return;
	}
	if (!source.problem.empty()) {
		LogError(std::string(kLogTag) + " exstats_weapons.ini: " + source.problem);
	}
	LogInfo(std::string(kLogTag) + " loaded " + source.path + " (" +
			std::to_string(m_cfg.weapons.size()) + " weapons, coefficient " +
			(m_cfg.coefficient_active ? "on" : "off") + ")");
}

// --- database ------------------------------------------------------------------------------------

void LrWeaponsPlugin::OnCoreIsReady() {
	// OnCoreIsReadyHook() of the original: the connection first, then the table and the hooks.
	m_table = TableName(g_pLRCore->GetTableName());
	m_db = plgdb::Database::Create(
		levels_ranks::MakeDatabaseConfig(g_pLRCore->GetDatabaseConfig(), "lr.sqlite3"));
	if (m_db == nullptr) {
		LogError(std::string(kLogTag) + " the database configuration is invalid - weapon statistics are off");
		return;
	}
	// The DDL is queued (FIFO before every later statement), like the original's asynchronous
	// CREATE TABLE query; its result is checked and a failure is retried (the database may not be
	// up yet).
	m_table_ready = false;
	IssueCreateTable();

	g_pLRCore->HookOnPlayerLoaded([this](int slot, const char* steamid) { OnPlayerLoaded(slot, steamid); });
	g_pLRCore->HookOnResetPlayerStats(
		[this](int slot, const char* steamid) { OnResetPlayerStats(slot, steamid); });
	g_pLRCore->HookOnPlayerKilledPreEvent(
		[this](void* event, int& exp_caused, int, int) { OnPlayerKilledPre(event, exp_caused); });
	g_pLRCore->HookOnDatabaseCleanup([this](int type) { OnDatabaseCleanup(type); });
}

void LrWeaponsPlugin::IssueCreateTable() {
	if (m_db == nullptr) {
		return;
	}
	m_ddl_requested_at = NowSeconds();
	plgdb::Statement ddl{CreateTableSql(m_table, m_db->GetDriver()), {}};
	m_db->Query(std::move(ddl.sql), std::move(ddl.params), [this](const plgdb::Result& result) {
		if (result.Ok()) {
			m_table_ready = true;
			return;
		}
		LogError(std::string(kLogTag) + " creating the " + m_table + " table failed: " + result.Error());
	});
}

void LrWeaponsPlugin::LogLoadError(const std::string& error) {
	const int64_t now = NowSeconds();
	if (now - m_last_load_error_log < kErrorLogSeconds) {
		return;
	}
	m_last_load_error_log = now;
	LogError(std::string(kLogTag) + " loading the weapon statistics failed: " + error);
}

void LrWeaponsPlugin::LogSaveError(const std::string& error) {
	const int64_t now = NowSeconds();
	if (now - m_last_save_error_log < kErrorLogSeconds) {
		return;
	}
	m_last_save_error_log = now;
	LogError(std::string(kLogTag) + " saving the weapon statistics failed: " + error);
}

void LrWeaponsPlugin::StartLoad(int slot, const std::string& id) {
	const std::size_t index = static_cast<std::size_t>(slot);
	const uint64_t seq = ++m_load_seq[index];
	m_load_in_flight[index] = true;
	m_loaded[index] = false;
	m_load_retry_at[index] = 0;
	// The per-weapon SELECT of the original, in one query; the result pass fills the counters
	// (a weapon without a row gets 0 and is_new = true, exactly like the empty result there).
	Statement statement = LoadPlayerStatement(m_table, id);
	m_db->Query(std::move(statement.sql), std::move(statement.params),
				[this, slot, id, seq](const plgdb::Result& result) {
					const std::size_t index = static_cast<std::size_t>(slot);
					if (m_load_seq[index] != seq || m_steamids[index] != id) {
						// The slot was taken by another player while the query was in flight (a fast
						// reconnect): this row is not theirs (the core drops stale loads the same way).
						return;
					}
					m_load_in_flight[index] = false;
					if (!result.Ok()) {
						// An error is not "no rows": keep the slot unloaded (no kill is counted
						// or saved) and retry; only a successful empty result assigns is_new = true.
						LogLoadError(result.Error());
						m_load_retry_at[index] = NowSeconds() + kLoadRetrySeconds;
						return;
					}
					ApplyLoadedWeapons(m_state[index], m_cfg, result);
					m_loaded[index] = true;
				});
}

void LrWeaponsPlugin::UpdateRetries() {
	if (m_db == nullptr) {
		return;
	}
	const int64_t now = NowSeconds();
	if (!m_table_ready) {
		if (now - m_ddl_requested_at >= kDdlRetrySeconds) {
			IssueCreateTable();
		}
		return; // no load can succeed before the table exists
	}
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		const std::size_t index = static_cast<std::size_t>(slot);
		if (m_loaded[index] || m_load_in_flight[index] || m_steamids[index].empty()) {
			continue;
		}
		if (now < m_load_retry_at[index]) {
			continue;
		}
		StartLoad(slot, m_steamids[index]);
	}
}

void LrWeaponsPlugin::OnPlayerLoaded(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	const std::string id = steamid != nullptr ? steamid : "";
	m_steamids[index] = id;
	++m_load_seq[index]; // a load of the previous occupant (or an earlier attempt) must not land here
	m_load_in_flight[index] = false;
	m_loaded[index] = false;
	m_load_retry_at[index] = 0;
	if (id.empty()) {
		return;
	}
	if (m_table_ready) {
		StartLoad(slot, id);
	}
	// Otherwise UpdateRetries() starts the load once the CREATE TABLE succeeded.
}

void LrWeaponsPlugin::OnResetPlayerStats(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	const std::string id = steamid != nullptr ? steamid : "";
	m_steamids[index] = id;
	// An answer of a load that is still in flight would put the pre-reset rows back into the slot.
	++m_load_seq[index];
	m_load_in_flight[index] = false;
	// The local counters go now; the statement zeroes the rows of the player (all weapons).
	ZeroSlot(m_state[index], m_cfg);
	if (!m_table_ready) {
		// The rows cannot be zeroed in the database yet. The slot keeps its loaded flag: once the
		// table exists the next save writes the zero-based counters (the load path must not bring
		// the pre-reset kills back from the database).
		return;
	}
	Statement statement = ResetPlayerStatement(m_table, id);
	m_db->Query(std::move(statement.sql), std::move(statement.params), [this](const plgdb::Result& result) {
		if (!result.Ok()) {
			LogSaveError(result.Error());
		}
	});
}

void LrWeaponsPlugin::OnClientDisconnect(int32_t slot) {
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	if (!g_pLRCore->GetSettingsValue(levels_ranks::LR_DB_SaveDataPlayer_Mode)) {
		SaveDataPlayer(static_cast<int>(slot));
	}
	// SaveDataPlayer above ran while the client data was still valid; drop the cached id now -
	// s2sdk resets the client once the disconnect callbacks return, and this is still the same slot.
	const std::size_t index = static_cast<std::size_t>(slot);
	++m_load_seq[index]; // an in-flight load of this occupant must not land in the reused slot
	m_load_in_flight[index] = false;
	m_loaded[index] = false;
	m_steamids[index].clear();
	ZeroSlot(m_state[index], m_cfg);
}

void LrWeaponsPlugin::OnDatabaseCleanup(int type) {
	// OnDatabaseCleanupHook() of the original: LR_AllData / LR_StatsData.
	if (type != levels_ranks::LR_AllData && type != levels_ranks::LR_StatsData) {
		return;
	}
	if (m_db != nullptr && m_table_ready) {
		m_db->Query(TruncateSql(m_table, m_db->GetDriver()), {});
	}
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		if (!PlayerUsable(slot)) {
			continue;
		}
		// The local counters are zeroed even while the database is unavailable, so a load that
		// succeeds later cannot bring the cleaned kills back from memory.
		ZeroSlot(m_state[static_cast<std::size_t>(slot)], m_cfg);
		const std::string id = SteamIdOf(slot);
		if (!id.empty() && m_db != nullptr && m_table_ready) {
			Statement statement = ResetPlayerStatement(m_table, id);
			m_db->Query(std::move(statement.sql), std::move(statement.params));
		}
	}
}

// --- game -----------------------------------------------------------------------------------------

std::string LrWeaponsPlugin::SteamIdOf(int slot) const {
	if (slot < 0 || slot >= kMaxSlots) {
		return {};
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	// engine->GetPlayerNetworkIDString(slot) of the original. The id cached at OnPlayerLoaded is the
	// same value the LR core converted, it is available even after OnMapEnd - where s2sdk's client
	// list is torn down and a client query crashes (PITFALLS B2) - so the cache wins and the engine
	// read is only the fallback.
	if (!m_steamids[index].empty()) {
		return m_steamids[index];
	}
	if (!g_mapReady || __s2sdk_GetClientAuthId == nullptr) {
		return {};
	}
	try {
		return ConvertSteamId(ToString(s2sdk::GetClientAuthId(slot)));
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " reading the steam id of slot " + std::to_string(slot) +
				 " failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " reading the steam id of slot " + std::to_string(slot) + " failed");
	}
	return {};
}

bool LrWeaponsPlugin::PlayerUsable(int slot) const {
	// The original required an attacker with a pawn and a readable steam id.
	return HasPawnAndSteamID64(slot);
}

void LrWeaponsPlugin::SaveDataPlayer(int slot, int weapon) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr || !m_table_ready) {
		return;
	}
	const std::size_t slot_index = static_cast<std::size_t>(slot);
	if (!m_loaded[slot_index]) {
		return; // the rows are not in memory (or a load failed): a save would insert/overwrite them
	}
	const std::string id = SteamIdOf(slot);
	if (id.empty()) {
		return; // the guard for the original's ConvertSteamID(nullptr) crash
	}
	SlotState& state = m_state[slot_index];
	if (weapon != -1) {
		SaveWeapon(slot, id, weapon);
		return;
	}
	for (int i = 0; i < static_cast<int>(m_cfg.weapons.size()); ++i) {
		if (state.kills[static_cast<std::size_t>(i)] != 0) {
			SaveWeapon(slot, id, i);
		}
	}
}

void LrWeaponsPlugin::SaveWeapon(int slot, const std::string& id, int index) {
	const std::size_t slot_index = static_cast<std::size_t>(slot);
	SlotState& state = m_state[slot_index];
	const bool was_new = state.is_new[static_cast<std::size_t>(index)];
	Statement statement = SaveWeaponStatement(m_table, m_cfg, state, id, index);
	if (statement.sql.empty()) {
		return;
	}
	m_db->Query(std::move(statement.sql), std::move(statement.params),
				[this, slot, id, was_new](const plgdb::Result& result) {
					if (result.Ok()) {
						return;
					}
					LogSaveError(result.Error());
					if (!was_new || slot < 0 || slot >= kMaxSlots ||
						m_steamids[static_cast<std::size_t>(slot)] != id) {
						return; // an UPDATE retries with the next save; other writers are not ours
					}
					// The INSERT did not run and its reason is unknown (a lost connection, a
					// missing table, or a row another writer created in between). Re-read the
					// player's rows instead of guessing INSERT or UPDATE; saves resume when the
					// load lands and the counters are consistent with the table again.
					m_loaded[static_cast<std::size_t>(slot)] = false;
					m_load_retry_at[static_cast<std::size_t>(slot)] = NowSeconds() + kLoadRetrySeconds;
				});
}

void LrWeaponsPlugin::OnPlayerKilledPre(void* event, int& exp_caused) {
	// OnPlayerKilledPreHook() of the original. The raw s2sdk event handle only lives inside this
	// callback (the LR core documents the same guarantee the original's IGameEvent* had).
	if (event == nullptr || !g_mapReady || !g_pLRCore->CheckCountPlayers()) {
		return;
	}
	// "attacker" of player_death is a player_controller_and_pawn field (core.gameevents): the slot
	// comes from IGameEvent::GetPlayerSlot, NOT from IGameEvent::GetInt. The two are different
	// engine calls and GetInt answers the raw event value (the legacy user id - a number that is
	// not a slot). A non-player attacker (the world, or no player at all) answers -1
	// (INVALID_PLAYER_SLOT_INDEX), which the range guard below drops. Without the binding the field
	// cannot be resolved at all, so the hook fails soft instead of falling back to GetInt
	// (PITFALLS A1: no crash, no wrong slot).
	if (__s2sdk_GetEventPlayerSlot == nullptr) {
		return;
	}
	int32_t attacker = -1;
	try {
		attacker = s2sdk::GetEventPlayerSlot(event, "attacker");
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPlayerKilledPre: reading the attacker slot failed: " + ex.what());
		return;
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPlayerKilledPre: reading the attacker slot failed with an unknown exception");
		return;
	}
	if (attacker < 0 || attacker >= kMaxSlots) {
		return; // the crash guard of the original module; -1 is the world / no player
	}
	if (!PlayerUsable(attacker)) {
		return;
	}
	std::string weapon_name;
	try {
		weapon_name = (__s2sdk_GetEventString != nullptr)
						  ? ToString(s2sdk::GetEventString(event, "weapon"))
						  : std::string();
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPlayerKilledPre: reading \"weapon\" failed: " + ex.what());
		return;
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPlayerKilledPre: reading \"weapon\" failed with an unknown exception");
		return;
	}
	if (weapon_name.empty()) {
		return;
	}
	const std::string classname = NormalizeClassname(weapon_name.c_str());
	const int index = m_cfg.IndexOf(classname);
	if (index < 0) {
		return;
	}
	if (m_cfg.coefficient_active) {
		// The experience coefficient is applied even while the rows are still loading: it is
		// part of the kill's experience, not of the statistics.
		exp_caused = RoundToNearest(static_cast<float>(exp_caused) *
									m_cfg.weapons[static_cast<std::size_t>(index)].coefficient);
	}
	if (!m_loaded[static_cast<std::size_t>(attacker)]) {
		// The row set is not in memory (or a load failed and is being retried): the successful
		// load replaces the counters, so counting now would only lose the kill.
		return;
	}
	SlotState& state = m_state[static_cast<std::size_t>(attacker)];
	state.kills[static_cast<std::size_t>(index)]++;
	if (g_pLRCore->GetSettingsValue(levels_ranks::LR_DB_SaveDataPlayer_Mode)) {
		SaveDataPlayer(attacker, index);
	}
}

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult LrWeaponsPlugin::OnPluginStart() {
	try {
		// The map gate and the config first: nothing below may assume a map (PITFALLS B1), and only
		// the filesystem is touched here.
		RegisterMapListeners();
		LoadSettings();

		// The ClientDisconnect hook of the original.
		if (__s2sdk_OnClientDisconnect_Register != nullptr) {
			s2sdk::OnClientDisconnect_Register(&OnClientDisconnectListener);
			m_disconnect_registered = true;
		} else {
			LogWarn(std::string(kLogTag) + " the s2sdk disconnect listener is unavailable - the counters "
										   "are saved per kill only (lr_db_savedata_player_mode)");
		}

		// AllPluginsLoaded() of the original.
		if (g_pLRCore->CoreIsLoaded()) {
			OnCoreIsReady();
		} else {
			g_pLRCore->HookOnCoreIsReady([this] { OnCoreIsReady(); });
		}
		LogInfo(std::string(kLogTag) + " ready");
		return plg::PluginResult{};
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginStart failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginStart failed with an unknown exception");
	}
	return plg::PluginResult{}; // never Failed: that stops every plugify plugin
}

plg::PluginResult LrWeaponsPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		// PITFALLS B1/B2: the map-ready latch (the host pumps this from the server frame), then the
		// database results and the retries.
		PollMapGate();
		if (m_db != nullptr) {
			m_db->Poll();
		}
		UpdateRetries();
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginUpdate failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginUpdate failed with an unknown exception");
	}
	return plg::PluginResult{};
}

plg::PluginResult LrWeaponsPlugin::OnPluginEnd() {
	// PITFALLS A7: nothing may point into this library after the unload. The gate closes first, so
	// no client query can happen while the listeners are being removed.
	g_mapReady = false;
	try {
		if (m_disconnect_registered) {
			m_disconnect_registered = false;
			if (__s2sdk_OnClientDisconnect_Unregister != nullptr) {
				s2sdk::OnClientDisconnect_Unregister(&OnClientDisconnectListener);
			}
		}
	} catch (...) {
		// nothing left to do at unload
	}
	try {
		// The Unhook* are no-ops when levels_ranks is not loaded (lr_api.hpp Unregister checks the
		// null binding slot); the Available() guard only keeps that intent visible here.
		if (g_pLRCore->Available()) {
			g_pLRCore->UnhookOnPlayerLoaded();
			g_pLRCore->UnhookOnResetPlayerStats();
			g_pLRCore->UnhookOnPlayerKilledPre();
			g_pLRCore->UnhookOnDatabaseCleanup();
			g_pLRCore->UnhookOnCoreIsReady();
		}
	} catch (...) {
		// levels_ranks is gone: its callbacks died with it
	}
	try {
		UnregisterMapListeners();
	} catch (...) {
		// nothing left to do at unload
	}
	g_mapStartSeen = false;
	g_mapEndSeen = false;
	m_db.reset();
	return plg::PluginResult{};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
