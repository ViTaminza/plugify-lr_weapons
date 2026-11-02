#include "weapons_core.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace lr_weapons {
namespace {

char Lower(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (Lower(a[i]) != Lower(b[i])) {
			return false;
		}
	}
	return true;
}

std::string_view TrimSpaces(std::string_view text) {
	const auto begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string_view::npos) {
		return {};
	}
	const auto end = text.find_last_not_of(" \t\r\n");
	return text.substr(begin, end - begin + 1);
}

// --- the KV1 tokenizer and grammar --------------------------------------------------------------
//
//   file  := node* EOF
//   node  := token [ token ] [ '{' node* '}' ]
//   token := '"' escaped '"' | raw
//
// The grammar is the one the original's KeyValues (and pisexcompat's re-implementation) accepted:
// quoted and unquoted tokens, // and /* */ comments anywhere between tokens, \" \\ \n \r \t escapes
// inside quotes (an unknown escape is kept verbatim), and a key that carries both a value and a
// block. A malformed file is rejected as a whole: an unterminated string, an unterminated block
// comment, a missing '}' or a stray top-level '}' set `m_failed` and ParseKv1 returns nullopt -
// never a half-read tree that would silently override the loader's defaults.
class Parser {
public:
	explicit Parser(std::string_view text) : m_text(text) {}

	bool Parse(KvNode& root) {
		ParseNodes(root, /*insideBlock=*/false);
		return !m_failed;
	}

private:
	// Whitespace, // line comments and /* */ block comments between tokens.
	void SkipSpace() {
		while (m_pos < m_text.size() && !m_failed) {
			const char c = m_text[m_pos];
			if (std::isspace(static_cast<unsigned char>(c))) {
				++m_pos;
				continue;
			}
			if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/') {
				m_pos += 2;
				while (m_pos < m_text.size() && m_text[m_pos] != '\n') {
					++m_pos;
				}
				continue;
			}
			if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '*') {
				const std::size_t end = m_text.find("*/", m_pos + 2);
				if (end == std::string_view::npos) {
					m_failed = true; // unterminated block comment
					m_pos = m_text.size();
					return;
				}
				m_pos = end + 2;
				continue;
			}
			break;
		}
	}

	// One token; false at the end of the input or at a '}' (nothing was read then).
	bool ReadToken(std::string& out) {
		SkipSpace();
		if (m_failed || m_pos >= m_text.size() || m_text[m_pos] == '}') {
			return false;
		}
		out.clear();
		if (m_text[m_pos] == '"') {
			++m_pos;
			while (m_pos < m_text.size()) {
				const char c = m_text[m_pos++];
				if (c == '\\' && m_pos < m_text.size()) {
					const char esc = m_text[m_pos++];
					switch (esc) {
					case 'n': out.push_back('\n'); break;
					case 'r': out.push_back('\r'); break;
					case 't': out.push_back('\t'); break;
					case '"': out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					default:
						// Unknown escapes are kept verbatim, like the original's parser.
						out.push_back('\\');
						out.push_back(esc);
						break;
					}
					continue;
				}
				if (c == '"') {
					return true;
				}
				out.push_back(c);
			}
			m_failed = true; // unterminated string
			return true;     // the caller stops on m_failed
		}
		while (m_pos < m_text.size()) {
			const char c = m_text[m_pos];
			if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '"') {
				break;
			}
			// A comment starts a new token boundary: "value/*c*/" reads as "value".
			if (c == '/' && m_pos + 1 < m_text.size() &&
				(m_text[m_pos + 1] == '/' || m_text[m_pos + 1] == '*')) {
				break;
			}
			out.push_back(c);
			++m_pos;
		}
		return true;
	}

	bool PeekIs(char c) {
		SkipSpace();
		return !m_failed && m_pos < m_text.size() && m_text[m_pos] == c;
	}

	void ParseNodes(KvNode& parent, bool insideBlock) {
		while (!m_failed) {
			SkipSpace();
			if (m_failed) {
				return;
			}
			if (m_pos >= m_text.size()) {
				if (insideBlock) {
					m_failed = true; // missing '}'
				}
				return;
			}
			if (m_text[m_pos] == '}') {
				if (!insideBlock) {
					m_failed = true; // stray top-level '}'
				}
				++m_pos;
				return;
			}
			std::string key;
			if (!ReadToken(key)) {
				return; // end of input or a '}' handled above (ReadToken may have consumed nothing)
			}
			if (m_failed) {
				return;
			}
			if (!key.empty() && key[0] == '#') {
				// #base / #include / #define: no inclusion, skip the line.
				while (m_pos < m_text.size() && m_text[m_pos] != '\n') {
					++m_pos;
				}
				continue;
			}
			parent.children.push_back(KvNode{});
			// Filled through an index: the recursion below may reallocate parent.children.
			const std::size_t index = parent.children.size() - 1;
			parent.children[index].name = std::move(key);
			if (!PeekIs('{')) {
				if (m_failed) {
					return;
				}
				std::string value;
				if (ReadToken(value)) {
					if (m_failed) {
						return;
					}
					parent.children[index].value = std::move(value);
					parent.children[index].hasValue = true;
				}
			}
			if (PeekIs('{')) {
				if (m_failed) {
					return;
				}
				++m_pos;
				KvNode child = std::move(parent.children[index]);
				ParseNodes(child, /*insideBlock=*/true);
				parent.children[index] = std::move(child);
				if (m_failed) {
					return;
				}
			}
		}
	}

	std::string_view m_text;
	std::size_t m_pos = 0;
	bool m_failed = false;
};

// A UTF-8 BOM is not part of the first key of the file (the production files are CRLF, but a BOM
// is the one byte order mark a KV1 file may carry).
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

} // namespace

// --- the KV1 reader -----------------------------------------------------------------------------

const KvNode* KvNode::Find(std::string_view key) const {
	if (key.empty()) {
		return nullptr;
	}
	for (const KvNode& child : children) {
		if (EqualNoCase(child.name, key)) {
			return &child;
		}
	}
	return nullptr;
}

std::string_view KvNode::GetString(std::string_view key, std::string_view def) const {
	const KvNode* found = Find(key);
	if (found == nullptr || !found->hasValue) {
		return def;
	}
	return found->value;
}

int KvNode::GetInt(std::string_view key, int def) const {
	const std::string_view text = TrimSpaces(GetString(key));
	if (text.empty()) {
		return def;
	}
	int number = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
	if (result.ec != std::errc{}) {
		// Valve falls back to atoi semantics ("5 // comment", "1.5", "abc" -> 0/partial).
		return std::atoi(std::string(text).c_str());
	}
	return number;
}

float KvNode::GetFloat(std::string_view key, float def) const {
	const std::string_view text = TrimSpaces(GetString(key));
	if (text.empty()) {
		return def;
	}
	const std::string copy(text);
	char* end = nullptr;
	const float value = std::strtof(copy.c_str(), &end);
	if (end == copy.c_str()) {
		return def;
	}
	return value;
}

std::optional<KvNode> ParseKv1(std::string_view text) {
	if (text.size() >= kUtf8Bom.size() && text.compare(0, kUtf8Bom.size(), kUtf8Bom) == 0) {
		text.remove_prefix(kUtf8Bom.size());
	}
	KvNode root;
	Parser parser(text);
	if (!parser.Parse(root)) {
		return std::nullopt;
	}
	// A single top-level block is the file's root ("LR_ExStatsWeapons" { ... }), as in Valve's
	// loader: `ParseKv1(...)->GetInt("weapon_coefficient")` reads the key *inside* the block.
	if (root.children.size() == 1 && !root.children.front().children.empty()) {
		return std::move(root.children.front());
	}
	return root;
}

std::optional<KvNode> LoadKv1File(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	std::ostringstream content;
	content << file.rdbuf();
	return ParseKv1(content.str());
}

// --- where the file is read from -----------------------------------------------------------------

std::string JoinPath(std::string_view base, std::string_view name) {
	if (base.empty()) {
		return std::string(name);
	}
	if (name.empty()) {
		return std::string(base);
	}
	std::string out(base);
	if (out.back() != '/') {
		out.push_back('/');
	}
	out.append(name);
	return out;
}

bool FileExists(const std::string& path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(path, std::ios::binary);
	return file.good();
}

std::vector<std::string> ConfigCandidates(std::string_view configsDir) {
	std::vector<std::string> candidates;
	if (configsDir.empty()) {
		return candidates;
	}
	candidates.reserve(2);
	candidates.push_back(JoinPath(JoinPath(configsDir, kLevelsRanksDir), kConfigFile));
	candidates.push_back(JoinPath(JoinPath(configsDir, kPluginDir), kConfigFile));
	return candidates;
}

// --- the configuration ---------------------------------------------------------------------------

int Config::IndexOf(std::string_view classname) const {
	for (std::size_t i = 0; i < weapons.size(); ++i) {
		if (weapons[i].classname == classname) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

void ParseConfig(const KvNode& kv, Config& out, std::string* problem) {
	out.weapons.clear();
	// LoadSettings() of the original: the weapon_coefficient key of the config, default 1 (a
	// missing key and an empty value both keep the default, like KeyValues::GetInt(key, 1)).
	out.coefficient_active = kv.GetInt("weapon_coefficient", 1) != 0;

	for (const KvNode& key : kv.children) {
		// The original's FOR_EACH_SUBKEY walked the *blocks* only (GetFirstTrueSubKey), so the
		// scalar "weapon_coefficient" key and any other scalar are skipped.
		if (key.children.empty()) {
			continue;
		}
		if (out.weapons.size() >= static_cast<std::size_t>(kMaxWeapons)) {
			if (problem != nullptr) {
				*problem = "more than " + std::to_string(kMaxWeapons) +
						   " weapon blocks; the surplus is ignored";
			}
			break;
		}
		WeaponConfig weapon;
		weapon.classname = key.name;
		weapon.name = std::string(key.GetString("name", ""));
		weapon.coefficient = key.GetFloat("coefficient", 1.0f);
		weapon.show_top = key.GetInt("showtop", 0) != 0;
		if (!weapon.classname.empty()) {
			out.weapons.push_back(std::move(weapon));
		}
	}
}

ConfigSource LoadConfig(std::string_view configsDir, Config& out) {
	ConfigSource source;
	const std::vector<std::string> candidates = ConfigCandidates(configsDir);
	if (candidates.empty()) {
		source.error = "the Plugify configs directory is unavailable";
		return source;
	}
	for (const std::string& path : candidates) {
		if (!FileExists(path)) {
			continue;
		}
		source.path = path;
		const std::optional<KvNode> root = LoadKv1File(path);
		if (!root) {
			// The first existing file is authoritative: a malformed primary config is reported,
			// never masked by the plugin-directory copy.
			source.error = "failed to load " + path + " (not valid KeyValues)";
			return source;
		}
		ParseConfig(*root, out, &source.problem);
		source.loaded = true;
		return source;
	}
	std::string tried;
	for (const std::string& path : candidates) {
		if (!tried.empty()) {
			tried += " or ";
		}
		tried += path;
	}
	source.error = "failed to load " + tried;
	return source;
}

// --- the kill-handler helpers ---------------------------------------------------------------------

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

// --- SQL -----------------------------------------------------------------------------------------

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
	// An error is not "no rows": the slot keeps its counters and the plugin retries the load.
	// Zeroing here would let the next save overwrite the real kills with the ones counted since.
	if (!result.Ok()) {
		return;
	}
	const std::optional<std::size_t> class_column = result.ColumnIndex("classname");
	const std::optional<std::size_t> kills_column = result.ColumnIndex("kills");
	for (std::size_t i = 0; i < cfg.weapons.size(); ++i) {
		state.is_new[i] = true;
		state.kills[i] = 0;
	}
	if (!class_column.has_value() || !kills_column.has_value()) {
		return; // an unexpected shape leaves the zeros above (the original's empty result)
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
