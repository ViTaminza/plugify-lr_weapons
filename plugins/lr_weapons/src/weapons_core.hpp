#pragma once
// Engine-free core of the lr_weapons port (the Pisex LR module "[LR] ExStats Weapons", the
// original's lr_weapons.cpp): the exstats_weapons.ini model, the per-slot kill counters, the
// weapon classname resolution of the kill handler and the SQL of the `<lr_table>_weapons` table.
//
// Self-contained on purpose (docs/PLUGIN_STANDARD.md §7: no pisexcompat, no helper library): the
// config is read by the KV1 reader below, which accepts exactly the grammar the original's
// KeyValues parser did. The engine-side half (s2sdk reads, the disconnect listener, the LR core
// hooks) lives in src/plugin.cpp; everything here is testable against SQLite and MySQL.
//
// The original loaded the file as
//     KeyValues kv("LR_ExStatsWeapons");
//     kv.LoadFromFile("addons/configs/levels_ranks/exstats_weapons.ini", "GAME");
// i.e. one "LR_ExStatsWeapons" { ... } block with a scalar "weapon_coefficient" key and one block
// per weapon. The production file is the original Metamod file, unchanged (PLUGIN_STANDARD §7:
// production KeyValues files are read as they are, no conversion):
//
//     "LR_ExStatsWeapons"
//     {
//         "weapon_coefficient" "1"     // Включить ли множитель ...
//
//         "weapon_knife"               // ClassName оружия
//         {
//             "name"        "Knife"
//             "coefficient" "5.0"
//         }
//     }
//
// The reader's grammar (the subset pisexcompat's KeyValues accepted):
//   * quoted ("key" "value") and unquoted (key value) tokens,
//   * nested "key" { ... } blocks; a key may carry a value and a block,
//   * // line comments and /* */ block comments, including a trailing comment after a value,
//   * the escapes \" \\ \n \r \t inside quoted strings (an unknown escape stays verbatim),
//   * an optional UTF-8 BOM at the start of the file,
//   * #base / #include / #define lines are skipped instead of followed (no file inclusion).
// A malformed file - an unterminated string, an unterminated block comment, a missing '}' or a
// stray top-level '}' - is rejected as a whole (std::nullopt), never returned half-read: the
// loader must not let a truncated tree silently override the defaults the original used.
//
// Like Valve's loader, a file that is a single "Root" { ... } block is returned as that block, so
// `ParseConfig(*LoadKv1File(...))` reads the keys inside "LR_ExStatsWeapons" { ... }. Key lookup is
// case-insensitive (Valve's FindKey), the stored spelling is kept.

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <plgdb/db.hpp>

namespace lr_weapons {

inline constexpr int kMaxSlots = 64;
// The original's g_iWeaponsStats[64][96] / g_sWeaponsClassName[96][64] / g_hWeapons.
inline constexpr int kMaxWeapons = 96;

// --- the KV1 reader -----------------------------------------------------------------------------

// One node of the parsed tree: a key with an optional value and optional "key" { ... } children.
struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive, like Valve's FindKey), or nullptr.
	const KvNode* Find(std::string_view key) const;
	// The child's value; `def` when the child is missing or has no value.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const;
	// The child's value as an int, with Valve's atoi fallback ("5x" -> 5, "abc" -> 0); `def` when
	// the child is missing or its value is empty (KeyValues::GetInt(key, def)).
	int GetInt(std::string_view key, int def = 0) const;
	// The child's value as a float, strtof semantics ("1.5x" -> 1.5); `def` when the child is
	// missing, its value is empty or it does not start with a number (KeyValues::GetFloat).
	float GetFloat(std::string_view key, float def = 0.0f) const;
};

// The file text; std::nullopt for malformed input (see the grammar above).
std::optional<KvNode> ParseKv1(std::string_view text);
// ParseKv1 over the bytes of `path`; std::nullopt when the file cannot be read or is malformed.
std::optional<KvNode> LoadKv1File(const std::string& path);

// --- where the file is read from -----------------------------------------------------------------

// The Pisex LR installation keeps every module's config in one directory; the plugin's own folder
// (PLUGIN_STANDARD §3) is the fallback for a clean Plugify install.
inline constexpr std::string_view kLevelsRanksDir = "levels_ranks";
inline constexpr std::string_view kPluginDir = "lr_weapons";
inline constexpr std::string_view kConfigFile = "exstats_weapons.ini";

std::string JoinPath(std::string_view base, std::string_view name);
bool FileExists(const std::string& path);

// <configsDir>/levels_ranks/exstats_weapons.ini, then <configsDir>/lr_weapons/exstats_weapons.ini.
// Empty when |configsDir| is empty (the host did not supply plg::GetConfigsDir()).
std::vector<std::string> ConfigCandidates(std::string_view configsDir);

// --- the configuration (the exstats_weapons.ini model) -------------------------------------------

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

// The outcome of LoadConfig(): what was read and what was wrong with it.
struct ConfigSource {
	bool loaded = false;    // a candidate file was found and parsed into the Config
	std::string path;       // the file that was read (empty when no candidate existed)
	std::string error;      // why nothing was loaded (empty when `loaded` is true)
	std::string problem;    // non-fatal parse note (e.g. more weapon blocks than kMaxWeapons)
};

// Tries ConfigCandidates() in order and parses the first file that exists into `out`. The first
// existing file is authoritative: a malformed one is reported through `error` instead of silently
// falling back (a half-broken primary config must not be masked by an old copy). `out` is only
// modified when a file was parsed; the caller keeps its defaults otherwise (fail soft).
ConfigSource LoadConfig(std::string_view configsDir, Config& out);

// LoadSettings() of the original: "weapon_coefficient" + one block per weapon (the scalar key is
// not a "true subkey", so it is skipped exactly like FOR_EACH_SUBKEY skipped it). Entries beyond
// kMaxWeapons are dropped (the original wrote past its arrays).
void ParseConfig(const KvNode& kv, Config& out, std::string* problem = nullptr);

// The classname the kill handler looks up: "weapon_<event weapon>", with the original's knife
// special case (a 'k' prefix or "bayonet" -> "weapon_knife").
std::string NormalizeClassname(const char* weapon);

// RoundToNearest() of the original: the rounded value as an int.
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
// (the OnPlayerLoadedHook callback of the original). A failed result (Result::Ok() false) leaves
// the state untouched - the caller retries the load instead of treating it as "no rows".
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
