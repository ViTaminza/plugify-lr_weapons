// Tests for plugins/lr_weapons/src/weapons_core.hpp (engine-free core of the ported
// "[LR] ExStats Weapons" module).
//
//   * exstats_weapons.ini parsing against the live production file
//     (ref/prod-27016/addons/configs/levels_ranks/exstats_weapons.ini),
//   * the kill-handler helpers (weapon classname resolution, rounding, steam id conversion),
//   * the SQL layer: CREATE TABLE / INSERT / UPDATE / SELECT / reset / truncate against SQLite
//     ":memory:" always and against MySQL when MYSQL_HOST is set (test.sh provides
//     MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE). Rows created in MySQL are
//     deleted at the end.

#include <plg/plugin.hpp>

#include <pisex/keyvalues.hpp>
#include <pisex/paths.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#include "weapons_core.hpp"

// A real plugin gets these from PLUGIFY_PLUGIN; the test leaves them null. The path resolver is
// driven through pisex::SetPluginName/SetConfigDir instead.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                            \
	do {                                                                                       \
		++g_checks;                                                                            \
		if (!(expr)) {                                                                         \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),         \
						g_section.data(), __FILE__, __LINE__, #expr);                          \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const long long a_ = static_cast<long long>(actual);                                   \
		const long long e_ = static_cast<long long>(expected);                                 \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_, e_);                                                     \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const std::string a_ = (actual);                                                       \
		const std::string e_ = (expected);                                                     \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                    \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_.c_str(), e_.c_str());                                     \
		}                                                                                      \
	} while (false)

using namespace lr_weapons;

// --- configuration ------------------------------------------------------------------------------

void TestRealConfig() {
	Section("real exstats_weapons.ini");
	pisex::SetPluginName("levels_ranks");
	pisex::SetConfigDir(LR_WEAPONS_TEST_DATA_DIR);

	pisex::KeyValues kv("LR_ExStatsWeapons");
	CHECK(kv.LoadFromFile("addons/configs/levels_ranks/exstats_weapons.ini"));

	Config cfg;
	std::string problem;
	ParseConfig(kv, cfg, &problem);
	CHECK(problem.empty());
	CHECK_EQ_INT(cfg.weapons.size(), 38); // the production file's weapon blocks
	CHECK(cfg.coefficient_active);

	// The scalar "weapon_coefficient" key is not a weapon block.
	CHECK_EQ_INT(cfg.IndexOf("weapon_coefficient"), -1);
	CHECK_EQ_INT(cfg.IndexOf("weapon_ak47"), 28);
	CHECK_EQ_STR(cfg.weapons[cfg.IndexOf("weapon_ak47")].name, "AK-47");
	CHECK_EQ_INT(cfg.weapons[cfg.IndexOf("weapon_ak47")].coefficient * 10.0f, 9); // 0.9
	CHECK(cfg.weapons[cfg.IndexOf("weapon_ak47")].show_top);
	CHECK_EQ_STR(cfg.weapons[0].classname, "weapon_knife");
	CHECK_EQ_INT(cfg.weapons[0].coefficient * 10.0f, 50); // 5.0
	CHECK(!cfg.weapons[0].show_top);
	CHECK_EQ_INT(cfg.weapons[cfg.IndexOf("weapon_awp")].coefficient * 10.0f, 6); // 0.6
}

void TestConfigVariants() {
	Section("config variants");
	{
		pisex::KeyValues kv("LR_ExStatsWeapons");
		CHECK(kv.LoadFromBuffer(
			"\"LR_ExStatsWeapons\"\n{\n"
			"    \"weapon_coefficient\" \"0\"\n"
			"    \"weapon_ak47\" { \"name\" \"AK-47\" \"coefficient\" \"2.0\" }\n"
			"}\n"));
		Config cfg;
		ParseConfig(kv, cfg);
		CHECK(!cfg.coefficient_active);
		CHECK_EQ_INT(cfg.weapons.size(), 1);
		CHECK_EQ_INT(cfg.weapons[0].coefficient * 10.0f, 20);
	}
	{
		// A weapon block without a coefficient keeps the default 1.0 of GetFloat("coefficient", 1.0).
		pisex::KeyValues kv("LR_ExStatsWeapons");
		CHECK(kv.LoadFromBuffer(
			"\"LR_ExStatsWeapons\"\n{\n    \"weapon_mp9\" { \"name\" \"MP9\" }\n}\n"));
		Config cfg;
		ParseConfig(kv, cfg);
		CHECK_EQ_INT(cfg.weapons.size(), 1);
		CHECK_EQ_INT(cfg.weapons[0].coefficient * 10.0f, 10);
	}
	{
		// More than kMaxWeapons blocks are dropped (the original wrote past its arrays here).
		std::string text = "\"LR_ExStatsWeapons\"\n{\n";
		for (int i = 0; i < kMaxWeapons + 5; ++i) {
			text += "    \"weapon_test" + std::to_string(i) + "\" { \"name\" \"T\" }\n";
		}
		text += "}\n";
		pisex::KeyValues kv("LR_ExStatsWeapons");
		CHECK(kv.LoadFromBuffer(text));
		Config cfg;
		std::string problem;
		ParseConfig(kv, cfg, &problem);
		CHECK_EQ_INT(cfg.weapons.size(), kMaxWeapons);
		CHECK(!problem.empty());
	}
}

// --- the kill-handler helpers ---------------------------------------------------------------------

void TestHelpers() {
	Section("helpers");
	CHECK_EQ_STR(NormalizeClassname("ak47"), "weapon_ak47");
	CHECK_EQ_STR(NormalizeClassname("bayonet"), "weapon_knife");
	CHECK_EQ_STR(NormalizeClassname("knife"), "weapon_knife");
	CHECK_EQ_STR(NormalizeClassname("karambit"), "weapon_knife"); // the original's name[0] == 'k'
	CHECK_EQ_STR(NormalizeClassname(""), "weapon_");
	CHECK_EQ_STR(NormalizeClassname(nullptr), "weapon_");

	// RoundToNearest: static_cast<int>(round(value)) - 0.5 rounds away from zero, not to even.
	CHECK_EQ_INT(RoundToNearest(0.0f), 0);
	CHECK_EQ_INT(RoundToNearest(0.4f), 0);
	CHECK_EQ_INT(RoundToNearest(0.5f), 1);
	CHECK_EQ_INT(RoundToNearest(1.5f), 2);
	CHECK_EQ_INT(RoundToNearest(2.5f), 3);
	CHECK_EQ_INT(RoundToNearest(-0.5f), -1);
	CHECK_EQ_INT(RoundToNearest(10.0f * 0.9f), 9);

	// engine->GetPlayerNetworkIDString() -> ConvertSteamID of the original.
	CHECK_EQ_STR(ConvertSteamId("[U:1:46116860184329472]"), "STEAM_1:0:23058430092164736");
	CHECK_EQ_STR(ConvertSteamId("[U:1:123]" ), "STEAM_1:1:61");
	CHECK_EQ_STR(ConvertSteamId("[U:1:124]" ), "STEAM_1:0:62");
	CHECK_EQ_STR(ConvertSteamId("STEAM_1:0:62"), "STEAM_1:0:62"); // s2sdk's GetClientAuthId form
	CHECK_EQ_STR(ConvertSteamId(""), "");
	CHECK_EQ_STR(ConvertSteamId("BOT"), "BOT"); // unusable input is kept verbatim, never crashes
	// The table name of the original: g_sTableName + "_weapons".
	CHECK_EQ_STR(TableName("lvl_base"), "lvl_base_weapons");
}

// --- SQL ---------------------------------------------------------------------------------------

plgdb::Config SqliteConfig() {
	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = ":memory:";
	return config;
}

bool MySqlConfig(plgdb::Config& config, std::string& error) {
	const char* host = std::getenv("MYSQL_HOST");
	if (host == nullptr || *host == '\0') {
		return false;
	}
	config.driver = plgdb::Driver::MySQL;
	config.host = host;
	config.user = std::getenv("MYSQL_USER") != nullptr ? std::getenv("MYSQL_USER") : "";
	config.password = std::getenv("MYSQL_PASSWORD") != nullptr ? std::getenv("MYSQL_PASSWORD") : "";
	config.database = std::getenv("MYSQL_DATABASE") != nullptr ? std::getenv("MYSQL_DATABASE") : "";
	const char* port = std::getenv("MYSQL_PORT");
	if (port != nullptr && *port != '\0') {
		try {
			const int value = std::stoi(port);
			if (value < 1 || value > 65535) {
				error = "MYSQL_PORT out of range";
				return false;
			}
			config.port = static_cast<uint16_t>(value);
		} catch (const std::exception&) {
			error = "MYSQL_PORT is not a number";
			return false;
		}
	}
	return true;
}

// The full data path of the module on one concrete table (the `<lr_table>_weapons` of the core).
void TestDatabase(const plgdb::Config& config, std::string_view label, const std::string& table,
				  bool cleanup) {
	Section(std::string("database ") + std::string(label));
	std::unique_ptr<plgdb::Database> db = plgdb::Database::Create(config);
	CHECK(db != nullptr);
	if (db == nullptr) {
		return;
	}

	// The config the module runs with: the production one.
	pisex::SetPluginName("levels_ranks");
	pisex::SetConfigDir(LR_WEAPONS_TEST_DATA_DIR);
	pisex::KeyValues kv("LR_ExStatsWeapons");
	CHECK(kv.LoadFromFile("addons/configs/levels_ranks/exstats_weapons.ini"));
	Config cfg;
	ParseConfig(kv, cfg);

	const plgdb::Result created = db->QuerySync(CreateTableSql(table, db->GetDriver()), {});
	CHECK(created.Ok());

	const std::string steam = "STEAM_1:1:90000001";
	State state{};
	SlotState& slot = state[3];

	// A fresh player: every weapon is is_new with 0 kills.
	{
		Statement load = LoadPlayerStatement(table, steam);
		const plgdb::Result result = db->QuerySync(load.sql, load.params);
		CHECK(result.Ok());
		ApplyLoadedWeapons(slot, cfg, result);
		CHECK(slot.is_new[0]);
		CHECK_EQ_INT(slot.kills[0], 0);
	}

	// Two kills with the knife and one with the AK: the first save of each weapon is the
	// INSERT of the original (is_new cleared), the second one the UPDATE.
	const int knife = cfg.IndexOf("weapon_knife");
	const int ak = cfg.IndexOf("weapon_ak47");
	CHECK_EQ_INT(knife, 0);
	slot.kills[knife]++;
	Statement insert = SaveWeaponStatement(table, cfg, slot, steam, knife);
	CHECK(insert.sql.rfind("INSERT INTO", 0) == 0);
	CHECK(!slot.is_new[knife]);
	CHECK(db->QuerySync(insert.sql, insert.params).Ok());

	slot.kills[knife]++;
	Statement update = SaveWeaponStatement(table, cfg, slot, steam, knife);
	CHECK(update.sql.rfind("UPDATE", 0) == 0);
	CHECK(db->QuerySync(update.sql, update.params).Ok());

	slot.kills[ak] += 2;
	Statement insert_ak = SaveWeaponStatement(table, cfg, slot, steam, ak);
	CHECK(db->QuerySync(insert_ak.sql, insert_ak.params).Ok());

	// A save of a weapon that was never touched keeps a zero row (the original's SaveDataPlayer(-1)
	// only walks the non-zero counters; the per-index path is only called after an increment).
	{
		Statement load = LoadPlayerStatement(table, steam);
		const plgdb::Result result = db->QuerySync(load.sql, load.params);
		CHECK(result.Ok());
		CHECK_EQ_INT(result.RowCount(), 2);

		SlotState reloaded{};
		ApplyLoadedWeapons(reloaded, cfg, result);
		CHECK(!reloaded.is_new[knife]);
		CHECK_EQ_INT(reloaded.kills[knife], 2);
		CHECK_EQ_INT(reloaded.kills[ak], 2);
		CHECK(reloaded.is_new[cfg.IndexOf("weapon_awp")]);
		CHECK_EQ_INT(reloaded.kills[cfg.IndexOf("weapon_awp")], 0);
	}

	// OnResetLoadedHook(): kills = 0 in the database and in memory.
	{
		Statement reset = ResetPlayerStatement(table, steam);
		CHECK(db->QuerySync(reset.sql, reset.params).Ok());
		ZeroSlot(slot, cfg);
		CHECK_EQ_INT(slot.kills[knife], 0);
		Statement load = LoadPlayerStatement(table, steam);
		const plgdb::Result result = db->QuerySync(load.sql, load.params);
		CHECK(result.Ok());
		CHECK_EQ_INT(result.RowCount(), 2); // the rows stay, only `kills` is zeroed
		CHECK_EQ_INT(result.GetInt(0, 1, -1) + result.GetInt(1, 1, -1), 0);
	}

	// OnDatabaseCleanupHook(): the TRUNCATE/DELETE.
	CHECK(db->QuerySync(TruncateSql(table, db->GetDriver()), {}).Ok());
	{
		Statement load = LoadPlayerStatement(table, steam);
		const plgdb::Result result = db->QuerySync(load.sql, load.params);
		CHECK(result.Ok());
		CHECK_EQ_INT(result.RowCount(), 0);
	}

	if (cleanup) {
		db->QuerySync("DROP TABLE `" + table + "`;", {});
	}
}

} // namespace

int main() {
	std::printf("lr_weapons_tests\n");

	std::random_device device;
	std::mt19937_64 rng(device());
	const std::string tag = std::to_string(rng() % 1000000ULL);

	TestRealConfig();
	TestConfigVariants();
	TestHelpers();

	TestDatabase(SqliteConfig(), "sqlite (:memory:)", "lvl_base_weapons_sqlite", /*cleanup=*/false);

	plgdb::Config mysql;
	std::string error;
	if (MySqlConfig(mysql, error)) {
		std::printf("== mysql: %s:%u/%s\n", mysql.host.c_str(), mysql.port, mysql.database.c_str());
		TestDatabase(mysql, "mysql", "lvl_weapons_test_" + tag, /*cleanup=*/true);
	} else if (!error.empty()) {
		std::printf("FATAL: %s\n", error.c_str());
		++g_failures;
	}

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
