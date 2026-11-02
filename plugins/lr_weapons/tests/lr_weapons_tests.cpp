// Tests for plugins/lr_weapons/src/weapons_core.{hpp,cpp} (engine-free core of the ported
// "[LR] ExStats Weapons" module).
//
//   * the shipped exstats_weapons.ini through the plugin's own KV1 reader, including the search
//     order (<configs>/levels_ranks first, <configs>/lr_weapons fallback) and the rule that a
//     malformed primary file is reported instead of being masked by the fallback copy,
//   * the reader itself: quotes, unquoted tokens, nested blocks, // and /* */ comments, #base
//     lines, escapes, a UTF-8 BOM, Valve's atoi/strtof value parsing and the rejection of
//     malformed input as a whole (std::nullopt),
//   * the config model (weapon blocks only, the coefficient defaults, the kMaxWeapons cap),
//   * the kill-handler helpers (weapon classname resolution, rounding, steam id conversion),
//   * the SQL text (CREATE TABLE per driver, TRUNCATE/DELETE),
//   * the SQL layer: CREATE TABLE / INSERT / UPDATE / SELECT / reset / truncate against SQLite
//     ":memory:" always and against MySQL when MYSQL_HOST is set (test.sh provides
//     MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE). Rows created in MySQL are
//     deleted at the end.
//
// No engine, no Plugify host, no pisexcompat: only the plugin's own core (and plgdb, which the
// module really uses) is compiled in.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "weapons_core.hpp"

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
		const std::string a_((actual));                                                        \
		const std::string e_((expected));                                                      \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                    \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_.c_str(), e_.c_str());                                     \
		}                                                                                      \
	} while (false)

using namespace lr_weapons;

bool EndsWith(std::string_view text, std::string_view suffix) {
	return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// --- the KV1 reader -------------------------------------------------------------------------------

void TestKvReader() {
	Section("KV1 reader");
	// Quoted and unquoted tokens, nested blocks, a key carrying both a value and a block, and
	// case-insensitive lookups (Valve's FindKey keeps the stored spelling).
	{
		const std::optional<KvNode> root = ParseKv1("Root\n{\n"
													"  \"quoted\" \"a value\"\n"
													"  unquoted 42\n"
													"  \"both\" \"v\" { \"inner\" \"x\" }\n"
													"}\n");
		CHECK(root.has_value());
		if (root) {
			CHECK_EQ_STR(root->name, "Root");
			CHECK_EQ_STR(root->GetString("quoted"), "a value");
			CHECK_EQ_STR(root->GetString("QUOTED"), "a value");
			CHECK_EQ_INT(root->GetInt("unquoted"), 42);
			const KvNode* both = root->Find("both");
			CHECK(both != nullptr);
			if (both != nullptr) {
				CHECK(both->hasValue);
				CHECK_EQ_STR(both->GetString("inner"), "x");
			}
			// A missing key: the default of the getter.
			CHECK(root->Find("nope") == nullptr);
			CHECK_EQ_STR(root->GetString("nope", "fallback"), "fallback");
			CHECK_EQ_INT(root->GetInt("nope", 7), 7);
		}
	}
	// // and /* */ comments (a trailing comment after a value included), #base / #include lines
	// skipped instead of followed. The production file is CRLF with such trailing comments.
	{
		const std::optional<KvNode> root = ParseKv1("// header\r\n"
													"#base \"other.ini\"\r\n"
													"Root { /* inline */ \"a\" /* between */ \"1\" // after\r\n"
													"       \"b\" \"2\"/* glued */\r\n"
													"       /* multi\r\n        line */ \"c\" \"3\"\r\n"
													"}\r\n");
		CHECK(root.has_value());
		if (root) {
			CHECK_EQ_INT(root->GetInt("a"), 1);
			CHECK_EQ_INT(root->GetInt("b"), 2);
			CHECK_EQ_INT(root->GetInt("c"), 3);
		}
		// A comment may contain braces and quotes without confusing the tokenizer.
		const std::optional<KvNode> braces = ParseKv1("Root { /* \"a\" { } \" */ \"a\" \"1\" }");
		CHECK(braces.has_value());
		if (braces) {
			CHECK_EQ_INT(braces->GetInt("a"), 1);
		}
	}
	// Escapes inside quotes (\" \\ \n \r \t); an unknown escape is kept verbatim.
	{
		const std::optional<KvNode> root = ParseKv1("Root { \"quote\" \"a\\\"b\" \"slash\" \"a\\\\b\" "
													"\"nl\" \"a\\nb\" \"cr\" \"a\\rb\" \"tab\" \"a\\tb\" "
													"\"unknown\" \"a\\qb\" }");
		CHECK(root.has_value());
		if (root) {
			CHECK_EQ_STR(root->GetString("quote"), "a\"b");
			CHECK_EQ_STR(root->GetString("slash"), "a\\b");
			CHECK_EQ_STR(root->GetString("nl"), "a\nb");
			CHECK_EQ_STR(root->GetString("cr"), "a\rb");
			CHECK_EQ_STR(root->GetString("tab"), "a\tb");
			CHECK_EQ_STR(root->GetString("unknown"), "a\\qb");
		}
	}
	// A UTF-8 BOM is ignored (it would otherwise become part of the first key).
	{
		const std::string text =
			std::string("\xEF\xBB\xBF") + "\"LR_ExStatsWeapons\" { \"weapon_coefficient\" \"0\" }";
		const std::optional<KvNode> root = ParseKv1(text);
		CHECK(root.has_value());
		if (root) {
			CHECK_EQ_STR(root->name, "LR_ExStatsWeapons");
			CHECK_EQ_INT(root->GetInt("weapon_coefficient"), 0);
		}
	}
	// A single top-level block becomes the root (Valve's loader merged it into the object); a flat
	// file keeps its top-level keys; an empty file is an empty tree, not an error.
	{
		const std::optional<KvNode> unwrapped = ParseKv1("\"LR_ExStatsWeapons\" { \"a\" \"1\" }");
		CHECK(unwrapped.has_value());
		if (unwrapped) {
			CHECK_EQ_INT(unwrapped->GetInt("a"), 1);
		}
		const std::optional<KvNode> flat = ParseKv1("\"a\" \"1\" \"b\" \"2\"");
		CHECK(flat.has_value());
		if (flat) {
			CHECK_EQ_INT(flat->GetInt("a"), 1);
			CHECK_EQ_INT(flat->GetInt("b"), 2);
		}
		const std::optional<KvNode> empty = ParseKv1("");
		CHECK(empty.has_value());
		if (empty) {
			CHECK(empty->children.empty());
		}
	}
	// Valve's value parsing: a value is trimmed, integers fall back to atoi ("5x" -> 5, "abc" ->
	// 0), floats use strtof ("1.5x" -> 1.5), a missing or empty value keeps the default.
	{
		const std::optional<KvNode> root = ParseKv1("Root { \"partial\" \"5 // x\" \"bad\" \"abc\" "
													"\"float\" \"1.5x\" \"empty\" \"\" }");
		CHECK(root.has_value());
		if (root) {
			CHECK_EQ_INT(root->GetInt("partial", -1), 5);
			CHECK_EQ_INT(root->GetInt("bad", 7), 0);
			CHECK_EQ_INT(root->GetInt("empty", 7), 7);
			CHECK_EQ_INT(root->GetInt("missing", 9), 9);
			CHECK_EQ_INT(root->GetFloat("float", -1.0f) * 10.0f, 15);
			CHECK_EQ_INT(root->GetFloat("bad", -1.0f) * 10.0f, -10);
			CHECK_EQ_INT(root->GetFloat("missing", 2.5f) * 10.0f, 25);
		}
	}
	// Malformed input is rejected as a whole (std::nullopt), never half a tree.
	CHECK(!ParseKv1("\"a\" { \"b\" \"unterminated }").has_value()); // unterminated string
	CHECK(!ParseKv1("\"a\" { \"b\" \"c\"").has_value());            // missing '}'
	CHECK(!ParseKv1("\"a\" \"b\" }").has_value());                  // stray top-level '}'
	CHECK(!ParseKv1("\"a\" { \"b\" \"c\" } /* open").has_value());  // unterminated block comment
	CHECK(!ParseKv1("\"a\" { /* \"b\" \"c\" }").has_value());       // '}' hidden by a comment

	// LoadKv1File: the shipped file (CRLF, comments) parses, a missing path is nullopt.
	{
		const std::string path = std::string(LR_WEAPONS_TEST_DATA_DIR) + "/levels_ranks/exstats_weapons.ini";
		CHECK(FileExists(path));
		const std::optional<KvNode> root = LoadKv1File(path);
		CHECK(root.has_value());
		if (root) {
			CHECK_EQ_INT(root->GetInt("weapon_coefficient", -1), 1);
			CHECK(root->Find("weapon_ak47") != nullptr);
		}
		CHECK(!LoadKv1File(std::string(LR_WEAPONS_TEST_DATA_DIR) + "/no-such-file.ini").has_value());
	}
}

// --- where the file is read from -------------------------------------------------------------------

void TestConfigCandidates() {
	Section("config candidates");
	const std::vector<std::string> candidates = ConfigCandidates("/srv/plugify/configs");
	CHECK_EQ_INT(candidates.size(), 2);
	if (candidates.size() == 2) {
		CHECK_EQ_STR(candidates[0], "/srv/plugify/configs/levels_ranks/exstats_weapons.ini");
		CHECK_EQ_STR(candidates[1], "/srv/plugify/configs/lr_weapons/exstats_weapons.ini");
	}
	// A trailing slash is not doubled.
	const std::vector<std::string> withSlash = ConfigCandidates("/cfg/");
	CHECK_EQ_INT(withSlash.size(), 2);
	if (withSlash.size() == 2) {
		CHECK_EQ_STR(withSlash[0], "/cfg/levels_ranks/exstats_weapons.ini");
		CHECK_EQ_STR(withSlash[1], "/cfg/lr_weapons/exstats_weapons.ini");
	}
	// No configs directory (the host did not supply plg::GetConfigsDir()): nothing to try.
	CHECK(ConfigCandidates("").empty());
	CHECK_EQ_STR(JoinPath("", "a.ini"), "a.ini");
	CHECK_EQ_STR(JoinPath("/cfg", ""), "/cfg");
	CHECK(!FileExists(""));
	CHECK(!FileExists("/definitely/not/here.ini"));
}

// --- the configuration -----------------------------------------------------------------------------

void TestRealConfig() {
	Section("real exstats_weapons.ini");
	Config cfg;
	const ConfigSource source = LoadConfig(LR_WEAPONS_TEST_DATA_DIR, cfg);
	CHECK(source.loaded);
	CHECK(source.error.empty());
	CHECK(source.problem.empty());
	// tests/data also holds a lr_weapons/exstats_weapons.ini copy: the levels_ranks path must win.
	CHECK(EndsWith(source.path, "levels_ranks/exstats_weapons.ini"));
	CHECK_EQ_INT(cfg.weapons.size(), 38); // the shipped file's weapon blocks
	CHECK(cfg.coefficient_active);

	// The scalar "weapon_coefficient" key is not a weapon block.
	CHECK_EQ_INT(cfg.IndexOf("weapon_coefficient"), -1);
	CHECK_EQ_INT(cfg.IndexOf("weapon_ak47"), 28);
	CHECK_EQ_STR(cfg.weapons[cfg.IndexOf("weapon_ak47")].name, "AK-47");
	CHECK_EQ_INT(cfg.weapons[cfg.IndexOf("weapon_ak47")].coefficient * 10.0f, 9); // 0.9
	CHECK(cfg.weapons[cfg.IndexOf("weapon_ak47")].show_top);
	CHECK_EQ_STR(cfg.weapons[0].classname, "weapon_knife");
	CHECK_EQ_STR(cfg.weapons[0].name, "Knife");
	CHECK_EQ_INT(cfg.weapons[0].coefficient * 10.0f, 50); // 5.0 (not the 0.1 of the plugin copy)
	CHECK(!cfg.weapons[0].show_top);
	CHECK_EQ_INT(cfg.weapons[cfg.IndexOf("weapon_awp")].coefficient * 10.0f, 6); // 0.6
	CHECK(cfg.weapons[cfg.IndexOf("weapon_awp")].show_top);
	CHECK_EQ_STR(cfg.weapons[cfg.IndexOf("weapon_hegrenade")].name, "Hegrenade");

	// CRLF line endings do not leak into the keys or the values.
	for (const WeaponConfig& weapon : cfg.weapons) {
		CHECK(weapon.classname.find('\r') == std::string::npos);
		CHECK(weapon.name.find('\r') == std::string::npos);
	}
	// The file order is kept (the original indexes its arrays by it) and the classnames are unique.
	for (std::size_t i = 1; i < cfg.weapons.size(); ++i) {
		CHECK(cfg.IndexOf(cfg.weapons[i].classname) == static_cast<int>(i));
	}
}

void TestConfigFallback() {
	Section("config fallback");
	// tests/data/fallback only holds lr_weapons/exstats_weapons.ini, and that file starts with a
	// UTF-8 BOM: the second candidate and the BOM skip are exercised together.
	Config cfg;
	const std::string dir = std::string(LR_WEAPONS_TEST_DATA_DIR) + "/fallback";
	const ConfigSource source = LoadConfig(dir, cfg);
	CHECK(source.loaded);
	CHECK(source.error.empty());
	CHECK(source.problem.empty());
	CHECK(EndsWith(source.path, "lr_weapons/exstats_weapons.ini"));
	CHECK(!cfg.coefficient_active); // "weapon_coefficient" "0"
	CHECK_EQ_INT(cfg.weapons.size(), 2);
	CHECK_EQ_STR(cfg.weapons[0].classname, "weapon_awp");
	CHECK_EQ_INT(cfg.weapons[0].coefficient * 10.0f, 6);
	CHECK(cfg.weapons[0].show_top);
	CHECK_EQ_INT(cfg.weapons[1].coefficient * 10.0f, 9);
}

void TestConfigBroken() {
	Section("broken primary config");
	// tests/data/broken/levels_ranks holds a file with a missing '}' and
	// tests/data/broken/lr_weapons a perfectly valid copy: the malformed primary must be reported,
	// never masked by the fallback.
	Config cfg;
	WeaponConfig sentinel;
	sentinel.classname = "weapon_sentinel";
	cfg.weapons.push_back(sentinel);
	const std::string dir = std::string(LR_WEAPONS_TEST_DATA_DIR) + "/broken";
	const ConfigSource source = LoadConfig(dir, cfg);
	CHECK(!source.loaded);
	CHECK(source.error.find("not valid KeyValues") != std::string::npos);
	CHECK(source.error.find("levels_ranks/exstats_weapons.ini") != std::string::npos);
	CHECK(EndsWith(source.path, "levels_ranks/exstats_weapons.ini"));
	// Nothing was parsed, so the caller keeps its previous state.
	CHECK_EQ_INT(cfg.weapons.size(), 1);
	CHECK_EQ_STR(cfg.weapons[0].classname, "weapon_sentinel");
}

void TestConfigMissing() {
	Section("missing config");
	{
		Config cfg;
		const std::string dir = std::string(LR_WEAPONS_TEST_DATA_DIR) + "/does-not-exist";
		const ConfigSource source = LoadConfig(dir, cfg);
		CHECK(!source.loaded);
		CHECK(source.path.empty());
		// The message names both candidates, so the log tells the admin where the file belongs.
		CHECK(source.error.find("levels_ranks/exstats_weapons.ini") != std::string::npos);
		CHECK(source.error.find("lr_weapons/exstats_weapons.ini") != std::string::npos);
		CHECK(cfg.weapons.empty());
	}
	{
		Config cfg;
		const ConfigSource source = LoadConfig({}, cfg);
		CHECK(!source.loaded);
		CHECK(source.error.find("configs directory") != std::string::npos);
	}
}

void TestConfigVariants() {
	Section("config variants");
	// "weapon_coefficient" "0" disables the per-weapon coefficient.
	{
		const std::optional<KvNode> root =
			ParseKv1("\"LR_ExStatsWeapons\"\n{\n"
					 "    \"weapon_coefficient\" \"0\"\n"
					 "    \"weapon_ak47\" { \"name\" \"AK-47\" \"coefficient\" \"2.0\" }\n"
					 "}\n");
		CHECK(root.has_value());
		if (root) {
			Config cfg;
			ParseConfig(*root, cfg);
			CHECK(!cfg.coefficient_active);
			CHECK_EQ_INT(cfg.weapons.size(), 1);
			CHECK_EQ_INT(cfg.weapons[0].coefficient * 10.0f, 20);
		}
	}
	// A missing/empty/unparsable coefficient keeps the default 1.0 of GetFloat("coefficient", 1.0).
	for (const char* block : {"\"name\" \"MP9\"", "\"coefficient\" \"\"", "\"coefficient\" \"abc\""}) {
		const std::optional<KvNode> root =
			ParseKv1(std::string("\"LR_ExStatsWeapons\"\n{\n    \"weapon_mp9\" { ") + block + " }\n}\n");
		CHECK(root.has_value());
		if (root) {
			Config cfg;
			ParseConfig(*root, cfg);
			CHECK_EQ_INT(cfg.weapons.size(), 1);
			CHECK_EQ_INT(cfg.weapons[0].coefficient * 10.0f, 10);
			CHECK(!cfg.weapons[0].show_top);
		}
	}
	// A block without children is not a "true subkey": FOR_EACH_SUBKEY skipped it in the original.
	{
		const std::optional<KvNode> root =
			ParseKv1("\"LR_ExStatsWeapons\" { \"weapon_empty\" { } \"weapon_mp9\" { \"name\" \"MP9\" } }");
		CHECK(root.has_value());
		if (root) {
			Config cfg;
			ParseConfig(*root, cfg);
			CHECK_EQ_INT(cfg.weapons.size(), 1);
			CHECK_EQ_INT(cfg.IndexOf("weapon_empty"), -1);
			CHECK_EQ_STR(cfg.weapons[0].classname, "weapon_mp9");
		}
	}
	// A block with an empty name cannot be looked up and is dropped.
	{
		const std::optional<KvNode> root =
			ParseKv1("\"LR_ExStatsWeapons\" { \"\" { \"name\" \"X\" } \"weapon_mp9\" { \"name\" \"MP9\" } }");
		CHECK(root.has_value());
		if (root) {
			Config cfg;
			ParseConfig(*root, cfg);
			CHECK_EQ_INT(cfg.weapons.size(), 1);
			CHECK_EQ_STR(cfg.weapons[0].classname, "weapon_mp9");
		}
	}
	// More than kMaxWeapons blocks are dropped (the original wrote past its arrays here) and the
	// fact is reported through `problem`.
	{
		std::string text = "\"LR_ExStatsWeapons\"\n{\n";
		for (int i = 0; i < kMaxWeapons + 5; ++i) {
			text += "    \"weapon_test" + std::to_string(i) + "\" { \"name\" \"T\" }\n";
		}
		text += "}\n";
		const std::optional<KvNode> root = ParseKv1(text);
		CHECK(root.has_value());
		if (root) {
			Config cfg;
			std::string problem;
			ParseConfig(*root, cfg, &problem);
			CHECK_EQ_INT(cfg.weapons.size(), kMaxWeapons);
			CHECK(!problem.empty());
			CHECK(problem.find(std::to_string(kMaxWeapons)) != std::string::npos);
		}
	}
	// An empty root leaves the defaults: coefficient on, no weapons (nothing is counted). A reload
	// drops the previous weapon list instead of appending to it.
	{
		const std::optional<KvNode> root = ParseKv1("\"LR_ExStatsWeapons\" { }");
		CHECK(root.has_value());
		Config cfg;
		WeaponConfig previous;
		previous.classname = "weapon_previous";
		cfg.weapons.push_back(previous);
		if (root) {
			ParseConfig(*root, cfg);
			CHECK(cfg.coefficient_active);
			CHECK(cfg.weapons.empty());
		}
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
	CHECK_EQ_STR(ConvertSteamId("[U:1:123]"), "STEAM_1:1:61");
	CHECK_EQ_STR(ConvertSteamId("[U:1:124]"), "STEAM_1:0:62");
	CHECK_EQ_STR(ConvertSteamId("STEAM_1:0:62"), "STEAM_1:0:62"); // s2sdk's GetClientAuthId form
	CHECK_EQ_STR(ConvertSteamId("  STEAM_1:1:77  "), "STEAM_1:1:77");
	CHECK_EQ_STR(ConvertSteamId(""), "");
	CHECK_EQ_STR(ConvertSteamId("BOT"), "BOT");            // unusable input is kept verbatim, never crashes
	CHECK_EQ_STR(ConvertSteamId("[U:1:abc]"), "U:1:abc");  // no account id: stripped but unconverted
	// The table name of the original: g_sTableName + "_weapons".
	CHECK_EQ_STR(TableName("lvl_base"), "lvl_base_weapons");
	CHECK_EQ_STR(TableName(""), "_weapons");
}

void TestSqlText() {
	Section("SQL text");
	// CREATE TABLE: the original's MySQL statement byte for byte, and the SQLite equivalent (no
	// CHARSET/COLLATE clauses, same columns and primary key).
	const std::string mysql = CreateTableSql("lvl_base_weapons", plgdb::Driver::MySQL);
	CHECK(mysql.rfind("CREATE TABLE IF NOT EXISTS `lvl_base_weapons`", 0) == 0);
	CHECK(mysql.find("CHARSET = utf8 COLLATE utf8_general_ci") != std::string::npos);
	CHECK(mysql.find("PRIMARY KEY (`steam`, `classname`)") != std::string::npos);
	const std::string sqlite = CreateTableSql("lvl_base_weapons", plgdb::Driver::SQLite);
	CHECK(sqlite.rfind("CREATE TABLE IF NOT EXISTS `lvl_base_weapons`", 0) == 0);
	CHECK(sqlite.find("CHARSET") == std::string::npos);
	CHECK(sqlite.find("PRIMARY KEY (`steam`, `classname`)") != std::string::npos);
	// OnDatabaseCleanupHook: TRUNCATE on MySQL, DELETE on SQLite.
	CHECK_EQ_STR(TruncateSql("lvl_base_weapons", plgdb::Driver::MySQL), "TRUNCATE TABLE `lvl_base_weapons`;");
	CHECK_EQ_STR(TruncateSql("lvl_base_weapons", plgdb::Driver::SQLite), "DELETE FROM `lvl_base_weapons`;");
	// The value is always a bound parameter, never interpolated.
	Statement load = LoadPlayerStatement("lvl_base_weapons", "STEAM_1:1:5");
	CHECK_EQ_STR(load.sql, "SELECT `classname`, `kills` FROM `lvl_base_weapons` WHERE `steam` = ?;");
	CHECK_EQ_INT(load.params.size(), 1);
	if (load.params.size() == 1) {
		CHECK_EQ_STR(std::get<std::string>(load.params[0]), "STEAM_1:1:5");
	}
	Statement reset = ResetPlayerStatement("lvl_base_weapons", "STEAM_1:1:5");
	CHECK_EQ_STR(reset.sql, "UPDATE `lvl_base_weapons` SET `kills` = 0 WHERE `steam` = ?;");
	// A save of an index outside the config is empty (the caller's bug guard).
	Config cfg;
	SlotState slot;
	CHECK(SaveWeaponStatement("t", cfg, slot, "s", 0).sql.empty());
}

void TestApplyLoadedError() {
	Section("ApplyLoadedWeapons (a failed query is not 'no rows')");
	Config cfg;
	CHECK(LoadConfig(LR_WEAPONS_TEST_DATA_DIR, cfg).loaded);

	State state{};
	SlotState& slot = state[3];
	slot.kills[0] = 5;
	slot.is_new[0] = false;

	plgdb::Result failed;
	failed.error = "plgdb: connect failed";
	ApplyLoadedWeapons(slot, cfg, failed);
	CHECK_EQ_INT(slot.kills[0], 5); // the counters of the session survive; the plugin retries
	CHECK(!slot.is_new[0]);

	// A successful (empty) result is a player without rows: every weapon is is_new with 0 kills.
	plgdb::Result missing;
	missing.columns = {"classname", "kills"};
	ApplyLoadedWeapons(slot, cfg, missing);
	CHECK_EQ_INT(slot.kills[0], 0);
	CHECK(slot.is_new[0]);

	// A result with the rows of another player: only the listed classnames get their kills, an
	// unknown classname is ignored and the rest stays is_new.
	plgdb::Result rows;
	rows.columns = {"classname", "kills"};
	rows.rows = {{std::string("weapon_ak47"), int64_t{7}}, {std::string("weapon_unknown"), int64_t{3}}};
	ApplyLoadedWeapons(slot, cfg, rows);
	const int ak = cfg.IndexOf("weapon_ak47");
	const int awp = cfg.IndexOf("weapon_awp");
	CHECK(ak >= 0);
	CHECK(awp >= 0);
	if (ak >= 0 && awp >= 0) {
		CHECK(!slot.is_new[static_cast<std::size_t>(ak)]);
		CHECK_EQ_INT(slot.kills[static_cast<std::size_t>(ak)], 7);
		CHECK(slot.is_new[static_cast<std::size_t>(awp)]);
		CHECK_EQ_INT(slot.kills[static_cast<std::size_t>(awp)], 0);
	}

	// A result whose columns are not the expected ones (a broken query) leaves the zeros above.
	plgdb::Result odd;
	odd.columns = {"something"};
	odd.rows = {{std::string("x")}};
	ApplyLoadedWeapons(slot, cfg, odd);
	CHECK(slot.is_new[static_cast<std::size_t>(ak)]);
	CHECK_EQ_INT(slot.kills[static_cast<std::size_t>(ak)], 0);
}

// --- SQL -----------------------------------------------------------------------------------------

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

	// The config the module runs with: the shipped one.
	Config cfg;
	const ConfigSource source = LoadConfig(LR_WEAPONS_TEST_DATA_DIR, cfg);
	CHECK(source.loaded);
	CHECK_EQ_INT(cfg.weapons.size(), 38);

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

	TestKvReader();
	TestConfigCandidates();
	TestRealConfig();
	TestConfigFallback();
	TestConfigBroken();
	TestConfigMissing();
	TestConfigVariants();
	TestHelpers();
	TestSqlText();
	TestApplyLoadedError();

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
