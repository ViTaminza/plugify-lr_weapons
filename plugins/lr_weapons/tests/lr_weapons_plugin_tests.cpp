// lr_weapons_plugin_tests — the raw-game-event test of the pure-s2sdk lr_weapons plugin.
//
// The plugin TU is compiled into this test (this file includes ../src/plugin.cpp, the
// vip_af_tests pattern), so the real kill-hook path runs end to end: the levels_ranks trampoline of
// lr_api.hpp hands over the raw s2sdk event handle as int64, and the fakes below answer the event
// field exports of the fake event object.
//
// Why this test exists (the bug class it pins):
//   player_death.attacker is a "player_controller_and_pawn" field (core.gameevents). s2sdk's
//   GetEventInt calls IGameEvent::GetInt, GetEventPlayerSlot calls IGameEvent::GetPlayerSlot - two
//   different engine APIs. GetInt answers the raw event value (the legacy user id), which is NOT a
//   slot. The fake event below therefore answers a deliberately different value for GetEventInt,
//   and every case asserts that GetEventInt is never called at all.
//
// Covered:
//   1. the attacker slot comes from GetEventPlayerSlot: valid slot 0 (decoy int 12) and slot 3
//      (decoy int 0, slot 0 not even a usable client) still apply the weapon coefficient;
//   2. -1 / world: no coefficient, and the weapon field is not even read, although the (unused) int
//      value names a fully usable client;
//   3. a missing player-slot binding fails soft (no coefficient, no crash, no GetEventInt fallback);
//   4. the guard order of PITFALLS B1/B2/A1: null event, no map yet, CheckCountPlayers() false, the
//      OnMapEnd window (and an OnPluginUpdate inside it), then the next OnMapStart re-opens the gate;
//   5. PITFALLS C2: the attacker must be a live, human client with a controller, a pawn and a
//      readable SteamID64 (SourceTV / fake client / no pawn / no controller / SteamID64 0 /
//      not connected / not in game all drop the kill);
//   6. the weapon side: "knife" maps to weapon_knife, an empty or unknown classname drops the kill;
//   7. PITFALLS A7: OnPluginEnd unhooks the LR kill callback, the disconnect listener and the map
//      listeners and closes the gate.
//
// No engine, no pisexcompat. Run by ./test.sh (build/tests/lr_weapons_plugin_tests).

#include "../src/plugin.cpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, std::string_view what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
	}
}

void CheckEqInt(long long actual, long long expected, std::string_view what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %.*s (got %lld, expected %lld)\n", static_cast<int>(what.size()),
					what.data(), actual, expected);
	}
}

std::string ToStd(const plg::string& text) {
	return std::string(std::string_view(text));
}

// --- the scenario ---------------------------------------------------------------------------------

struct ClientState {
	bool connected = false;
	bool in_game = false;
	bool source_tv = false;
	bool fake = false;
	bool controller = false;
	bool pawn = false;
	uint64_t steamid64 = 0;
};

ClientState g_clients[lr_weapons::kMaxSlots];

// The fake raw event. `player_slot` is what GetEventPlayerSlot answers (the truth), `raw_int` is
// what GetEventInt would answer - the plugin must never ask for it.
struct FakeEvent {
	int32_t player_slot = -1;
	int32_t raw_int = 0;
	std::string weapon;
};

FakeEvent g_event;

int g_playerSlotCalls = 0;
int g_eventIntCalls = 0;
int g_weaponCalls = 0;
std::string g_lastPlayerSlotKey;
std::string g_lastEventIntKey;

bool g_countPlayers = true;
int32_t g_settingsValue = 0; // LR_DB_SaveDataPlayer_Mode: 0 = save on disconnect

// A usable client: all PITFALLS C2 layers pass. |slot| out of range stays unusable.
void SetUsable(int slot) {
	for (ClientState& client : g_clients) {
		client = ClientState{};
	}
	if (slot < 0 || slot >= lr_weapons::kMaxSlots) {
		return;
	}
	ClientState& client = g_clients[slot];
	client.connected = true;
	client.in_game = true;
	client.controller = true;
	client.pawn = true;
	client.steamid64 = 76561197960287930ull;
}

// --- s2sdk fakes ----------------------------------------------------------------------------------

void FakeLog(std::string_view message, plg::Severity /*severity*/, const plg::source_location& /*location*/) {
	std::printf("  log: %.*s\n", static_cast<int>(message.size()), message.data());
}

plg::string FakeGetConfigsDir() {
	return plg::string(LR_WEAPONS_TEST_DATA_DIR);
}

int32_t FakeGetEventPlayerSlot(void* event, const plg::string& key) {
	++g_playerSlotCalls;
	g_lastPlayerSlotKey = ToStd(key);
	if (event != &g_event) {
		return -1;
	}
	return ToStd(key) == "attacker" ? g_event.player_slot : -1;
}

// GetEventInt is the wrong API for a player field: the test records every call and fails when one
// happens. It still answers |raw_int| so a regression would produce a wrong slot instead of a crash.
int32_t FakeGetEventInt(void* event, const plg::string& key) {
	++g_eventIntCalls;
	g_lastEventIntKey = ToStd(key);
	if (event != &g_event) {
		return -1;
	}
	return g_event.raw_int;
}

plg::string FakeGetEventString(void* event, const plg::string& key) {
	++g_weaponCalls;
	if (event != &g_event || ToStd(key) != "weapon") {
		return plg::string();
	}
	return plg::string(g_event.weapon);
}

bool InRange(int32_t slot) {
	return slot >= 0 && slot < lr_weapons::kMaxSlots;
}

bool FakeIsClientConnected(int32_t slot) {
	return InRange(slot) && g_clients[slot].connected;
}

bool FakeIsClientInGame(int32_t slot) {
	return InRange(slot) && g_clients[slot].in_game;
}

bool FakeIsClientSourceTV(int32_t slot) {
	return InRange(slot) && g_clients[slot].source_tv;
}

bool FakeIsFakeClient(int32_t slot) {
	return InRange(slot) && g_clients[slot].fake;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	if (!InRange(slot) || !g_clients[slot].controller) {
		return nullptr;
	}
	return reinterpret_cast<void*>(static_cast<intptr_t>(0x1000 + slot));
}

void* FakeGetClientPawn(int32_t slot) {
	if (!InRange(slot) || !g_clients[slot].pawn) {
		return nullptr;
	}
	return reinterpret_cast<void*>(static_cast<intptr_t>(0x2000 + slot));
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	return InRange(slot) ? g_clients[slot].steamid64 : 0;
}

plg::string FakeGetClientAuthId(int32_t slot) {
	(void)slot;
	return plg::string("STEAM_1:1:42");
}

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;
s2sdk::OnClientDisconnectCallback g_onClientDisconnect = nullptr;

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_onMapStart = callback;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_onMapEnd = callback;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
}

void FakeOnClientDisconnectRegister(s2sdk::OnClientDisconnectCallback callback) {
	g_onClientDisconnect = callback;
}

void FakeOnClientDisconnectUnregister(s2sdk::OnClientDisconnectCallback callback) {
	if (g_onClientDisconnect == callback) {
		g_onClientDisconnect = nullptr;
	}
}

// --- levels_ranks fakes ---------------------------------------------------------------------------

levels_ranks::PlayerKilledPreCallback g_killedPre = nullptr;

bool FakeIsCoreLoaded() {
	return true;
}

plg::string FakeGetTableName() {
	return plg::string("lvl_base");
}

// A throwaway in-memory SQLite database, so the plugin's own connection path runs as in production.
plg::vector<plg::string> FakeGetDatabaseConfig() {
	return plg::vector<plg::string>{plg::string("sqlite"), plg::string(":memory:"), plg::string(),
									plg::string(), plg::string(), plg::string()};
}

bool FakeCheckCountPlayers() {
	return g_countPlayers;
}

int32_t FakeGetSettingsValue(int32_t /*setting*/) {
	return g_settingsValue;
}

void FakeOnCoreIsReadyRegister(levels_ranks::CoreReadyCallback callback) {
	(void)callback;
}

void FakeOnCoreIsReadyUnregister(levels_ranks::CoreReadyCallback callback) {
	(void)callback;
}

void FakeOnPlayerLoadedRegister(levels_ranks::PlayerLoadedCallback callback) {
	(void)callback;
}

void FakeOnPlayerLoadedUnregister(levels_ranks::PlayerLoadedCallback callback) {
	(void)callback;
}

void FakeOnResetPlayerStatsRegister(levels_ranks::ResetPlayerStatsCallback callback) {
	(void)callback;
}

void FakeOnResetPlayerStatsUnregister(levels_ranks::ResetPlayerStatsCallback callback) {
	(void)callback;
}

void FakeOnPlayerKilledPreRegister(levels_ranks::PlayerKilledPreCallback callback) {
	g_killedPre = callback;
}

void FakeOnPlayerKilledPreUnregister(levels_ranks::PlayerKilledPreCallback callback) {
	if (g_killedPre == callback) {
		g_killedPre = nullptr;
	}
}

void FakeOnDatabaseCleanupRegister(levels_ranks::DatabaseCleanupCallback callback) {
	(void)callback;
}

void FakeOnDatabaseCleanupUnregister(levels_ranks::DatabaseCleanupCallback callback) {
	(void)callback;
}

// --- fixtures -------------------------------------------------------------------------------------

void InstallFakes() {
	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;

	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_OnClientDisconnect_Register = &FakeOnClientDisconnectRegister;
	__s2sdk_OnClientDisconnect_Unregister = &FakeOnClientDisconnectUnregister;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientAuthId = &FakeGetClientAuthId;

	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_GetEventInt = &FakeGetEventInt;
	__s2sdk_GetEventString = &FakeGetEventString;

	__levels_ranks_IsCoreLoaded = &FakeIsCoreLoaded;
	__levels_ranks_GetTableName = &FakeGetTableName;
	__levels_ranks_GetDatabaseConfig = &FakeGetDatabaseConfig;
	__levels_ranks_CheckCountPlayers = &FakeCheckCountPlayers;
	__levels_ranks_GetSettingsValue = &FakeGetSettingsValue;
	__levels_ranks_OnCoreIsReady_Register = &FakeOnCoreIsReadyRegister;
	__levels_ranks_OnCoreIsReady_Unregister = &FakeOnCoreIsReadyUnregister;
	__levels_ranks_OnPlayerLoaded_Register = &FakeOnPlayerLoadedRegister;
	__levels_ranks_OnPlayerLoaded_Unregister = &FakeOnPlayerLoadedUnregister;
	__levels_ranks_OnResetPlayerStats_Register = &FakeOnResetPlayerStatsRegister;
	__levels_ranks_OnResetPlayerStats_Unregister = &FakeOnResetPlayerStatsUnregister;
	__levels_ranks_OnPlayerKilledPre_Register = &FakeOnPlayerKilledPreRegister;
	__levels_ranks_OnPlayerKilledPre_Unregister = &FakeOnPlayerKilledPreUnregister;
	__levels_ranks_OnDatabaseCleanup_Register = &FakeOnDatabaseCleanupRegister;
	__levels_ranks_OnDatabaseCleanup_Unregister = &FakeOnDatabaseCleanupUnregister;
}

void ResetScenario() {
	for (ClientState& client : g_clients) {
		client = ClientState{};
	}
	g_event = FakeEvent{};
	g_playerSlotCalls = 0;
	g_eventIntCalls = 0;
	g_weaponCalls = 0;
	g_lastPlayerSlotKey.clear();
	g_lastEventIntKey.clear();
	g_countPlayers = true;
	g_settingsValue = 0;
}

// The kill the LR core reports: the raw s2sdk event handle travels as int64 through lr_api's
// trampoline (levels_ranks does `static_cast<int64_t>(reinterpret_cast<intptr_t>(event))`).
// Returns the (possibly rewritten) experience the module caused.
int32_t FireKill(int32_t exp, bool null_event = false) {
	if (g_killedPre == nullptr) {
		Check(false, "the LR kill callback is not registered");
		return exp;
	}
	int32_t caused = exp;
	const int64_t event_id =
		null_event ? 0 : static_cast<int64_t>(reinterpret_cast<intptr_t>(&g_event));
	g_killedPre(event_id, caused, /*exp_victim=*/50, /*exp_attacker=*/100);
	return caused;
}

void StartPlugin() {
	InstallFakes();
	ResetScenario();
	g_plugin.OnPluginStart();
	Check(g_onMapStart != nullptr, "the OnMapStart listener is registered");
	Check(g_onMapEnd != nullptr, "the OnMapEnd listener is registered");
	Check(g_onClientDisconnect != nullptr, "the OnClientDisconnect listener is registered");
	Check(g_killedPre != nullptr, "the LR OnPlayerKilledPre callback is registered");
}

// --- cases ----------------------------------------------------------------------------------------

void TestNoMapYet() {
	std::printf("-- no map yet\n");
	SetUsable(0);
	g_event = FakeEvent{0, 12, "ak47"};
	const int player_slot_calls = g_playerSlotCalls;
	CheckEqInt(FireKill(100), 100, "no coefficient is applied before the first map (B1)");
	CheckEqInt(g_playerSlotCalls, player_slot_calls, "no event field is read before the first map");
	CheckEqInt(g_eventIntCalls, 0, "GetEventInt is never called");
}

void TestPlayerSlotSource() {
	std::printf("-- the attacker slot comes from GetEventPlayerSlot\n");
	// Valid slot 0 with a decoy int value: the slot 0 client is usable, the decoy 12 is not.
	SetUsable(0);
	g_event = FakeEvent{0, 12, "ak47"};
	int player_slot_calls = g_playerSlotCalls;
	int weapon_calls = g_weaponCalls;
	CheckEqInt(FireKill(100), 90, "slot 0: the AK-47 coefficient 0.9 is applied");
	CheckEqInt(g_playerSlotCalls, player_slot_calls + 1, "the attacker field is read once");
	CheckEqInt(g_weaponCalls, weapon_calls + 1, "the weapon field is read once");
	Check(g_lastPlayerSlotKey == "attacker", "the player slot is read from the \"attacker\" key");
	CheckEqInt(g_eventIntCalls, 0, "GetEventInt is never called");

	// Valid slot 3 while the decoy int value is 0 - and slot 0 is not a usable client at all: only
	// the player-slot answer can produce the coefficient.
	SetUsable(3);
	g_event = FakeEvent{3, 0, "ak47"};
	player_slot_calls = g_playerSlotCalls;
	CheckEqInt(FireKill(100), 90, "slot 3 wins over the decoy int 0");
	CheckEqInt(g_playerSlotCalls, player_slot_calls + 1, "the attacker field is read once");
	CheckEqInt(g_eventIntCalls, 0, "GetEventInt is never called");

	// The knife special case of the original still works on the classname side (coefficient 5.0).
	SetUsable(0);
	g_event = FakeEvent{0, 40, "knife"};
	CheckEqInt(FireKill(100), 500, "\"knife\" resolves to weapon_knife (coefficient 5.0)");
	CheckEqInt(g_eventIntCalls, 0, "GetEventInt is never called");
}

void TestWorldAttacker() {
	std::printf("-- the world is not a player\n");
	// -1 is INVALID_PLAYER_SLOT_INDEX: no player at all, although the unused int value names a fully
	// usable client (a GetEventInt-based port would credit slot 2 here).
	SetUsable(2);
	g_event = FakeEvent{-1, 2, "ak47"};
	int player_slot_calls = g_playerSlotCalls;
	int weapon_calls = g_weaponCalls;
	CheckEqInt(FireKill(100), 100, "a world attacker causes no coefficient");
	CheckEqInt(g_playerSlotCalls, player_slot_calls + 1, "the attacker field is read once");
	CheckEqInt(g_weaponCalls, weapon_calls, "the weapon is not read for a non-player attacker");
	CheckEqInt(g_eventIntCalls, 0, "GetEventInt is never called");

	// The slot range guard still holds for a malformed answer.
	g_event = FakeEvent{lr_weapons::kMaxSlots, lr_weapons::kMaxSlots, "ak47"};
	CheckEqInt(FireKill(100), 100, "a slot above the range causes no coefficient");
	CheckEqInt(g_eventIntCalls, 0, "GetEventInt is never called");
}

void TestMissingPlayerSlotBinding() {
	std::printf("-- no player-slot binding\n");
	SetUsable(2);
	g_event = FakeEvent{5, 2, "ak47"};
	__s2sdk_GetEventPlayerSlot = nullptr;
	int player_slot_calls = g_playerSlotCalls;
	CheckEqInt(FireKill(100), 100, "without the binding the kill is dropped (fail soft)");
	CheckEqInt(g_playerSlotCalls, player_slot_calls, "the missing binding is not called");
	CheckEqInt(g_eventIntCalls, 0, "there is no GetEventInt fallback");
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	// The usable client is slot 2; the decoy int 5 stays irrelevant.
	g_event = FakeEvent{2, 5, "ak47"};
	CheckEqInt(FireKill(100), 90, "the binding restores the coefficient");
}

void TestEventGuards() {
	std::printf("-- event guards\n");
	SetUsable(6);
	g_event = FakeEvent{6, 6, "ak47"};
	CheckEqInt(FireKill(100), 90, "the baseline kill is credited");

	CheckEqInt(FireKill(100, /*null_event=*/true), 100, "a null event handle is a no-op");
	g_countPlayers = false;
	CheckEqInt(FireKill(100), 100, "CheckCountPlayers() false drops the kill");
	g_countPlayers = true;
}

void TestClientGuards() {
	std::printf("-- client guards (C2)\n");
	SetUsable(6);
	g_event = FakeEvent{6, 6, "ak47"};
	int player_slot_calls = g_playerSlotCalls;
	struct Variant {
		const char* what;
		bool ClientState::*flag;
		bool value;
	};
	const Variant variants[] = {
		{"SourceTV", &ClientState::source_tv, true},
		{"a fake client", &ClientState::fake, true},
		{"a missing pawn", &ClientState::pawn, false},
		{"a missing controller", &ClientState::controller, false},
		{"not connected", &ClientState::connected, false},
		{"not in game", &ClientState::in_game, false},
	};
	for (const Variant& variant : variants) {
		g_clients[6].*variant.flag = variant.value;
		CheckEqInt(FireKill(100), 100, std::string("an attacker with ") + variant.what + " is dropped");
		g_clients[6] = ClientState{};
		g_clients[6].connected = true;
		g_clients[6].in_game = true;
		g_clients[6].controller = true;
		g_clients[6].pawn = true;
		g_clients[6].steamid64 = 76561197960287930ull;
	}
	// A zero SteamID64 is a bot/unauthenticated client.
	g_clients[6].steamid64 = 0;
	CheckEqInt(FireKill(100), 100, "an attacker without a SteamID64 is dropped");
	g_clients[6].steamid64 = 76561197960287930ull;
	// The player field is read before the client checks, so every variant above read it.
	CheckEqInt(g_playerSlotCalls, player_slot_calls + 7, "the attacker field is read once per kill");
	CheckEqInt(g_eventIntCalls, 0, "GetEventInt is never called");
}

void TestWeaponSide() {
	std::printf("-- weapon side\n");
	SetUsable(6);
	g_event = FakeEvent{6, 6, ""};
	int weapon_calls = g_weaponCalls;
	CheckEqInt(FireKill(100), 100, "an empty weapon class causes no coefficient");
	CheckEqInt(g_weaponCalls, weapon_calls + 1, "the weapon field is read once");
	g_event.weapon = "weapon_not_in_the_config";
	CheckEqInt(FireKill(100), 100, "a classname outside the config causes no coefficient");
	CheckEqInt(g_eventIntCalls, 0, "GetEventInt is never called");
}

void TestMapEndWindow() {
	std::printf("-- the OnMapEnd window (B2)\n");
	SetUsable(6);
	g_event = FakeEvent{6, 6, "ak47"};
	Check(g_onMapEnd != nullptr, "the OnMapEnd listener is available");
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
	int player_slot_calls = g_playerSlotCalls;
	CheckEqInt(FireKill(100), 100, "no kill is credited in the OnMapEnd window");
	CheckEqInt(g_playerSlotCalls, player_slot_calls, "no event field is read in the OnMapEnd window");
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CheckEqInt(FireKill(100), 100, "OnPluginUpdate does not re-open the gate after OnMapEnd");
	CheckEqInt(g_playerSlotCalls, player_slot_calls, "still no event field is read");
	Check(g_onMapStart != nullptr, "the OnMapStart listener is available");
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
	CheckEqInt(FireKill(100), 90, "the next OnMapStart re-opens the gate");
	CheckEqInt(g_eventIntCalls, 0, "GetEventInt is never called");
}

void TestUnload() {
	std::printf("-- unload (A7)\n");
	g_plugin.OnPluginEnd();
	Check(g_killedPre == nullptr, "the LR kill callback is unhooked");
	Check(g_onMapStart == nullptr, "the OnMapStart listener is removed");
	Check(g_onMapEnd == nullptr, "the OnMapEnd listener is removed");
	Check(g_onClientDisconnect == nullptr, "the OnClientDisconnect listener is removed");
	Check(!g_mapReady, "the map gate is closed on unload");
	CheckEqInt(g_eventIntCalls, 0, "GetEventInt was never called in the whole run");
}

} // namespace

int main() {
	std::printf("lr_weapons_plugin_tests\n");
	StartPlugin();
	TestNoMapYet();
	// The OnMapStart callback opens the gate for the rest of the run (PITFALLS B1).
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	} else {
		Check(false, "no OnMapStart listener - the rest of the run cannot be exercised");
	}
	TestPlayerSlotSource();
	TestWorldAttacker();
	TestMissingPlayerSlotBinding();
	TestEventGuards();
	TestClientGuards();
	TestWeaponSide();
	TestMapEndWindow();
	TestUnload();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
