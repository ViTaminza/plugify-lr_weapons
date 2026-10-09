// lr_weapons — Plugify C++ port of the Pisex LR module "[LR] ExStats Weapons"
// (ref/src/cs2-lvl_ranks_modules/LR_ExStatsWeapons/lr_weapons.cpp, 369 lines + lr_weapons.h;
// the production plugin addons/lr_modules/lr_weapons, vdf lr_weapons.vdf).
//
// What it does (identical to the original): counts kills per weapon classname into the
// `<lr_table>_weapons` table (steam, classname, kills), applies the per-weapon experience
// coefficient to the kill's experience, saves a row per kill when
// lr_db_savedata_player_mode is set and the whole slot on disconnect otherwise, zeroes the
// counters on !reset/cleanup.
//
// Port notes (the rest is parity):
//   * The original hooked IServerGameClients::ClientDisconnect through SourceHook and read
//     pPlayerController->m_hPawn()/m_steamID(); the port registers s2sdk's OnClientDisconnect_Post
//     (the same point the levels_ranks core uses) and takes the steam id from the engine value
//     with the id cached at OnPlayerLoaded as the fallback. The original's "player must still have
//     a pawn" test is dropped - at disconnect time it is racy and the counters are what has to
//     reach the database (the LR core documents the same deviation).
//   * exstats_weapons.ini is read at plugin start, like Load(); the crash guards the production
//     copy carries (slot range, empty network id) are kept.
//   * The database is its own plgdb connection with the core's settings
//     (GetDatabaseConfig(), "lr.sqlite3" for the SQLite fallback), because a Plugify plugin
//     cannot share another plugin's connection object; the table name/columns are the original's.
//   * The per-weapon SELECT of OnPlayerLoadedHook is one query for the whole row set (the original
//     issued one per weapon); the applied state is identical.
//   * No menus of its own (the original module had none; the "name"/"showtop" fields are parsed
//     and kept for config parity only).

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>

#include <s2sdk/s2sdk.hpp>

#include <lr_api.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "weapons_core.hpp"

namespace {

// GetLogTag() of the original.
constexpr char kLogTag[] = "[LR-Weapons]";

// The original's g_pLRCore.
levels_ranks::LrApi* g_pLRCore = &levels_ranks::LrApi::Get();

using namespace lr_weapons;

void OnClientDisconnectListener(int32_t slot, s2sdk::NetworkDisconnectionReason reason);

class LrWeaponsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// lr_weapons::OnClientDisconnect of the original (s2sdk OnClientDisconnect_Post context).
	void OnClientDisconnect(int32_t slot);

private:
	void LoadSettings();
	void OnCoreIsReady();
	void OnPlayerLoaded(int slot, const char* steamid);
	void OnResetPlayerStats(int slot, const char* steamid);
	void OnPlayerKilledPre(pisex::IGameEvent* event, int& exp_caused);
	void OnDatabaseCleanup(int type);

	// SaveDataPlayer() of the original; weapon = -1 saves every non-zero counter.
	void SaveDataPlayer(int slot, int weapon = -1);
	// Queues one statement (no callback: the original's queries were fire and forget too).
	void RunStatement(Statement statement);
	// engine->GetPlayerNetworkIDString(slot) + ConvertSteamID, with the cached id as the fallback.
	std::string SteamIdOf(int slot) const;
	bool PlayerUsable(int slot) const;

	Config m_cfg;
	State m_state{};
	std::array<std::string, kMaxSlots> m_steamids{};
	std::unique_ptr<plgdb::Database> m_db;
	std::string m_table; // <lr_table>_weapons
	bool m_disconnect_registered = false;
};

LrWeaponsPlugin g_plugin;

void OnClientDisconnectListener(int32_t slot, s2sdk::NetworkDisconnectionReason /*reason*/) {
	try {
		g_plugin.OnClientDisconnect(slot);
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnClientDisconnect failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnClientDisconnect failed with an unknown exception", kLogTag);
	}
}

// --- configuration -------------------------------------------------------------------------------

void LrWeaponsPlugin::LoadSettings() {
	// LoadSettings() of the original: KeyValues("LR_ExStatsWeapons")
	// -> addons/configs/levels_ranks/exstats_weapons.ini.
	pisex::KeyValues kv("LR_ExStatsWeapons");
	if (!kv.LoadFromFile("addons/configs/levels_ranks/exstats_weapons.ini")) {
		pisex::ErrorLog("%s Failed to load addons/configs/levels_ranks/exstats_weapons.ini", kLogTag);
		return;
	}
	std::string problem;
	ParseConfig(kv, m_cfg, &problem);
	if (!problem.empty()) {
		pisex::ErrorLog("%s exstats_weapons.ini: %s", kLogTag, problem.c_str());
	}
}

// --- database ------------------------------------------------------------------------------------

void LrWeaponsPlugin::RunStatement(Statement statement) {
	if (m_db == nullptr || statement.sql.empty()) {
		return;
	}
	m_db->Query(std::move(statement.sql), std::move(statement.params));
}

void LrWeaponsPlugin::OnCoreIsReady() {
	// OnCoreIsReadyHook() of the original: the connection first, then the table and the hooks.
	m_table = TableName(g_pLRCore->GetTableName());
	m_db = plgdb::Database::Create(
		levels_ranks::MakeDatabaseConfig(g_pLRCore->GetDatabaseConfig(), "lr.sqlite3"));
	if (m_db == nullptr) {
		pisex::ErrorLog("%s the database configuration is invalid - weapon statistics are off", kLogTag);
		return;
	}
	// The DDL is queued (FIFO before every later statement), like the original's async
	// g_pConnection->Query of the CREATE TABLE.
	m_db->Query(CreateTableSql(m_table, m_db->GetDriver()), {});

	g_pLRCore->HookOnPlayerLoaded([this](int slot, const char* steamid) { OnPlayerLoaded(slot, steamid); });
	g_pLRCore->HookOnResetPlayerStats(
		[this](int slot, const char* steamid) { OnResetPlayerStats(slot, steamid); });
	g_pLRCore->HookOnPlayerKilledPre([this](pisex::IGameEvent* event, int& exp_caused, int, int) {
		OnPlayerKilledPre(event, exp_caused);
	});
	g_pLRCore->HookOnDatabaseCleanup([this](int type) { OnDatabaseCleanup(type); });
}

void LrWeaponsPlugin::OnPlayerLoaded(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::string id = steamid != nullptr ? steamid : "";
	m_steamids[static_cast<std::size_t>(slot)] = id;
	// The per-weapon SELECT of the original, in one query; the result pass fills the counters
	// (a weapon without a row gets 0 and is_new = true, exactly like the empty result there).
	Statement statement = LoadPlayerStatement(m_table, id);
	m_db->Query(std::move(statement.sql), std::move(statement.params),
				[this, slot](const plgdb::Result& result) {
					ApplyLoadedWeapons(m_state[static_cast<std::size_t>(slot)], m_cfg, result);
				});
}

void LrWeaponsPlugin::OnResetPlayerStats(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::string id = steamid != nullptr ? steamid : "";
	m_steamids[static_cast<std::size_t>(slot)] = id;
	Statement statement = ResetPlayerStatement(m_table, id);
	m_db->Query(std::move(statement.sql), std::move(statement.params), [this, slot](const plgdb::Result&) {
		ZeroSlot(m_state[static_cast<std::size_t>(slot)], m_cfg);
	});
}

void LrWeaponsPlugin::OnClientDisconnect(int32_t slot) {
	//     if(!g_pLRCore->GetSettingsValue(LR_DB_SaveDataPlayer_Mode)) SaveDataPlayer(slot.Get());
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	if (!g_pLRCore->GetSettingsValue(levels_ranks::LR_DB_SaveDataPlayer_Mode)) {
		SaveDataPlayer(static_cast<int>(slot));
	}
	m_steamids[static_cast<std::size_t>(slot)].clear(); // the slot is free again
}

void LrWeaponsPlugin::OnDatabaseCleanup(int type) {
	// OnDatabaseCleanupHook() of the original: LR_AllData / LR_StatsData.
	if (type != levels_ranks::LR_AllData && type != levels_ranks::LR_StatsData) {
		return;
	}
	if (m_db != nullptr) {
		m_db->Query(TruncateSql(m_table, m_db->GetDriver()), {});
	}
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		if (!PlayerUsable(slot)) {
			continue;
		}
		ZeroSlot(m_state[static_cast<std::size_t>(slot)], m_cfg);
		const std::string id = SteamIdOf(slot);
		if (!id.empty() && m_db != nullptr) {
			Statement statement = ResetPlayerStatement(m_table, id);
			m_db->Query(std::move(statement.sql), std::move(statement.params));
		}
	}
}

// --- game -----------------------------------------------------------------------------------------

std::string LrWeaponsPlugin::SteamIdOf(int slot) const {
	// engine->GetPlayerNetworkIDString(slot) in the original.
	const std::string network_id = pisex::g_pPlayers->GetSteamID(slot);
	std::string id = ConvertSteamId(network_id);
	if (id.empty() && slot >= 0 && slot < kMaxSlots) {
		id = m_steamids[static_cast<std::size_t>(slot)];
	}
	return id;
}

bool LrWeaponsPlugin::PlayerUsable(int slot) const {
	if (slot < 0 || slot >= kMaxSlots) {
		return false;
	}
	// pAttacker && pAttacker->GetPlayerPawn() && pAttacker->m_steamID() > 0 of the original.
	return pisex::GetControllerEntity(slot) != 0 && pisex::GetPawnEntity(slot) != 0 &&
		   pisex::g_pPlayers->GetSteamID64(slot) > 0;
}

void LrWeaponsPlugin::SaveDataPlayer(int slot, int weapon) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::string id = SteamIdOf(slot);
	if (id.empty()) {
		return; // the guard for the original's ConvertSteamID(nullptr) crash
	}
	SlotState& state = m_state[static_cast<std::size_t>(slot)];
	if (weapon != -1) {
		RunStatement(SaveWeaponStatement(m_table, m_cfg, state, id, weapon));
		return;
	}
	for (int i = 0; i < static_cast<int>(m_cfg.weapons.size()); ++i) {
		if (state.kills[static_cast<std::size_t>(i)] != 0) {
			RunStatement(SaveWeaponStatement(m_table, m_cfg, state, id, i));
		}
	}
}

void LrWeaponsPlugin::OnPlayerKilledPre(pisex::IGameEvent* event, int& exp_caused) {
	// OnPlayerKilledPreHook() of the original.
	if (event == nullptr || !g_pLRCore->CheckCountPlayers()) {
		return;
	}
	const int attacker = event->GetInt("attacker");
	if (attacker < 0 || attacker >= kMaxSlots) {
		return; // the production crash guard of the module
	}
	if (!PlayerUsable(attacker)) {
		return;
	}
	const char* weapon_name = event->GetString("weapon");
	if (weapon_name == nullptr || weapon_name[0] == '\0') {
		return;
	}
	const std::string classname = NormalizeClassname(weapon_name);
	const int index = m_cfg.IndexOf(classname);
	if (index < 0) {
		return;
	}
	// if(g_bWeaponsCoeffActive) { iExpCaused = RoundToNearest(iExpCaused * coefficient); }
	if (m_cfg.coefficient_active) {
		exp_caused = RoundToNearest(static_cast<float>(exp_caused) *
									m_cfg.weapons[static_cast<std::size_t>(index)].coefficient);
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
		pisex::Init("levels_ranks");
		LoadSettings();

		// SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientDisconnect, ...) of the original.
		if (__s2sdk_OnClientDisconnect_Post_Register != nullptr) {
			s2sdk::OnClientDisconnect_Post_Register(&OnClientDisconnectListener);
			m_disconnect_registered = true;
		}

		// AllPluginsLoaded() of the original.
		if (g_pLRCore->CoreIsLoaded()) {
			OnCoreIsReady();
		} else {
			g_pLRCore->HookOnCoreIsReady([this] { OnCoreIsReady(); });
		}
		return plg::PluginResult{};
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnPluginStart failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{}; // never Failed: that stops every plugify plugin
}

plg::PluginResult LrWeaponsPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		pisex::Poll();
		if (m_db != nullptr) {
			m_db->Poll();
		}
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnPluginUpdate failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnPluginUpdate failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{};
}

plg::PluginResult LrWeaponsPlugin::OnPluginEnd() {
	try {
		if (m_disconnect_registered && __s2sdk_OnClientDisconnect_Post_Unregister != nullptr) {
			s2sdk::OnClientDisconnect_Post_Unregister(&OnClientDisconnectListener);
			m_disconnect_registered = false;
		}
		// The Unhook* are no-ops when levels_ranks is not loaded (lr_api.hpp Unregister checks the
		// null binding slot); the Available() guard only keeps that intent visible here.
		if (g_pLRCore->Available()) {
			g_pLRCore->UnhookOnPlayerLoaded();
			g_pLRCore->UnhookOnResetPlayerStats();
			g_pLRCore->UnhookOnPlayerKilledPre();
			g_pLRCore->UnhookOnDatabaseCleanup();
			g_pLRCore->UnhookOnCoreIsReady();
		}
		pisex::Shutdown();
		m_db.reset();
	} catch (...) {
		// nothing left to do at unload
	}
	return plg::PluginResult{};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
