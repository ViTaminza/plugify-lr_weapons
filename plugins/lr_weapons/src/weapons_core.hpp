#pragma once
// Engine-free core of the lr_weapons port (the Pisex LR module "[LR] ExStats Weapons",
// ref/src/cs2-lvl_ranks_modules/LR_ExStatsWeapons/lr_weapons.cpp, 369 lines): the
// exstats_weapons.ini model, the per-slot kill counters, the weapon classname resolution of the
// kill handler and the SQL of the `<lr_table>_weapons` table.
//
// The engine-side half (s2sdk reads, the disconnect listener, the LR core hooks) lives in
// src/plugin.cpp; everything here is testable against SQLite and MySQL.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <pisex/keyvalues.hpp>
#include <plgdb/db.hpp>

namespace lr_weapons {

inline constexpr int kMaxSlots = 64;
// The original's g_iWeaponsStats[64][96] / g_sWeaponsClassName[96][64] / g_hWeapons.
inline constexpr int kMaxWeapons = 96;

struct WeaponConfig {
	std::string classname;      // the KeyValues key, e.g. "weapon_ak47"
	std::string name;           // "name", e.g. "AK-47" (menu text; kept for parity)
	float coefficient = 1.0f;   // "coefficient", default 1.0
	bool show_top = false;      // "showtop" (menu flag of the original; kept for parity)
};

struct Config {
	// "weapon_coefficient" (0/1, default 1): whether the per-weapon coefficient is applied.
	bool coefficient_active = true;
	// The weapons in file order (the original keeps the order for its indexing and menus).
	std::vector<WeaponConfig> weapons;

	// Index of a classname in `weapons`, -1 when the config has no such weapon.
	int IndexOf(std::string_view classname) const;
};

// LoadSettings() of the original: "weapon_coefficient" + one block per weapon (the scalar key is
// not a "true subkey", so it is skipped exactly like FOR_EACH_SUBKEY skipped it). Entries beyond
// kMaxWeapons are dropped (the original wrote past its arrays).
void ParseConfig(const pisex::KeyValues& kv, Config& out, std::string* problem = nullptr);

// The classname the kill handler looks up: "weapon_<event weapon>", with the original's knife
// special case (`sBuffer[0] == 'k' || !strcmp(sBuffer, "bayonet")` -> "weapon_knife").
std::string NormalizeClassname(const char* weapon);

// RoundToNearest() of the original: static_cast<int>(round(value)).
int RoundToNearest(float value);

// engine->GetPlayerNetworkIDString() + ConvertSteamID() of the original: CS2's "[U:1:accountid]"
// becomes "STEAM_1:x:y" (the core's own conversion, so both write the same `steam` value); a value
// that already is a STEAM_ id is passed through, anything unusable becomes "".
std::string ConvertSteamId(std::string_view network_id);

// --- per-slot state (the original's g_iWeaponsStats / g_bWeaponsNew) ---------------------------

struct SlotState {
	std::array<int, kMaxWeapons> kills{};
	std::array<bool, kMaxWeapons> is_new{};
};

using State = std::array<SlotState, kMaxSlots>;

// --- SQL ---------------------------------------------------------------------------------------
// Statements are built here and executed by the plugin (async, on plgdb's worker) or by the tests
// (QuerySync). Values are always bound parameters, never interpolated.

// `<base>_weapons` (the original's g_sTableName + "_weapons").
std::string TableName(std::string_view base);

std::string CreateTableSql(std::string_view table, plgdb::Driver driver);

// SELECT `kills` FROM ... WHERE classname = ? AND steam = ? - the load of OnPlayerLoadedHook; the
// port runs one query for the whole row set instead of one per weapon (same result).
struct Statement {
	std::string sql;
	plgdb::Params params;
};
Statement LoadPlayerStatement(std::string_view table, std::string_view steamid);

// Applies a LoadPlayerStatement result (columns: `classname`, `kills`) to the slot state: a
// weapon with a row gets its kills and is_new=false, a weapon without a row gets 0 and is_new=true
// (the OnPlayerLoadedHook callback of the original).
void ApplyLoadedWeapons(SlotState& state, const Config& cfg, const plgdb::Result& result);

// SaveDataPlayer(slot, weapon) of the original: INSERT for a row that does not exist yet (then
// is_new is cleared), UPDATE otherwise.
Statement SaveWeaponStatement(std::string_view table, const Config& cfg, SlotState& state,
							  std::string_view steamid, int weapon);

// OnResetLoadedHook(): UPDATE ... SET `kills` = 0 WHERE `steam` = ? plus zeroing the local
// counters (the original zeroed them in the query callback; the plugin calls ZeroSlot there).
Statement ResetPlayerStatement(std::string_view table, std::string_view steamid);
void ZeroSlot(SlotState& state, const Config& cfg);

// OnDatabaseCleanupHook() for LR_AllData / LR_StatsData.
std::string TruncateSql(std::string_view table, plgdb::Driver driver);

} // namespace lr_weapons
