#include "weapons_core.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

namespace lr_weapons {
namespace {

std::string_view TrimSpaces(std::string_view text) {
	const auto begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string_view::npos) {
		return {};
	}
	const auto end = text.find_last_not_of(" \t\r\n");
	return text.substr(begin, end - begin + 1);
}

} // namespace

int Config::IndexOf(std::string_view classname) const {
	for (std::size_t i = 0; i < weapons.size(); ++i) {
		if (weapons[i].classname == classname) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

void ParseConfig(const pisex::KeyValues& kv, Config& out, std::string* problem) {
	out.weapons.clear();
	// LoadSettings() of the original: the weapon_coefficient key of the config, default 1.
	out.coefficient_active = kv.GetInt("weapon_coefficient", 1) != 0;

	for (const pisex::KeyValues* key = kv.GetFirstTrueSubKey(); key != nullptr;
		 key = key->GetNextTrueSubKey()) {
		// The original's FOR_EACH_SUBKEY skipped scalar children, GetFirstTrueSubKey does the same.
		if (out.weapons.size() >= static_cast<std::size_t>(kMaxWeapons)) {
			if (problem != nullptr) {
				*problem = "more than " + std::to_string(kMaxWeapons) +
						   " weapon blocks; the surplus is ignored";
			}
			break;
		}
		WeaponConfig weapon;
		weapon.classname = key->GetName() != nullptr ? key->GetName() : "";
		weapon.name = std::string(key->GetString("name", ""));
		weapon.coefficient = key->GetFloat("coefficient", 1.0f);
		weapon.show_top = key->GetInt("showtop", 0) != 0;
		if (!weapon.classname.empty()) {
			out.weapons.push_back(std::move(weapon));
		}
	}
}

std::string NormalizeClassname(const char* weapon) {
	const std::string name(weapon != nullptr ? weapon : "");
	// The original formatted "weapon_%s" and handled the knife branch first.
	if (!name.empty() && (name[0] == 'k' || name == "bayonet")) {
		return "weapon_knife";
	}
	return "weapon_" + name;
}

int RoundToNearest(float value) {
	return static_cast<int>(std::round(value));
}

std::string ConvertSteamId(std::string_view network_id) {
	const std::string trimmed(TrimSpaces(network_id));
	if (trimmed.empty()) {
		return {};
	}
	if (trimmed.rfind("STEAM_", 0) == 0) {
		return trimmed; // s2sdk's GetClientAuthId hands out STEAM_ ids already
	}
	// "[U:1:accountid]" and friends: strip the brackets, split on ':', rebuild STEAM_1.
	std::string text = trimmed;
	text.erase(std::remove(text.begin(), text.end(), '['), text.end());
	text.erase(std::remove(text.begin(), text.end(), ']'), text.end());

	std::vector<std::string> parts;
	std::size_t start = 0;
	while (start <= text.size()) {
		const std::size_t colon = text.find(':', start);
		if (colon == std::string::npos) {
			parts.push_back(text.substr(start));
			break;
		}
		parts.push_back(text.substr(start, colon - start));
		start = colon + 1;
	}
	if (parts.size() < 3) {
		return text; // the original crashed on such input (usteamid_split[2]); the port keeps it verbatim
	}
	// 64 bit: the original's std::stoi threw for an account id above INT_MAX (a valid, if rare,
	// Steam account id), which would have taken the server down.
	int64_t account = 0;
	const std::string_view account_text = TrimSpaces(parts[2]);
	const auto result =
		std::from_chars(account_text.data(), account_text.data() + account_text.size(), account);
	if (result.ec != std::errc{}) {
		return text;
	}
	return "STEAM_1:" + std::string(account % 2 == 0 ? "0:" : "1:") + std::to_string(account / 2);
}

std::string TableName(std::string_view base) {
	return std::string(base) + "_weapons";
}

std::string CreateTableSql(std::string_view table, plgdb::Driver driver) {
	const std::string name(table);
	if (driver == plgdb::Driver::MySQL) {
		// The CREATE TABLE of the original, byte for byte (the table lands in the same database as
		// the LR core's).
		return "CREATE TABLE IF NOT EXISTS `" + name + "` ("
			   "`steam` varchar(32) NOT NULL default '', "
			   "`classname` varchar(64) NOT NULL default '', "
			   "`kills` int NOT NULL default 0, "
			   "PRIMARY KEY (`steam`, `classname`)"
			   ") CHARSET = utf8 COLLATE utf8_general_ci;";
	}
	// SQLite has no CHARSET/COLLATE clauses; the columns and the primary key are the same.
	return "CREATE TABLE IF NOT EXISTS `" + name + "` ("
		   "`steam` TEXT NOT NULL DEFAULT '', "
		   "`classname` TEXT NOT NULL DEFAULT '', "
		   "`kills` INTEGER NOT NULL DEFAULT 0, "
		   "PRIMARY KEY (`steam`, `classname`)"
		   ");";
}

Statement LoadPlayerStatement(std::string_view table, std::string_view steamid) {
	Statement statement;
	statement.sql = "SELECT `classname`, `kills` FROM `" + std::string(table) +
					"` WHERE `steam` = ?;";
	statement.params.emplace_back(std::string(steamid));
	return statement;
}

void ApplyLoadedWeapons(SlotState& state, const Config& cfg, const plgdb::Result& result) {
	const std::optional<std::size_t> class_column = result.ColumnIndex("classname");
	const std::optional<std::size_t> kills_column = result.ColumnIndex("kills");
	for (std::size_t i = 0; i < cfg.weapons.size(); ++i) {
		state.is_new[i] = true;
		state.kills[i] = 0;
	}
	if (!result.Ok() || !class_column.has_value() || !kills_column.has_value()) {
		return; // an error or an unexpected shape leaves the zeros above (the original's empty result)
	}
	for (std::size_t row = 0; row < result.RowCount(); ++row) {
		const int index = cfg.IndexOf(result.GetString(row, *class_column));
		if (index >= 0) {
			state.is_new[static_cast<std::size_t>(index)] = false;
			state.kills[static_cast<std::size_t>(index)] =
				static_cast<int>(result.GetInt(row, *kills_column, 0));
		}
	}
}

Statement SaveWeaponStatement(std::string_view table, const Config& cfg, SlotState& state,
							  std::string_view steamid, int weapon) {
	Statement statement;
	if (weapon < 0 || weapon >= static_cast<int>(cfg.weapons.size())) {
		return statement; // caller bug: nothing to save
	}
	const std::size_t index = static_cast<std::size_t>(weapon);
	const std::string& classname = cfg.weapons[index].classname;
	if (state.is_new[index]) {
		// INSERT INTO `<t>_weapons` (`steam`, `classname`, `kills`) VALUES (?, ?, ?);
		statement.sql = "INSERT INTO `" + std::string(table) +
						"` (`steam`, `classname`, `kills`) VALUES (?, ?, ?);";
		statement.params.emplace_back(std::string(steamid));
		statement.params.emplace_back(classname);
		statement.params.emplace_back(static_cast<int64_t>(state.kills[index]));
		state.is_new[index] = false; // the original cleared the flag when it built the INSERT
	} else {
		statement.sql = "UPDATE `" + std::string(table) +
						"` SET `kills` = ? WHERE `steam` = ? AND `classname` = ?;";
		statement.params.emplace_back(static_cast<int64_t>(state.kills[index]));
		statement.params.emplace_back(std::string(steamid));
		statement.params.emplace_back(classname);
	}
	return statement;
}

Statement ResetPlayerStatement(std::string_view table, std::string_view steamid) {
	Statement statement;
	statement.sql = "UPDATE `" + std::string(table) + "` SET `kills` = 0 WHERE `steam` = ?;";
	statement.params.emplace_back(std::string(steamid));
	return statement;
}

void ZeroSlot(SlotState& state, const Config& cfg) {
	for (std::size_t i = 0; i < cfg.weapons.size(); ++i) {
		state.kills[i] = 0;
	}
}

std::string TruncateSql(std::string_view table, plgdb::Driver driver) {
	// SQLite has no TRUNCATE; the effect (an empty table) is the same.
	return (driver == plgdb::Driver::MySQL ? "TRUNCATE TABLE `" : "DELETE FROM `") +
		   std::string(table) + "`;";
}

} // namespace lr_weapons
