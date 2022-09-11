// Copyright 2022 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "events.h"

#include "item.h"
#include "luaenv.h"
#include "luameta.h"
#include "player.h"

Events::Events() : scriptInterface("Event Interface") { scriptInterface.initState(); }

bool Events::load()
{
	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file("data/events/events.xml");
	if (!result) {
		printXMLError("Error - Events::load", "data/events/events.xml", result);
		return false;
	}

	info = {};

	std::set<std::string> classes;
	for (auto eventNode : doc.child("events").children()) {
		if (!eventNode.attribute("enabled").as_bool()) {
			continue;
		}

		const std::string& className = eventNode.attribute("class").as_string();
		auto res = classes.insert(className);
		if (res.second) {
			const std::string& lowercase = boost::algorithm::to_lower_copy(className);
			if (scriptInterface.loadFile("data/events/scripts/" + lowercase + ".lua") != 0) {
				std::cout << "[Warning - Events::load] Can not load script: " << lowercase << ".lua" << std::endl;
				std::cout << scriptInterface.getLastLuaError() << std::endl;
			}
		}

		const std::string& methodName = eventNode.attribute("method").as_string();
		const int32_t event = scriptInterface.getMetaEvent(className, methodName);
		if (className == "Creature") {
			if (methodName == "onChangeOutfit") {
				info.creatureOnChangeOutfit = event;
			} else if (methodName == "onAreaCombat") {
				info.creatureOnAreaCombat = event;
			} else if (methodName == "onTargetCombat") {
				info.creatureOnTargetCombat = event;
			} else if (methodName == "onHear") {
				info.creatureOnHear = event;
			} else {
				std::cout << "[Warning - Events::load] Unknown creature method: " << methodName << std::endl;
			}
		} else if (className == "Party") {
			if (methodName == "onJoin") {
				info.partyOnJoin = event;
			} else if (methodName == "onLeave") {
				info.partyOnLeave = event;
			} else if (methodName == "onDisband") {
				info.partyOnDisband = event;
			} else if (methodName == "onShareExperience") {
				info.partyOnShareExperience = event;
			} else {
				std::cout << "[Warning - Events::load] Unknown party method: " << methodName << std::endl;
			}
		} else if (className == "Player") {
			if (methodName == "onBrowseField") {
				info.playerOnBrowseField = event;
			} else if (methodName == "onLook") {
				info.playerOnLook = event;
			} else if (methodName == "onLookInBattleList") {
				info.playerOnLookInBattleList = event;
			} else if (methodName == "onLookInTrade") {
				info.playerOnLookInTrade = event;
			} else if (methodName == "onLookInShop") {
				info.playerOnLookInShop = event;
			} else if (methodName == "onLookInMarket") {
				info.playerOnLookInMarket = event;
			} else if (methodName == "onTradeRequest") {
				info.playerOnTradeRequest = event;
			} else if (methodName == "onTradeAccept") {
				info.playerOnTradeAccept = event;
			} else if (methodName == "onTradeCompleted") {
				info.playerOnTradeCompleted = event;
			} else if (methodName == "onPodiumRequest") {
				info.playerOnPodiumRequest = event;
			} else if (methodName == "onPodiumEdit") {
				info.playerOnPodiumEdit = event;
			} else if (methodName == "onMoveItem") {
				info.playerOnMoveItem = event;
			} else if (methodName == "onItemMoved") {
				info.playerOnItemMoved = event;
			} else if (methodName == "onMoveCreature") {
				info.playerOnMoveCreature = event;
			} else if (methodName == "onReportRuleViolation") {
				info.playerOnReportRuleViolation = event;
			} else if (methodName == "onReportBug") {
				info.playerOnReportBug = event;
			} else if (methodName == "onTurn") {
				info.playerOnTurn = event;
			} else if (methodName == "onGainExperience") {
				info.playerOnGainExperience = event;
			} else if (methodName == "onLoseExperience") {
				info.playerOnLoseExperience = event;
			} else if (methodName == "onGainSkillTries") {
				info.playerOnGainSkillTries = event;
			} else if (methodName == "onWrapItem") {
				info.playerOnWrapItem = event;
			} else if (methodName == "onInventoryUpdate") {
				info.playerOnInventoryUpdate = event;
			} else {
				std::cout << "[Warning - Events::load] Unknown player method: " << methodName << std::endl;
			}
		} else if (className == "Monster") {
			if (methodName == "onDropLoot") {
				info.monsterOnDropLoot = event;
			} else if (methodName == "onSpawn") {
				info.monsterOnSpawn = event;
			} else {
				std::cout << "[Warning - Events::load] Unknown monster method: " << methodName << std::endl;
			}
		} else {
			std::cout << "[Warning - Events::load] Unknown class: " << className << std::endl;
		}
	}
	return true;
}

// Monster
bool Events::eventMonsterOnSpawn(Monster* monster, const Position& position, bool startup, bool artificial)
{
	// Monster:onSpawn(position, startup, artificial)
	using namespace tfs;

	if (info.monsterOnSpawn == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::monsterOnSpawn] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.monsterOnSpawn, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.monsterOnSpawn);

	lua::pushUserdata<Monster>(L, monster);
	lua::setMetatable(L, -1, "Monster");
	lua::pushPosition(L, position);
	lua::pushBoolean(L, startup);
	lua::pushBoolean(L, artificial);

	return scriptInterface.callFunction(4);
}

// Creature
bool Events::eventCreatureOnChangeOutfit(Creature* creature, const Outfit_t& outfit)
{
	// Creature:onChangeOutfit(outfit) or Creature.onChangeOutfit(self, outfit)
	using namespace tfs;

	if (info.creatureOnChangeOutfit == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventCreatureOnChangeOutfit] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.creatureOnChangeOutfit, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.creatureOnChangeOutfit);

	lua::pushUserdata<Creature>(L, creature);
	lua::setCreatureMetatable(L, -1, creature);

	lua::pushOutfit(L, outfit);

	return scriptInterface.callFunction(2);
}

ReturnValue Events::eventCreatureOnAreaCombat(Creature* creature, Tile* tile, bool aggressive)
{
	// Creature:onAreaCombat(tile, aggressive) or Creature.onAreaCombat(self, tile, aggressive)
	using namespace tfs;

	if (info.creatureOnAreaCombat == -1) {
		return RETURNVALUE_NOERROR;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventCreatureOnAreaCombat] Call stack overflow" << std::endl;
		return RETURNVALUE_NOTPOSSIBLE;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.creatureOnAreaCombat, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.creatureOnAreaCombat);

	if (creature) {
		lua::pushUserdata<Creature>(L, creature);
		lua::setCreatureMetatable(L, -1, creature);
	} else {
		lua_pushnil(L);
	}

	lua::pushUserdata<Tile>(L, tile);
	lua::setMetatable(L, -1, "Tile");

	lua::pushBoolean(L, aggressive);

	ReturnValue returnValue;
	if (lua::protectedCall(L, 3, 1) != 0) {
		returnValue = RETURNVALUE_NOTPOSSIBLE;
		lua::reportError(nullptr, lua::popString(L));
	} else {
		returnValue = lua::getNumber<ReturnValue>(L, -1);
		lua_pop(L, 1);
	}

	lua::resetScriptEnv();
	return returnValue;
}

ReturnValue Events::eventCreatureOnTargetCombat(Creature* creature, Creature* target)
{
	// Creature:onTargetCombat(target) or Creature.onTargetCombat(self, target)
	using namespace tfs;

	if (info.creatureOnTargetCombat == -1) {
		return RETURNVALUE_NOERROR;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventCreatureOnTargetCombat] Call stack overflow" << std::endl;
		return RETURNVALUE_NOTPOSSIBLE;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.creatureOnTargetCombat, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.creatureOnTargetCombat);

	if (creature) {
		lua::pushUserdata<Creature>(L, creature);
		lua::setCreatureMetatable(L, -1, creature);
	} else {
		lua_pushnil(L);
	}

	lua::pushUserdata<Creature>(L, target);
	lua::setCreatureMetatable(L, -1, target);

	ReturnValue returnValue;
	if (lua::protectedCall(L, 2, 1) != 0) {
		returnValue = RETURNVALUE_NOTPOSSIBLE;
		lua::reportError(nullptr, lua::popString(L));
	} else {
		returnValue = lua::getNumber<ReturnValue>(L, -1);
		lua_pop(L, 1);
	}

	lua::resetScriptEnv();
	return returnValue;
}

void Events::eventCreatureOnHear(Creature* creature, Creature* speaker, const std::string& words, SpeakClasses type)
{
	// Creature:onHear(speaker, words, type)
	using namespace tfs;

	if (info.creatureOnHear == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventCreatureOnHear] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.creatureOnHear, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.creatureOnHear);

	lua::pushUserdata<Creature>(L, creature);
	lua::setCreatureMetatable(L, -1, creature);

	lua::pushUserdata<Creature>(L, speaker);
	lua::setCreatureMetatable(L, -1, speaker);

	lua::pushString(L, words);
	lua_pushnumber(L, type);

	scriptInterface.callVoidFunction(4);
}

// Party
bool Events::eventPartyOnJoin(Party* party, Player* player)
{
	// Party:onJoin(player) or Party.onJoin(self, player)
	using namespace tfs;

	if (info.partyOnJoin == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPartyOnJoin] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.partyOnJoin, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.partyOnJoin);

	lua::pushUserdata<Party>(L, party);
	lua::setMetatable(L, -1, "Party");

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	return scriptInterface.callFunction(2);
}

bool Events::eventPartyOnLeave(Party* party, Player* player)
{
	// Party:onLeave(player) or Party.onLeave(self, player)
	using namespace tfs;

	if (info.partyOnLeave == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPartyOnLeave] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.partyOnLeave, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.partyOnLeave);

	lua::pushUserdata<Party>(L, party);
	lua::setMetatable(L, -1, "Party");

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	return scriptInterface.callFunction(2);
}

bool Events::eventPartyOnDisband(Party* party)
{
	// Party:onDisband() or Party.onDisband(self)
	using namespace tfs;

	if (info.partyOnDisband == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPartyOnDisband] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.partyOnDisband, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.partyOnDisband);

	lua::pushUserdata<Party>(L, party);
	lua::setMetatable(L, -1, "Party");

	return scriptInterface.callFunction(1);
}

void Events::eventPartyOnShareExperience(Party* party, uint64_t& exp)
{
	// Party:onShareExperience(exp) or Party.onShareExperience(self, exp)
	using namespace tfs;

	if (info.partyOnShareExperience == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPartyOnShareExperience] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.partyOnShareExperience, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.partyOnShareExperience);

	lua::pushUserdata<Party>(L, party);
	lua::setMetatable(L, -1, "Party");

	lua_pushnumber(L, exp);

	if (lua::protectedCall(L, 2, 1) != 0) {
		lua::reportError(nullptr, lua::popString(L));
	} else {
		exp = lua::getNumber<uint64_t>(L, -1);
		lua_pop(L, 1);
	}

	lua::resetScriptEnv();
}

// Player
bool Events::eventPlayerOnBrowseField(Player* player, const Position& position)
{
	// Player:onBrowseField(position) or Player.onBrowseField(self, position)
	using namespace tfs;

	if (info.playerOnBrowseField == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnBrowseField] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnBrowseField, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnBrowseField);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushPosition(L, position);

	return scriptInterface.callFunction(2);
}

void Events::eventPlayerOnLook(Player* player, const Position& position, Thing* thing, uint8_t stackpos,
                               int32_t lookDistance)
{
	// Player:onLook(thing, position, distance) or Player.onLook(self, thing, position, distance)
	using namespace tfs;

	if (info.playerOnLook == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnLook] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnLook, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnLook);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	if (Creature* creature = thing->getCreature()) {
		lua::pushUserdata<Creature>(L, creature);
		lua::setCreatureMetatable(L, -1, creature);
	} else if (Item* item = thing->getItem()) {
		lua::pushUserdata<Item>(L, item);
		lua::setItemMetatable(L, -1, item);
	} else {
		lua_pushnil(L);
	}

	lua::pushPosition(L, position, stackpos);
	lua_pushnumber(L, lookDistance);

	scriptInterface.callVoidFunction(4);
}

void Events::eventPlayerOnLookInBattleList(Player* player, Creature* creature, int32_t lookDistance)
{
	// Player:onLookInBattleList(creature, position, distance) or Player.onLookInBattleList(self, creature, position,
	// distance)
	using namespace tfs;

	if (info.playerOnLookInBattleList == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnLookInBattleList] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnLookInBattleList, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnLookInBattleList);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Creature>(L, creature);
	lua::setCreatureMetatable(L, -1, creature);

	lua_pushnumber(L, lookDistance);

	scriptInterface.callVoidFunction(3);
}

void Events::eventPlayerOnLookInTrade(Player* player, Player* partner, Item* item, int32_t lookDistance)
{
	// Player:onLookInTrade(partner, item, distance) or Player.onLookInTrade(self, partner, item, distance)
	using namespace tfs;

	if (info.playerOnLookInTrade == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnLookInTrade] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnLookInTrade, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnLookInTrade);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Player>(L, partner);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Item>(L, item);
	lua::setItemMetatable(L, -1, item);

	lua_pushnumber(L, lookDistance);

	scriptInterface.callVoidFunction(4);
}

bool Events::eventPlayerOnLookInShop(Player* player, const ItemType* itemType, uint8_t count)
{
	// Player:onLookInShop(itemType, count) or Player.onLookInShop(self, itemType, count)
	using namespace tfs;

	if (info.playerOnLookInShop == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnLookInShop] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnLookInShop, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnLookInShop);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<const ItemType>(L, itemType);
	lua::setMetatable(L, -1, "ItemType");

	lua_pushnumber(L, count);

	return scriptInterface.callFunction(3);
}

bool Events::eventPlayerOnLookInMarket(Player* player, const ItemType* itemType)
{
	// Player:onLookInMarket(itemType) or Player.onLookInMarket(self, itemType)
	using namespace tfs;

	if (info.playerOnLookInMarket == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnLookInMarket] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnLookInMarket, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnLookInMarket);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<const ItemType>(L, itemType);
	lua::setMetatable(L, -1, "ItemType");

	return scriptInterface.callFunction(2);
}

ReturnValue Events::eventPlayerOnMoveItem(Player* player, Item* item, uint16_t count, const Position& fromPosition,
                                          const Position& toPosition, Cylinder* fromCylinder, Cylinder* toCylinder)
{
	// Player:onMoveItem(item, count, fromPosition, toPosition) or Player.onMoveItem(self, item, count, fromPosition,
	// toPosition, fromCylinder, toCylinder)
	using namespace tfs;

	if (info.playerOnMoveItem == -1) {
		return RETURNVALUE_NOERROR;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnMoveItem] Call stack overflow" << std::endl;
		return RETURNVALUE_NOTPOSSIBLE;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnMoveItem, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnMoveItem);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Item>(L, item);
	lua::setItemMetatable(L, -1, item);

	lua_pushnumber(L, count);
	lua::pushPosition(L, fromPosition);
	lua::pushPosition(L, toPosition);

	lua::pushCylinder(L, fromCylinder);
	lua::pushCylinder(L, toCylinder);

	ReturnValue returnValue;
	if (lua::protectedCall(L, 7, 1) != 0) {
		returnValue = RETURNVALUE_NOTPOSSIBLE;
		lua::reportError(nullptr, lua::popString(L));
	} else {
		returnValue = lua::getNumber<ReturnValue>(L, -1);
		lua_pop(L, 1);
	}

	lua::resetScriptEnv();
	return returnValue;
}

void Events::eventPlayerOnItemMoved(Player* player, Item* item, uint16_t count, const Position& fromPosition,
                                    const Position& toPosition, Cylinder* fromCylinder, Cylinder* toCylinder)
{
	// Player:onItemMoved(item, count, fromPosition, toPosition) or Player.onItemMoved(self, item, count, fromPosition,
	// toPosition, fromCylinder, toCylinder)
	using namespace tfs;

	if (info.playerOnItemMoved == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnItemMoved] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnItemMoved, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnItemMoved);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Item>(L, item);
	lua::setItemMetatable(L, -1, item);

	lua_pushnumber(L, count);
	lua::pushPosition(L, fromPosition);
	lua::pushPosition(L, toPosition);

	lua::pushCylinder(L, fromCylinder);
	lua::pushCylinder(L, toCylinder);

	scriptInterface.callVoidFunction(7);
}

bool Events::eventPlayerOnMoveCreature(Player* player, Creature* creature, const Position& fromPosition,
                                       const Position& toPosition)
{
	// Player:onMoveCreature(creature, fromPosition, toPosition) or Player.onMoveCreature(self, creature, fromPosition,
	// toPosition)
	using namespace tfs;

	if (info.playerOnMoveCreature == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnMoveCreature] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnMoveCreature, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnMoveCreature);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Creature>(L, creature);
	lua::setCreatureMetatable(L, -1, creature);

	lua::pushPosition(L, fromPosition);
	lua::pushPosition(L, toPosition);

	return scriptInterface.callFunction(4);
}

void Events::eventPlayerOnReportRuleViolation(Player* player, const std::string& targetName, uint8_t reportType,
                                              uint8_t reportReason, const std::string& comment,
                                              const std::string& translation)
{
	// Player:onReportRuleViolation(targetName, reportType, reportReason, comment, translation)
	using namespace tfs;

	if (info.playerOnReportRuleViolation == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnReportRuleViolation] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnReportRuleViolation, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnReportRuleViolation);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushString(L, targetName);

	lua_pushnumber(L, reportType);
	lua_pushnumber(L, reportReason);

	lua::pushString(L, comment);
	lua::pushString(L, translation);

	scriptInterface.callVoidFunction(6);
}

bool Events::eventPlayerOnReportBug(Player* player, const std::string& message, const Position& position,
                                    uint8_t category)
{
	// Player:onReportBug(message, position, category)
	using namespace tfs;

	if (info.playerOnReportBug == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnReportBug] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnReportBug, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnReportBug);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushString(L, message);
	lua::pushPosition(L, position);
	lua_pushnumber(L, category);

	return scriptInterface.callFunction(4);
}

bool Events::eventPlayerOnTurn(Player* player, Direction direction)
{
	// Player:onTurn(direction) or Player.onTurn(self, direction)
	using namespace tfs;

	if (info.playerOnTurn == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnTurn] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnTurn, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnTurn);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua_pushnumber(L, direction);

	return scriptInterface.callFunction(2);
}

bool Events::eventPlayerOnTradeRequest(Player* player, Player* target, Item* item)
{
	// Player:onTradeRequest(target, item)
	using namespace tfs;

	if (info.playerOnTradeRequest == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnTradeRequest] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnTradeRequest, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnTradeRequest);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Player>(L, target);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Item>(L, item);
	lua::setItemMetatable(L, -1, item);

	return scriptInterface.callFunction(3);
}

bool Events::eventPlayerOnTradeAccept(Player* player, Player* target, Item* item, Item* targetItem)
{
	// Player:onTradeAccept(target, item, targetItem)
	using namespace tfs;

	if (info.playerOnTradeAccept == -1) {
		return true;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnTradeAccept] Call stack overflow" << std::endl;
		return false;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnTradeAccept, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnTradeAccept);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Player>(L, target);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Item>(L, item);
	lua::setItemMetatable(L, -1, item);

	lua::pushUserdata<Item>(L, targetItem);
	lua::setItemMetatable(L, -1, targetItem);

	return scriptInterface.callFunction(4);
}

void Events::eventPlayerOnTradeCompleted(Player* player, Player* target, Item* item, Item* targetItem, bool isSuccess)
{
	// Player:onTradeCompleted(target, item, targetItem, isSuccess)
	using namespace tfs;

	if (info.playerOnTradeCompleted == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnTradeCompleted] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnTradeCompleted, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnTradeCompleted);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Player>(L, target);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Item>(L, item);
	lua::setItemMetatable(L, -1, item);

	lua::pushUserdata<Item>(L, targetItem);
	lua::setItemMetatable(L, -1, targetItem);

	lua::pushBoolean(L, isSuccess);

	return scriptInterface.callVoidFunction(5);
}

void Events::eventPlayerOnPodiumRequest(Player* player, Item* item)
{
	// Player:onPodiumRequest(item) or Player.onPodiumRequest(self, item)
	using namespace tfs;

	if (info.playerOnPodiumRequest == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnPodiumRequest] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnPodiumRequest, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnPodiumRequest);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Item>(L, item);
	lua::setItemMetatable(L, -1, item);

	scriptInterface.callFunction(2);
}

void Events::eventPlayerOnPodiumEdit(Player* player, Item* item, const Outfit_t& outfit, bool podiumVisible,
                                     Direction direction)
{
	// Player:onPodiumEdit(item, outfit, direction, isVisible) or Player.onPodiumEdit(self, item, outfit, direction,
	// isVisible)
	using namespace tfs;

	if (info.playerOnPodiumEdit == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnPodiumEdit] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnPodiumEdit, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnPodiumEdit);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Item>(L, item);
	lua::setItemMetatable(L, -1, item);

	lua::pushOutfit(L, outfit);

	lua_pushnumber(L, direction);
	lua_pushboolean(L, podiumVisible);

	scriptInterface.callFunction(5);
}

void Events::eventPlayerOnGainExperience(Player* player, Creature* source, uint64_t& exp, uint64_t rawExp)
{
	// Player:onGainExperience(source, exp, rawExp) rawExp gives the original exp which is not multiplied
	using namespace tfs;

	if (info.playerOnGainExperience == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnGainExperience] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnGainExperience, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnGainExperience);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	if (source) {
		lua::pushUserdata<Creature>(L, source);
		lua::setCreatureMetatable(L, -1, source);
	} else {
		lua_pushnil(L);
	}

	lua_pushnumber(L, exp);
	lua_pushnumber(L, rawExp);

	if (lua::protectedCall(L, 4, 1) != 0) {
		lua::reportError(nullptr, lua::popString(L));
	} else {
		exp = lua::getNumber<uint64_t>(L, -1);
		lua_pop(L, 1);
	}

	lua::resetScriptEnv();
}

void Events::eventPlayerOnLoseExperience(Player* player, uint64_t& exp)
{
	// Player:onLoseExperience(exp)
	using namespace tfs;

	if (info.playerOnLoseExperience == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnLoseExperience] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnLoseExperience, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnLoseExperience);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua_pushnumber(L, exp);

	if (lua::protectedCall(L, 2, 1) != 0) {
		lua::reportError(nullptr, lua::popString(L));
	} else {
		exp = lua::getNumber<uint64_t>(L, -1);
		lua_pop(L, 1);
	}

	lua::resetScriptEnv();
}

void Events::eventPlayerOnGainSkillTries(Player* player, skills_t skill, uint64_t& tries)
{
	// Player:onGainSkillTries(skill, tries)
	using namespace tfs;

	if (info.playerOnGainSkillTries == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnGainSkillTries] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnGainSkillTries, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnGainSkillTries);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua_pushnumber(L, skill);
	lua_pushnumber(L, tries);

	if (lua::protectedCall(L, 3, 1) != 0) {
		lua::reportError(nullptr, lua::popString(L));
	} else {
		tries = lua::getNumber<uint64_t>(L, -1);
		lua_pop(L, 1);
	}

	lua::resetScriptEnv();
}

void Events::eventPlayerOnWrapItem(Player* player, Item* item)
{
	// Player:onWrapItem(item)
	using namespace tfs;

	if (info.playerOnWrapItem == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnWrapItem] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnWrapItem, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnWrapItem);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Item>(L, item);
	lua::setItemMetatable(L, -1, item);

	scriptInterface.callVoidFunction(2);
}

void Events::eventPlayerOnInventoryUpdate(Player* player, Item* item, slots_t slot, bool equip)
{
	// Player:onInventoryUpdate(item, slot, equip)
	using namespace tfs;

	if (info.playerOnInventoryUpdate == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventPlayerOnInventoryUpdate] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.playerOnInventoryUpdate, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.playerOnInventoryUpdate);

	lua::pushUserdata<Player>(L, player);
	lua::setMetatable(L, -1, "Player");

	lua::pushUserdata<Item>(L, item);
	lua::setItemMetatable(L, -1, item);

	lua_pushnumber(L, slot);
	lua::pushBoolean(L, equip);

	scriptInterface.callVoidFunction(4);
}

void Events::eventMonsterOnDropLoot(Monster* monster, Container* corpse)
{
	// Monster:onDropLoot(corpse)
	using namespace tfs;

	if (info.monsterOnDropLoot == -1) {
		return;
	}

	if (!lua::reserveScriptEnv()) {
		std::cout << "[Error - Events::eventMonsterOnDropLoot] Call stack overflow" << std::endl;
		return;
	}

	lua::ScriptEnvironment* env = lua::getScriptEnv();
	env->setScriptId(info.monsterOnDropLoot, &scriptInterface);

	lua_State* L = scriptInterface.getLuaState();
	scriptInterface.pushFunction(info.monsterOnDropLoot);

	lua::pushUserdata<Monster>(L, monster);
	lua::setMetatable(L, -1, "Monster");

	lua::pushUserdata<Container>(L, corpse);
	lua::setMetatable(L, -1, "Container");

	return scriptInterface.callVoidFunction(2);
}
