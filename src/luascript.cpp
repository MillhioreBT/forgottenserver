// Copyright 2022 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "luascript.h"

#include "bed.h"
#include "chat.h"
#include "combat.h"
#include "condition.h"
#include "configmanager.h"
#include "databasemanager.h"
#include "databasetasks.h"
#include "events.h"
#include "game.h"
#include "housetile.h"
#include "iologindata.h"
#include "iomapserialize.h"
#include "iomarket.h"
#include "luaapi.h"
#include "luaenv.h"
#include "luaerror.h"
#include "luameta.h"
#include "luavariant.h"
#include "modules/luaregister.h"
#include "monster.h"
#include "npc.h"
#include "outfit.h"
#include "party.h"
#include "player.h"
#include "podium.h"
#include "protocolstatus.h"
#include "scheduler.h"
#include "script.h"
#include "spectators.h"
#include "spells.h"
#include "teleport.h"

#include <boost/range/adaptor/reversed.hpp>

extern Chat* g_chat;
extern Game* g_game;
extern Monsters g_monsters;
extern ConfigManager g_config;
extern Vocations g_vocations;
extern Spells* g_spells;
extern Events* g_events;
extern Actions* g_actions;

extern Scripts* g_scripts;

extern LuaEnvironment* g_luaEnvironment;

LuaScriptInterface::LuaScriptInterface(std::string interfaceName) : interfaceName(std::move(interfaceName))
{
	// if (!g_luaEnvironment->getLuaState()) {
	// 	g_luaEnvironment->initState();
	// }
}

LuaScriptInterface::~LuaScriptInterface() { closeState(); }

bool LuaScriptInterface::reInitState()
{
	g_luaEnvironment->clearCombatObjects(this);
	g_luaEnvironment->clearAreaObjects(this);

	closeState();
	return initState();
}

int32_t LuaScriptInterface::loadFile(const std::string& file, Npc* npc /* = nullptr*/)
{
	// loads file as a chunk at stack top
	int ret = luaL_loadfile(luaState, file.c_str());
	if (ret != 0) {
		lastLuaError = tfs::lua::popString(luaState);
		return -1;
	}

	// check that it is loaded as a function
	if (!lua_isfunction(luaState, -1)) {
		lua_pop(luaState, 1);
		return -1;
	}

	loadingFile = file;

	if (!tfs::lua::reserveScriptEnv()) {
		lua_pop(luaState, 1);
		return -1;
	}

	tfs::lua::ScriptEnvironment* env = tfs::lua::getScriptEnv();
	env->setScriptId(EVENT_ID_LOADING, this);
	env->setNpc(npc);

	// execute it
	ret = tfs::lua::protectedCall(luaState, 0, 0);
	if (ret != 0) {
		tfs::lua::reportError(nullptr, tfs::lua::popString(luaState));
		tfs::lua::resetScriptEnv();
		return -1;
	}

	tfs::lua::resetScriptEnv();
	return 0;
}

int32_t LuaScriptInterface::getEvent(const std::string& eventName)
{
	// get our events table
	lua_rawgeti(luaState, LUA_REGISTRYINDEX, eventTableRef);
	if (!lua_istable(luaState, -1)) {
		lua_pop(luaState, 1);
		return -1;
	}

	// get current event function pointer
	lua_getglobal(luaState, eventName.c_str());
	if (!lua_isfunction(luaState, -1)) {
		lua_pop(luaState, 2);
		return -1;
	}

	// save in our events table
	lua_pushvalue(luaState, -1);
	lua_rawseti(luaState, -3, runningEventId);
	lua_pop(luaState, 2);

	// reset global value of this event
	lua_pushnil(luaState);
	lua_setglobal(luaState, eventName.c_str());

	cacheFiles[runningEventId] = loadingFile + ":" + eventName;
	return runningEventId++;
}

int32_t LuaScriptInterface::getEvent()
{
	// check if function is on the stack
	if (!lua_isfunction(luaState, -1)) {
		return -1;
	}

	// get our events table
	lua_rawgeti(luaState, LUA_REGISTRYINDEX, eventTableRef);
	if (!lua_istable(luaState, -1)) {
		lua_pop(luaState, 1);
		return -1;
	}

	// save in our events table
	lua_pushvalue(luaState, -2);
	lua_rawseti(luaState, -2, runningEventId);
	lua_pop(luaState, 2);

	cacheFiles[runningEventId] = loadingFile + ":callback";
	return runningEventId++;
}

int32_t LuaScriptInterface::getMetaEvent(const std::string& globalName, const std::string& eventName)
{
	// get our events table
	lua_rawgeti(luaState, LUA_REGISTRYINDEX, eventTableRef);
	if (!lua_istable(luaState, -1)) {
		lua_pop(luaState, 1);
		return -1;
	}

	// get current event function pointer
	lua_getglobal(luaState, globalName.c_str());
	lua_getfield(luaState, -1, eventName.c_str());
	if (!lua_isfunction(luaState, -1)) {
		lua_pop(luaState, 3);
		return -1;
	}

	// save in our events table
	lua_pushvalue(luaState, -1);
	lua_rawseti(luaState, -4, runningEventId);
	lua_pop(luaState, 1);

	// reset global value of this event
	lua_pushnil(luaState);
	lua_setfield(luaState, -2, eventName.c_str());
	lua_pop(luaState, 2);

	cacheFiles[runningEventId] = loadingFile + ":" + globalName + "@" + eventName;
	return runningEventId++;
}

const std::string& LuaScriptInterface::getFileById(int32_t scriptId)
{
	if (scriptId == EVENT_ID_LOADING) {
		return loadingFile;
	}

	auto it = cacheFiles.find(scriptId);
	if (it == cacheFiles.end()) {
		static const std::string& unk = "(Unknown scriptfile)";
		return unk;
	}
	return it->second;
}

bool LuaScriptInterface::pushFunction(int32_t functionId)
{
	lua_rawgeti(luaState, LUA_REGISTRYINDEX, eventTableRef);
	if (!lua_istable(luaState, -1)) {
		return false;
	}

	lua_rawgeti(luaState, -1, functionId);
	lua_replace(luaState, -2);
	return lua_isfunction(luaState, -1);
}

bool LuaScriptInterface::initState()
{
	luaState = g_luaEnvironment->getLuaState();
	if (!luaState) {
		return false;
	}

	lua_newtable(luaState);
	eventTableRef = luaL_ref(luaState, LUA_REGISTRYINDEX);
	runningEventId = EVENT_ID_USER;
	return true;
}

bool LuaScriptInterface::closeState()
{
	if (!g_luaEnvironment->getLuaState() || !luaState) {
		return false;
	}

	cacheFiles.clear();
	if (eventTableRef != -1) {
		luaL_unref(luaState, LUA_REGISTRYINDEX, eventTableRef);
		eventTableRef = -1;
	}

	luaState = nullptr;
	return true;
}

bool LuaScriptInterface::callFunction(int params)
{
	bool result = false;
	int size = lua_gettop(luaState);
	if (tfs::lua::protectedCall(luaState, params, 1) != 0) {
		tfs::lua::reportError(nullptr, tfs::lua::getString(luaState, -1));
	} else {
		result = tfs::lua::getBoolean(luaState, -1);
	}

	lua_pop(luaState, 1);
	if ((lua_gettop(luaState) + params + 1) != size) {
		tfs::lua::reportError(nullptr, "Stack size changed!");
	}

	tfs::lua::resetScriptEnv();
	return result;
}

void LuaScriptInterface::callVoidFunction(int params)
{
	int size = lua_gettop(luaState);
	if (tfs::lua::protectedCall(luaState, params, 0) != 0) {
		tfs::lua::reportError(nullptr, tfs::lua::popString(luaState));
	}

	if ((lua_gettop(luaState) + params + 1) != size) {
		tfs::lua::reportError(nullptr, "Stack size changed!");
	}

	tfs::lua::resetScriptEnv();
}

void LuaScriptInterface::registerClass(const std::string& className, const std::string& baseClass,
                                       lua_CFunction newFunction /* = nullptr*/)
{
	// className = {}
	lua_newtable(luaState);
	lua_pushvalue(luaState, -1);
	lua_setglobal(luaState, className.c_str());
	int methods = lua_gettop(luaState);

	// methodsTable = {}
	lua_newtable(luaState);
	int methodsTable = lua_gettop(luaState);

	if (newFunction) {
		// className.__call = newFunction
		lua_pushcfunction(luaState, newFunction);
		lua_setfield(luaState, methodsTable, "__call");
	}

	uint32_t parents = 0;
	if (!baseClass.empty()) {
		lua_getglobal(luaState, baseClass.c_str());
		lua_rawgeti(luaState, -1, 'p');
		parents = tfs::lua::getNumber<uint32_t>(luaState, -1) + 1;
		lua_pop(luaState, 1);
		lua_setfield(luaState, methodsTable, "__index");
	}

	// setmetatable(className, methodsTable)
	lua_setmetatable(luaState, methods);

	// className.metatable = {}
	luaL_newmetatable(luaState, className.c_str());
	int metatable = lua_gettop(luaState);

	// className.metatable.__metatable = className
	lua_pushvalue(luaState, methods);
	lua_setfield(luaState, metatable, "__metatable");

	// className.metatable.__index = className
	lua_pushvalue(luaState, methods);
	lua_setfield(luaState, metatable, "__index");

	// className.metatable['h'] = hash
	lua_pushnumber(luaState, std::hash<std::string>()(className));
	lua_rawseti(luaState, metatable, 'h');

	// className.metatable['p'] = parents
	lua_pushnumber(luaState, parents);
	lua_rawseti(luaState, metatable, 'p');

	// className.metatable['t'] = type
	if (className == "Item") {
		lua_pushnumber(luaState, tfs::lua::LuaData_Item);
	} else if (className == "Container") {
		lua_pushnumber(luaState, tfs::lua::LuaData_Container);
	} else if (className == "Teleport") {
		lua_pushnumber(luaState, tfs::lua::LuaData_Teleport);
	} else if (className == "Podium") {
		lua_pushnumber(luaState, tfs::lua::LuaData_Podium);
	} else if (className == "Player") {
		lua_pushnumber(luaState, tfs::lua::LuaData_Player);
	} else if (className == "Monster") {
		lua_pushnumber(luaState, tfs::lua::LuaData_Monster);
	} else if (className == "Npc") {
		lua_pushnumber(luaState, tfs::lua::LuaData_Npc);
	} else if (className == "Tile") {
		lua_pushnumber(luaState, tfs::lua::LuaData_Tile);
	} else {
		lua_pushnumber(luaState, tfs::lua::LuaData_Unknown);
	}
	lua_rawseti(luaState, metatable, 't');

	// pop className, className.metatable
	lua_pop(luaState, 2);
}

void LuaScriptInterface::registerTable(const std::string& tableName)
{
	// _G[tableName] = {}
	lua_newtable(luaState);
	lua_setglobal(luaState, tableName.c_str());
}

void LuaScriptInterface::registerMethod(const std::string& globalName, const std::string& methodName,
                                        lua_CFunction func)
{
	// globalName.methodName = func
	lua_getglobal(luaState, globalName.c_str());
	lua_pushcfunction(luaState, func);
	lua_setfield(luaState, -2, methodName.c_str());

	// pop globalName
	lua_pop(luaState, 1);
}

void LuaScriptInterface::registerMetaMethod(const std::string& className, const std::string& methodName,
                                            lua_CFunction func)
{
	// className.metatable.methodName = func
	luaL_getmetatable(luaState, className.c_str());
	lua_pushcfunction(luaState, func);
	lua_setfield(luaState, -2, methodName.c_str());

	// pop className.metatable
	lua_pop(luaState, 1);
}

void LuaScriptInterface::registerGlobalMethod(const std::string& functionName, lua_CFunction func)
{
	// _G[functionName] = func
	lua_pushcfunction(luaState, func);
	lua_setglobal(luaState, functionName.c_str());
}

void LuaScriptInterface::registerVariable(const std::string& tableName, const std::string& name, lua_Number value)
{
	// tableName.name = value
	lua_getglobal(luaState, tableName.c_str());
	tfs::lua::setField(luaState, name.c_str(), value);

	// pop tableName
	lua_pop(luaState, 1);
}

void LuaScriptInterface::registerGlobalVariable(const std::string& name, lua_Number value)
{
	// _G[name] = value
	lua_pushnumber(luaState, value);
	lua_setglobal(luaState, name.c_str());
}

void LuaScriptInterface::registerGlobalBoolean(const std::string& name, bool value)
{
	// _G[name] = value
	tfs::lua::pushBoolean(luaState, value);
	lua_setglobal(luaState, name.c_str());
}

namespace {

int luaDoPlayerAddItem(lua_State* L)
{
	// doPlayerAddItem(cid, itemid, <optional: default: 1> count/subtype, <optional: default: 1> canDropOnMap)
	// doPlayerAddItem(cid, itemid, <optional: default: 1> count, <optional: default: 1> canDropOnMap, <optional:
	// default: 1>subtype)
	Player* player = tfs::lua::getPlayer(L, 1);
	if (!player) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_PLAYER_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	uint16_t itemId = tfs::lua::getNumber<uint16_t>(L, 2);
	int32_t count = tfs::lua::getNumber<int32_t>(L, 3, 1);
	bool canDropOnMap = tfs::lua::getBoolean(L, 4, true);
	uint16_t subType = tfs::lua::getNumber<uint16_t>(L, 5, 1);

	const ItemType& it = Item::items[itemId];
	int32_t itemCount;

	auto parameters = lua_gettop(L);
	if (parameters > 4) {
		// subtype already supplied, count then is the amount
		itemCount = std::max<int32_t>(1, count);
	} else if (it.hasSubType()) {
		if (it.stackable) {
			itemCount = static_cast<int32_t>(std::ceil(static_cast<float>(count) / 100));
		} else {
			itemCount = 1;
		}
		subType = count;
	} else {
		itemCount = std::max<int32_t>(1, count);
	}

	while (itemCount > 0) {
		uint16_t stackCount = subType;
		if (it.stackable && stackCount > 100) {
			stackCount = 100;
		}

		Item* newItem = Item::CreateItem(itemId, stackCount);
		if (!newItem) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_ITEM_NOT_FOUND));
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		if (it.stackable) {
			subType -= stackCount;
		}

		ReturnValue ret = g_game->internalPlayerAddItem(player, newItem, canDropOnMap);
		if (ret != RETURNVALUE_NOERROR) {
			delete newItem;
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		if (--itemCount == 0) {
			if (newItem->getParent()) {
				uint32_t uid = tfs::lua::getScriptEnv()->addThing(newItem);
				lua_pushnumber(L, uid);
				return 1;
			} else {
				// stackable item stacked with existing object, newItem will be released
				tfs::lua::pushBoolean(L, false);
				return 1;
			}
		}
	}

	tfs::lua::pushBoolean(L, false);
	return 1;
}

int luaDebugPrint(lua_State* L)
{
	// debugPrint(text)
	reportErrorFunc(L, tfs::lua::getString(L, -1));
	return 0;
}

int luaGetWorldTime(lua_State* L)
{
	// getWorldTime()
	int16_t time = g_game->getWorldTime();
	lua_pushnumber(L, time);
	return 1;
}

int luaGetWorldLight(lua_State* L)
{
	// getWorldLight()
	LightInfo lightInfo = g_game->getWorldLightInfo();
	lua_pushnumber(L, lightInfo.level);
	lua_pushnumber(L, lightInfo.color);
	return 2;
}

int luaSetWorldLight(lua_State* L)
{
	// setWorldLight(level, color)
	if (g_config.getBoolean(ConfigManager::DEFAULT_WORLD_LIGHT)) {
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	LightInfo lightInfo;
	lightInfo.level = tfs::lua::getNumber<uint8_t>(L, 1);
	lightInfo.color = tfs::lua::getNumber<uint8_t>(L, 2);
	g_game->setWorldLightInfo(lightInfo);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaGetWorldUpTime(lua_State* L)
{
	// getWorldUpTime()
	uint64_t uptime = (OTSYS_TIME() - ProtocolStatus::start) / 1000;
	lua_pushnumber(L, uptime);
	return 1;
}

int luaGetSubTypeName(lua_State* L)
{
	// getSubTypeName(subType)
	int32_t subType = tfs::lua::getNumber<int32_t>(L, 1);
	if (subType > 0) {
		tfs::lua::pushString(L, Item::items[subType].name);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

static bool getArea(lua_State* L, std::vector<uint32_t>& vec, uint32_t& rows)
{
	lua_pushnil(L);
	for (rows = 0; lua_next(L, -2) != 0; ++rows) {
		if (!lua_istable(L, -1)) {
			return false;
		}

		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			if (!tfs::lua::isNumber(L, -1)) {
				return false;
			}
			vec.push_back(tfs::lua::getNumber<uint32_t>(L, -1));
			lua_pop(L, 1);
		}

		lua_pop(L, 1);
	}

	lua_pop(L, 1);
	return (rows != 0);
}

int luaCreateCombatArea(lua_State* L)
{
	// createCombatArea({area}, <optional> {extArea})
	tfs::lua::ScriptEnvironment* env = tfs::lua::getScriptEnv();
	if (env->getScriptId() != EVENT_ID_LOADING) {
		reportErrorFunc(L, "This function can only be used while loading the script.");
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	uint32_t areaId = g_luaEnvironment->createAreaObject(env->getScriptInterface());
	AreaCombat* area = g_luaEnvironment->getAreaObject(areaId);

	int parameters = lua_gettop(L);
	if (parameters >= 2) {
		uint32_t rowsExtArea;
		std::vector<uint32_t> vecExtArea;
		if (!lua_istable(L, 2) || !getArea(L, vecExtArea, rowsExtArea)) {
			reportErrorFunc(L, "Invalid extended area table.");
			tfs::lua::pushBoolean(L, false);
			return 1;
		}
		area->setupExtArea(vecExtArea, rowsExtArea);
	}

	uint32_t rowsArea = 0;
	std::vector<uint32_t> vecArea;
	if (!lua_istable(L, 1) || !getArea(L, vecArea, rowsArea)) {
		reportErrorFunc(L, "Invalid area table.");
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	area->setupArea(vecArea, rowsArea);
	lua_pushnumber(L, areaId);
	return 1;
}

int luaDoAreaCombat(lua_State* L)
{
	// doAreaCombat(cid, type, pos, area, min, max, effect[, origin = ORIGIN_SPELL[, blockArmor = false[, blockShield =
	// false[, ignoreResistances = false]]]])
	Creature* creature = tfs::lua::getCreature(L, 1);
	if (!creature && (!tfs::lua::isNumber(L, 1) || tfs::lua::getNumber<uint32_t>(L, 1) != 0)) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	uint32_t areaId = tfs::lua::getNumber<uint32_t>(L, 4);
	const AreaCombat* area = g_luaEnvironment->getAreaObject(areaId);
	if (area || areaId == 0) {
		CombatType_t combatType = tfs::lua::getNumber<CombatType_t>(L, 2);

		CombatParams params;
		params.combatType = combatType;
		params.impactEffect = tfs::lua::getNumber<uint8_t>(L, 7);
		params.blockedByArmor = tfs::lua::getBoolean(L, 8, false);
		params.blockedByShield = tfs::lua::getBoolean(L, 9, false);
		params.ignoreResistances = tfs::lua::getBoolean(L, 10, false);

		CombatDamage damage;
		damage.origin = tfs::lua::getNumber<CombatOrigin>(L, 8, ORIGIN_SPELL);
		damage.primary.type = combatType;
		damage.primary.value = normal_random(tfs::lua::getNumber<int32_t>(L, 6), tfs::lua::getNumber<int32_t>(L, 5));

		Combat::doAreaCombat(creature, tfs::lua::getPosition(L, 3), area, damage, params);
		tfs::lua::pushBoolean(L, true);
	} else {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_AREA_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
	}
	return 1;
}

int luaDoTargetCombat(lua_State* L)
{
	// doTargetCombat(cid, target, type, min, max, effect[, origin = ORIGIN_SPELL[, blockArmor = false[, blockShield =
	// false[, ignoreResistances = false]]]])
	Creature* creature = tfs::lua::getCreature(L, 1);
	if (!creature && (!tfs::lua::isNumber(L, 1) || tfs::lua::getNumber<uint32_t>(L, 1) != 0)) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	Creature* target = tfs::lua::getCreature(L, 2);
	if (!target) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	CombatType_t combatType = tfs::lua::getNumber<CombatType_t>(L, 3);

	CombatParams params;
	params.combatType = combatType;
	params.impactEffect = tfs::lua::getNumber<uint8_t>(L, 6);
	params.blockedByArmor = tfs::lua::getBoolean(L, 8, false);
	params.blockedByShield = tfs::lua::getBoolean(L, 9, false);
	params.ignoreResistances = tfs::lua::getBoolean(L, 10, false);

	CombatDamage damage;
	damage.origin = tfs::lua::getNumber<CombatOrigin>(L, 7, ORIGIN_SPELL);
	damage.primary.type = combatType;
	damage.primary.value = normal_random(tfs::lua::getNumber<int32_t>(L, 4), tfs::lua::getNumber<int32_t>(L, 5));

	Combat::doTargetCombat(creature, target, damage, params);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaDoChallengeCreature(lua_State* L)
{
	// doChallengeCreature(cid, target[, force = false])
	Creature* creature = tfs::lua::getCreature(L, 1);
	if (!creature) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	Creature* target = tfs::lua::getCreature(L, 2);
	if (!target) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	target->challengeCreature(creature, tfs::lua::getBoolean(L, 3, false));
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaIsValidUID(lua_State* L)
{
	// isValidUID(uid)
	tfs::lua::pushBoolean(L, tfs::lua::getScriptEnv()->getThingByUID(tfs::lua::getNumber<uint32_t>(L, -1)) != nullptr);
	return 1;
}

int luaIsDepot(lua_State* L)
{
	// isDepot(uid)
	Container* container = tfs::lua::getScriptEnv()->getContainerByUID(tfs::lua::getNumber<uint32_t>(L, -1));
	tfs::lua::pushBoolean(L, container && container->getDepotLocker());
	return 1;
}

int luaIsMoveable(lua_State* L)
{
	// isMoveable(uid)
	// isMovable(uid)
	Thing* thing = tfs::lua::getScriptEnv()->getThingByUID(tfs::lua::getNumber<uint32_t>(L, -1));
	tfs::lua::pushBoolean(L, thing && thing->isPushable());
	return 1;
}

int luaDoAddContainerItem(lua_State* L)
{
	// doAddContainerItem(uid, itemid, <optional> count/subtype)
	uint32_t uid = tfs::lua::getNumber<uint32_t>(L, 1);

	tfs::lua::ScriptEnvironment* env = tfs::lua::getScriptEnv();
	Container* container = env->getContainerByUID(uid);
	if (!container) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CONTAINER_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	uint16_t itemId = tfs::lua::getNumber<uint16_t>(L, 2);
	const ItemType& it = Item::items[itemId];

	int32_t itemCount = 1;
	int32_t subType = 1;
	uint32_t count = tfs::lua::getNumber<uint32_t>(L, 3, 1);

	if (it.hasSubType()) {
		if (it.stackable) {
			itemCount = static_cast<int32_t>(std::ceil(static_cast<float>(count) / 100));
		}

		subType = count;
	} else {
		itemCount = std::max<int32_t>(1, count);
	}

	while (itemCount > 0) {
		int32_t stackCount = std::min<int32_t>(100, subType);
		Item* newItem = Item::CreateItem(itemId, stackCount);
		if (!newItem) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_ITEM_NOT_FOUND));
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		if (it.stackable) {
			subType -= stackCount;
		}

		ReturnValue ret = g_game->internalAddItem(container, newItem);
		if (ret != RETURNVALUE_NOERROR) {
			delete newItem;
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		if (--itemCount == 0) {
			if (newItem->getParent()) {
				lua_pushnumber(L, env->addThing(newItem));
			} else {
				// stackable item stacked with existing object, newItem will be released
				tfs::lua::pushBoolean(L, false);
			}
			return 1;
		}
	}

	tfs::lua::pushBoolean(L, false);
	return 1;
}

int luaGetDepotId(lua_State* L)
{
	// getDepotId(uid)
	uint32_t uid = tfs::lua::getNumber<uint32_t>(L, -1);

	Container* container = tfs::lua::getScriptEnv()->getContainerByUID(uid);
	if (!container) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CONTAINER_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	DepotLocker* depotLocker = container->getDepotLocker();
	if (!depotLocker) {
		reportErrorFunc(L, "Depot not found");
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	lua_pushnumber(L, depotLocker->getDepotId());
	return 1;
}

int luaAddEvent(lua_State* L)
{
	// addEvent(callback, delay, ...)
	int parameters = lua_gettop(L);
	if (parameters < 2) {
		reportErrorFunc(L, fmt::format("Not enough parameters: {:d}.", parameters));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	if (!lua_isfunction(L, 1)) {
		reportErrorFunc(L, "callback parameter should be a function.");
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	if (!tfs::lua::isNumber(L, 2)) {
		reportErrorFunc(L, "delay parameter should be a number.");
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	if (g_config.getBoolean(ConfigManager::WARN_UNSAFE_SCRIPTS) ||
	    g_config.getBoolean(ConfigManager::CONVERT_UNSAFE_SCRIPTS)) {
		std::vector<std::pair<int32_t, tfs::lua::LuaDataType>> indexes;
		for (int i = 3; i <= parameters; ++i) {
			if (lua_getmetatable(L, i) == 0) {
				continue;
			}
			lua_rawgeti(L, -1, 't');

			auto type = tfs::lua::getNumber<tfs::lua::LuaDataType>(L, -1);
			if (type != tfs::lua::LuaData_Unknown && type != tfs::lua::LuaData_Tile) {
				indexes.push_back({i, type});
			}
			lua_pop(L, 2);
		}

		if (!indexes.empty()) {
			if (g_config.getBoolean(ConfigManager::WARN_UNSAFE_SCRIPTS)) {
				bool plural = indexes.size() > 1;

				std::string warningString = "Argument";
				if (plural) {
					warningString += 's';
				}

				for (const auto& entry : indexes) {
					if (entry == indexes.front()) {
						warningString += ' ';
					} else if (entry == indexes.back()) {
						warningString += " and ";
					} else {
						warningString += ", ";
					}
					warningString += '#';
					warningString += std::to_string(entry.first);
				}

				if (plural) {
					warningString += " are unsafe";
				} else {
					warningString += " is unsafe";
				}

				reportErrorFunc(L, warningString);
			}

			if (g_config.getBoolean(ConfigManager::CONVERT_UNSAFE_SCRIPTS)) {
				for (const auto& entry : indexes) {
					switch (entry.second) {
						case tfs::lua::LuaData_Item:
						case tfs::lua::LuaData_Container:
						case tfs::lua::LuaData_Teleport:
						case tfs::lua::LuaData_Podium: {
							lua_getglobal(L, "Item");
							lua_getfield(L, -1, "getUniqueId");
							break;
						}
						case tfs::lua::LuaData_Player:
						case tfs::lua::LuaData_Monster:
						case tfs::lua::LuaData_Npc: {
							lua_getglobal(L, "Creature");
							lua_getfield(L, -1, "getId");
							break;
						}
						default:
							break;
					}
					lua_replace(L, -2);
					lua_pushvalue(L, entry.first);
					lua_call(L, 1, 1);
					lua_replace(L, entry.first);
				}
			}
		}
	}

	LuaTimerEventDesc eventDesc;
	eventDesc.parameters.reserve(parameters -
	                             2); // safe to use -2 since we garanteed that there is at least two parameters
	for (int i = 0; i < parameters - 2; ++i) {
		eventDesc.parameters.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
	}

	uint32_t delay = std::max<uint32_t>(100, tfs::lua::getNumber<uint32_t>(L, 2));
	lua_pop(L, 1);

	eventDesc.function = luaL_ref(L, LUA_REGISTRYINDEX);
	eventDesc.scriptId = tfs::lua::getScriptEnv()->getScriptId();

	auto& lastTimerEventId = g_luaEnvironment->lastEventTimerId;
	eventDesc.eventId = g_scheduler.addEvent(
	    createSchedulerTask(delay, [=]() { g_luaEnvironment->executeTimerEvent(lastTimerEventId); }));

	g_luaEnvironment->timerEvents.emplace(lastTimerEventId, std::move(eventDesc));
	lua_pushnumber(L, lastTimerEventId++);
	return 1;
}

int luaStopEvent(lua_State* L)
{
	// stopEvent(eventid)
	uint32_t eventId = tfs::lua::getNumber<uint32_t>(L, 1);

	auto& timerEvents = g_luaEnvironment->timerEvents;
	auto it = timerEvents.find(eventId);
	if (it == timerEvents.end()) {
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	LuaTimerEventDesc timerEventDesc = std::move(it->second);
	timerEvents.erase(it);

	g_scheduler.stopEvent(timerEventDesc.eventId);
	luaL_unref(L, LUA_REGISTRYINDEX, timerEventDesc.function);

	for (auto parameter : timerEventDesc.parameters) {
		luaL_unref(L, LUA_REGISTRYINDEX, parameter);
	}

	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaSaveServer(lua_State* L)
{
	g_game->saveGameState();
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaCleanMap(lua_State* L)
{
	lua_pushnumber(L, g_game->map.clean());
	return 1;
}

int luaIsInWar(lua_State* L)
{
	// isInWar(cid, target)
	Player* player = tfs::lua::getPlayer(L, 1);
	if (!player) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_PLAYER_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	Player* targetPlayer = tfs::lua::getPlayer(L, 2);
	if (!targetPlayer) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_PLAYER_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	tfs::lua::pushBoolean(L, player->isInWar(targetPlayer));
	return 1;
}

int luaGetWaypointPositionByName(lua_State* L)
{
	// getWaypointPositionByName(name)
	auto& waypoints = g_game->map.waypoints;

	auto it = waypoints.find(tfs::lua::getString(L, -1));
	if (it != waypoints.end()) {
		tfs::lua::pushPosition(L, it->second);
	} else {
		tfs::lua::pushBoolean(L, false);
	}
	return 1;
}

int luaSendChannelMessage(lua_State* L)
{
	// sendChannelMessage(channelId, type, message)
	uint32_t channelId = tfs::lua::getNumber<uint32_t>(L, 1);
	ChatChannel* channel = g_chat->getChannelById(channelId);
	if (!channel) {
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	SpeakClasses type = tfs::lua::getNumber<SpeakClasses>(L, 2);
	std::string message = tfs::lua::getString(L, 3);
	channel->sendToAll(message, type);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaSendGuildChannelMessage(lua_State* L)
{
	// sendGuildChannelMessage(guildId, type, message)
	uint32_t guildId = tfs::lua::getNumber<uint32_t>(L, 1);
	ChatChannel* channel = g_chat->getGuildChannelById(guildId);
	if (!channel) {
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	SpeakClasses type = tfs::lua::getNumber<SpeakClasses>(L, 2);
	std::string message = tfs::lua::getString(L, 3);
	channel->sendToAll(message, type);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaIsScriptsInterface(lua_State* L)
{
	// isScriptsInterface()
	if (tfs::lua::getScriptEnv()->getScriptInterface() == &g_scripts->getScriptInterface()) {
		tfs::lua::pushBoolean(L, true);
	} else {
		reportErrorFunc(L, "EventCallback: can only be called inside (data/scripts/)");
		tfs::lua::pushBoolean(L, false);
	}
	return 1;
}

#ifndef LUAJIT_VERSION

int luaBitNot(lua_State* L)
{
	lua_pushnumber(L, ~tfs::lua::getNumber<uint32_t>(L, -1));
	return 1;
}

#define MULTIOP(name, op) \
	int luaBit##name(lua_State* L) \
	{ \
		int n = lua_gettop(L); \
		uint32_t w = tfs::lua::getNumber<uint32_t>(L, -1); \
		for (int i = 1; i < n; ++i) w op tfs::lua::getNumber<uint32_t>(L, i); \
		lua_pushnumber(L, w); \
		return 1; \
	}

MULTIOP(And, &=)
MULTIOP(Or, |=)
MULTIOP(Xor, ^=)

#define SHIFTOP(name, op) \
	int luaBit##name(lua_State* L) \
	{ \
		uint32_t n1 = tfs::lua::getNumber<uint32_t>(L, 1), n2 = tfs::lua::getNumber<uint32_t>(L, 2); \
		lua_pushnumber(L, (n1 op n2)); \
		return 1; \
	}

SHIFTOP(LeftShift, <<)
SHIFTOP(RightShift, >>)

#endif

int luaConfigManagerGetString(lua_State* L)
{
	tfs::lua::pushString(L, g_config.getString(tfs::lua::getNumber<ConfigManager::string_config_t>(L, -1)));
	return 1;
}

int luaConfigManagerGetNumber(lua_State* L)
{
	lua_pushnumber(L, g_config.getNumber(tfs::lua::getNumber<ConfigManager::integer_config_t>(L, -1)));
	return 1;
}

int luaConfigManagerGetBoolean(lua_State* L)
{
	tfs::lua::pushBoolean(L, g_config.getBoolean(tfs::lua::getNumber<ConfigManager::boolean_config_t>(L, -1)));
	return 1;
}

int luaDatabaseExecute(lua_State* L)
{
	tfs::lua::pushBoolean(L, Database::getInstance().executeQuery(tfs::lua::getString(L, -1)));
	return 1;
}

int luaDatabaseAsyncExecute(lua_State* L)
{
	std::function<void(DBResult_ptr, bool)> callback;
	if (lua_gettop(L) > 1) {
		int32_t ref = luaL_ref(L, LUA_REGISTRYINDEX);
		auto scriptId = tfs::lua::getScriptEnv()->getScriptId();
		callback = [ref, scriptId](DBResult_ptr, bool success) {
			lua_State* luaState = g_luaEnvironment->getLuaState();
			if (!luaState) {
				return;
			}

			if (!tfs::lua::reserveScriptEnv()) {
				luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
				return;
			}

			lua_rawgeti(luaState, LUA_REGISTRYINDEX, ref);
			tfs::lua::pushBoolean(luaState, success);
			auto env = tfs::lua::getScriptEnv();
			env->setScriptId(scriptId, g_luaEnvironment);
			g_luaEnvironment->callFunction(1);

			luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
		};
	}
	g_databaseTasks.addTask(tfs::lua::getString(L, -1), callback);
	return 0;
}

int luaDatabaseStoreQuery(lua_State* L)
{
	if (DBResult_ptr res = Database::getInstance().storeQuery(tfs::lua::getString(L, -1))) {
		lua_pushnumber(L, tfs::lua::ScriptEnvironment::addResult(res));
	} else {
		tfs::lua::pushBoolean(L, false);
	}
	return 1;
}

int luaDatabaseAsyncStoreQuery(lua_State* L)
{
	std::function<void(DBResult_ptr, bool)> callback;
	if (lua_gettop(L) > 1) {
		int32_t ref = luaL_ref(L, LUA_REGISTRYINDEX);
		auto scriptId = tfs::lua::getScriptEnv()->getScriptId();
		callback = [ref, scriptId](DBResult_ptr result, bool) {
			lua_State* luaState = g_luaEnvironment->getLuaState();
			if (!luaState) {
				return;
			}

			if (!tfs::lua::reserveScriptEnv()) {
				luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
				return;
			}

			lua_rawgeti(luaState, LUA_REGISTRYINDEX, ref);
			if (result) {
				lua_pushnumber(luaState, tfs::lua::ScriptEnvironment::addResult(result));
			} else {
				tfs::lua::pushBoolean(luaState, false);
			}
			auto env = tfs::lua::getScriptEnv();
			env->setScriptId(scriptId, g_luaEnvironment);
			g_luaEnvironment->callFunction(1);

			luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
		};
	}
	g_databaseTasks.addTask(tfs::lua::getString(L, -1), callback, true);
	return 0;
}

int luaDatabaseEscapeString(lua_State* L)
{
	tfs::lua::pushString(L, Database::getInstance().escapeString(tfs::lua::getString(L, -1)));
	return 1;
}

int luaDatabaseEscapeBlob(lua_State* L)
{
	uint32_t length = tfs::lua::getNumber<uint32_t>(L, 2);
	tfs::lua::pushString(L, Database::getInstance().escapeBlob(tfs::lua::getString(L, 1).c_str(), length));
	return 1;
}

int luaDatabaseLastInsertId(lua_State* L)
{
	lua_pushnumber(L, Database::getInstance().getLastInsertId());
	return 1;
}

int luaDatabaseTableExists(lua_State* L)
{
	tfs::lua::pushBoolean(L, DatabaseManager::tableExists(tfs::lua::getString(L, -1)));
	return 1;
}

int luaResultGetNumber(lua_State* L)
{
	DBResult_ptr res = tfs::lua::ScriptEnvironment::getResultByID(tfs::lua::getNumber<uint32_t>(L, 1));
	if (!res) {
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	const std::string& s = tfs::lua::getString(L, 2);
	lua_pushnumber(L, res->getNumber<int64_t>(s));
	return 1;
}

int luaResultGetString(lua_State* L)
{
	DBResult_ptr res = tfs::lua::ScriptEnvironment::getResultByID(tfs::lua::getNumber<uint32_t>(L, 1));
	if (!res) {
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	const std::string& s = tfs::lua::getString(L, 2);
	tfs::lua::pushString(L, res->getString(s));
	return 1;
}

int luaResultGetStream(lua_State* L)
{
	DBResult_ptr res = tfs::lua::ScriptEnvironment::getResultByID(tfs::lua::getNumber<uint32_t>(L, 1));
	if (!res) {
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	unsigned long length;
	const char* stream = res->getStream(tfs::lua::getString(L, 2), length);
	lua_pushlstring(L, stream, length);
	lua_pushnumber(L, length);
	return 2;
}

int luaResultNext(lua_State* L)
{
	DBResult_ptr res = tfs::lua::ScriptEnvironment::getResultByID(tfs::lua::getNumber<uint32_t>(L, -1));
	if (!res) {
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	tfs::lua::pushBoolean(L, res->next());
	return 1;
}

int luaResultFree(lua_State* L)
{
	tfs::lua::pushBoolean(L, tfs::lua::ScriptEnvironment::removeResult(tfs::lua::getNumber<uint32_t>(L, -1)));
	return 1;
}

// _G
int luaIsType(lua_State* L)
{
	// isType(derived, base)
	lua_getmetatable(L, -2);
	lua_getmetatable(L, -2);

	lua_rawgeti(L, -2, 'p');
	uint_fast8_t parentsB = tfs::lua::getNumber<uint_fast8_t>(L, 1);

	lua_rawgeti(L, -3, 'h');
	size_t hashB = tfs::lua::getNumber<size_t>(L, 1);

	lua_rawgeti(L, -3, 'p');
	uint_fast8_t parentsA = tfs::lua::getNumber<uint_fast8_t>(L, 1);
	for (uint_fast8_t i = parentsA; i < parentsB; ++i) {
		lua_getfield(L, -3, "__index");
		lua_replace(L, -4);
	}

	lua_rawgeti(L, -4, 'h');
	size_t hashA = tfs::lua::getNumber<size_t>(L, 1);

	tfs::lua::pushBoolean(L, hashA == hashB);
	return 1;
}

int luaRawGetMetatable(lua_State* L)
{
	// rawgetmetatable(metatableName)
	luaL_getmetatable(L, tfs::lua::getString(L, 1).c_str());
	return 1;
}

// os
int luaSystemTime(lua_State* L)
{
	// os.mtime()
	lua_pushnumber(L, OTSYS_TIME());
	return 1;
}

// table
int luaTableCreate(lua_State* L)
{
	// table.create(arrayLength, keyLength)
	lua_createtable(L, tfs::lua::getNumber<int32_t>(L, 1), tfs::lua::getNumber<int32_t>(L, 2));
	return 1;
}

int luaTablePack(lua_State* L)
{
	// table.pack(...)
	int i;
	int n = lua_gettop(L);     /* number of elements to pack */
	lua_createtable(L, n, 1);  /* create result table */
	lua_insert(L, 1);          /* put it at index 1 */
	for (i = n; i >= 1; i--) { /* assign elements */
		lua_rawseti(L, 1, i);
	}
	if (luaL_callmeta(L, -1, "__index") != 0) {
		lua_replace(L, -2);
	}
	lua_pushinteger(L, n);
	lua_setfield(L, 1, "n"); /* t.n = number of elements */
	return 1;                /* return table */
}

// Game
int luaGameGetSpectators(lua_State* L)
{
	// Game.getSpectators(position[, multifloor = false[, onlyPlayer = false[, minRangeX = 0[, maxRangeX = 0[, minRangeY
	// = 0[, maxRangeY = 0]]]]]])
	const Position& position = tfs::lua::getPosition(L, 1);
	bool multifloor = tfs::lua::getBoolean(L, 2, false);
	bool onlyPlayers = tfs::lua::getBoolean(L, 3, false);
	int32_t minRangeX = tfs::lua::getNumber<int32_t>(L, 4, 0);
	int32_t maxRangeX = tfs::lua::getNumber<int32_t>(L, 5, 0);
	int32_t minRangeY = tfs::lua::getNumber<int32_t>(L, 6, 0);
	int32_t maxRangeY = tfs::lua::getNumber<int32_t>(L, 7, 0);

	SpectatorVec spectators;
	g_game->map.getSpectators(spectators, position, multifloor, onlyPlayers, minRangeX, maxRangeX, minRangeY,
	                          maxRangeY);

	lua_createtable(L, spectators.size(), 0);

	int index = 0;
	for (Creature* creature : spectators) {
		tfs::lua::pushUserdata<Creature>(L, creature);
		tfs::lua::setCreatureMetatable(L, -1, creature);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaGameGetPlayers(lua_State* L)
{
	// Game.getPlayers()
	lua_createtable(L, g_game->getPlayersOnline(), 0);

	int index = 0;
	for (const auto& playerEntry : g_game->getPlayers()) {
		tfs::lua::pushUserdata<Player>(L, playerEntry.second);
		tfs::lua::setMetatable(L, -1, "Player");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaGameGetNpcs(lua_State* L)
{
	// Game.getNpcs()
	lua_createtable(L, g_game->getNpcsOnline(), 0);

	int index = 0;
	for (const auto& npcEntry : g_game->getNpcs()) {
		tfs::lua::pushUserdata<Npc>(L, npcEntry.second);
		tfs::lua::setMetatable(L, -1, "Npc");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaGameGetMonsters(lua_State* L)
{
	// Game.getMonsters()
	lua_createtable(L, g_game->getMonstersOnline(), 0);

	int index = 0;
	for (const auto& monsterEntry : g_game->getMonsters()) {
		tfs::lua::pushUserdata<Monster>(L, monsterEntry.second);
		tfs::lua::setMetatable(L, -1, "Monster");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaGameLoadMap(lua_State* L)
{
	// Game.loadMap(path)
	const std::string& path = tfs::lua::getString(L, 1);
	g_dispatcher.addTask(createTask([path]() {
		try {
			g_game->loadMap(path);
		} catch (const std::exception& e) {
			// FIXME: Should only catch some exceptions
			std::cout << "[Error - luaGameLoadMap] Failed to load map: " << e.what() << std::endl;
		}
	}));
	return 0;
}

int luaGameGetExperienceStage(lua_State* L)
{
	// Game.getExperienceStage(level)
	uint32_t level = tfs::lua::getNumber<uint32_t>(L, 1);
	lua_pushnumber(L, g_config.getExperienceStage(level));
	return 1;
}

int luaGameGetExperienceForLevel(lua_State* L)
{
	// Game.getExperienceForLevel(level)
	const uint32_t level = tfs::lua::getNumber<uint32_t>(L, 1);
	if (level == 0) {
		lua_pushnumber(L, 0);
	} else {
		lua_pushnumber(L, Player::getExpForLevel(level));
	}
	return 1;
}

int luaGameGetMonsterCount(lua_State* L)
{
	// Game.getMonsterCount()
	lua_pushnumber(L, g_game->getMonstersOnline());
	return 1;
}

int luaGameGetPlayerCount(lua_State* L)
{
	// Game.getPlayerCount()
	lua_pushnumber(L, g_game->getPlayersOnline());
	return 1;
}

int luaGameGetNpcCount(lua_State* L)
{
	// Game.getNpcCount()
	lua_pushnumber(L, g_game->getNpcsOnline());
	return 1;
}

int luaGameGetMonsterTypes(lua_State* L)
{
	// Game.getMonsterTypes()
	auto& type = g_monsters.monsters;
	lua_createtable(L, type.size(), 0);

	for (auto& mType : type) {
		tfs::lua::pushUserdata<MonsterType>(L, &mType.second);
		tfs::lua::setMetatable(L, -1, "MonsterType");
		lua_setfield(L, -2, mType.first.c_str());
	}
	return 1;
}

int luaGameGetCurrencyItems(lua_State* L)
{
	// Game.getCurrencyItems()
	const auto& currencyItems = Item::items.currencyItems;
	size_t size = currencyItems.size();
	lua_createtable(L, size, 0);

	for (const auto& it : currencyItems) {
		const ItemType& itemType = Item::items[it.second];
		tfs::lua::pushUserdata<const ItemType>(L, &itemType);
		tfs::lua::setMetatable(L, -1, "ItemType");
		lua_rawseti(L, -2, size--);
	}
	return 1;
}

int luaGameGetItemTypeByClientId(lua_State* L)
{
	// Game.getItemTypeByClientId(clientId)
	uint16_t spriteId = tfs::lua::getNumber<uint16_t>(L, 1);
	const ItemType& itemType = Item::items.getItemIdByClientId(spriteId);
	if (itemType.id != 0) {
		tfs::lua::pushUserdata<const ItemType>(L, &itemType);
		tfs::lua::setMetatable(L, -1, "ItemType");
	} else {
		lua_pushnil(L);
	}

	return 1;
}

int luaGameGetMountIdByLookType(lua_State* L)
{
	// Game.getMountIdByLookType(lookType)
	Mount* mount = nullptr;
	if (tfs::lua::isNumber(L, 1)) {
		mount = g_game->mounts.getMountByClientID(tfs::lua::getNumber<uint16_t>(L, 1));
	}

	if (mount) {
		lua_pushnumber(L, mount->id);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGameGetTowns(lua_State* L)
{
	// Game.getTowns()
	const auto& towns = g_game->map.towns.getTowns();
	lua_createtable(L, towns.size(), 0);

	int index = 0;
	for (auto townEntry : towns) {
		tfs::lua::pushUserdata<Town>(L, townEntry.second);
		tfs::lua::setMetatable(L, -1, "Town");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaGameGetHouses(lua_State* L)
{
	// Game.getHouses()
	const auto& houses = g_game->map.houses.getHouses();
	lua_createtable(L, houses.size(), 0);

	int index = 0;
	for (auto houseEntry : houses) {
		tfs::lua::pushUserdata<House>(L, houseEntry.second);
		tfs::lua::setMetatable(L, -1, "House");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaGameGetOutfits(lua_State* L)
{
	// Game.getOutfits(playerSex)
	if (!tfs::lua::isNumber(L, 1)) {
		lua_pushnil(L);
		return 1;
	}

	PlayerSex_t playerSex = tfs::lua::getNumber<PlayerSex_t>(L, 1);
	if (playerSex > PLAYERSEX_LAST) {
		lua_pushnil(L);
		return 1;
	}

	const auto& outfits = Outfits::getInstance().getOutfits(playerSex);
	lua_createtable(L, outfits.size(), 0);

	int index = 0;
	for (const auto& outfit : outfits) {
		tfs::lua::pushOutfitClass(L, &outfit);
		lua_rawseti(L, -2, ++index);
	}

	return 1;
}

int luaGameGetMounts(lua_State* L)
{
	// Game.getMounts()
	const auto& mounts = g_game->mounts.getMounts();
	lua_createtable(L, mounts.size(), 0);

	int index = 0;
	for (const auto& mount : mounts) {
		tfs::lua::pushMount(L, &mount);
		lua_rawseti(L, -2, ++index);
	}

	return 1;
}

int luaGameGetGameState(lua_State* L)
{
	// Game.getGameState()
	lua_pushnumber(L, g_game->getGameState());
	return 1;
}

int luaGameSetGameState(lua_State* L)
{
	// Game.setGameState(state)
	GameState_t state = tfs::lua::getNumber<GameState_t>(L, 1);
	g_game->setGameState(state);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaGameGetWorldType(lua_State* L)
{
	// Game.getWorldType()
	lua_pushnumber(L, g_game->getWorldType());
	return 1;
}

int luaGameSetWorldType(lua_State* L)
{
	// Game.setWorldType(type)
	WorldType_t type = tfs::lua::getNumber<WorldType_t>(L, 1);
	g_game->setWorldType(type);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaGameGetReturnMessage(lua_State* L)
{
	// Game.getReturnMessage(value)
	ReturnValue value = tfs::lua::getNumber<ReturnValue>(L, 1);
	tfs::lua::pushString(L, getReturnMessage(value));
	return 1;
}

int luaGameGetItemAttributeByName(lua_State* L)
{
	// Game.getItemAttributeByName(name)
	lua_pushnumber(L, stringToItemAttribute(tfs::lua::getString(L, 1)));
	return 1;
}

int luaGameCreateItem(lua_State* L)
{
	// Game.createItem(itemId[, count[, position]])
	uint16_t count = tfs::lua::getNumber<uint16_t>(L, 2, 1);
	uint16_t id;
	if (tfs::lua::isNumber(L, 1)) {
		id = tfs::lua::getNumber<uint16_t>(L, 1);
	} else {
		id = Item::items.getItemIdByName(tfs::lua::getString(L, 1));
		if (id == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	const ItemType& it = Item::items[id];
	if (it.stackable) {
		count = std::min<uint16_t>(count, 100);
	}

	Item* item = Item::CreateItem(id, count);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	if (lua_gettop(L) >= 3) {
		const Position& position = tfs::lua::getPosition(L, 3);
		Tile* tile = g_game->map.getTile(position);
		if (!tile) {
			delete item;
			lua_pushnil(L);
			return 1;
		}

		g_game->internalAddItem(tile, item, INDEX_WHEREEVER, FLAG_NOLIMIT);
	} else {
		tfs::lua::getScriptEnv()->addTempItem(item);
		item->setParent(VirtualCylinder::virtualCylinder);
	}

	tfs::lua::pushUserdata<Item>(L, item);
	tfs::lua::setItemMetatable(L, -1, item);
	return 1;
}

int luaGameCreateContainer(lua_State* L)
{
	// Game.createContainer(itemId, size[, position])
	uint16_t size = tfs::lua::getNumber<uint16_t>(L, 2);
	uint16_t id;
	if (tfs::lua::isNumber(L, 1)) {
		id = tfs::lua::getNumber<uint16_t>(L, 1);
	} else {
		id = Item::items.getItemIdByName(tfs::lua::getString(L, 1));
		if (id == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	Container* container = Item::CreateItemAsContainer(id, size);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	if (lua_gettop(L) >= 3) {
		const Position& position = tfs::lua::getPosition(L, 3);
		Tile* tile = g_game->map.getTile(position);
		if (!tile) {
			delete container;
			lua_pushnil(L);
			return 1;
		}

		g_game->internalAddItem(tile, container, INDEX_WHEREEVER, FLAG_NOLIMIT);
	} else {
		tfs::lua::getScriptEnv()->addTempItem(container);
		container->setParent(VirtualCylinder::virtualCylinder);
	}

	tfs::lua::pushUserdata<Container>(L, container);
	tfs::lua::setMetatable(L, -1, "Container");
	return 1;
}

int luaGameCreateMonster(lua_State* L)
{
	// Game.createMonster(monsterName, position[, extended = false[, force = false[, magicEffect = CONST_ME_TELEPORT]]])
	Monster* monster = Monster::createMonster(tfs::lua::getString(L, 1));
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	const Position& position = tfs::lua::getPosition(L, 2);
	bool extended = tfs::lua::getBoolean(L, 3, false);
	bool force = tfs::lua::getBoolean(L, 4, false);
	MagicEffectClasses magicEffect = tfs::lua::getNumber<MagicEffectClasses>(L, 5, CONST_ME_TELEPORT);
	if (g_events->eventMonsterOnSpawn(monster, position, false, true) || force) {
		if (g_game->placeCreature(monster, position, extended, force, magicEffect)) {
			tfs::lua::pushUserdata<Monster>(L, monster);
			tfs::lua::setMetatable(L, -1, "Monster");
		} else {
			delete monster;
			lua_pushnil(L);
		}
	} else {
		delete monster;
		lua_pushnil(L);
	}
	return 1;
}

int luaGameCreateNpc(lua_State* L)
{
	// Game.createNpc(npcName, position[, extended = false[, force = false[, magicEffect = CONST_ME_TELEPORT]]])
	Npc* npc = Npc::createNpc(tfs::lua::getString(L, 1));
	if (!npc) {
		lua_pushnil(L);
		return 1;
	}

	const Position& position = tfs::lua::getPosition(L, 2);
	bool extended = tfs::lua::getBoolean(L, 3, false);
	bool force = tfs::lua::getBoolean(L, 4, false);
	MagicEffectClasses magicEffect = tfs::lua::getNumber<MagicEffectClasses>(L, 5, CONST_ME_TELEPORT);
	if (g_game->placeCreature(npc, position, extended, force, magicEffect)) {
		tfs::lua::pushUserdata<Npc>(L, npc);
		tfs::lua::setMetatable(L, -1, "Npc");
	} else {
		delete npc;
		lua_pushnil(L);
	}
	return 1;
}

int luaGameCreateTile(lua_State* L)
{
	// Game.createTile(x, y, z[, isDynamic = false])
	// Game.createTile(position[, isDynamic = false])
	Position position;
	bool isDynamic;
	if (lua_istable(L, 1)) {
		position = tfs::lua::getPosition(L, 1);
		isDynamic = tfs::lua::getBoolean(L, 2, false);
	} else {
		position.x = tfs::lua::getNumber<uint16_t>(L, 1);
		position.y = tfs::lua::getNumber<uint16_t>(L, 2);
		position.z = tfs::lua::getNumber<uint16_t>(L, 3);
		isDynamic = tfs::lua::getBoolean(L, 4, false);
	}

	Tile* tile = g_game->map.getTile(position);
	if (!tile) {
		if (isDynamic) {
			tile = new DynamicTile(position.x, position.y, position.z);
		} else {
			tile = new StaticTile(position.x, position.y, position.z);
		}

		g_game->map.setTile(position, tile);
	}

	tfs::lua::pushUserdata(L, tile);
	tfs::lua::setMetatable(L, -1, "Tile");
	return 1;
}

int luaGameCreateMonsterType(lua_State* L)
{
	// Game.createMonsterType(name)
	if (tfs::lua::getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "MonsterTypes can only be registered in the Scripts interface.");
		lua_pushnil(L);
		return 1;
	}

	const std::string& name = tfs::lua::getString(L, 1);
	if (name.length() == 0) {
		lua_pushnil(L);
		return 1;
	}

	MonsterType* monsterType = g_monsters.getMonsterType(name, false);
	if (!monsterType) {
		monsterType = &g_monsters.monsters[boost::algorithm::to_lower_copy(name)];
		monsterType->name = name;
		monsterType->nameDescription = "a " + name;
	} else {
		monsterType->info.lootItems.clear();
		monsterType->info.attackSpells.clear();
		monsterType->info.defenseSpells.clear();
		monsterType->info.scripts.clear();
		monsterType->info.thinkEvent = -1;
		monsterType->info.creatureAppearEvent = -1;
		monsterType->info.creatureDisappearEvent = -1;
		monsterType->info.creatureMoveEvent = -1;
		monsterType->info.creatureSayEvent = -1;
	}

	tfs::lua::pushUserdata<MonsterType>(L, monsterType);
	tfs::lua::setMetatable(L, -1, "MonsterType");
	return 1;
}

int luaGameStartRaid(lua_State* L)
{
	// Game.startRaid(raidName)
	const std::string& raidName = tfs::lua::getString(L, 1);

	Raid* raid = g_game->raids.getRaidByName(raidName);
	if (!raid || !raid->isLoaded()) {
		lua_pushnumber(L, RETURNVALUE_NOSUCHRAIDEXISTS);
		return 1;
	}

	if (g_game->raids.getRunning()) {
		lua_pushnumber(L, RETURNVALUE_ANOTHERRAIDISALREADYEXECUTING);
		return 1;
	}

	g_game->raids.setRunning(raid);
	raid->startRaid();
	lua_pushnumber(L, RETURNVALUE_NOERROR);
	return 1;
}

int luaGameGetClientVersion(lua_State* L)
{
	// Game.getClientVersion()
	lua_createtable(L, 0, 3);
	tfs::lua::setField(L, "min", CLIENT_VERSION_MIN);
	tfs::lua::setField(L, "max", CLIENT_VERSION_MAX);
	tfs::lua::setField(L, "string", CLIENT_VERSION_STR);
	return 1;
}

int luaGameReload(lua_State* L)
{
	// Game.reload(reloadType)
	ReloadTypes_t reloadType = tfs::lua::getNumber<ReloadTypes_t>(L, 1);
	if (reloadType == RELOAD_TYPE_GLOBAL) {
		tfs::lua::pushBoolean(L, g_luaEnvironment->loadFile("data/global.lua") == 0);
		tfs::lua::pushBoolean(L, g_scripts->loadScripts("scripts/lib", true, true));
	} else {
		tfs::lua::pushBoolean(L, g_game->reload(reloadType));
	}
	lua_gc(g_luaEnvironment->getLuaState(), LUA_GCCOLLECT, 0);
	return 1;
}

int luaGameGetAccountStorageValue(lua_State* L)
{
	// Game.getAccountStorageValue(accountId, key)
	uint32_t accountId = tfs::lua::getNumber<uint32_t>(L, 1);
	uint32_t key = tfs::lua::getNumber<uint32_t>(L, 2);

	lua_pushnumber(L, g_game->getAccountStorageValue(accountId, key));

	return 1;
}

int luaGameSetAccountStorageValue(lua_State* L)
{
	// Game.setAccountStorageValue(accountId, key, value)
	uint32_t accountId = tfs::lua::getNumber<uint32_t>(L, 1);
	uint32_t key = tfs::lua::getNumber<uint32_t>(L, 2);
	int32_t value = tfs::lua::getNumber<int32_t>(L, 3);

	g_game->setAccountStorageValue(accountId, key, value);
	lua_pushboolean(L, true);

	return 1;
}

int luaGameSaveAccountStorageValues(lua_State* L)
{
	// Game.saveAccountStorageValues()
	lua_pushboolean(L, g_game->saveAccountStorageValues());

	return 1;
}

// Variant
int luaVariantCreate(lua_State* L)
{
	// Variant(number or string or position or thing)
	LuaVariant variant;
	if (lua_isuserdata(L, 2)) {
		if (Thing* thing = tfs::lua::getThing(L, 2)) {
			variant.setTargetPosition(thing->getPosition());
		}
	} else if (lua_istable(L, 2)) {
		variant.setPosition(tfs::lua::getPosition(L, 2));
	} else if (tfs::lua::isNumber(L, 2)) {
		variant.setNumber(tfs::lua::getNumber<uint32_t>(L, 2));
	} else if (lua_isstring(L, 2)) {
		variant.setString(tfs::lua::getString(L, 2));
	}
	tfs::lua::pushVariant(L, variant);
	return 1;
}

int luaVariantGetNumber(lua_State* L)
{
	// Variant:getNumber()
	const LuaVariant& variant = tfs::lua::getVariant(L, 1);
	if (variant.isNumber()) {
		lua_pushnumber(L, variant.getNumber());
	} else {
		lua_pushnumber(L, 0);
	}
	return 1;
}

int luaVariantGetString(lua_State* L)
{
	// Variant:getString()
	const LuaVariant& variant = tfs::lua::getVariant(L, 1);
	if (variant.isString()) {
		tfs::lua::pushString(L, variant.getString());
	} else {
		tfs::lua::pushString(L, std::string());
	}
	return 1;
}

int luaVariantGetPosition(lua_State* L)
{
	// Variant:getPosition()
	const LuaVariant& variant = tfs::lua::getVariant(L, 1);
	if (variant.isPosition()) {
		tfs::lua::pushPosition(L, variant.getPosition());
	} else if (variant.isTargetPosition()) {
		tfs::lua::pushPosition(L, variant.getTargetPosition());
	} else {
		tfs::lua::pushPosition(L, Position());
	}
	return 1;
}

// Position
int luaPositionCreate(lua_State* L)
{
	// Position([x = 0[, y = 0[, z = 0[, stackpos = 0]]]])
	// Position([position])
	if (lua_gettop(L) <= 1) {
		tfs::lua::pushPosition(L, Position());
		return 1;
	}

	int32_t stackpos;
	if (lua_istable(L, 2)) {
		auto&& [position, stackpos] = tfs::lua::getStackPosition(L, 2);
		tfs::lua::pushPosition(L, position, stackpos);
	} else {
		uint16_t x = tfs::lua::getNumber<uint16_t>(L, 2, 0);
		uint16_t y = tfs::lua::getNumber<uint16_t>(L, 3, 0);
		uint8_t z = tfs::lua::getNumber<uint8_t>(L, 4, 0);
		stackpos = tfs::lua::getNumber<int32_t>(L, 5, 0);

		tfs::lua::pushPosition(L, Position(x, y, z), stackpos);
	}
	return 1;
}

int luaPositionAdd(lua_State* L)
{
	// positionValue = position + positionEx
	auto&& [position, stackpos] = tfs::lua::getStackPosition(L, 1);

	Position positionEx;
	if (stackpos == 0) {
		std::tie(positionEx, stackpos) = tfs::lua::getStackPosition(L, 2);
	} else {
		positionEx = tfs::lua::getPosition(L, 2);
	}

	tfs::lua::pushPosition(L, position + positionEx, stackpos);
	return 1;
}

int luaPositionSub(lua_State* L)
{
	// positionValue = position - positionEx
	auto&& [position, stackpos] = tfs::lua::getStackPosition(L, 1);

	Position positionEx;
	if (stackpos == 0) {
		std::tie(positionEx, stackpos) = tfs::lua::getStackPosition(L, 2);
	} else {
		positionEx = tfs::lua::getPosition(L, 2);
	}

	tfs::lua::pushPosition(L, position - positionEx, stackpos);
	return 1;
}

int luaPositionCompare(lua_State* L)
{
	// position == positionEx
	const Position& positionEx = tfs::lua::getPosition(L, 2);
	const Position& position = tfs::lua::getPosition(L, 1);
	tfs::lua::pushBoolean(L, position == positionEx);
	return 1;
}

int luaPositionGetDistance(lua_State* L)
{
	// position:getDistance(positionEx)
	const Position& positionEx = tfs::lua::getPosition(L, 2);
	const Position& position = tfs::lua::getPosition(L, 1);
	lua_pushnumber(L, std::max<int32_t>(std::max<int32_t>(std::abs(Position::getDistanceX(position, positionEx)),
	                                                      std::abs(Position::getDistanceY(position, positionEx))),
	                                    std::abs(Position::getDistanceZ(position, positionEx))));
	return 1;
}

int luaPositionIsSightClear(lua_State* L)
{
	// position:isSightClear(positionEx[, sameFloor = true])
	bool sameFloor = tfs::lua::getBoolean(L, 3, true);
	const Position& positionEx = tfs::lua::getPosition(L, 2);
	const Position& position = tfs::lua::getPosition(L, 1);
	tfs::lua::pushBoolean(L, g_game->isSightClear(position, positionEx, sameFloor));
	return 1;
}

int luaPositionSendMagicEffect(lua_State* L)
{
	// position:sendMagicEffect(magicEffect[, player = nullptr])
	SpectatorVec spectators;
	if (lua_gettop(L) >= 3) {
		Player* player = tfs::lua::getPlayer(L, 3);
		if (player) {
			spectators.emplace_back(player);
		}
	}

	MagicEffectClasses magicEffect = tfs::lua::getNumber<MagicEffectClasses>(L, 2);
	if (magicEffect == CONST_ME_NONE) {
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	const Position& position = tfs::lua::getPosition(L, 1);
	if (!spectators.empty()) {
		Game::addMagicEffect(spectators, position, magicEffect);
	} else {
		g_game->addMagicEffect(position, magicEffect);
	}

	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaPositionSendDistanceEffect(lua_State* L)
{
	// position:sendDistanceEffect(positionEx, distanceEffect[, player = nullptr])
	SpectatorVec spectators;
	if (lua_gettop(L) >= 4) {
		Player* player = tfs::lua::getPlayer(L, 4);
		if (player) {
			spectators.emplace_back(player);
		}
	}

	ShootType_t distanceEffect = tfs::lua::getNumber<ShootType_t>(L, 3);
	const Position& positionEx = tfs::lua::getPosition(L, 2);
	const Position& position = tfs::lua::getPosition(L, 1);
	if (!spectators.empty()) {
		Game::addDistanceEffect(spectators, position, positionEx, distanceEffect);
	} else {
		g_game->addDistanceEffect(position, positionEx, distanceEffect);
	}

	tfs::lua::pushBoolean(L, true);
	return 1;
}

// NetworkMessage
int luaNetworkMessageCreate(lua_State* L)
{
	// NetworkMessage()
	tfs::lua::pushUserdata<NetworkMessage>(L, new NetworkMessage);
	tfs::lua::setMetatable(L, -1, "NetworkMessage");
	return 1;
}

int luaNetworkMessageDelete(lua_State* L)
{
	NetworkMessage** messagePtr = tfs::lua::getRawUserdata<NetworkMessage>(L, 1);
	if (messagePtr && *messagePtr) {
		delete *messagePtr;
		*messagePtr = nullptr;
	}
	return 0;
}

int luaNetworkMessageGetByte(lua_State* L)
{
	// networkMessage:getByte()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		lua_pushnumber(L, message->getByte());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageGetU16(lua_State* L)
{
	// networkMessage:getU16()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		lua_pushnumber(L, message->get<uint16_t>());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageGetU32(lua_State* L)
{
	// networkMessage:getU32()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		lua_pushnumber(L, message->get<uint32_t>());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageGetU64(lua_State* L)
{
	// networkMessage:getU64()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		lua_pushnumber(L, message->get<uint64_t>());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageGetString(lua_State* L)
{
	// networkMessage:getString()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		tfs::lua::pushString(L, message->getString());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageGetPosition(lua_State* L)
{
	// networkMessage:getPosition()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		tfs::lua::pushPosition(L, message->getPosition());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageAddByte(lua_State* L)
{
	// networkMessage:addByte(number)
	uint8_t number = tfs::lua::getNumber<uint8_t>(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addByte(number);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageAddU16(lua_State* L)
{
	// networkMessage:addU16(number)
	uint16_t number = tfs::lua::getNumber<uint16_t>(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->add<uint16_t>(number);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageAddU32(lua_State* L)
{
	// networkMessage:addU32(number)
	uint32_t number = tfs::lua::getNumber<uint32_t>(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->add<uint32_t>(number);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageAddU64(lua_State* L)
{
	// networkMessage:addU64(number)
	uint64_t number = tfs::lua::getNumber<uint64_t>(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->add<uint64_t>(number);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageAddString(lua_State* L)
{
	// networkMessage:addString(string)
	const std::string& string = tfs::lua::getString(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addString(string);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageAddPosition(lua_State* L)
{
	// networkMessage:addPosition(position)
	const Position& position = tfs::lua::getPosition(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addPosition(position);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageAddDouble(lua_State* L)
{
	// networkMessage:addDouble(number)
	double number = tfs::lua::getNumber<double>(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addDouble(number);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageAddItem(lua_State* L)
{
	// networkMessage:addItem(item)
	Item* item = tfs::lua::getUserdata<Item>(L, 2);
	if (!item) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_ITEM_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addItem(item);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageAddItemId(lua_State* L)
{
	// networkMessage:addItemId(itemId)
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (!message) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t itemId;
	if (tfs::lua::isNumber(L, 2)) {
		itemId = tfs::lua::getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	message->addItemId(itemId);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaNetworkMessageReset(lua_State* L)
{
	// networkMessage:reset()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->reset();
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageSeek(lua_State* L)
{
	// networkMessage:seek(position)
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message && tfs::lua::isNumber(L, 2)) {
		tfs::lua::pushBoolean(L, message->setBufferPosition(tfs::lua::getNumber<uint16_t>(L, 2)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageTell(lua_State* L)
{
	// networkMessage:tell()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		lua_pushnumber(L, message->getBufferPosition() - message->INITIAL_BUFFER_POSITION);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageLength(lua_State* L)
{
	// networkMessage:len()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		lua_pushnumber(L, message->getLength());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageSkipBytes(lua_State* L)
{
	// networkMessage:skipBytes(number)
	int16_t number = tfs::lua::getNumber<int16_t>(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->skipBytes(number);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNetworkMessageSendToPlayer(lua_State* L)
{
	// networkMessage:sendToPlayer(player)
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (!message) {
		lua_pushnil(L);
		return 1;
	}

	Player* player = tfs::lua::getPlayer(L, 2);
	if (player) {
		player->sendNetworkMessage(*message);
		tfs::lua::pushBoolean(L, true);
	} else {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_PLAYER_NOT_FOUND));
		lua_pushnil(L);
	}
	return 1;
}

// ModalWindow
int luaModalWindowCreate(lua_State* L)
{
	// ModalWindow(id, title, message)
	const std::string& message = tfs::lua::getString(L, 4);
	const std::string& title = tfs::lua::getString(L, 3);
	uint32_t id = tfs::lua::getNumber<uint32_t>(L, 2);

	tfs::lua::pushUserdata<ModalWindow>(L, new ModalWindow(id, title, message));
	tfs::lua::setMetatable(L, -1, "ModalWindow");
	return 1;
}

int luaModalWindowDelete(lua_State* L)
{
	ModalWindow** windowPtr = tfs::lua::getRawUserdata<ModalWindow>(L, 1);
	if (windowPtr && *windowPtr) {
		delete *windowPtr;
		*windowPtr = nullptr;
	}
	return 0;
}

int luaModalWindowGetId(lua_State* L)
{
	// modalWindow:getId()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		lua_pushnumber(L, window->id);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowGetTitle(lua_State* L)
{
	// modalWindow:getTitle()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		tfs::lua::pushString(L, window->title);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowGetMessage(lua_State* L)
{
	// modalWindow:getMessage()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		tfs::lua::pushString(L, window->message);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowSetTitle(lua_State* L)
{
	// modalWindow:setTitle(text)
	const std::string& text = tfs::lua::getString(L, 2);
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->title = text;
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowSetMessage(lua_State* L)
{
	// modalWindow:setMessage(text)
	const std::string& text = tfs::lua::getString(L, 2);
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->message = text;
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowGetButtonCount(lua_State* L)
{
	// modalWindow:getButtonCount()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		lua_pushnumber(L, window->buttons.size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowGetChoiceCount(lua_State* L)
{
	// modalWindow:getChoiceCount()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		lua_pushnumber(L, window->choices.size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowAddButton(lua_State* L)
{
	// modalWindow:addButton(id, text)
	const std::string& text = tfs::lua::getString(L, 3);
	uint8_t id = tfs::lua::getNumber<uint8_t>(L, 2);
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->buttons.emplace_back(text, id);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowAddChoice(lua_State* L)
{
	// modalWindow:addChoice(id, text)
	const std::string& text = tfs::lua::getString(L, 3);
	uint8_t id = tfs::lua::getNumber<uint8_t>(L, 2);
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->choices.emplace_back(text, id);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowGetDefaultEnterButton(lua_State* L)
{
	// modalWindow:getDefaultEnterButton()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		lua_pushnumber(L, window->defaultEnterButton);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowSetDefaultEnterButton(lua_State* L)
{
	// modalWindow:setDefaultEnterButton(buttonId)
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->defaultEnterButton = tfs::lua::getNumber<uint8_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowGetDefaultEscapeButton(lua_State* L)
{
	// modalWindow:getDefaultEscapeButton()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		lua_pushnumber(L, window->defaultEscapeButton);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowSetDefaultEscapeButton(lua_State* L)
{
	// modalWindow:setDefaultEscapeButton(buttonId)
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->defaultEscapeButton = tfs::lua::getNumber<uint8_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowHasPriority(lua_State* L)
{
	// modalWindow:hasPriority()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		tfs::lua::pushBoolean(L, window->priority);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowSetPriority(lua_State* L)
{
	// modalWindow:setPriority(priority)
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->priority = tfs::lua::getBoolean(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaModalWindowSendToPlayer(lua_State* L)
{
	// modalWindow:sendToPlayer(player)
	Player* player = tfs::lua::getPlayer(L, 2);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		if (!player->hasModalWindowOpen(window->id)) {
			player->sendModalWindow(*window);
		}
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Item
int luaItemCreate(lua_State* L)
{
	// Item(uid)
	uint32_t id = tfs::lua::getNumber<uint32_t>(L, 2);

	Item* item = tfs::lua::getScriptEnv()->getItemByUID(id);
	if (item) {
		tfs::lua::pushUserdata<Item>(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemIsItem(lua_State* L)
{
	// item:isItem()
	tfs::lua::pushBoolean(L, tfs::lua::getUserdata<const Item>(L, 1) != nullptr);
	return 1;
}

int luaItemGetParent(lua_State* L)
{
	// item:getParent()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	Cylinder* parent = item->getParent();
	if (!parent) {
		lua_pushnil(L);
		return 1;
	}

	tfs::lua::pushCylinder(L, parent);
	return 1;
}

int luaItemGetTopParent(lua_State* L)
{
	// item:getTopParent()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	Cylinder* topParent = item->getTopParent();
	if (!topParent) {
		lua_pushnil(L);
		return 1;
	}

	tfs::lua::pushCylinder(L, topParent);
	return 1;
}

int luaItemGetId(lua_State* L)
{
	// item:getId()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		lua_pushnumber(L, item->getID());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemClone(lua_State* L)
{
	// item:clone()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	Item* clone = item->clone();
	if (!clone) {
		lua_pushnil(L);
		return 1;
	}

	tfs::lua::getScriptEnv()->addTempItem(clone);
	clone->setParent(VirtualCylinder::virtualCylinder);

	tfs::lua::pushUserdata<Item>(L, clone);
	tfs::lua::setItemMetatable(L, -1, clone);
	return 1;
}

int luaItemSplit(lua_State* L)
{
	// item:split([count = 1])
	Item** itemPtr = tfs::lua::getRawUserdata<Item>(L, 1);
	if (!itemPtr) {
		lua_pushnil(L);
		return 1;
	}

	Item* item = *itemPtr;
	if (!item || !item->isStackable()) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t count = std::min<uint16_t>(tfs::lua::getNumber<uint16_t>(L, 2, 1), item->getItemCount());
	uint16_t diff = item->getItemCount() - count;

	Item* splitItem = item->clone();
	if (!splitItem) {
		lua_pushnil(L);
		return 1;
	}

	splitItem->setItemCount(count);

	tfs::lua::ScriptEnvironment* env = tfs::lua::getScriptEnv();
	uint32_t uid = env->addThing(item);

	Item* newItem = g_game->transformItem(item, item->getID(), diff);
	if (item->isRemoved()) {
		env->removeItemByUID(uid);
	}

	if (newItem && newItem != item) {
		env->insertItem(uid, newItem);
	}

	*itemPtr = newItem;

	splitItem->setParent(VirtualCylinder::virtualCylinder);
	env->addTempItem(splitItem);

	tfs::lua::pushUserdata<Item>(L, splitItem);
	tfs::lua::setItemMetatable(L, -1, splitItem);
	return 1;
}

int luaItemRemove(lua_State* L)
{
	// item:remove([count = -1])
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		int32_t count = tfs::lua::getNumber<int32_t>(L, 2, -1);
		tfs::lua::pushBoolean(L, g_game->internalRemoveItem(item, count) == RETURNVALUE_NOERROR);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetUniqueId(lua_State* L)
{
	// item:getUniqueId()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		uint32_t uniqueId = item->getUniqueId();
		if (uniqueId == 0) {
			uniqueId = tfs::lua::getScriptEnv()->addThing(item);
		}
		lua_pushnumber(L, uniqueId);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetActionId(lua_State* L)
{
	// item:getActionId()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		lua_pushnumber(L, item->getActionId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemSetActionId(lua_State* L)
{
	// item:setActionId(actionId)
	uint16_t actionId = tfs::lua::getNumber<uint16_t>(L, 2);
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		item->setActionId(actionId);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetCount(lua_State* L)
{
	// item:getCount()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		lua_pushnumber(L, item->getItemCount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetCharges(lua_State* L)
{
	// item:getCharges()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		lua_pushnumber(L, item->getCharges());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetFluidType(lua_State* L)
{
	// item:getFluidType()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		lua_pushnumber(L, item->getFluidType());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetWeight(lua_State* L)
{
	// item:getWeight()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		lua_pushnumber(L, item->getWeight());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetWorth(lua_State* L)
{
	// item:getWorth()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		lua_pushnumber(L, item->getWorth());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetSubType(lua_State* L)
{
	// item:getSubType()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		lua_pushnumber(L, item->getSubType());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetName(lua_State* L)
{
	// item:getName()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushString(L, item->getName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetPluralName(lua_State* L)
{
	// item:getPluralName()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushString(L, item->getPluralName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetArticle(lua_State* L)
{
	// item:getArticle()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushString(L, item->getArticle());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetPosition(lua_State* L)
{
	// item:getPosition()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushPosition(L, item->getPosition());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetTile(lua_State* L)
{
	// item:getTile()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	Tile* tile = item->getTile();
	if (tile) {
		tfs::lua::pushUserdata<Tile>(L, tile);
		tfs::lua::setMetatable(L, -1, "Tile");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemHasAttribute(lua_State* L)
{
	// item:hasAttribute(key)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	itemAttrTypes attribute;
	if (tfs::lua::isNumber(L, 2)) {
		attribute = tfs::lua::getNumber<itemAttrTypes>(L, 2);
	} else if (lua_isstring(L, 2)) {
		attribute = stringToItemAttribute(tfs::lua::getString(L, 2));
	} else {
		attribute = ITEM_ATTRIBUTE_NONE;
	}

	tfs::lua::pushBoolean(L, item->hasAttribute(attribute));
	return 1;
}

int luaItemGetAttribute(lua_State* L)
{
	// item:getAttribute(key)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	itemAttrTypes attribute;
	if (tfs::lua::isNumber(L, 2)) {
		attribute = tfs::lua::getNumber<itemAttrTypes>(L, 2);
	} else if (lua_isstring(L, 2)) {
		attribute = stringToItemAttribute(tfs::lua::getString(L, 2));
	} else {
		attribute = ITEM_ATTRIBUTE_NONE;
	}

	if (ItemAttributes::isIntAttrType(attribute)) {
		lua_pushnumber(L, item->getIntAttr(attribute));
	} else if (ItemAttributes::isStrAttrType(attribute)) {
		tfs::lua::pushString(L, item->getStrAttr(attribute));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemSetAttribute(lua_State* L)
{
	// item:setAttribute(key, value)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	itemAttrTypes attribute;
	if (tfs::lua::isNumber(L, 2)) {
		attribute = tfs::lua::getNumber<itemAttrTypes>(L, 2);
	} else if (lua_isstring(L, 2)) {
		attribute = stringToItemAttribute(tfs::lua::getString(L, 2));
	} else {
		attribute = ITEM_ATTRIBUTE_NONE;
	}

	if (ItemAttributes::isIntAttrType(attribute)) {
		if (attribute == ITEM_ATTRIBUTE_UNIQUEID) {
			reportErrorFunc(L, "Attempt to set protected key \"uid\"");
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		item->setIntAttr(attribute, tfs::lua::getNumber<int32_t>(L, 3));
		tfs::lua::pushBoolean(L, true);
	} else if (ItemAttributes::isStrAttrType(attribute)) {
		item->setStrAttr(attribute, tfs::lua::getString(L, 3));
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemRemoveAttribute(lua_State* L)
{
	// item:removeAttribute(key)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	itemAttrTypes attribute;
	if (tfs::lua::isNumber(L, 2)) {
		attribute = tfs::lua::getNumber<itemAttrTypes>(L, 2);
	} else if (lua_isstring(L, 2)) {
		attribute = stringToItemAttribute(tfs::lua::getString(L, 2));
	} else {
		attribute = ITEM_ATTRIBUTE_NONE;
	}

	bool ret = attribute != ITEM_ATTRIBUTE_UNIQUEID;
	if (ret) {
		item->removeAttribute(attribute);
	} else {
		reportErrorFunc(L, "Attempt to erase protected key \"uid\"");
	}
	tfs::lua::pushBoolean(L, ret);
	return 1;
}

int luaItemGetCustomAttribute(lua_State* L)
{
	// item:getCustomAttribute(key)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	const ItemAttributes::CustomAttribute* attr;
	if (tfs::lua::isNumber(L, 2)) {
		attr = item->getCustomAttribute(tfs::lua::getNumber<int64_t>(L, 2));
	} else if (lua_isstring(L, 2)) {
		attr = item->getCustomAttribute(tfs::lua::getString(L, 2));
	} else {
		lua_pushnil(L);
		return 1;
	}

	if (attr) {
		attr->pushToLua(L);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemSetCustomAttribute(lua_State* L)
{
	// item:setCustomAttribute(key, value)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	std::string key;
	if (tfs::lua::isNumber(L, 2)) {
		key = std::to_string(tfs::lua::getNumber<int64_t>(L, 2));
	} else if (lua_isstring(L, 2)) {
		key = tfs::lua::getString(L, 2);
	} else {
		lua_pushnil(L);
		return 1;
	}

	ItemAttributes::CustomAttribute val;
	if (tfs::lua::isNumber(L, 3)) {
		double tmp = tfs::lua::getNumber<double>(L, 3);
		if (std::floor(tmp) < tmp) {
			val.set<double>(tmp);
		} else {
			val.set<int64_t>(tmp);
		}
	} else if (lua_isstring(L, 3)) {
		val.set<std::string>(tfs::lua::getString(L, 3));
	} else if (lua_isboolean(L, 3)) {
		val.set<bool>(tfs::lua::getBoolean(L, 3));
	} else {
		lua_pushnil(L);
		return 1;
	}

	item->setCustomAttribute(key, val);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaItemRemoveCustomAttribute(lua_State* L)
{
	// item:removeCustomAttribute(key)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	if (tfs::lua::isNumber(L, 2)) {
		tfs::lua::pushBoolean(L, item->removeCustomAttribute(tfs::lua::getNumber<int64_t>(L, 2)));
	} else if (lua_isstring(L, 2)) {
		tfs::lua::pushBoolean(L, item->removeCustomAttribute(tfs::lua::getString(L, 2)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemMoveTo(lua_State* L)
{
	// item:moveTo(position or cylinder[, flags])
	Item** itemPtr = tfs::lua::getRawUserdata<Item>(L, 1);
	if (!itemPtr) {
		lua_pushnil(L);
		return 1;
	}

	Item* item = *itemPtr;
	if (!item || item->isRemoved()) {
		lua_pushnil(L);
		return 1;
	}

	Cylinder* toCylinder;
	if (lua_isuserdata(L, 2)) {
		auto type = tfs::lua::getUserdataType(L, 2);
		switch (type) {
			case tfs::lua::LuaData_Container:
				toCylinder = tfs::lua::getUserdata<Container>(L, 2);
				break;
			case tfs::lua::LuaData_Player:
				toCylinder = tfs::lua::getUserdata<Player>(L, 2);
				break;
			case tfs::lua::LuaData_Tile:
				toCylinder = tfs::lua::getUserdata<Tile>(L, 2);
				break;
			default:
				toCylinder = nullptr;
				break;
		}
	} else {
		toCylinder = g_game->map.getTile(tfs::lua::getPosition(L, 2));
	}

	if (!toCylinder) {
		lua_pushnil(L);
		return 1;
	}

	if (item->getParent() == toCylinder) {
		tfs::lua::pushBoolean(L, true);
		return 1;
	}

	uint32_t flags = tfs::lua::getNumber<uint32_t>(
	    L, 3, FLAG_NOLIMIT | FLAG_IGNOREBLOCKITEM | FLAG_IGNOREBLOCKCREATURE | FLAG_IGNORENOTMOVEABLE);

	if (item->getParent() == VirtualCylinder::virtualCylinder) {
		tfs::lua::pushBoolean(L,
		                      g_game->internalAddItem(toCylinder, item, INDEX_WHEREEVER, flags) == RETURNVALUE_NOERROR);
	} else {
		Item* moveItem = nullptr;
		ReturnValue ret = g_game->internalMoveItem(item->getParent(), toCylinder, INDEX_WHEREEVER, item,
		                                           item->getItemCount(), &moveItem, flags);
		if (moveItem) {
			*itemPtr = moveItem;
		}
		tfs::lua::pushBoolean(L, ret == RETURNVALUE_NOERROR);
	}
	return 1;
}

int luaItemTransform(lua_State* L)
{
	// item:transform(itemId[, count/subType = -1])
	Item** itemPtr = tfs::lua::getRawUserdata<Item>(L, 1);
	if (!itemPtr) {
		lua_pushnil(L);
		return 1;
	}

	Item*& item = *itemPtr;
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t itemId;
	if (tfs::lua::isNumber(L, 2)) {
		itemId = tfs::lua::getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	int32_t subType = tfs::lua::getNumber<int32_t>(L, 3, -1);
	if (item->getID() == itemId && (subType == -1 || subType == item->getSubType())) {
		tfs::lua::pushBoolean(L, true);
		return 1;
	}

	const ItemType& it = Item::items[itemId];
	if (it.stackable) {
		subType = std::min<int32_t>(subType, 100);
	}

	tfs::lua::ScriptEnvironment* env = tfs::lua::getScriptEnv();
	uint32_t uid = env->addThing(item);

	Item* newItem = g_game->transformItem(item, itemId, subType);
	if (item->isRemoved()) {
		env->removeItemByUID(uid);
	}

	if (newItem && newItem != item) {
		env->insertItem(uid, newItem);
	}

	item = newItem;
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaItemDecay(lua_State* L)
{
	// item:decay(decayId)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		if (tfs::lua::isNumber(L, 2)) {
			item->setDecayTo(tfs::lua::getNumber<int32_t>(L, 2));
		}

		g_game->startDecay(item);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemGetSpecialDescription(lua_State* L)
{
	// item:getSpecialDescription()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushString(L, item->getSpecialDescription());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemHasProperty(lua_State* L)
{
	// item:hasProperty(property)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		ITEMPROPERTY property = tfs::lua::getNumber<ITEMPROPERTY>(L, 2);
		tfs::lua::pushBoolean(L, item->hasProperty(property));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemIsLoadedFromMap(lua_State* L)
{
	// item:isLoadedFromMap()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushBoolean(L, item->isLoadedFromMap());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemSetStoreItem(lua_State* L)
{
	// item:setStoreItem(storeItem)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	item->setStoreItem(tfs::lua::getBoolean(L, 2, false));
	return 1;
}

int luaItemIsStoreItem(lua_State* L)
{
	// item:isStoreItem()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushBoolean(L, item->isStoreItem());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemSetReflect(lua_State* L)
{
	// item:setReflect(combatType, reflect)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	item->setReflect(tfs::lua::getNumber<CombatType_t>(L, 2), tfs::lua::getReflect(L, 3));
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaItemGetReflect(lua_State* L)
{
	// item:getReflect(combatType[, total = true])
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushReflect(
		    L, item->getReflect(tfs::lua::getNumber<CombatType_t>(L, 2), tfs::lua::getBoolean(L, 3, true)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemSetBoostPercent(lua_State* L)
{
	// item:setBoostPercent(combatType, percent)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	item->setBoostPercent(tfs::lua::getNumber<CombatType_t>(L, 2), tfs::lua::getNumber<uint16_t>(L, 3));
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaItemGetBoostPercent(lua_State* L)
{
	// item:getBoostPercent(combatType[, total = true])
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		lua_pushnumber(
		    L, item->getBoostPercent(tfs::lua::getNumber<CombatType_t>(L, 2), tfs::lua::getBoolean(L, 3, true)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Container
int luaContainerCreate(lua_State* L)
{
	// Container(uid)
	uint32_t id = tfs::lua::getNumber<uint32_t>(L, 2);

	Container* container = tfs::lua::getScriptEnv()->getContainerByUID(id);
	if (container) {
		tfs::lua::pushUserdata(L, container);
		tfs::lua::setMetatable(L, -1, "Container");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaContainerGetSize(lua_State* L)
{
	// container:getSize()
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (container) {
		lua_pushnumber(L, container->size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaContainerGetCapacity(lua_State* L)
{
	// container:getCapacity()
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (container) {
		lua_pushnumber(L, container->capacity());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaContainerGetEmptySlots(lua_State* L)
{
	// container:getEmptySlots([recursive = false])
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	uint32_t slots = container->capacity() - container->size();
	bool recursive = tfs::lua::getBoolean(L, 2, false);
	if (recursive) {
		for (ContainerIterator it = container->iterator(); it.hasNext(); it.advance()) {
			if (Container* tmpContainer = (*it)->getContainer()) {
				slots += tmpContainer->capacity() - tmpContainer->size();
			}
		}
	}
	lua_pushnumber(L, slots);
	return 1;
}

int luaContainerGetItemHoldingCount(lua_State* L)
{
	// container:getItemHoldingCount()
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (container) {
		lua_pushnumber(L, container->getItemHoldingCount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaContainerGetItem(lua_State* L)
{
	// container:getItem(index)
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	uint32_t index = tfs::lua::getNumber<uint32_t>(L, 2);
	Item* item = container->getItemByIndex(index);
	if (item) {
		tfs::lua::pushUserdata<Item>(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaContainerHasItem(lua_State* L)
{
	// container:hasItem(item)
	Item* item = tfs::lua::getUserdata<Item>(L, 2);
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (container) {
		tfs::lua::pushBoolean(L, container->isHoldingItem(item));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaContainerAddItem(lua_State* L)
{
	// container:addItem(itemId[, count/subType = 1[, index = INDEX_WHEREEVER[, flags = 0]]])
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t itemId;
	if (tfs::lua::isNumber(L, 2)) {
		itemId = tfs::lua::getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	uint32_t count = tfs::lua::getNumber<uint32_t>(L, 3, 1);
	const ItemType& it = Item::items[itemId];
	if (it.stackable) {
		count = std::min<uint16_t>(count, 100);
	}

	Item* item = Item::CreateItem(itemId, count);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	int32_t index = tfs::lua::getNumber<int32_t>(L, 4, INDEX_WHEREEVER);
	uint32_t flags = tfs::lua::getNumber<uint32_t>(L, 5, 0);

	ReturnValue ret = g_game->internalAddItem(container, item, index, flags);
	if (ret == RETURNVALUE_NOERROR) {
		tfs::lua::pushUserdata<Item>(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		delete item;
		lua_pushnil(L);
	}
	return 1;
}

int luaContainerAddItemEx(lua_State* L)
{
	// container:addItemEx(item[, index = INDEX_WHEREEVER[, flags = 0]])
	Item* item = tfs::lua::getUserdata<Item>(L, 2);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	if (item->getParent() != VirtualCylinder::virtualCylinder) {
		reportErrorFunc(L, "Item already has a parent");
		lua_pushnil(L);
		return 1;
	}

	int32_t index = tfs::lua::getNumber<int32_t>(L, 3, INDEX_WHEREEVER);
	uint32_t flags = tfs::lua::getNumber<uint32_t>(L, 4, 0);
	ReturnValue ret = g_game->internalAddItem(container, item, index, flags);
	if (ret == RETURNVALUE_NOERROR) {
		tfs::lua::ScriptEnvironment::removeTempItem(item);
	}
	lua_pushnumber(L, ret);
	return 1;
}

int luaContainerGetCorpseOwner(lua_State* L)
{
	// container:getCorpseOwner()
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (container) {
		lua_pushnumber(L, container->getCorpseOwner());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaContainerGetItemCountById(lua_State* L)
{
	// container:getItemCountById(itemId[, subType = -1])
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t itemId;
	if (tfs::lua::isNumber(L, 2)) {
		itemId = tfs::lua::getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	int32_t subType = tfs::lua::getNumber<int32_t>(L, 3, -1);
	lua_pushnumber(L, container->getItemTypeCount(itemId, subType));
	return 1;
}

int luaContainerGetItems(lua_State* L)
{
	// container:getItems([recursive = false])
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	bool recursive = tfs::lua::getBoolean(L, 2, false);
	std::vector<Item*> items = container->getItems(recursive);

	lua_createtable(L, items.size(), 0);

	int index = 0;
	for (Item* item : items) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

// Teleport
int luaTeleportCreate(lua_State* L)
{
	// Teleport(uid)
	uint32_t id = tfs::lua::getNumber<uint32_t>(L, 2);

	Item* item = tfs::lua::getScriptEnv()->getItemByUID(id);
	if (item && item->getTeleport()) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setMetatable(L, -1, "Teleport");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaTeleportGetDestination(lua_State* L)
{
	// teleport:getDestination()
	Teleport* teleport = tfs::lua::getUserdata<Teleport>(L, 1);
	if (teleport) {
		tfs::lua::pushPosition(L, teleport->getDestPos());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaTeleportSetDestination(lua_State* L)
{
	// teleport:setDestination(position)
	Teleport* teleport = tfs::lua::getUserdata<Teleport>(L, 1);
	if (teleport) {
		teleport->setDestPos(tfs::lua::getPosition(L, 2));
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Podium
int luaPodiumCreate(lua_State* L)
{
	// Podium(uid)
	uint32_t id = tfs::lua::getNumber<uint32_t>(L, 2);

	Item* item = tfs::lua::getScriptEnv()->getItemByUID(id);
	if (item && item->getPodium()) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setMetatable(L, -1, "Podium");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPodiumGetOutfit(lua_State* L)
{
	// podium:getOutfit()
	const Podium* podium = tfs::lua::getUserdata<const Podium>(L, 1);
	if (podium) {
		tfs::lua::pushOutfit(L, podium->getOutfit());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPodiumSetOutfit(lua_State* L)
{
	// podium:setOutfit(outfit)
	Podium* podium = tfs::lua::getUserdata<Podium>(L, 1);
	if (podium) {
		podium->setOutfit(tfs::lua::getOutfit(L, 2));
		g_game->updatePodium(podium);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPodiumHasFlag(lua_State* L)
{
	// podium:hasFlag(flag)
	Podium* podium = tfs::lua::getUserdata<Podium>(L, 1);
	if (podium) {
		PodiumFlags flag = tfs::lua::getNumber<PodiumFlags>(L, 2);
		tfs::lua::pushBoolean(L, podium->hasFlag(flag));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPodiumSetFlag(lua_State* L)
{
	// podium:setFlag(flag, value)
	uint8_t value = tfs::lua::getBoolean(L, 3);
	PodiumFlags flag = tfs::lua::getNumber<PodiumFlags>(L, 2);
	Podium* podium = tfs::lua::getUserdata<Podium>(L, 1);

	if (podium) {
		podium->setFlagValue(flag, value);
		g_game->updatePodium(podium);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPodiumGetDirection(lua_State* L)
{
	// podium:getDirection()
	const Podium* podium = tfs::lua::getUserdata<const Podium>(L, 1);
	if (podium) {
		lua_pushnumber(L, podium->getDirection());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPodiumSetDirection(lua_State* L)
{
	// podium:setDirection(direction)
	Podium* podium = tfs::lua::getUserdata<Podium>(L, 1);
	if (podium) {
		podium->setDirection(tfs::lua::getNumber<Direction>(L, 2));
		g_game->updatePodium(podium);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Creature
int luaCreatureCreate(lua_State* L)
{
	// Creature(id or name or userdata)
	Creature* creature;
	if (tfs::lua::isNumber(L, 2)) {
		creature = g_game->getCreatureByID(tfs::lua::getNumber<uint32_t>(L, 2));
	} else if (lua_isstring(L, 2)) {
		creature = g_game->getCreatureByName(tfs::lua::getString(L, 2));
	} else if (lua_isuserdata(L, 2)) {
		tfs::lua::LuaDataType type = tfs::lua::getUserdataType(L, 2);
		if (type != tfs::lua::LuaData_Player && type != tfs::lua::LuaData_Monster && type != tfs::lua::LuaData_Npc) {
			lua_pushnil(L);
			return 1;
		}
		creature = tfs::lua::getUserdata<Creature>(L, 2);
	} else {
		creature = nullptr;
	}

	if (creature) {
		tfs::lua::pushUserdata<Creature>(L, creature);
		tfs::lua::setCreatureMetatable(L, -1, creature);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetEvents(lua_State* L)
{
	// creature:getEvents(type)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	CreatureEventType_t eventType = tfs::lua::getNumber<CreatureEventType_t>(L, 2);
	const auto& eventList = creature->getCreatureEvents(eventType);
	lua_createtable(L, eventList.size(), 0);

	int index = 0;
	for (CreatureEvent* event : eventList) {
		tfs::lua::pushString(L, event->getName());
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaCreatureRegisterEvent(lua_State* L)
{
	// creature:registerEvent(name)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		const std::string& name = tfs::lua::getString(L, 2);
		tfs::lua::pushBoolean(L, creature->registerCreatureEvent(name));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureUnregisterEvent(lua_State* L)
{
	// creature:unregisterEvent(name)
	const std::string& name = tfs::lua::getString(L, 2);
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		tfs::lua::pushBoolean(L, creature->unregisterCreatureEvent(name));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureIsRemoved(lua_State* L)
{
	// creature:isRemoved()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		tfs::lua::pushBoolean(L, creature->isRemoved());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureIsCreature(lua_State* L)
{
	// creature:isCreature()
	tfs::lua::pushBoolean(L, tfs::lua::getUserdata<const Creature>(L, 1) != nullptr);
	return 1;
}

int luaCreatureIsInGhostMode(lua_State* L)
{
	// creature:isInGhostMode()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		tfs::lua::pushBoolean(L, creature->isInGhostMode());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureIsHealthHidden(lua_State* L)
{
	// creature:isHealthHidden()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		tfs::lua::pushBoolean(L, creature->isHealthHidden());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureIsMovementBlocked(lua_State* L)
{
	// creature:isMovementBlocked()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		tfs::lua::pushBoolean(L, creature->isMovementBlocked());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureCanSee(lua_State* L)
{
	// creature:canSee(position)
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		const Position& position = tfs::lua::getPosition(L, 2);
		tfs::lua::pushBoolean(L, creature->canSee(position));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureCanSeeCreature(lua_State* L)
{
	// creature:canSeeCreature(creature)
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		const Creature* otherCreature = tfs::lua::getCreature(L, 2);
		if (!otherCreature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		tfs::lua::pushBoolean(L, creature->canSeeCreature(otherCreature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureCanSeeGhostMode(lua_State* L)
{
	// creature:canSeeGhostMode(creature)
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		const Creature* otherCreature = tfs::lua::getCreature(L, 2);
		if (!otherCreature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		tfs::lua::pushBoolean(L, creature->canSeeGhostMode(otherCreature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureCanSeeInvisibility(lua_State* L)
{
	// creature:canSeeInvisibility()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		tfs::lua::pushBoolean(L, creature->canSeeInvisibility());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetParent(lua_State* L)
{
	// creature:getParent()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	Cylinder* parent = creature->getParent();
	if (!parent) {
		lua_pushnil(L);
		return 1;
	}

	tfs::lua::pushCylinder(L, parent);
	return 1;
}

int luaCreatureGetId(lua_State* L)
{
	// creature:getId()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		lua_pushnumber(L, creature->getID());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetName(lua_State* L)
{
	// creature:getName()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		tfs::lua::pushString(L, creature->getName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetTarget(lua_State* L)
{
	// creature:getTarget()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	Creature* target = creature->getAttackedCreature();
	if (target) {
		tfs::lua::pushUserdata<Creature>(L, target);
		tfs::lua::setCreatureMetatable(L, -1, target);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureSetTarget(lua_State* L)
{
	// creature:setTarget(target)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		tfs::lua::pushBoolean(L, creature->setAttackedCreature(tfs::lua::getCreature(L, 2)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetFollowCreature(lua_State* L)
{
	// creature:getFollowCreature()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	Creature* followCreature = creature->getFollowCreature();
	if (followCreature) {
		tfs::lua::pushUserdata<Creature>(L, followCreature);
		tfs::lua::setCreatureMetatable(L, -1, followCreature);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureSetFollowCreature(lua_State* L)
{
	// creature:setFollowCreature(followedCreature)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		tfs::lua::pushBoolean(L, creature->setFollowCreature(tfs::lua::getCreature(L, 2)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetMaster(lua_State* L)
{
	// creature:getMaster()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	Creature* master = creature->getMaster();
	if (!master) {
		lua_pushnil(L);
		return 1;
	}

	tfs::lua::pushUserdata<Creature>(L, master);
	tfs::lua::setCreatureMetatable(L, -1, master);
	return 1;
}

int luaCreatureSetMaster(lua_State* L)
{
	// creature:setMaster(master)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	tfs::lua::pushBoolean(L, creature->setMaster(tfs::lua::getCreature(L, 2)));

	// update summon icon
	SpectatorVec spectators;
	g_game->map.getSpectators(spectators, creature->getPosition(), true, true);

	for (Creature* spectator : spectators) {
		spectator->getPlayer()->sendUpdateTileCreature(creature);
	}
	return 1;
}

int luaCreatureGetLight(lua_State* L)
{
	// creature:getLight()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	LightInfo lightInfo = creature->getCreatureLight();
	lua_pushnumber(L, lightInfo.level);
	lua_pushnumber(L, lightInfo.color);
	return 2;
}

int luaCreatureSetLight(lua_State* L)
{
	// creature:setLight(color, level)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	LightInfo light;
	light.color = tfs::lua::getNumber<uint8_t>(L, 2);
	light.level = tfs::lua::getNumber<uint8_t>(L, 3);
	creature->setCreatureLight(light);
	g_game->changeLight(creature);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaCreatureGetSpeed(lua_State* L)
{
	// creature:getSpeed()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		lua_pushnumber(L, creature->getSpeed());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetBaseSpeed(lua_State* L)
{
	// creature:getBaseSpeed()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		lua_pushnumber(L, creature->getBaseSpeed());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureChangeSpeed(lua_State* L)
{
	// creature:changeSpeed(delta)
	Creature* creature = tfs::lua::getCreature(L, 1);
	if (!creature) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	int32_t delta = tfs::lua::getNumber<int32_t>(L, 2);
	g_game->changeSpeed(creature, delta);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaCreatureSetDropLoot(lua_State* L)
{
	// creature:setDropLoot(doDrop)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		creature->setDropLoot(tfs::lua::getBoolean(L, 2));
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureSetSkillLoss(lua_State* L)
{
	// creature:setSkillLoss(skillLoss)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		creature->setSkillLoss(tfs::lua::getBoolean(L, 2));
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetPosition(lua_State* L)
{
	// creature:getPosition()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		tfs::lua::pushPosition(L, creature->getPosition());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetTile(lua_State* L)
{
	// creature:getTile()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	Tile* tile = creature->getTile();
	if (tile) {
		tfs::lua::pushUserdata<Tile>(L, tile);
		tfs::lua::setMetatable(L, -1, "Tile");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetDirection(lua_State* L)
{
	// creature:getDirection()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		lua_pushnumber(L, creature->getDirection());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureSetDirection(lua_State* L)
{
	// creature:setDirection(direction)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		tfs::lua::pushBoolean(L, g_game->internalCreatureTurn(creature, tfs::lua::getNumber<Direction>(L, 2)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetHealth(lua_State* L)
{
	// creature:getHealth()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		lua_pushnumber(L, creature->getHealth());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureSetHealth(lua_State* L)
{
	// creature:setHealth(health)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	creature->setHealth(std::min<int32_t>(tfs::lua::getNumber<uint32_t>(L, 2), creature->getMaxHealth()));
	g_game->addCreatureHealth(creature);

	Player* player = creature->getPlayer();
	if (player) {
		player->sendStats();
	}
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaCreatureAddHealth(lua_State* L)
{
	// creature:addHealth(healthChange)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	CombatDamage damage;
	damage.primary.value = tfs::lua::getNumber<int32_t>(L, 2);
	if (damage.primary.value >= 0) {
		damage.primary.type = COMBAT_HEALING;
	} else {
		damage.primary.type = COMBAT_UNDEFINEDDAMAGE;
	}
	tfs::lua::pushBoolean(L, g_game->combatChangeHealth(nullptr, creature, damage));
	return 1;
}

int luaCreatureGetMaxHealth(lua_State* L)
{
	// creature:getMaxHealth()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		lua_pushnumber(L, creature->getMaxHealth());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureSetMaxHealth(lua_State* L)
{
	// creature:setMaxHealth(maxHealth)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	creature->setMaxHealth(tfs::lua::getNumber<uint32_t>(L, 2));
	creature->setHealth(std::min<int32_t>(creature->getHealth(), creature->getMaxHealth()));
	g_game->addCreatureHealth(creature);

	Player* player = creature->getPlayer();
	if (player) {
		player->sendStats();
	}
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaCreatureSetHiddenHealth(lua_State* L)
{
	// creature:setHiddenHealth(hide)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		creature->setHiddenHealth(tfs::lua::getBoolean(L, 2));
		g_game->addCreatureHealth(creature);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureSetMovementBlocked(lua_State* L)
{
	// creature:setMovementBlocked(state)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		creature->setMovementBlocked(tfs::lua::getBoolean(L, 2));
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetSkull(lua_State* L)
{
	// creature:getSkull()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		lua_pushnumber(L, creature->getSkull());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureSetSkull(lua_State* L)
{
	// creature:setSkull(skull)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		creature->setSkull(tfs::lua::getNumber<Skulls_t>(L, 2));
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetOutfit(lua_State* L)
{
	// creature:getOutfit()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		tfs::lua::pushOutfit(L, creature->getCurrentOutfit());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureSetOutfit(lua_State* L)
{
	// creature:setOutfit(outfit)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		creature->setDefaultOutfit(tfs::lua::getOutfit(L, 2));
		g_game->internalCreatureChangeOutfit(creature, creature->getDefaultOutfit());
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetCondition(lua_State* L)
{
	// creature:getCondition(conditionType[, conditionId = CONDITIONID_COMBAT[, subId = 0]])
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	ConditionType_t conditionType = tfs::lua::getNumber<ConditionType_t>(L, 2);
	ConditionId_t conditionId = tfs::lua::getNumber<ConditionId_t>(L, 3, CONDITIONID_COMBAT);
	uint32_t subId = tfs::lua::getNumber<uint32_t>(L, 4, 0);

	Condition* condition = creature->getCondition(conditionType, conditionId, subId);
	if (condition) {
		tfs::lua::pushUserdata<Condition>(L, condition);
		tfs::lua::setWeakMetatable(L, -1, "Condition");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureAddCondition(lua_State* L)
{
	// creature:addCondition(condition[, force = false])
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 2);
	if (creature && condition) {
		bool force = tfs::lua::getBoolean(L, 3, false);
		tfs::lua::pushBoolean(L, creature->addCondition(condition->clone(), force));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureRemoveCondition(lua_State* L)
{
	// creature:removeCondition(conditionType[, conditionId = CONDITIONID_COMBAT[, subId = 0[, force = false]]])
	// creature:removeCondition(condition[, force = false])
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	Condition* condition = nullptr;

	bool force = false;

	if (lua_isuserdata(L, 2)) {
		condition = tfs::lua::getUserdata<Condition>(L, 2);
		force = tfs::lua::getBoolean(L, 3, false);
	} else {
		ConditionType_t conditionType = tfs::lua::getNumber<ConditionType_t>(L, 2);
		ConditionId_t conditionId = tfs::lua::getNumber<ConditionId_t>(L, 3, CONDITIONID_COMBAT);
		uint32_t subId = tfs::lua::getNumber<uint32_t>(L, 4, 0);
		condition = creature->getCondition(conditionType, conditionId, subId);
		force = tfs::lua::getBoolean(L, 5, false);
	}

	if (condition) {
		creature->removeCondition(condition, force);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureHasCondition(lua_State* L)
{
	// creature:hasCondition(conditionType[, subId = 0])
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	ConditionType_t conditionType = tfs::lua::getNumber<ConditionType_t>(L, 2);
	uint32_t subId = tfs::lua::getNumber<uint32_t>(L, 3, 0);
	tfs::lua::pushBoolean(L, creature->hasCondition(conditionType, subId));
	return 1;
}

int luaCreatureIsImmune(lua_State* L)
{
	// creature:isImmune(condition or conditionType)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	if (tfs::lua::isNumber(L, 2)) {
		tfs::lua::pushBoolean(L, creature->isImmune(tfs::lua::getNumber<ConditionType_t>(L, 2)));
	} else if (Condition* condition = tfs::lua::getUserdata<Condition>(L, 2)) {
		tfs::lua::pushBoolean(L, creature->isImmune(condition->getType()));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureRemove(lua_State* L)
{
	// creature:remove()
	Creature** creaturePtr = tfs::lua::getRawUserdata<Creature>(L, 1);
	if (!creaturePtr) {
		lua_pushnil(L);
		return 1;
	}

	Creature* creature = *creaturePtr;
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	Player* player = creature->getPlayer();
	if (player) {
		player->kickPlayer(true);
	} else {
		g_game->removeCreature(creature);
	}

	*creaturePtr = nullptr;
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaCreatureTeleportTo(lua_State* L)
{
	// creature:teleportTo(position[, pushMovement = false])
	bool pushMovement = tfs::lua::getBoolean(L, 3, false);

	const Position& position = tfs::lua::getPosition(L, 2);
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	const Position oldPosition = creature->getPosition();
	if (g_game->internalTeleport(creature, position, pushMovement) != RETURNVALUE_NOERROR) {
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	if (pushMovement) {
		if (oldPosition.x == position.x) {
			if (oldPosition.y < position.y) {
				g_game->internalCreatureTurn(creature, DIRECTION_SOUTH);
			} else {
				g_game->internalCreatureTurn(creature, DIRECTION_NORTH);
			}
		} else if (oldPosition.x > position.x) {
			g_game->internalCreatureTurn(creature, DIRECTION_WEST);
		} else if (oldPosition.x < position.x) {
			g_game->internalCreatureTurn(creature, DIRECTION_EAST);
		}
	}
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaCreatureSay(lua_State* L)
{
	// creature:say(text[, type = TALKTYPE_MONSTER_SAY[, ghost = false[, target = nullptr[, position]]]])
	int parameters = lua_gettop(L);

	Position position;
	if (parameters >= 6) {
		position = tfs::lua::getPosition(L, 6);
		if (!position.x || !position.y) {
			reportErrorFunc(L, "Invalid position specified.");
			tfs::lua::pushBoolean(L, false);
			return 1;
		}
	}

	Creature* target = nullptr;
	if (parameters >= 5) {
		target = tfs::lua::getCreature(L, 5);
	}

	bool ghost = tfs::lua::getBoolean(L, 4, false);

	SpeakClasses type = tfs::lua::getNumber<SpeakClasses>(L, 3, TALKTYPE_MONSTER_SAY);
	const std::string& text = tfs::lua::getString(L, 2);
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	SpectatorVec spectators;
	if (target) {
		spectators.emplace_back(target);
	}

	// Prevent infinity echo on event onHear
	bool echo = tfs::lua::getScriptEnv()->getScriptId() == g_events->getScriptId(EventInfoId::CREATURE_ONHEAR);

	if (position.x != 0) {
		tfs::lua::pushBoolean(L,
		                      g_game->internalCreatureSay(creature, type, text, ghost, &spectators, &position, echo));
	} else {
		tfs::lua::pushBoolean(L, g_game->internalCreatureSay(creature, type, text, ghost, &spectators, nullptr, echo));
	}
	return 1;
}

int luaCreatureGetDamageMap(lua_State* L)
{
	// creature:getDamageMap()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, creature->getDamageMap().size(), 0);
	for (const auto& damageEntry : creature->getDamageMap()) {
		lua_createtable(L, 0, 2);
		tfs::lua::setField(L, "total", damageEntry.second.total);
		tfs::lua::setField(L, "ticks", damageEntry.second.ticks);
		lua_rawseti(L, -2, damageEntry.first);
	}
	return 1;
}

int luaCreatureGetSummons(lua_State* L)
{
	// creature:getSummons()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, creature->getSummonCount(), 0);

	int index = 0;
	for (Creature* summon : creature->getSummons()) {
		tfs::lua::pushUserdata<Creature>(L, summon);
		tfs::lua::setCreatureMetatable(L, -1, summon);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaCreatureGetDescription(lua_State* L)
{
	// creature:getDescription(distance)
	int32_t distance = tfs::lua::getNumber<int32_t>(L, 2);
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		tfs::lua::pushString(L, creature->getDescription(distance));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreatureGetPathTo(lua_State* L)
{
	// creature:getPathTo(pos[, minTargetDist = 0[, maxTargetDist = 1[, fullPathSearch = true[, clearSight = true[,
	// maxSearchDist = 0]]]]])
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	const Position& position = tfs::lua::getPosition(L, 2);

	FindPathParams fpp;
	fpp.minTargetDist = tfs::lua::getNumber<int32_t>(L, 3, 0);
	fpp.maxTargetDist = tfs::lua::getNumber<int32_t>(L, 4, 1);
	fpp.fullPathSearch = tfs::lua::getBoolean(L, 5, fpp.fullPathSearch);
	fpp.clearSight = tfs::lua::getBoolean(L, 6, fpp.clearSight);
	fpp.maxSearchDist = tfs::lua::getNumber<int32_t>(L, 7, fpp.maxSearchDist);

	std::vector<Direction> dirList;
	if (creature->getPathTo(position, dirList, fpp)) {
		lua_newtable(L);

		int index = 0;
		for (auto it = dirList.rbegin(); it != dirList.rend(); ++it) {
			lua_pushnumber(L, *it);
			lua_rawseti(L, -2, ++index);
		}
	} else {
		tfs::lua::pushBoolean(L, false);
	}
	return 1;
}

int luaCreatureMove(lua_State* L)
{
	// creature:move(direction)
	// creature:move(tile[, flags = 0])
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	if (tfs::lua::isNumber(L, 2)) {
		Direction direction = tfs::lua::getNumber<Direction>(L, 2);
		if (direction > DIRECTION_LAST) {
			lua_pushnil(L);
			return 1;
		}
		lua_pushnumber(L, g_game->internalMoveCreature(creature, direction, FLAG_NOLIMIT));
	} else {
		Tile* tile = tfs::lua::getUserdata<Tile>(L, 2);
		if (!tile) {
			lua_pushnil(L);
			return 1;
		}
		lua_pushnumber(L, g_game->internalMoveCreature(*creature, *tile, tfs::lua::getNumber<uint32_t>(L, 3)));
	}
	return 1;
}

int luaCreatureGetZone(lua_State* L)
{
	// creature:getZone()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		lua_pushnumber(L, creature->getZone());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Monster
int luaMonsterCreate(lua_State* L)
{
	// Monster(id or userdata)
	Monster* monster;
	if (tfs::lua::isNumber(L, 2)) {
		monster = g_game->getMonsterByID(tfs::lua::getNumber<uint32_t>(L, 2));
	} else if (lua_isuserdata(L, 2)) {
		if (tfs::lua::getUserdataType(L, 2) != tfs::lua::LuaData_Monster) {
			lua_pushnil(L);
			return 1;
		}
		monster = tfs::lua::getUserdata<Monster>(L, 2);
	} else {
		monster = nullptr;
	}

	if (monster) {
		tfs::lua::pushUserdata<Monster>(L, monster);
		tfs::lua::setMetatable(L, -1, "Monster");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterIsMonster(lua_State* L)
{
	// monster:isMonster()
	tfs::lua::pushBoolean(L, tfs::lua::getUserdata<const Monster>(L, 1) != nullptr);
	return 1;
}

int luaMonsterGetType(lua_State* L)
{
	// monster:getType()
	const Monster* monster = tfs::lua::getUserdata<const Monster>(L, 1);
	if (monster) {
		tfs::lua::pushUserdata(L, monster->getMonsterType());
		tfs::lua::setMetatable(L, -1, "MonsterType");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterRename(lua_State* L)
{
	// monster:rename(name[, nameDescription])
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	monster->setName(tfs::lua::getString(L, 2));
	if (lua_gettop(L) >= 3) {
		monster->setNameDescription(tfs::lua::getString(L, 3));
	}

	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaMonsterGetSpawnPosition(lua_State* L)
{
	// monster:getSpawnPosition()
	const Monster* monster = tfs::lua::getUserdata<const Monster>(L, 1);
	if (monster) {
		tfs::lua::pushPosition(L, monster->getMasterPos());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterIsInSpawnRange(lua_State* L)
{
	// monster:isInSpawnRange([position])
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		tfs::lua::pushBoolean(
		    L, monster->isInSpawnRange(lua_gettop(L) >= 2 ? tfs::lua::getPosition(L, 2) : monster->getPosition()));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterIsIdle(lua_State* L)
{
	// monster:isIdle()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		tfs::lua::pushBoolean(L, monster->getIdleStatus());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSetIdle(lua_State* L)
{
	// monster:setIdle(idle)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	monster->setIdle(tfs::lua::getBoolean(L, 2));
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaMonsterIsTarget(lua_State* L)
{
	// monster:isTarget(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		const Creature* creature = tfs::lua::getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		tfs::lua::pushBoolean(L, monster->isTarget(creature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterIsOpponent(lua_State* L)
{
	// monster:isOpponent(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		const Creature* creature = tfs::lua::getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		tfs::lua::pushBoolean(L, monster->isOpponent(creature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterIsFriend(lua_State* L)
{
	// monster:isFriend(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		const Creature* creature = tfs::lua::getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		tfs::lua::pushBoolean(L, monster->isFriend(creature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterAddFriend(lua_State* L)
{
	// monster:addFriend(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		Creature* creature = tfs::lua::getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		monster->addFriend(creature);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterRemoveFriend(lua_State* L)
{
	// monster:removeFriend(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		Creature* creature = tfs::lua::getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		monster->removeFriend(creature);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterGetFriendList(lua_State* L)
{
	// monster:getFriendList()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	const auto& friendList = monster->getFriendList();
	lua_createtable(L, friendList.size(), 0);

	int index = 0;
	for (Creature* creature : friendList) {
		tfs::lua::pushUserdata<Creature>(L, creature);
		tfs::lua::setCreatureMetatable(L, -1, creature);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaMonsterGetFriendCount(lua_State* L)
{
	// monster:getFriendCount()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		lua_pushnumber(L, monster->getFriendList().size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterAddTarget(lua_State* L)
{
	// monster:addTarget(creature[, pushFront = false])
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	Creature* creature = tfs::lua::getCreature(L, 2);
	if (!creature) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	bool pushFront = tfs::lua::getBoolean(L, 3, false);
	monster->addTarget(creature, pushFront);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaMonsterRemoveTarget(lua_State* L)
{
	// monster:removeTarget(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	Creature* creature = tfs::lua::getCreature(L, 2);
	if (!creature) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
		tfs::lua::pushBoolean(L, false);
		return 1;
	}

	monster->removeTarget(creature);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaMonsterGetTargetList(lua_State* L)
{
	// monster:getTargetList()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	const auto& targetList = monster->getTargetList();
	lua_createtable(L, targetList.size(), 0);

	int index = 0;
	for (Creature* creature : targetList) {
		tfs::lua::pushUserdata<Creature>(L, creature);
		tfs::lua::setCreatureMetatable(L, -1, creature);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaMonsterGetTargetCount(lua_State* L)
{
	// monster:getTargetCount()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		lua_pushnumber(L, monster->getTargetList().size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSelectTarget(lua_State* L)
{
	// monster:selectTarget(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		Creature* creature = tfs::lua::getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_CREATURE_NOT_FOUND));
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		tfs::lua::pushBoolean(L, monster->selectTarget(creature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSearchTarget(lua_State* L)
{
	// monster:searchTarget([searchType = TARGETSEARCH_DEFAULT])
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		TargetSearchType_t searchType = tfs::lua::getNumber<TargetSearchType_t>(L, 2, TARGETSEARCH_DEFAULT);
		tfs::lua::pushBoolean(L, monster->searchTarget(searchType));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterIsWalkingToSpawn(lua_State* L)
{
	// monster:isWalkingToSpawn()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		tfs::lua::pushBoolean(L, monster->isWalkingToSpawn());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterWalkToSpawn(lua_State* L)
{
	// monster:walkToSpawn()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		tfs::lua::pushBoolean(L, monster->walkToSpawn());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Npc
int luaNpcCreate(lua_State* L)
{
	// Npc([id or name or userdata])
	Npc* npc;
	if (lua_gettop(L) >= 2) {
		if (tfs::lua::isNumber(L, 2)) {
			npc = g_game->getNpcByID(tfs::lua::getNumber<uint32_t>(L, 2));
		} else if (lua_isstring(L, 2)) {
			npc = g_game->getNpcByName(tfs::lua::getString(L, 2));
		} else if (lua_isuserdata(L, 2)) {
			if (tfs::lua::getUserdataType(L, 2) != tfs::lua::LuaData_Npc) {
				lua_pushnil(L);
				return 1;
			}
			npc = tfs::lua::getUserdata<Npc>(L, 2);
		} else {
			npc = nullptr;
		}
	} else {
		npc = tfs::lua::getScriptEnv()->getNpc();
	}

	if (npc) {
		tfs::lua::pushUserdata<Npc>(L, npc);
		tfs::lua::setMetatable(L, -1, "Npc");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNpcIsNpc(lua_State* L)
{
	// npc:isNpc()
	tfs::lua::pushBoolean(L, tfs::lua::getUserdata<const Npc>(L, 1) != nullptr);
	return 1;
}

int luaNpcSetMasterPos(lua_State* L)
{
	// npc:setMasterPos(pos[, radius])
	Npc* npc = tfs::lua::getUserdata<Npc>(L, 1);
	if (!npc) {
		lua_pushnil(L);
		return 1;
	}

	const Position& pos = tfs::lua::getPosition(L, 2);
	int32_t radius = tfs::lua::getNumber<int32_t>(L, 3, 1);
	npc->setMasterPos(pos, radius);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaNpcGetSpeechBubble(lua_State* L)
{
	// npc:getSpeechBubble()
	Npc* npc = tfs::lua::getUserdata<Npc>(L, 1);
	if (npc) {
		lua_pushnumber(L, npc->getSpeechBubble());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaNpcSetSpeechBubble(lua_State* L)
{
	// npc:setSpeechBubble(speechBubble)
	Npc* npc = tfs::lua::getUserdata<Npc>(L, 1);
	if (!npc) {
		lua_pushnil(L);
		return 1;
	}

	if (!tfs::lua::isNumber(L, 2)) {
		lua_pushnil(L);
		return 1;
	}

	uint8_t speechBubble = tfs::lua::getNumber<uint8_t>(L, 2);
	if (speechBubble > SPEECHBUBBLE_LAST) {
		lua_pushnil(L);
	} else {
		npc->setSpeechBubble(speechBubble);
		tfs::lua::pushBoolean(L, true);
	}
	return 1;
}

// Guild
int luaGuildCreate(lua_State* L)
{
	// Guild(id)
	uint32_t id = tfs::lua::getNumber<uint32_t>(L, 2);

	Guild* guild = g_game->getGuild(id);
	if (guild) {
		tfs::lua::pushUserdata<Guild>(L, guild);
		tfs::lua::setMetatable(L, -1, "Guild");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGuildGetId(lua_State* L)
{
	// guild:getId()
	Guild* guild = tfs::lua::getUserdata<Guild>(L, 1);
	if (guild) {
		lua_pushnumber(L, guild->getId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGuildGetName(lua_State* L)
{
	// guild:getName()
	Guild* guild = tfs::lua::getUserdata<Guild>(L, 1);
	if (guild) {
		tfs::lua::pushString(L, guild->getName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGuildGetMembersOnline(lua_State* L)
{
	// guild:getMembersOnline()
	const Guild* guild = tfs::lua::getUserdata<const Guild>(L, 1);
	if (!guild) {
		lua_pushnil(L);
		return 1;
	}

	const auto& members = guild->getMembersOnline();
	lua_createtable(L, members.size(), 0);

	int index = 0;
	for (Player* player : members) {
		tfs::lua::pushUserdata<Player>(L, player);
		tfs::lua::setMetatable(L, -1, "Player");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaGuildAddRank(lua_State* L)
{
	// guild:addRank(id, name, level)
	Guild* guild = tfs::lua::getUserdata<Guild>(L, 1);
	if (guild) {
		uint32_t id = tfs::lua::getNumber<uint32_t>(L, 2);
		const std::string& name = tfs::lua::getString(L, 3);
		uint8_t level = tfs::lua::getNumber<uint8_t>(L, 4);
		guild->addRank(id, name, level);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGuildGetRankById(lua_State* L)
{
	// guild:getRankById(id)
	Guild* guild = tfs::lua::getUserdata<Guild>(L, 1);
	if (!guild) {
		lua_pushnil(L);
		return 1;
	}

	uint32_t id = tfs::lua::getNumber<uint32_t>(L, 2);
	GuildRank_ptr rank = guild->getRankById(id);
	if (rank) {
		lua_createtable(L, 0, 3);
		tfs::lua::setField(L, "id", rank->id);
		tfs::lua::setField(L, "name", rank->name);
		tfs::lua::setField(L, "level", rank->level);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGuildGetRankByLevel(lua_State* L)
{
	// guild:getRankByLevel(level)
	const Guild* guild = tfs::lua::getUserdata<const Guild>(L, 1);
	if (!guild) {
		lua_pushnil(L);
		return 1;
	}

	uint8_t level = tfs::lua::getNumber<uint8_t>(L, 2);
	GuildRank_ptr rank = guild->getRankByLevel(level);
	if (rank) {
		lua_createtable(L, 0, 3);
		tfs::lua::setField(L, "id", rank->id);
		tfs::lua::setField(L, "name", rank->name);
		tfs::lua::setField(L, "level", rank->level);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGuildGetMotd(lua_State* L)
{
	// guild:getMotd()
	Guild* guild = tfs::lua::getUserdata<Guild>(L, 1);
	if (guild) {
		tfs::lua::pushString(L, guild->getMotd());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGuildSetMotd(lua_State* L)
{
	// guild:setMotd(motd)
	const std::string& motd = tfs::lua::getString(L, 2);
	Guild* guild = tfs::lua::getUserdata<Guild>(L, 1);
	if (guild) {
		guild->setMotd(motd);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Group
int luaGroupCreate(lua_State* L)
{
	// Group(id)
	uint32_t id = tfs::lua::getNumber<uint32_t>(L, 2);

	Group* group = g_game->groups.getGroup(id);
	if (group) {
		tfs::lua::pushUserdata<Group>(L, group);
		tfs::lua::setMetatable(L, -1, "Group");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGroupGetId(lua_State* L)
{
	// group:getId()
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		lua_pushnumber(L, group->id);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGroupGetName(lua_State* L)
{
	// group:getName()
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		tfs::lua::pushString(L, group->name);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGroupGetFlags(lua_State* L)
{
	// group:getFlags()
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		lua_pushnumber(L, group->flags);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGroupGetAccess(lua_State* L)
{
	// group:getAccess()
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		tfs::lua::pushBoolean(L, group->access);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGroupGetMaxDepotItems(lua_State* L)
{
	// group:getMaxDepotItems()
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		lua_pushnumber(L, group->maxDepotItems);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGroupGetMaxVipEntries(lua_State* L)
{
	// group:getMaxVipEntries()
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		lua_pushnumber(L, group->maxVipEntries);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaGroupHasFlag(lua_State* L)
{
	// group:hasFlag(flag)
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		PlayerFlags flag = tfs::lua::getNumber<PlayerFlags>(L, 2);
		tfs::lua::pushBoolean(L, (group->flags & flag) != 0);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Vocation
int luaVocationCreate(lua_State* L)
{
	// Vocation(id or name)
	uint32_t id;
	if (tfs::lua::isNumber(L, 2)) {
		id = tfs::lua::getNumber<uint32_t>(L, 2);
	} else {
		id = g_vocations.getVocationId(tfs::lua::getString(L, 2));
	}

	Vocation* vocation = g_vocations.getVocation(id);
	if (vocation) {
		tfs::lua::pushUserdata<Vocation>(L, vocation);
		tfs::lua::setMetatable(L, -1, "Vocation");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetId(lua_State* L)
{
	// vocation:getId()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetClientId(lua_State* L)
{
	// vocation:getClientId()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getClientId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetName(lua_State* L)
{
	// vocation:getName()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		tfs::lua::pushString(L, vocation->getVocName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetDescription(lua_State* L)
{
	// vocation:getDescription()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		tfs::lua::pushString(L, vocation->getVocDescription());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetRequiredSkillTries(lua_State* L)
{
	// vocation:getRequiredSkillTries(skillType, skillLevel)
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		skills_t skillType = tfs::lua::getNumber<skills_t>(L, 2);
		uint16_t skillLevel = tfs::lua::getNumber<uint16_t>(L, 3);
		lua_pushnumber(L, vocation->getReqSkillTries(skillType, skillLevel));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetRequiredManaSpent(lua_State* L)
{
	// vocation:getRequiredManaSpent(magicLevel)
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		uint32_t magicLevel = tfs::lua::getNumber<uint32_t>(L, 2);
		lua_pushnumber(L, vocation->getReqMana(magicLevel));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetCapacityGain(lua_State* L)
{
	// vocation:getCapacityGain()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getCapGain());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetHealthGain(lua_State* L)
{
	// vocation:getHealthGain()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getHPGain());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetHealthGainTicks(lua_State* L)
{
	// vocation:getHealthGainTicks()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getHealthGainTicks());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetHealthGainAmount(lua_State* L)
{
	// vocation:getHealthGainAmount()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getHealthGainAmount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetManaGain(lua_State* L)
{
	// vocation:getManaGain()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getManaGain());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetManaGainTicks(lua_State* L)
{
	// vocation:getManaGainTicks()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getManaGainTicks());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetManaGainAmount(lua_State* L)
{
	// vocation:getManaGainAmount()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getManaGainAmount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetMaxSoul(lua_State* L)
{
	// vocation:getMaxSoul()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getSoulMax());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetSoulGainTicks(lua_State* L)
{
	// vocation:getSoulGainTicks()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getSoulGainTicks());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetAttackSpeed(lua_State* L)
{
	// vocation:getAttackSpeed()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getAttackSpeed());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetBaseSpeed(lua_State* L)
{
	// vocation:getBaseSpeed()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		lua_pushnumber(L, vocation->getBaseSpeed());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetDemotion(lua_State* L)
{
	// vocation:getDemotion()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (!vocation) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t fromId = vocation->getFromVocation();
	if (fromId == VOCATION_NONE) {
		lua_pushnil(L);
		return 1;
	}

	Vocation* demotedVocation = g_vocations.getVocation(fromId);
	if (demotedVocation && demotedVocation != vocation) {
		tfs::lua::pushUserdata<Vocation>(L, demotedVocation);
		tfs::lua::setMetatable(L, -1, "Vocation");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationGetPromotion(lua_State* L)
{
	// vocation:getPromotion()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (!vocation) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t promotedId = g_vocations.getPromotedVocation(vocation->getId());
	if (promotedId == VOCATION_NONE) {
		lua_pushnil(L);
		return 1;
	}

	Vocation* promotedVocation = g_vocations.getVocation(promotedId);
	if (promotedVocation && promotedVocation != vocation) {
		tfs::lua::pushUserdata<Vocation>(L, promotedVocation);
		tfs::lua::setMetatable(L, -1, "Vocation");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaVocationAllowsPvp(lua_State* L)
{
	// vocation:allowsPvp()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		tfs::lua::pushBoolean(L, vocation->allowsPvp());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Town
int luaTownCreate(lua_State* L)
{
	// Town(id or name)
	Town* town;
	if (tfs::lua::isNumber(L, 2)) {
		town = g_game->map.towns.getTown(tfs::lua::getNumber<uint32_t>(L, 2));
	} else if (lua_isstring(L, 2)) {
		town = g_game->map.towns.getTown(tfs::lua::getString(L, 2));
	} else {
		town = nullptr;
	}

	if (town) {
		tfs::lua::pushUserdata<Town>(L, town);
		tfs::lua::setMetatable(L, -1, "Town");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaTownGetId(lua_State* L)
{
	// town:getId()
	Town* town = tfs::lua::getUserdata<Town>(L, 1);
	if (town) {
		lua_pushnumber(L, town->getID());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaTownGetName(lua_State* L)
{
	// town:getName()
	Town* town = tfs::lua::getUserdata<Town>(L, 1);
	if (town) {
		tfs::lua::pushString(L, town->getName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaTownGetTemplePosition(lua_State* L)
{
	// town:getTemplePosition()
	Town* town = tfs::lua::getUserdata<Town>(L, 1);
	if (town) {
		tfs::lua::pushPosition(L, town->getTemplePosition());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// House
int luaHouseCreate(lua_State* L)
{
	// House(id)
	House* house = g_game->map.houses.getHouse(tfs::lua::getNumber<uint32_t>(L, 2));
	if (house) {
		tfs::lua::pushUserdata<House>(L, house);
		tfs::lua::setMetatable(L, -1, "House");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseGetId(lua_State* L)
{
	// house:getId()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		lua_pushnumber(L, house->getId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseGetName(lua_State* L)
{
	// house:getName()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		tfs::lua::pushString(L, house->getName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseGetTown(lua_State* L)
{
	// house:getTown()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	Town* town = g_game->map.towns.getTown(house->getTownId());
	if (town) {
		tfs::lua::pushUserdata<Town>(L, town);
		tfs::lua::setMetatable(L, -1, "Town");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseGetExitPosition(lua_State* L)
{
	// house:getExitPosition()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		tfs::lua::pushPosition(L, house->getEntryPosition());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseGetRent(lua_State* L)
{
	// house:getRent()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		lua_pushnumber(L, house->getRent());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseSetRent(lua_State* L)
{
	// house:setRent(rent)
	uint32_t rent = tfs::lua::getNumber<uint32_t>(L, 2);
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		house->setRent(rent);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseGetPaidUntil(lua_State* L)
{
	// house:getPaidUntil()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		lua_pushnumber(L, house->getPaidUntil());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseSetPaidUntil(lua_State* L)
{
	// house:setPaidUntil(timestamp)
	time_t timestamp = tfs::lua::getNumber<time_t>(L, 2);
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		house->setPaidUntil(timestamp);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseGetPayRentWarnings(lua_State* L)
{
	// house:getPayRentWarnings()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		lua_pushnumber(L, house->getPayRentWarnings());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseSetPayRentWarnings(lua_State* L)
{
	// house:setPayRentWarnings(warnings)
	uint32_t warnings = tfs::lua::getNumber<uint32_t>(L, 2);
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		house->setPayRentWarnings(warnings);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseGetOwnerName(lua_State* L)
{
	// house:getOwnerName()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		tfs::lua::pushString(L, house->getOwnerName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseGetOwnerGuid(lua_State* L)
{
	// house:getOwnerGuid()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		lua_pushnumber(L, house->getOwner());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseSetOwnerGuid(lua_State* L)
{
	// house:setOwnerGuid(guid[, updateDatabase = true])
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		uint32_t guid = tfs::lua::getNumber<uint32_t>(L, 2);
		bool updateDatabase = tfs::lua::getBoolean(L, 3, true);
		house->setOwner(guid, updateDatabase);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseStartTrade(lua_State* L)
{
	// house:startTrade(player, tradePartner)
	House* house = tfs::lua::getUserdata<House>(L, 1);
	Player* player = tfs::lua::getUserdata<Player>(L, 2);
	Player* tradePartner = tfs::lua::getUserdata<Player>(L, 3);

	if (!player || !tradePartner || !house) {
		lua_pushnil(L);
		return 1;
	}

	if (!Position::areInRange<2, 2, 0>(tradePartner->getPosition(), player->getPosition())) {
		lua_pushnumber(L, RETURNVALUE_TRADEPLAYERFARAWAY);
		return 1;
	}

	if (house->getOwner() != player->getGUID()) {
		lua_pushnumber(L, RETURNVALUE_YOUDONTOWNTHISHOUSE);
		return 1;
	}

	if (g_game->map.houses.getHouseByPlayerId(tradePartner->getGUID())) {
		lua_pushnumber(L, RETURNVALUE_TRADEPLAYERALREADYOWNSAHOUSE);
		return 1;
	}

	if (IOLoginData::hasBiddedOnHouse(tradePartner->getGUID())) {
		lua_pushnumber(L, RETURNVALUE_TRADEPLAYERHIGHESTBIDDER);
		return 1;
	}

	Item* transferItem = house->getTransferItem();
	if (!transferItem) {
		lua_pushnumber(L, RETURNVALUE_YOUCANNOTTRADETHISHOUSE);
		return 1;
	}

	transferItem->getParent()->setParent(player);
	if (!g_game->internalStartTrade(player, tradePartner, transferItem)) {
		house->resetTransferItem();
	}

	lua_pushnumber(L, RETURNVALUE_NOERROR);
	return 1;
}

int luaHouseGetBeds(lua_State* L)
{
	// house:getBeds()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	const auto& beds = house->getBeds();
	lua_createtable(L, beds.size(), 0);

	int index = 0;
	for (BedItem* bedItem : beds) {
		tfs::lua::pushUserdata<Item>(L, bedItem);
		tfs::lua::setItemMetatable(L, -1, bedItem);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaHouseGetBedCount(lua_State* L)
{
	// house:getBedCount()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		lua_pushnumber(L, house->getBedCount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseGetDoors(lua_State* L)
{
	// house:getDoors()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	const auto& doors = house->getDoors();
	lua_createtable(L, doors.size(), 0);

	int index = 0;
	for (Door* door : doors) {
		tfs::lua::pushUserdata<Item>(L, door);
		tfs::lua::setItemMetatable(L, -1, door);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaHouseGetDoorCount(lua_State* L)
{
	// house:getDoorCount()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		lua_pushnumber(L, house->getDoors().size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseGetDoorIdByPosition(lua_State* L)
{
	// house:getDoorIdByPosition(position)
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	Door* door = house->getDoorByPosition(tfs::lua::getPosition(L, 2));
	if (door) {
		lua_pushnumber(L, door->getDoorId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseGetTiles(lua_State* L)
{
	// house:getTiles()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	const auto& tiles = house->getTiles();
	lua_createtable(L, tiles.size(), 0);

	int index = 0;
	for (Tile* tile : tiles) {
		tfs::lua::pushUserdata<Tile>(L, tile);
		tfs::lua::setMetatable(L, -1, "Tile");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaHouseGetItems(lua_State* L)
{
	// house:getItems()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	const auto& tiles = house->getTiles();
	lua_newtable(L);

	int index = 0;
	for (Tile* tile : tiles) {
		TileItemVector* itemVector = tile->getItemList();
		if (itemVector) {
			for (Item* item : *itemVector) {
				tfs::lua::pushUserdata<Item>(L, item);
				tfs::lua::setItemMetatable(L, -1, item);
				lua_rawseti(L, -2, ++index);
			}
		}
	}
	return 1;
}

int luaHouseGetTileCount(lua_State* L)
{
	// house:getTileCount()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		lua_pushnumber(L, house->getTiles().size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaHouseCanEditAccessList(lua_State* L)
{
	// house:canEditAccessList(listId, player)
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	uint32_t listId = tfs::lua::getNumber<uint32_t>(L, 2);
	Player* player = tfs::lua::getPlayer(L, 3);

	tfs::lua::pushBoolean(L, house->canEditAccessList(listId, player));
	return 1;
}

int luaHouseGetAccessList(lua_State* L)
{
	// house:getAccessList(listId)
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	std::string list;
	uint32_t listId = tfs::lua::getNumber<uint32_t>(L, 2);
	if (house->getAccessList(listId, list)) {
		tfs::lua::pushString(L, list);
	} else {
		tfs::lua::pushBoolean(L, false);
	}
	return 1;
}

int luaHouseSetAccessList(lua_State* L)
{
	// house:setAccessList(listId, list)
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	uint32_t listId = tfs::lua::getNumber<uint32_t>(L, 2);
	const std::string& list = tfs::lua::getString(L, 3);
	house->setAccessList(listId, list);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaHouseKickPlayer(lua_State* L)
{
	// house:kickPlayer(player, targetPlayer)
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	tfs::lua::pushBoolean(L, house->kickPlayer(tfs::lua::getPlayer(L, 2), tfs::lua::getPlayer(L, 3)));
	return 1;
}

int luaHouseSave(lua_State* L)
{
	// house:save()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	tfs::lua::pushBoolean(L, IOMapSerialize::saveHouse(house));
	return 1;
}

// ItemType
int luaItemTypeCreate(lua_State* L)
{
	// ItemType(id or name)
	uint32_t id;
	if (tfs::lua::isNumber(L, 2)) {
		id = tfs::lua::getNumber<uint32_t>(L, 2);
	} else if (lua_isstring(L, 2)) {
		id = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
	} else {
		lua_pushnil(L);
		return 1;
	}

	const ItemType& itemType = Item::items[id];
	tfs::lua::pushUserdata<const ItemType>(L, &itemType);
	tfs::lua::setMetatable(L, -1, "ItemType");
	return 1;
}

int luaItemTypeIsCorpse(lua_State* L)
{
	// itemType:isCorpse()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->corpseType != RACE_NONE);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsDoor(lua_State* L)
{
	// itemType:isDoor()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->isDoor());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsContainer(lua_State* L)
{
	// itemType:isContainer()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->isContainer());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsFluidContainer(lua_State* L)
{
	// itemType:isFluidContainer()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->isFluidContainer());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsMovable(lua_State* L)
{
	// itemType:isMovable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->moveable);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsRune(lua_State* L)
{
	// itemType:isRune()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->isRune());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsStackable(lua_State* L)
{
	// itemType:isStackable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->stackable);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsReadable(lua_State* L)
{
	// itemType:isReadable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->canReadText);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsWritable(lua_State* L)
{
	// itemType:isWritable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->canWriteText);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsBlocking(lua_State* L)
{
	// itemType:isBlocking()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->blockProjectile || itemType->blockSolid);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsGroundTile(lua_State* L)
{
	// itemType:isGroundTile()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->isGroundTile());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsMagicField(lua_State* L)
{
	// itemType:isMagicField()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->isMagicField());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsUseable(lua_State* L)
{
	// itemType:isUseable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->isUseable());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsPickupable(lua_State* L)
{
	// itemType:isPickupable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->isPickupable());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetType(lua_State* L)
{
	// itemType:getType()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->type);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetGroup(lua_State* L)
{
	// itemType:getGroup()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->group);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetId(lua_State* L)
{
	// itemType:getId()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->id);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetClientId(lua_State* L)
{
	// itemType:getClientId()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->clientId);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetName(lua_State* L)
{
	// itemType:getName()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushString(L, itemType->name);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetPluralName(lua_State* L)
{
	// itemType:getPluralName()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushString(L, itemType->getPluralName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetArticle(lua_State* L)
{
	// itemType:getArticle()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushString(L, itemType->article);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetDescription(lua_State* L)
{
	// itemType:getDescription()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushString(L, itemType->description);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetSlotPosition(lua_State* L)
{
	// itemType:getSlotPosition()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->slotPosition);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetCharges(lua_State* L)
{
	// itemType:getCharges()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->charges);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetFluidSource(lua_State* L)
{
	// itemType:getFluidSource()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->fluidSource);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetCapacity(lua_State* L)
{
	// itemType:getCapacity()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->maxItems);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetWeight(lua_State* L)
{
	// itemType:getWeight([count = 1])
	uint16_t count = tfs::lua::getNumber<uint16_t>(L, 2, 1);

	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (!itemType) {
		lua_pushnil(L);
		return 1;
	}

	uint64_t weight = static_cast<uint64_t>(itemType->weight) * std::max<int32_t>(1, count);
	lua_pushnumber(L, weight);
	return 1;
}

int luaItemTypeGetWorth(lua_State* L)
{
	// itemType:getWorth()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (!itemType) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushnumber(L, itemType->worth);
	return 1;
}

int luaItemTypeGetHitChance(lua_State* L)
{
	// itemType:getHitChance()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->hitChance);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetShootRange(lua_State* L)
{
	// itemType:getShootRange()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->shootRange);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetAttack(lua_State* L)
{
	// itemType:getAttack()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->attack);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetAttackSpeed(lua_State* L)
{
	// itemType:getAttackSpeed()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->attackSpeed);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetDefense(lua_State* L)
{
	// itemType:getDefense()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->defense);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetExtraDefense(lua_State* L)
{
	// itemType:getExtraDefense()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->extraDefense);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetArmor(lua_State* L)
{
	// itemType:getArmor()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->armor);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetWeaponType(lua_State* L)
{
	// itemType:getWeaponType()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->weaponType);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetAmmoType(lua_State* L)
{
	// itemType:getAmmoType()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->ammoType);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetCorpseType(lua_State* L)
{
	// itemType:getCorpseType()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->corpseType);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetClassification(lua_State* L)
{
	// itemType:getClassification()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->classification);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetAbilities(lua_State* L)
{
	// itemType:getAbilities()
	ItemType* itemType = tfs::lua::getUserdata<ItemType>(L, 1);
	if (itemType) {
		Abilities& abilities = itemType->getAbilities();
		lua_createtable(L, 10, 12);
		tfs::lua::setField(L, "healthGain", abilities.healthGain);
		tfs::lua::setField(L, "healthTicks", abilities.healthTicks);
		tfs::lua::setField(L, "manaGain", abilities.manaGain);
		tfs::lua::setField(L, "manaTicks", abilities.manaTicks);
		tfs::lua::setField(L, "conditionImmunities", abilities.conditionImmunities);
		tfs::lua::setField(L, "conditionSuppressions", abilities.conditionSuppressions);
		tfs::lua::setField(L, "speed", abilities.speed);
		tfs::lua::setField(L, "elementDamage", abilities.elementDamage);
		tfs::lua::setField(L, "elementType", abilities.elementType);

		lua_pushboolean(L, abilities.manaShield);
		lua_setfield(L, -2, "manaShield");
		lua_pushboolean(L, abilities.invisible);
		lua_setfield(L, -2, "invisible");
		lua_pushboolean(L, abilities.regeneration);
		lua_setfield(L, -2, "regeneration");

		// Stats
		lua_createtable(L, 0, STAT_LAST + 1);
		for (int32_t i = STAT_FIRST; i <= STAT_LAST; i++) {
			lua_pushnumber(L, abilities.stats[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "stats");

		// Stats percent
		lua_createtable(L, 0, STAT_LAST + 1);
		for (int32_t i = STAT_FIRST; i <= STAT_LAST; i++) {
			lua_pushnumber(L, abilities.statsPercent[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "statsPercent");

		// Skills
		lua_createtable(L, 0, SKILL_LAST + 1);
		for (int32_t i = SKILL_FIRST; i <= SKILL_LAST; i++) {
			lua_pushnumber(L, abilities.skills[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "skills");

		// Special skills
		lua_createtable(L, 0, SPECIALSKILL_LAST + 1);
		for (int32_t i = SPECIALSKILL_FIRST; i <= SPECIALSKILL_LAST; i++) {
			lua_pushnumber(L, abilities.specialSkills[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "specialSkills");

		// Field absorb percent
		lua_createtable(L, 0, COMBAT_COUNT);
		for (int32_t i = 0; i < COMBAT_COUNT; i++) {
			lua_pushnumber(L, abilities.fieldAbsorbPercent[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "fieldAbsorbPercent");

		// Absorb percent
		lua_createtable(L, 0, COMBAT_COUNT);
		for (int32_t i = 0; i < COMBAT_COUNT; i++) {
			lua_pushnumber(L, abilities.absorbPercent[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "absorbPercent");

		// special magic level
		lua_createtable(L, 0, COMBAT_COUNT);
		for (int32_t i = 0; i < COMBAT_COUNT; i++) {
			lua_pushnumber(L, abilities.specialMagicLevelSkill[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "specialMagicLevel");

		// Damage boost percent
		lua_createtable(L, 0, COMBAT_COUNT);
		for (int32_t i = 0; i < COMBAT_COUNT; i++) {
			lua_pushnumber(L, abilities.boostPercent[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "boostPercent");

		// Reflect chance
		lua_createtable(L, 0, COMBAT_COUNT);
		for (int32_t i = 0; i < COMBAT_COUNT; i++) {
			lua_pushnumber(L, abilities.reflect[i].chance);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "reflectChance");

		// Reflect percent
		lua_createtable(L, 0, COMBAT_COUNT);
		for (int32_t i = 0; i < COMBAT_COUNT; i++) {
			lua_pushnumber(L, abilities.reflect[i].percent);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "reflectPercent");
	}
	return 1;
}

int luaItemTypeHasShowAttributes(lua_State* L)
{
	// itemType:hasShowAttributes()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->showAttributes);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeHasShowCount(lua_State* L)
{
	// itemType:hasShowCount()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->showCount);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeHasShowCharges(lua_State* L)
{
	// itemType:hasShowCharges()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->showCharges);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeHasShowDuration(lua_State* L)
{
	// itemType:hasShowDuration()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->showDuration);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeHasAllowDistRead(lua_State* L)
{
	// itemType:hasAllowDistRead()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->allowDistRead);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetWieldInfo(lua_State* L)
{
	// itemType:getWieldInfo()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->wieldInfo);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetDuration(lua_State* L)
{
	// itemType:getDuration()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->decayTime);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetLevelDoor(lua_State* L)
{
	// itemType:getLevelDoor()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->levelDoor);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetRuneSpellName(lua_State* L)
{
	// itemType:getRuneSpellName()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType && itemType->isRune()) {
		tfs::lua::pushString(L, itemType->runeSpellName);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetVocationString(lua_State* L)
{
	// itemType:getVocationString()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushString(L, itemType->vocationString);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetMinReqLevel(lua_State* L)
{
	// itemType:getMinReqLevel()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->minReqLevel);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetMinReqMagicLevel(lua_State* L)
{
	// itemType:getMinReqMagicLevel()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->minReqMagicLevel);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetMarketBuyStatistics(lua_State* L)
{
	// itemType:getMarketBuyStatistics()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		MarketStatistics* statistics = IOMarket::getInstance().getPurchaseStatistics(itemType->id);
		if (statistics) {
			lua_createtable(L, 4, 0);
			tfs::lua::setField(L, "numTransactions", statistics->numTransactions);
			tfs::lua::setField(L, "totalPrice", statistics->totalPrice);
			tfs::lua::setField(L, "highestPrice", statistics->highestPrice);
			tfs::lua::setField(L, "lowestPrice", statistics->lowestPrice);
		} else {
			lua_pushnil(L);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetMarketSellStatistics(lua_State* L)
{
	// itemType:getMarketSellStatistics()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		MarketStatistics* statistics = IOMarket::getInstance().getSaleStatistics(itemType->id);
		if (statistics) {
			lua_createtable(L, 4, 0);
			tfs::lua::setField(L, "numTransactions", statistics->numTransactions);
			tfs::lua::setField(L, "totalPrice", statistics->totalPrice);
			tfs::lua::setField(L, "highestPrice", statistics->highestPrice);
			tfs::lua::setField(L, "lowestPrice", statistics->lowestPrice);
		} else {
			lua_pushnil(L);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetElementType(lua_State* L)
{
	// itemType:getElementType()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (!itemType) {
		lua_pushnil(L);
		return 1;
	}

	auto& abilities = itemType->abilities;
	if (abilities) {
		lua_pushnumber(L, abilities->elementType);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetElementDamage(lua_State* L)
{
	// itemType:getElementDamage()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (!itemType) {
		lua_pushnil(L);
		return 1;
	}

	auto& abilities = itemType->abilities;
	if (abilities) {
		lua_pushnumber(L, abilities->elementDamage);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetTransformEquipId(lua_State* L)
{
	// itemType:getTransformEquipId()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->transformEquipTo);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetTransformDeEquipId(lua_State* L)
{
	// itemType:getTransformDeEquipId()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->transformDeEquipTo);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetDestroyId(lua_State* L)
{
	// itemType:getDestroyId()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->destroyTo);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetDecayId(lua_State* L)
{
	// itemType:getDecayId()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->decayTo);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeGetRequiredLevel(lua_State* L)
{
	// itemType:getRequiredLevel()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushnumber(L, itemType->minReqLevel);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeHasSubType(lua_State* L)
{
	// itemType:hasSubType()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->hasSubType());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaItemTypeIsStoreItem(lua_State* L)
{
	// itemType:isStoreItem()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushBoolean(L, itemType->storeItem);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Combat
int luaCombatCreate(lua_State* L)
{
	// Combat()
	tfs::lua::pushSharedPtr(L, g_luaEnvironment->createCombatObject(tfs::lua::getScriptEnv()->getScriptInterface()));
	tfs::lua::setMetatable(L, -1, "Combat");
	return 1;
}

int luaCombatDelete(lua_State* L)
{
	Combat_ptr& combat = tfs::lua::getSharedPtr<Combat>(L, 1);
	if (combat) {
		combat.reset();
	}
	return 0;
}

int luaCombatSetParameter(lua_State* L)
{
	// combat:setParameter(key, value)
	const Combat_ptr& combat = tfs::lua::getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	CombatParam_t key = tfs::lua::getNumber<CombatParam_t>(L, 2);
	uint32_t value;
	if (lua_isboolean(L, 3)) {
		value = tfs::lua::getBoolean(L, 3) ? 1 : 0;
	} else {
		value = tfs::lua::getNumber<uint32_t>(L, 3);
	}
	combat->setParam(key, value);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaCombatGetParameter(lua_State* L)
{
	// combat:getParameter(key)
	const Combat_ptr& combat = tfs::lua::getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	int32_t value = combat->getParam(tfs::lua::getNumber<CombatParam_t>(L, 2));
	if (value == std::numeric_limits<int32_t>().max()) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushnumber(L, value);
	return 1;
}

int luaCombatSetFormula(lua_State* L)
{
	// combat:setFormula(type, mina, minb, maxa, maxb)
	const Combat_ptr& combat = tfs::lua::getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	formulaType_t type = tfs::lua::getNumber<formulaType_t>(L, 2);
	double mina = tfs::lua::getNumber<double>(L, 3);
	double minb = tfs::lua::getNumber<double>(L, 4);
	double maxa = tfs::lua::getNumber<double>(L, 5);
	double maxb = tfs::lua::getNumber<double>(L, 6);
	combat->setPlayerCombatValues(type, mina, minb, maxa, maxb);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaCombatSetArea(lua_State* L)
{
	// combat:setArea(area)
	if (tfs::lua::getScriptEnv()->getScriptId() != EVENT_ID_LOADING) {
		reportErrorFunc(L, "This function can only be used while loading the script.");
		lua_pushnil(L);
		return 1;
	}

	const AreaCombat* area = g_luaEnvironment->getAreaObject(tfs::lua::getNumber<uint32_t>(L, 2));
	if (!area) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_AREA_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	const Combat_ptr& combat = tfs::lua::getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	combat->setArea(new AreaCombat(*area));
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaCombatAddCondition(lua_State* L)
{
	// combat:addCondition(condition)
	const Combat_ptr& combat = tfs::lua::getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	Condition* condition = tfs::lua::getUserdata<Condition>(L, 2);
	if (condition) {
		combat->addCondition(condition->clone());
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCombatClearConditions(lua_State* L)
{
	// combat:clearConditions()
	const Combat_ptr& combat = tfs::lua::getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	combat->clearConditions();
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaCombatSetCallback(lua_State* L)
{
	// combat:setCallback(key, function)
	const Combat_ptr& combat = tfs::lua::getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	CallBackParam_t key = tfs::lua::getNumber<CallBackParam_t>(L, 2);
	if (!combat->setCallback(key)) {
		lua_pushnil(L);
		return 1;
	}

	CallBack* callback = combat->getCallback(key);
	if (!callback) {
		lua_pushnil(L);
		return 1;
	}

	const std::string& function = tfs::lua::getString(L, 3);
	tfs::lua::pushBoolean(L, callback->loadCallBack(tfs::lua::getScriptEnv()->getScriptInterface(), function));
	return 1;
}

int luaCombatSetOrigin(lua_State* L)
{
	// combat:setOrigin(origin)
	const Combat_ptr& combat = tfs::lua::getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	combat->setOrigin(tfs::lua::getNumber<CombatOrigin>(L, 2));
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaCombatExecute(lua_State* L)
{
	// combat:execute(creature, variant)
	const Combat_ptr& combat = tfs::lua::getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	if (lua_isuserdata(L, 2)) {
		auto type = tfs::lua::getUserdataType(L, 2);
		if (type != tfs::lua::LuaData_Player && type != tfs::lua::LuaData_Monster && type != tfs::lua::LuaData_Npc) {
			tfs::lua::pushBoolean(L, false);
			return 1;
		}
	}

	Creature* creature = tfs::lua::getCreature(L, 2);

	const LuaVariant& variant = tfs::lua::getVariant(L, 3);
	switch (variant.type()) {
		case VARIANT_NUMBER: {
			Creature* target = g_game->getCreatureByID(variant.getNumber());
			if (!target) {
				tfs::lua::pushBoolean(L, false);
				return 1;
			}

			if (combat->hasArea()) {
				combat->doCombat(creature, target->getPosition());
			} else {
				combat->doCombat(creature, target);
			}
			break;
		}

		case VARIANT_POSITION: {
			combat->doCombat(creature, variant.getPosition());
			break;
		}

		case VARIANT_TARGETPOSITION: {
			if (combat->hasArea()) {
				combat->doCombat(creature, variant.getTargetPosition());
			} else {
				combat->postCombatEffects(creature, variant.getTargetPosition());
				g_game->addMagicEffect(variant.getTargetPosition(), CONST_ME_POFF);
			}
			break;
		}

		case VARIANT_STRING: {
			Player* target = g_game->getPlayerByName(variant.getString());
			if (!target) {
				tfs::lua::pushBoolean(L, false);
				return 1;
			}

			combat->doCombat(creature, target);
			break;
		}

		case VARIANT_NONE: {
			reportErrorFunc(L, tfs::lua::getErrorDesc(tfs::lua::LUA_ERROR_VARIANT_NOT_FOUND));
			tfs::lua::pushBoolean(L, false);
			return 1;
		}

		default: {
			break;
		}
	}

	tfs::lua::pushBoolean(L, true);
	return 1;
}

// Condition
int luaConditionCreate(lua_State* L)
{
	// Condition(conditionType[, conditionId = CONDITIONID_COMBAT])
	ConditionType_t conditionType = tfs::lua::getNumber<ConditionType_t>(L, 2);
	ConditionId_t conditionId = tfs::lua::getNumber<ConditionId_t>(L, 3, CONDITIONID_COMBAT);

	Condition* condition = Condition::createCondition(conditionId, conditionType, 0, 0);
	if (condition) {
		tfs::lua::pushUserdata<Condition>(L, condition);
		tfs::lua::setMetatable(L, -1, "Condition");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaConditionDelete(lua_State* L)
{
	// condition:delete()
	Condition** conditionPtr = tfs::lua::getRawUserdata<Condition>(L, 1);
	if (conditionPtr && *conditionPtr) {
		delete *conditionPtr;
		*conditionPtr = nullptr;
	}
	return 0;
}

int luaConditionGetId(lua_State* L)
{
	// condition:getId()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		lua_pushnumber(L, condition->getId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaConditionGetSubId(lua_State* L)
{
	// condition:getSubId()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		lua_pushnumber(L, condition->getSubId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaConditionGetType(lua_State* L)
{
	// condition:getType()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		lua_pushnumber(L, condition->getType());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaConditionGetIcons(lua_State* L)
{
	// condition:getIcons()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		lua_pushnumber(L, condition->getIcons());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaConditionGetEndTime(lua_State* L)
{
	// condition:getEndTime()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		lua_pushnumber(L, condition->getEndTime());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaConditionClone(lua_State* L)
{
	// condition:clone()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		tfs::lua::pushUserdata<Condition>(L, condition->clone());
		tfs::lua::setMetatable(L, -1, "Condition");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaConditionGetTicks(lua_State* L)
{
	// condition:getTicks()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		lua_pushnumber(L, condition->getTicks());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaConditionSetTicks(lua_State* L)
{
	// condition:setTicks(ticks)
	int32_t ticks = tfs::lua::getNumber<int32_t>(L, 2);
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		condition->setTicks(ticks);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaConditionSetParameter(lua_State* L)
{
	// condition:setParameter(key, value)
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (!condition) {
		lua_pushnil(L);
		return 1;
	}

	ConditionParam_t key = tfs::lua::getNumber<ConditionParam_t>(L, 2);
	int32_t value;
	if (lua_isboolean(L, 3)) {
		value = tfs::lua::getBoolean(L, 3) ? 1 : 0;
	} else {
		value = tfs::lua::getNumber<int32_t>(L, 3);
	}
	condition->setParam(key, value);
	tfs::lua::pushBoolean(L, true);
	return 1;
}

int luaConditionGetParameter(lua_State* L)
{
	// condition:getParameter(key)
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (!condition) {
		lua_pushnil(L);
		return 1;
	}

	int32_t value = condition->getParam(tfs::lua::getNumber<ConditionParam_t>(L, 2));
	if (value == std::numeric_limits<int32_t>().max()) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushnumber(L, value);
	return 1;
}

int luaConditionSetFormula(lua_State* L)
{
	// condition:setFormula(mina, minb, maxa, maxb)
	double maxb = tfs::lua::getNumber<double>(L, 5);
	double maxa = tfs::lua::getNumber<double>(L, 4);
	double minb = tfs::lua::getNumber<double>(L, 3);
	double mina = tfs::lua::getNumber<double>(L, 2);
	ConditionSpeed* condition = dynamic_cast<ConditionSpeed*>(tfs::lua::getUserdata<Condition>(L, 1));
	if (condition) {
		condition->setFormulaVars(mina, minb, maxa, maxb);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaConditionSetOutfit(lua_State* L)
{
	// condition:setOutfit(outfit)
	// condition:setOutfit(lookTypeEx, lookType, lookHead, lookBody, lookLegs, lookFeet[, lookAddons[, lookMount]])
	Outfit_t outfit;
	if (lua_istable(L, 2)) {
		outfit = tfs::lua::getOutfit(L, 2);
	} else {
		outfit.lookMount = tfs::lua::getNumber<uint16_t>(L, 9, outfit.lookMount);
		outfit.lookAddons = tfs::lua::getNumber<uint8_t>(L, 8, outfit.lookAddons);
		outfit.lookFeet = tfs::lua::getNumber<uint8_t>(L, 7);
		outfit.lookLegs = tfs::lua::getNumber<uint8_t>(L, 6);
		outfit.lookBody = tfs::lua::getNumber<uint8_t>(L, 5);
		outfit.lookHead = tfs::lua::getNumber<uint8_t>(L, 4);
		outfit.lookType = tfs::lua::getNumber<uint16_t>(L, 3);
		outfit.lookTypeEx = tfs::lua::getNumber<uint16_t>(L, 2);
	}

	ConditionOutfit* condition = dynamic_cast<ConditionOutfit*>(tfs::lua::getUserdata<Condition>(L, 1));
	if (condition) {
		condition->setOutfit(outfit);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaConditionAddDamage(lua_State* L)
{
	// condition:addDamage(rounds, time, value)
	int32_t value = tfs::lua::getNumber<int32_t>(L, 4);
	int32_t time = tfs::lua::getNumber<int32_t>(L, 3);
	int32_t rounds = tfs::lua::getNumber<int32_t>(L, 2);
	ConditionDamage* condition = dynamic_cast<ConditionDamage*>(tfs::lua::getUserdata<Condition>(L, 1));
	if (condition) {
		tfs::lua::pushBoolean(L, condition->addDamage(rounds, time, value));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Outfit
int luaOutfitCreate(lua_State* L)
{
	// Outfit(looktype)
	const Outfit* outfit = Outfits::getInstance().getOutfitByLookType(tfs::lua::getNumber<uint16_t>(L, 2));
	if (outfit) {
		tfs::lua::pushOutfitClass(L, outfit);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaOutfitCompare(lua_State* L)
{
	// outfit == outfitEx
	Outfit outfitEx = tfs::lua::getOutfitClass(L, 2);
	Outfit outfit = tfs::lua::getOutfitClass(L, 1);
	tfs::lua::pushBoolean(L, outfit == outfitEx);
	return 1;
}

// MonsterType
int luaMonsterTypeCreate(lua_State* L)
{
	// MonsterType(name)
	MonsterType* monsterType = g_monsters.getMonsterType(tfs::lua::getString(L, 2));
	if (monsterType) {
		tfs::lua::pushUserdata<MonsterType>(L, monsterType);
		tfs::lua::setMetatable(L, -1, "MonsterType");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeIsAttackable(lua_State* L)
{
	// get: monsterType:isAttackable() set: monsterType:isAttackable(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.isAttackable);
		} else {
			monsterType->info.isAttackable = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeIsChallengeable(lua_State* L)
{
	// get: monsterType:isChallengeable() set: monsterType:isChallengeable(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.isChallengeable);
		} else {
			monsterType->info.isChallengeable = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeIsConvinceable(lua_State* L)
{
	// get: monsterType:isConvinceable() set: monsterType:isConvinceable(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.isConvinceable);
		} else {
			monsterType->info.isConvinceable = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeIsSummonable(lua_State* L)
{
	// get: monsterType:isSummonable() set: monsterType:isSummonable(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.isSummonable);
		} else {
			monsterType->info.isSummonable = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeIsIgnoringSpawnBlock(lua_State* L)
{
	// get: monsterType:isIgnoringSpawnBlock() set: monsterType:isIgnoringSpawnBlock(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.isIgnoringSpawnBlock);
		} else {
			monsterType->info.isIgnoringSpawnBlock = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeIsIllusionable(lua_State* L)
{
	// get: monsterType:isIllusionable() set: monsterType:isIllusionable(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.isIllusionable);
		} else {
			monsterType->info.isIllusionable = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeIsHostile(lua_State* L)
{
	// get: monsterType:isHostile() set: monsterType:isHostile(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.isHostile);
		} else {
			monsterType->info.isHostile = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeIsPushable(lua_State* L)
{
	// get: monsterType:isPushable() set: monsterType:isPushable(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.pushable);
		} else {
			monsterType->info.pushable = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeIsHealthHidden(lua_State* L)
{
	// get: monsterType:isHealthHidden() set: monsterType:isHealthHidden(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.hiddenHealth);
		} else {
			monsterType->info.hiddenHealth = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeIsBoss(lua_State* L)
{
	// get: monsterType:isBoss() set: monsterType:isBoss(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.isBoss);
		} else {
			monsterType->info.isBoss = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeCanPushItems(lua_State* L)
{
	// get: monsterType:canPushItems() set: monsterType:canPushItems(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.canPushItems);
		} else {
			monsterType->info.canPushItems = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeCanPushCreatures(lua_State* L)
{
	// get: monsterType:canPushCreatures() set: monsterType:canPushCreatures(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.canPushCreatures);
		} else {
			monsterType->info.canPushCreatures = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeCanWalkOnEnergy(lua_State* L)
{
	// get: monsterType:canWalkOnEnergy() set: monsterType:canWalkOnEnergy(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.canWalkOnEnergy);
		} else {
			monsterType->info.canWalkOnEnergy = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeCanWalkOnFire(lua_State* L)
{
	// get: monsterType:canWalkOnFire() set: monsterType:canWalkOnFire(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.canWalkOnFire);
		} else {
			monsterType->info.canWalkOnFire = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeCanWalkOnPoison(lua_State* L)
{
	// get: monsterType:canWalkOnPoison() set: monsterType:canWalkOnPoison(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, monsterType->info.canWalkOnPoison);
		} else {
			monsterType->info.canWalkOnPoison = tfs::lua::getBoolean(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int32_t luaMonsterTypeName(lua_State* L)
{
	// get: monsterType:name() set: monsterType:name(name)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushString(L, monsterType->name);
		} else {
			monsterType->name = tfs::lua::getString(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeNameDescription(lua_State* L)
{
	// get: monsterType:nameDescription() set: monsterType:nameDescription(desc)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushString(L, monsterType->nameDescription);
		} else {
			monsterType->nameDescription = tfs::lua::getString(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeHealth(lua_State* L)
{
	// get: monsterType:health() set: monsterType:health(health)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.health);
		} else {
			monsterType->info.health = tfs::lua::getNumber<int32_t>(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeMaxHealth(lua_State* L)
{
	// get: monsterType:maxHealth() set: monsterType:maxHealth(health)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.healthMax);
		} else {
			monsterType->info.healthMax = tfs::lua::getNumber<int32_t>(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeRunHealth(lua_State* L)
{
	// get: monsterType:runHealth() set: monsterType:runHealth(health)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.runAwayHealth);
		} else {
			monsterType->info.runAwayHealth = tfs::lua::getNumber<int32_t>(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeExperience(lua_State* L)
{
	// get: monsterType:experience() set: monsterType:experience(exp)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.experience);
		} else {
			monsterType->info.experience = tfs::lua::getNumber<uint64_t>(L, 2);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeSkull(lua_State* L)
{
	// get: monsterType:skull() set: monsterType:skull(str/constant)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.skull);
		} else {
			if (tfs::lua::isNumber(L, 2)) {
				monsterType->info.skull = tfs::lua::getNumber<Skulls_t>(L, 2);
			} else {
				monsterType->info.skull = getSkullType(tfs::lua::getString(L, 2));
			}
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeCombatImmunities(lua_State* L)
{
	// get: monsterType:combatImmunities() set: monsterType:combatImmunities(immunity)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.damageImmunities);
		} else {
			std::string immunity = tfs::lua::getString(L, 2);
			if (immunity == "physical") {
				monsterType->info.damageImmunities |= COMBAT_PHYSICALDAMAGE;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "energy") {
				monsterType->info.damageImmunities |= COMBAT_ENERGYDAMAGE;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "fire") {
				monsterType->info.damageImmunities |= COMBAT_FIREDAMAGE;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "poison" || immunity == "earth") {
				monsterType->info.damageImmunities |= COMBAT_EARTHDAMAGE;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "drown") {
				monsterType->info.damageImmunities |= COMBAT_DROWNDAMAGE;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "ice") {
				monsterType->info.damageImmunities |= COMBAT_ICEDAMAGE;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "holy") {
				monsterType->info.damageImmunities |= COMBAT_HOLYDAMAGE;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "death") {
				monsterType->info.damageImmunities |= COMBAT_DEATHDAMAGE;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "lifedrain") {
				monsterType->info.damageImmunities |= COMBAT_LIFEDRAIN;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "manadrain") {
				monsterType->info.damageImmunities |= COMBAT_MANADRAIN;
				tfs::lua::pushBoolean(L, true);
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Unknown immunity name " << immunity
				          << " for monster: " << monsterType->name << std::endl;
				lua_pushnil(L);
			}
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeConditionImmunities(lua_State* L)
{
	// get: monsterType:conditionImmunities() set: monsterType:conditionImmunities(immunity)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.conditionImmunities);
		} else {
			std::string immunity = tfs::lua::getString(L, 2);
			if (immunity == "physical") {
				monsterType->info.conditionImmunities |= CONDITION_BLEEDING;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "energy") {
				monsterType->info.conditionImmunities |= CONDITION_ENERGY;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "fire") {
				monsterType->info.conditionImmunities |= CONDITION_FIRE;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "poison" || immunity == "earth") {
				monsterType->info.conditionImmunities |= CONDITION_POISON;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "drown") {
				monsterType->info.conditionImmunities |= CONDITION_DROWN;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "ice") {
				monsterType->info.conditionImmunities |= CONDITION_FREEZING;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "holy") {
				monsterType->info.conditionImmunities |= CONDITION_DAZZLED;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "death") {
				monsterType->info.conditionImmunities |= CONDITION_CURSED;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "paralyze") {
				monsterType->info.conditionImmunities |= CONDITION_PARALYZE;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "outfit") {
				monsterType->info.conditionImmunities |= CONDITION_OUTFIT;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "drunk") {
				monsterType->info.conditionImmunities |= CONDITION_DRUNK;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "invisible" || immunity == "invisibility") {
				monsterType->info.conditionImmunities |= CONDITION_INVISIBLE;
				tfs::lua::pushBoolean(L, true);
			} else if (immunity == "bleed") {
				monsterType->info.conditionImmunities |= CONDITION_BLEEDING;
				tfs::lua::pushBoolean(L, true);
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Unknown immunity name " << immunity
				          << " for monster: " << monsterType->name << std::endl;
				lua_pushnil(L);
			}
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeGetAttackList(lua_State* L)
{
	// monsterType:getAttackList()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, monsterType->info.attackSpells.size(), 0);

	int index = 0;
	for (const auto& spellBlock : monsterType->info.attackSpells) {
		lua_createtable(L, 0, 8);

		tfs::lua::setField(L, "chance", spellBlock.chance);
		tfs::lua::setField(L, "isCombatSpell", spellBlock.combatSpell ? 1 : 0);
		tfs::lua::setField(L, "isMelee", spellBlock.isMelee ? 1 : 0);
		tfs::lua::setField(L, "minCombatValue", spellBlock.minCombatValue);
		tfs::lua::setField(L, "maxCombatValue", spellBlock.maxCombatValue);
		tfs::lua::setField(L, "range", spellBlock.range);
		tfs::lua::setField(L, "speed", spellBlock.speed);
		tfs::lua::pushUserdata<CombatSpell>(L, static_cast<CombatSpell*>(spellBlock.spell));
		lua_setfield(L, -2, "spell");

		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaMonsterTypeAddAttack(lua_State* L)
{
	// monsterType:addAttack(monsterspell)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 2);
		if (spell) {
			spellBlock_t sb;
			if (g_monsters.deserializeSpell(spell, sb, monsterType->name)) {
				monsterType->info.attackSpells.push_back(std::move(sb));
			} else {
				std::cout << monsterType->name << std::endl;
				std::cout << "[Warning - Monsters::loadMonster] Cant load spell. " << spell->name << std::endl;
			}
		} else {
			lua_pushnil(L);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeGetDefenseList(lua_State* L)
{
	// monsterType:getDefenseList()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, monsterType->info.defenseSpells.size(), 0);

	int index = 0;
	for (const auto& spellBlock : monsterType->info.defenseSpells) {
		lua_createtable(L, 0, 8);

		tfs::lua::setField(L, "chance", spellBlock.chance);
		tfs::lua::setField(L, "isCombatSpell", spellBlock.combatSpell ? 1 : 0);
		tfs::lua::setField(L, "isMelee", spellBlock.isMelee ? 1 : 0);
		tfs::lua::setField(L, "minCombatValue", spellBlock.minCombatValue);
		tfs::lua::setField(L, "maxCombatValue", spellBlock.maxCombatValue);
		tfs::lua::setField(L, "range", spellBlock.range);
		tfs::lua::setField(L, "speed", spellBlock.speed);
		tfs::lua::pushUserdata<CombatSpell>(L, static_cast<CombatSpell*>(spellBlock.spell));
		lua_setfield(L, -2, "spell");

		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaMonsterTypeAddDefense(lua_State* L)
{
	// monsterType:addDefense(monsterspell)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 2);
		if (spell) {
			spellBlock_t sb;
			if (g_monsters.deserializeSpell(spell, sb, monsterType->name)) {
				monsterType->info.defenseSpells.push_back(std::move(sb));
			} else {
				std::cout << monsterType->name << std::endl;
				std::cout << "[Warning - Monsters::loadMonster] Cant load spell. " << spell->name << std::endl;
			}
		} else {
			lua_pushnil(L);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeGetElementList(lua_State* L)
{
	// monsterType:getElementList()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, monsterType->info.elementMap.size(), 0);
	for (const auto& elementEntry : monsterType->info.elementMap) {
		lua_pushnumber(L, elementEntry.second);
		lua_rawseti(L, -2, elementEntry.first);
	}
	return 1;
}

int luaMonsterTypeAddElement(lua_State* L)
{
	// monsterType:addElement(type, percent)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		CombatType_t element = tfs::lua::getNumber<CombatType_t>(L, 2);
		monsterType->info.elementMap[element] = tfs::lua::getNumber<int32_t>(L, 3);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeGetVoices(lua_State* L)
{
	// monsterType:getVoices()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	int index = 0;
	lua_createtable(L, monsterType->info.voiceVector.size(), 0);
	for (const auto& voiceBlock : monsterType->info.voiceVector) {
		lua_createtable(L, 0, 2);
		tfs::lua::setField(L, "text", voiceBlock.text);
		tfs::lua::setField(L, "yellText", voiceBlock.yellText);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaMonsterTypeAddVoice(lua_State* L)
{
	// monsterType:addVoice(sentence, interval, chance, yell)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		voiceBlock_t voice;
		voice.text = tfs::lua::getString(L, 2);
		monsterType->info.yellSpeedTicks = tfs::lua::getNumber<uint32_t>(L, 3);
		monsterType->info.yellChance = tfs::lua::getNumber<uint32_t>(L, 4);
		voice.yellText = tfs::lua::getBoolean(L, 5);
		monsterType->info.voiceVector.push_back(voice);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeGetLoot(lua_State* L)
{
	// monsterType:getLoot()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	tfs::lua::pushLoot(L, monsterType->info.lootItems);
	return 1;
}

int luaMonsterTypeAddLoot(lua_State* L)
{
	// monsterType:addLoot(loot)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		Loot* loot = tfs::lua::getUserdata<Loot>(L, 2);
		if (loot) {
			monsterType->loadLoot(monsterType, loot->lootBlock);
			tfs::lua::pushBoolean(L, true);
		} else {
			lua_pushnil(L);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeGetCreatureEvents(lua_State* L)
{
	// monsterType:getCreatureEvents()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	int index = 0;
	lua_createtable(L, monsterType->info.scripts.size(), 0);
	for (const std::string& creatureEvent : monsterType->info.scripts) {
		tfs::lua::pushString(L, creatureEvent);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaMonsterTypeRegisterEvent(lua_State* L)
{
	// monsterType:registerEvent(name)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		monsterType->info.scripts.push_back(tfs::lua::getString(L, 2));
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeEventOnCallback(lua_State* L)
{
	// monsterType:onThink(callback)
	// monsterType:onAppear(callback)
	// monsterType:onDisappear(callback)
	// monsterType:onMove(callback)
	// monsterType:onSay(callback)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (monsterType->loadCallback(&g_scripts->getScriptInterface())) {
			tfs::lua::pushBoolean(L, true);
			return 1;
		}
		tfs::lua::pushBoolean(L, false);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeEventType(lua_State* L)
{
	// monstertype:eventType(event)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		monsterType->info.eventType = tfs::lua::getNumber<MonstersEvent_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeGetSummonList(lua_State* L)
{
	// monsterType:getSummonList()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	int index = 0;
	lua_createtable(L, monsterType->info.summons.size(), 0);
	for (const auto& summonBlock : monsterType->info.summons) {
		lua_createtable(L, 0, 3);
		tfs::lua::setField(L, "name", summonBlock.name);
		tfs::lua::setField(L, "speed", summonBlock.speed);
		tfs::lua::setField(L, "chance", summonBlock.chance);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaMonsterTypeAddSummon(lua_State* L)
{
	// monsterType:addSummon(name, interval, chance[, max = -1])
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		summonBlock_t summon;
		summon.name = tfs::lua::getString(L, 2);
		summon.speed = tfs::lua::getNumber<int32_t>(L, 3);
		summon.chance = tfs::lua::getNumber<int32_t>(L, 4);
		summon.max = tfs::lua::getNumber<int32_t>(L, 5, -1);
		monsterType->info.summons.push_back(summon);
		lua_pushboolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeMaxSummons(lua_State* L)
{
	// get: monsterType:maxSummons() set: monsterType:maxSummons(ammount)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.maxSummons);
		} else {
			monsterType->info.maxSummons = tfs::lua::getNumber<uint32_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeArmor(lua_State* L)
{
	// get: monsterType:armor() set: monsterType:armor(armor)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.armor);
		} else {
			monsterType->info.armor = tfs::lua::getNumber<int32_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeDefense(lua_State* L)
{
	// get: monsterType:defense() set: monsterType:defense(defense)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.defense);
		} else {
			monsterType->info.defense = tfs::lua::getNumber<int32_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeOutfit(lua_State* L)
{
	// get: monsterType:outfit() set: monsterType:outfit(outfit)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushOutfit(L, monsterType->info.outfit);
		} else {
			monsterType->info.outfit = tfs::lua::getOutfit(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeRace(lua_State* L)
{
	// get: monsterType:race() set: monsterType:race(race)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	std::string race = tfs::lua::getString(L, 2);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.race);
		} else {
			if (race == "venom") {
				monsterType->info.race = RACE_VENOM;
			} else if (race == "blood") {
				monsterType->info.race = RACE_BLOOD;
			} else if (race == "undead") {
				monsterType->info.race = RACE_UNDEAD;
			} else if (race == "fire") {
				monsterType->info.race = RACE_FIRE;
			} else if (race == "energy") {
				monsterType->info.race = RACE_ENERGY;
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Unknown race type " << race << "." << std::endl;
				lua_pushnil(L);
				return 1;
			}
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeCorpseId(lua_State* L)
{
	// get: monsterType:corpseId() set: monsterType:corpseId(id)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.lookcorpse);
		} else {
			monsterType->info.lookcorpse = tfs::lua::getNumber<uint16_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeManaCost(lua_State* L)
{
	// get: monsterType:manaCost() set: monsterType:manaCost(mana)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.manaCost);
		} else {
			monsterType->info.manaCost = tfs::lua::getNumber<uint32_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeBaseSpeed(lua_State* L)
{
	// get: monsterType:baseSpeed() set: monsterType:baseSpeed(speed)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.baseSpeed);
		} else {
			monsterType->info.baseSpeed = tfs::lua::getNumber<uint32_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeLight(lua_State* L)
{
	// get: monsterType:light() set: monsterType:light(color, level)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}
	if (lua_gettop(L) == 1) {
		lua_pushnumber(L, monsterType->info.light.level);
		lua_pushnumber(L, monsterType->info.light.color);
		return 2;
	} else {
		monsterType->info.light.color = tfs::lua::getNumber<uint8_t>(L, 2);
		monsterType->info.light.level = tfs::lua::getNumber<uint8_t>(L, 3);
		lua_pushboolean(L, true);
	}
	return 1;
}

int luaMonsterTypeStaticAttackChance(lua_State* L)
{
	// get: monsterType:staticAttackChance() set: monsterType:staticAttackChance(chance)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.staticAttackChance);
		} else {
			monsterType->info.staticAttackChance = tfs::lua::getNumber<uint32_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeTargetDistance(lua_State* L)
{
	// get: monsterType:targetDistance() set: monsterType:targetDistance(distance)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.targetDistance);
		} else {
			monsterType->info.targetDistance = tfs::lua::getNumber<int32_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeYellChance(lua_State* L)
{
	// get: monsterType:yellChance() set: monsterType:yellChance(chance)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.yellChance);
		} else {
			monsterType->info.yellChance = tfs::lua::getNumber<uint32_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeYellSpeedTicks(lua_State* L)
{
	// get: monsterType:yellSpeedTicks() set: monsterType:yellSpeedTicks(rate)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.yellSpeedTicks);
		} else {
			monsterType->info.yellSpeedTicks = tfs::lua::getNumber<uint32_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeChangeTargetChance(lua_State* L)
{
	// get: monsterType:changeTargetChance() set: monsterType:changeTargetChance(chance)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.changeTargetChance);
		} else {
			monsterType->info.changeTargetChance = tfs::lua::getNumber<int32_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterTypeChangeTargetSpeed(lua_State* L)
{
	// get: monsterType:changeTargetSpeed() set: monsterType:changeTargetSpeed(speed)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, monsterType->info.changeTargetSpeed);
		} else {
			monsterType->info.changeTargetSpeed = tfs::lua::getNumber<uint32_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Loot
int luaCreateLoot(lua_State* L)
{
	// Loot() will create a new loot item
	Loot* loot = new Loot();
	if (loot) {
		tfs::lua::pushUserdata<Loot>(L, loot);
		tfs::lua::setMetatable(L, -1, "Loot");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaDeleteLoot(lua_State* L)
{
	// loot:delete() loot:__gc()
	Loot** lootPtr = tfs::lua::getRawUserdata<Loot>(L, 1);
	if (lootPtr && *lootPtr) {
		delete *lootPtr;
		*lootPtr = nullptr;
	}
	return 0;
}

int luaLootSetId(lua_State* L)
{
	// loot:setId(id or name)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		if (tfs::lua::isNumber(L, 2)) {
			loot->lootBlock.id = tfs::lua::getNumber<uint16_t>(L, 2);
		} else {
			auto name = tfs::lua::getString(L, 2);
			auto ids = Item::items.nameToItems.equal_range(boost::algorithm::to_lower_copy(name));

			if (ids.first == Item::items.nameToItems.cend()) {
				std::cout << "[Warning - Loot:setId] Unknown loot item \"" << name << "\". " << std::endl;
				tfs::lua::pushBoolean(L, false);
				return 1;
			}

			if (std::next(ids.first) != ids.second) {
				std::cout << "[Warning - Loot:setId] Non-unique loot item \"" << name << "\". " << std::endl;
				tfs::lua::pushBoolean(L, false);
				return 1;
			}

			loot->lootBlock.id = ids.first->second;
		}
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaLootSetSubType(lua_State* L)
{
	// loot:setSubType(type)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.subType = tfs::lua::getNumber<uint16_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaLootSetChance(lua_State* L)
{
	// loot:setChance(chance)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.chance = tfs::lua::getNumber<uint32_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaLootSetMaxCount(lua_State* L)
{
	// loot:setMaxCount(max)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.countmax = tfs::lua::getNumber<uint32_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaLootSetActionId(lua_State* L)
{
	// loot:setActionId(actionid)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.actionId = tfs::lua::getNumber<uint32_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaLootSetDescription(lua_State* L)
{
	// loot:setDescription(desc)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.text = tfs::lua::getString(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaLootAddChildLoot(lua_State* L)
{
	// loot:addChildLoot(loot)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.childLoot.push_back(tfs::lua::getUserdata<Loot>(L, 2)->lootBlock);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// MonsterSpell
int luaCreateMonsterSpell(lua_State* L)
{
	// MonsterSpell() will create a new Monster Spell
	MonsterSpell* spell = new MonsterSpell();
	if (spell) {
		tfs::lua::pushUserdata<MonsterSpell>(L, spell);
		tfs::lua::setMetatable(L, -1, "MonsterSpell");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaDeleteMonsterSpell(lua_State* L)
{
	// monsterSpell:delete() monsterSpell:__gc()
	MonsterSpell** monsterSpellPtr = tfs::lua::getRawUserdata<MonsterSpell>(L, 1);
	if (monsterSpellPtr && *monsterSpellPtr) {
		delete *monsterSpellPtr;
		*monsterSpellPtr = nullptr;
	}
	return 0;
}

int luaMonsterSpellSetType(lua_State* L)
{
	// monsterSpell:setType(type)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->name = tfs::lua::getString(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetScriptName(lua_State* L)
{
	// monsterSpell:setScriptName(name)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->scriptName = tfs::lua::getString(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetChance(lua_State* L)
{
	// monsterSpell:setChance(chance)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->chance = tfs::lua::getNumber<uint8_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetInterval(lua_State* L)
{
	// monsterSpell:setInterval(interval)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->interval = tfs::lua::getNumber<uint16_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetRange(lua_State* L)
{
	// monsterSpell:setRange(range)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->range = tfs::lua::getNumber<uint8_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetCombatValue(lua_State* L)
{
	// monsterSpell:setCombatValue(min, max)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->minCombatValue = tfs::lua::getNumber<int32_t>(L, 2);
		spell->maxCombatValue = tfs::lua::getNumber<int32_t>(L, 3);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetCombatType(lua_State* L)
{
	// monsterSpell:setCombatType(combatType_t)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->combatType = tfs::lua::getNumber<CombatType_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetAttackValue(lua_State* L)
{
	// monsterSpell:setAttackValue(attack, skill)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->attack = tfs::lua::getNumber<int32_t>(L, 2);
		spell->skill = tfs::lua::getNumber<int32_t>(L, 3);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetNeedTarget(lua_State* L)
{
	// monsterSpell:setNeedTarget(bool)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->needTarget = tfs::lua::getBoolean(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetNeedDirection(lua_State* L)
{
	// monsterSpell:setNeedDirection(bool)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->needDirection = tfs::lua::getBoolean(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetCombatLength(lua_State* L)
{
	// monsterSpell:setCombatLength(length)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->length = tfs::lua::getNumber<int32_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetCombatSpread(lua_State* L)
{
	// monsterSpell:setCombatSpread(spread)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->spread = tfs::lua::getNumber<int32_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetCombatRadius(lua_State* L)
{
	// monsterSpell:setCombatRadius(radius)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->radius = tfs::lua::getNumber<int32_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetCombatRing(lua_State* L)
{
	// monsterSpell:setCombatRing(ring)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->ring = tfs::lua::getNumber<int32_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetConditionType(lua_State* L)
{
	// monsterSpell:setConditionType(type)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->conditionType = tfs::lua::getNumber<ConditionType_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetConditionDamage(lua_State* L)
{
	// monsterSpell:setConditionDamage(min, max, start)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->conditionMinDamage = tfs::lua::getNumber<int32_t>(L, 2);
		spell->conditionMaxDamage = tfs::lua::getNumber<int32_t>(L, 3);
		spell->conditionStartDamage = tfs::lua::getNumber<int32_t>(L, 4);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetConditionSpeedChange(lua_State* L)
{
	// monsterSpell:setConditionSpeedChange(minSpeed[, maxSpeed])
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->minSpeedChange = tfs::lua::getNumber<int32_t>(L, 2);
		spell->maxSpeedChange = tfs::lua::getNumber<int32_t>(L, 3, 0);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetConditionDuration(lua_State* L)
{
	// monsterSpell:setConditionDuration(duration)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->duration = tfs::lua::getNumber<int32_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetConditionDrunkenness(lua_State* L)
{
	// monsterSpell:setConditionDrunkenness(drunkenness)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->drunkenness = tfs::lua::getNumber<uint8_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetConditionTickInterval(lua_State* L)
{
	// monsterSpell:setConditionTickInterval(interval)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->tickInterval = tfs::lua::getNumber<int32_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetCombatShootEffect(lua_State* L)
{
	// monsterSpell:setCombatShootEffect(effect)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->shoot = tfs::lua::getNumber<ShootType_t>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetCombatEffect(lua_State* L)
{
	// monsterSpell:setCombatEffect(effect)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->effect = tfs::lua::getNumber<MagicEffectClasses>(L, 2);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaMonsterSpellSetOutfit(lua_State* L)
{
	// monsterSpell:setOutfit(outfit)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		if (lua_istable(L, 2)) {
			spell->outfit = tfs::lua::getOutfit(L, 2);
		} else if (tfs::lua::isNumber(L, 2)) {
			spell->outfit.lookTypeEx = tfs::lua::getNumber<uint16_t>(L, 2);
		} else if (lua_isstring(L, 2)) {
			MonsterType* mType = g_monsters.getMonsterType(tfs::lua::getString(L, 2));
			if (mType) {
				spell->outfit = mType->info.outfit;
			}
		}
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Party
int32_t luaPartyCreate(lua_State* L)
{
	// Party(userdata)
	Player* player = tfs::lua::getUserdata<Player>(L, 2);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	Party* party = player->getParty();
	if (!party) {
		party = new Party(player);
		g_game->updatePlayerShield(player);
		player->sendCreatureSkull(player);
		tfs::lua::pushUserdata<Party>(L, party);
		tfs::lua::setMetatable(L, -1, "Party");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartyDisband(lua_State* L)
{
	// party:disband()
	Party** partyPtr = tfs::lua::getRawUserdata<Party>(L, 1);
	if (partyPtr && *partyPtr) {
		Party*& party = *partyPtr;
		party->disband();
		party = nullptr;
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartyGetLeader(lua_State* L)
{
	// party:getLeader()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (!party) {
		lua_pushnil(L);
		return 1;
	}

	Player* leader = party->getLeader();
	if (leader) {
		tfs::lua::pushUserdata<Player>(L, leader);
		tfs::lua::setMetatable(L, -1, "Player");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartySetLeader(lua_State* L)
{
	// party:setLeader(player)
	Player* player = tfs::lua::getPlayer(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party && player) {
		tfs::lua::pushBoolean(L, party->passPartyLeadership(player));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartyGetMembers(lua_State* L)
{
	// party:getMembers()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (!party) {
		lua_pushnil(L);
		return 1;
	}

	int index = 0;
	lua_createtable(L, party->getMemberCount(), 0);
	for (Player* player : party->getMembers()) {
		tfs::lua::pushUserdata<Player>(L, player);
		tfs::lua::setMetatable(L, -1, "Player");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int luaPartyGetMemberCount(lua_State* L)
{
	// party:getMemberCount()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		lua_pushnumber(L, party->getMemberCount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartyGetInvitees(lua_State* L)
{
	// party:getInvitees()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		lua_createtable(L, party->getInvitationCount(), 0);

		int index = 0;
		for (Player* player : party->getInvitees()) {
			tfs::lua::pushUserdata<Player>(L, player);
			tfs::lua::setMetatable(L, -1, "Player");
			lua_rawseti(L, -2, ++index);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartyGetInviteeCount(lua_State* L)
{
	// party:getInviteeCount()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		lua_pushnumber(L, party->getInvitationCount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartyAddInvite(lua_State* L)
{
	// party:addInvite(player)
	Player* player = tfs::lua::getPlayer(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party && player) {
		tfs::lua::pushBoolean(L, party->invitePlayer(*player));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartyRemoveInvite(lua_State* L)
{
	// party:removeInvite(player)
	Player* player = tfs::lua::getPlayer(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party && player) {
		tfs::lua::pushBoolean(L, party->removeInvite(*player));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartyAddMember(lua_State* L)
{
	// party:addMember(player)
	Player* player = tfs::lua::getPlayer(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party && player) {
		tfs::lua::pushBoolean(L, party->joinParty(*player));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartyRemoveMember(lua_State* L)
{
	// party:removeMember(player)
	Player* player = tfs::lua::getPlayer(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party && player) {
		tfs::lua::pushBoolean(L, party->leaveParty(player));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartyIsSharedExperienceActive(lua_State* L)
{
	// party:isSharedExperienceActive()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		tfs::lua::pushBoolean(L, party->isSharedExperienceActive());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartyIsSharedExperienceEnabled(lua_State* L)
{
	// party:isSharedExperienceEnabled()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		tfs::lua::pushBoolean(L, party->isSharedExperienceEnabled());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartyShareExperience(lua_State* L)
{
	// party:shareExperience(experience)
	uint64_t experience = tfs::lua::getNumber<uint64_t>(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		party->shareExperience(experience);
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaPartySetSharedExperience(lua_State* L)
{
	// party:setSharedExperience(active)
	bool active = tfs::lua::getBoolean(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		tfs::lua::pushBoolean(L, party->setSharedExperience(party->getLeader(), active));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Spells
int luaSpellCreate(lua_State* L)
{
	// Spell(words, name or id) to get an existing spell
	// Spell(type) ex: Spell(SPELL_INSTANT) or Spell(SPELL_RUNE) to create a new spell
	if (lua_gettop(L) == 1) {
		std::cout << "[Error - Spell::luaSpellCreate] There is no parameter set!" << std::endl;
		lua_pushnil(L);
		return 1;
	}

	SpellType_t spellType = SPELL_UNDEFINED;

	if (tfs::lua::isNumber(L, 2)) {
		int32_t id = tfs::lua::getNumber<int32_t>(L, 2);
		RuneSpell* rune = g_spells->getRuneSpell(id);

		if (rune) {
			tfs::lua::pushUserdata<Spell>(L, rune);
			tfs::lua::setMetatable(L, -1, "Spell");
			return 1;
		}

		spellType = static_cast<SpellType_t>(id);
	} else if (lua_isstring(L, 2)) {
		std::string arg = tfs::lua::getString(L, 2);
		InstantSpell* instant = g_spells->getInstantSpellByName(arg);
		if (instant) {
			tfs::lua::pushUserdata<Spell>(L, instant);
			tfs::lua::setMetatable(L, -1, "Spell");
			return 1;
		}
		instant = g_spells->getInstantSpell(arg);
		if (instant) {
			tfs::lua::pushUserdata<Spell>(L, instant);
			tfs::lua::setMetatable(L, -1, "Spell");
			return 1;
		}
		RuneSpell* rune = g_spells->getRuneSpellByName(arg);
		if (rune) {
			tfs::lua::pushUserdata<Spell>(L, rune);
			tfs::lua::setMetatable(L, -1, "Spell");
			return 1;
		}

		std::string tmp = boost::algorithm::to_lower_copy(arg);
		if (tmp == "instant") {
			spellType = SPELL_INSTANT;
		} else if (tmp == "rune") {
			spellType = SPELL_RUNE;
		}
	}

	if (spellType == SPELL_INSTANT) {
		InstantSpell* spell = new InstantSpell(tfs::lua::getScriptEnv()->getScriptInterface());
		spell->fromLua = true;
		tfs::lua::pushUserdata<Spell>(L, spell);
		tfs::lua::setMetatable(L, -1, "Spell");
		spell->spellType = SPELL_INSTANT;
		return 1;
	} else if (spellType == SPELL_RUNE) {
		RuneSpell* spell = new RuneSpell(tfs::lua::getScriptEnv()->getScriptInterface());
		spell->fromLua = true;
		tfs::lua::pushUserdata<Spell>(L, spell);
		tfs::lua::setMetatable(L, -1, "Spell");
		spell->spellType = SPELL_RUNE;
		return 1;
	}

	lua_pushnil(L);
	return 1;
}

int luaSpellOnCastSpell(lua_State* L)
{
	// spell:onCastSpell(callback)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (spell->spellType == SPELL_INSTANT) {
			InstantSpell* instant = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
			if (!instant->loadCallback()) {
				tfs::lua::pushBoolean(L, false);
				return 1;
			}
			instant->scripted = true;
			tfs::lua::pushBoolean(L, true);
		} else if (spell->spellType == SPELL_RUNE) {
			RuneSpell* rune = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
			if (!rune->loadCallback()) {
				tfs::lua::pushBoolean(L, false);
				return 1;
			}
			rune->scripted = true;
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellRegister(lua_State* L)
{
	// spell:register()
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (spell->spellType == SPELL_INSTANT) {
			InstantSpell* instant = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
			if (!instant->isScripted()) {
				tfs::lua::pushBoolean(L, false);
				return 1;
			}
			tfs::lua::pushBoolean(L, g_spells->registerInstantLuaEvent(instant));
		} else if (spell->spellType == SPELL_RUNE) {
			RuneSpell* rune = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
			if (rune->getMagicLevel() != 0 || rune->getLevel() != 0) {
				// Change information in the ItemType to get accurate description
				ItemType& iType = Item::items.getItemType(rune->getRuneItemId());
				iType.name = rune->getName();
				iType.runeMagLevel = rune->getMagicLevel();
				iType.runeLevel = rune->getLevel();
				iType.charges = rune->getCharges();
			}
			if (!rune->isScripted()) {
				tfs::lua::pushBoolean(L, false);
				return 1;
			}
			tfs::lua::pushBoolean(L, g_spells->registerRuneLuaEvent(rune));
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellName(lua_State* L)
{
	// spell:name(name)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushString(L, spell->getName());
		} else {
			spell->setName(tfs::lua::getString(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellId(lua_State* L)
{
	// spell:id(id)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getId());
		} else {
			spell->setId(tfs::lua::getNumber<uint8_t>(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellGroup(lua_State* L)
{
	// spell:group(primaryGroup[, secondaryGroup])
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getGroup());
			lua_pushnumber(L, spell->getSecondaryGroup());
			return 2;
		} else if (lua_gettop(L) == 2) {
			SpellGroup_t group = tfs::lua::getNumber<SpellGroup_t>(L, 2);
			if (group) {
				spell->setGroup(group);
				tfs::lua::pushBoolean(L, true);
			} else if (lua_isstring(L, 2)) {
				group = stringToSpellGroup(tfs::lua::getString(L, 2));
				if (group != SPELLGROUP_NONE) {
					spell->setGroup(group);
				} else {
					std::cout << "[Warning - Spell::group] Unknown group: " << tfs::lua::getString(L, 2) << std::endl;
					tfs::lua::pushBoolean(L, false);
					return 1;
				}
				tfs::lua::pushBoolean(L, true);
			} else {
				std::cout << "[Warning - Spell::group] Unknown group: " << tfs::lua::getString(L, 2) << std::endl;
				tfs::lua::pushBoolean(L, false);
				return 1;
			}
		} else {
			SpellGroup_t primaryGroup = tfs::lua::getNumber<SpellGroup_t>(L, 2);
			SpellGroup_t secondaryGroup = tfs::lua::getNumber<SpellGroup_t>(L, 2);
			if (primaryGroup && secondaryGroup) {
				spell->setGroup(primaryGroup);
				spell->setSecondaryGroup(secondaryGroup);
				tfs::lua::pushBoolean(L, true);
			} else if (lua_isstring(L, 2) && lua_isstring(L, 3)) {
				primaryGroup = stringToSpellGroup(tfs::lua::getString(L, 2));
				if (primaryGroup != SPELLGROUP_NONE) {
					spell->setGroup(primaryGroup);
				} else {
					std::cout << "[Warning - Spell::group] Unknown primaryGroup: " << tfs::lua::getString(L, 2)
					          << std::endl;
					tfs::lua::pushBoolean(L, false);
					return 1;
				}
				secondaryGroup = stringToSpellGroup(tfs::lua::getString(L, 3));
				if (secondaryGroup != SPELLGROUP_NONE) {
					spell->setSecondaryGroup(secondaryGroup);
				} else {
					std::cout << "[Warning - Spell::group] Unknown secondaryGroup: " << tfs::lua::getString(L, 3)
					          << std::endl;
					tfs::lua::pushBoolean(L, false);
					return 1;
				}
				tfs::lua::pushBoolean(L, true);
			} else {
				std::cout << "[Warning - Spell::group] Unknown primaryGroup: " << tfs::lua::getString(L, 2)
				          << " or secondaryGroup: " << tfs::lua::getString(L, 3) << std::endl;
				tfs::lua::pushBoolean(L, false);
				return 1;
			}
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellCooldown(lua_State* L)
{
	// spell:cooldown(cooldown)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getCooldown());
		} else {
			spell->setCooldown(tfs::lua::getNumber<uint32_t>(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellGroupCooldown(lua_State* L)
{
	// spell:groupCooldown(primaryGroupCd[, secondaryGroupCd])
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getGroupCooldown());
			lua_pushnumber(L, spell->getSecondaryCooldown());
			return 2;
		} else if (lua_gettop(L) == 2) {
			spell->setGroupCooldown(tfs::lua::getNumber<uint32_t>(L, 2));
			tfs::lua::pushBoolean(L, true);
		} else {
			spell->setGroupCooldown(tfs::lua::getNumber<uint32_t>(L, 2));
			spell->setSecondaryCooldown(tfs::lua::getNumber<uint32_t>(L, 3));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellLevel(lua_State* L)
{
	// spell:level(lvl)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getLevel());
		} else {
			spell->setLevel(tfs::lua::getNumber<uint32_t>(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellMagicLevel(lua_State* L)
{
	// spell:magicLevel(lvl)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getMagicLevel());
		} else {
			spell->setMagicLevel(tfs::lua::getNumber<uint32_t>(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellMana(lua_State* L)
{
	// spell:mana(mana)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getMana());
		} else {
			spell->setMana(tfs::lua::getNumber<uint32_t>(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellManaPercent(lua_State* L)
{
	// spell:manaPercent(percent)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getManaPercent());
		} else {
			spell->setManaPercent(tfs::lua::getNumber<uint32_t>(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellSoul(lua_State* L)
{
	// spell:soul(soul)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getSoulCost());
		} else {
			spell->setSoulCost(tfs::lua::getNumber<uint32_t>(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellRange(lua_State* L)
{
	// spell:range(range)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getRange());
		} else {
			spell->setRange(tfs::lua::getNumber<int32_t>(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellPremium(lua_State* L)
{
	// spell:isPremium(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->isPremium());
		} else {
			spell->setPremium(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellEnabled(lua_State* L)
{
	// spell:isEnabled(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->isEnabled());
		} else {
			spell->setEnabled(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellNeedTarget(lua_State* L)
{
	// spell:needTarget(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getNeedTarget());
		} else {
			spell->setNeedTarget(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellNeedWeapon(lua_State* L)
{
	// spell:needWeapon(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getNeedWeapon());
		} else {
			spell->setNeedWeapon(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellNeedLearn(lua_State* L)
{
	// spell:needLearn(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getNeedLearn());
		} else {
			spell->setNeedLearn(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellSelfTarget(lua_State* L)
{
	// spell:isSelfTarget(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getSelfTarget());
		} else {
			spell->setSelfTarget(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellBlocking(lua_State* L)
{
	// spell:isBlocking(blockingSolid, blockingCreature)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getBlockingSolid());
			tfs::lua::pushBoolean(L, spell->getBlockingCreature());
			return 2;
		} else {
			spell->setBlockingSolid(tfs::lua::getBoolean(L, 2));
			spell->setBlockingCreature(tfs::lua::getBoolean(L, 3));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellAggressive(lua_State* L)
{
	// spell:isAggressive(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getAggressive());
		} else {
			spell->setAggressive(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellPzLock(lua_State* L)
{
	// spell:isPzLock(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getPzLock());
		} else {
			spell->setPzLock(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaSpellVocation(lua_State* L)
{
	// spell:vocation(vocation)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (!spell) {
		lua_pushnil(L);
		return 1;
	}

	if (lua_gettop(L) == 1) {
		lua_createtable(L, 0, 0);
		int i = 0;
		for (auto& voc : spell->getVocMap()) {
			std::string name = g_vocations.getVocation(voc.first)->getVocName();
			tfs::lua::pushString(L, name);
			lua_rawseti(L, -2, ++i);
		}
	} else {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		for (int i = 0; i < parameters; ++i) {
			std::vector<std::string> vocList = explodeString(tfs::lua::getString(L, 2 + i), ";");
			spell->addVocMap(g_vocations.getVocationId(vocList[0]),
			                 vocList.size() > 1 ? booleanString(vocList[1]) : false);
		}
		tfs::lua::pushBoolean(L, true);
	}
	return 1;
}

// only for InstantSpells
int luaSpellWords(lua_State* L)
{
	// spell:words(words[, separator = ""])
	InstantSpell* spell = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			tfs::lua::pushString(L, spell->getWords());
			tfs::lua::pushString(L, spell->getSeparator());
			return 2;
		} else {
			std::string sep = "";
			if (lua_gettop(L) == 3) {
				sep = tfs::lua::getString(L, 3);
			}
			spell->setWords(tfs::lua::getString(L, 2));
			spell->setSeparator(sep);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for InstantSpells
int luaSpellNeedDirection(lua_State* L)
{
	// spell:needDirection(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getNeedDirection());
		} else {
			spell->setNeedDirection(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for InstantSpells
int luaSpellHasParams(lua_State* L)
{
	// spell:hasParams(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getHasParam());
		} else {
			spell->setHasParam(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for InstantSpells
int luaSpellHasPlayerNameParam(lua_State* L)
{
	// spell:hasPlayerNameParam(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getHasPlayerNameParam());
		} else {
			spell->setHasPlayerNameParam(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for InstantSpells
int luaSpellNeedCasterTargetOrDirection(lua_State* L)
{
	// spell:needCasterTargetOrDirection(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getNeedCasterTargetOrDirection());
		} else {
			spell->setNeedCasterTargetOrDirection(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for InstantSpells
int luaSpellIsBlockingWalls(lua_State* L)
{
	// spell:blockWalls(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getBlockWalls());
		} else {
			spell->setBlockWalls(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int luaSpellRuneLevel(lua_State* L)
{
	// spell:runeLevel(level)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	int32_t level = tfs::lua::getNumber<int32_t>(L, 2);
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getLevel());
		} else {
			spell->setLevel(level);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int luaSpellRuneMagicLevel(lua_State* L)
{
	// spell:runeMagicLevel(magLevel)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	int32_t magLevel = tfs::lua::getNumber<int32_t>(L, 2);
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getMagicLevel());
		} else {
			spell->setMagicLevel(magLevel);
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int luaSpellRuneId(lua_State* L)
{
	// spell:runeId(id)
	RuneSpell* rune = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (rune) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (rune->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, rune->getRuneItemId());
		} else {
			rune->setRuneItemId(tfs::lua::getNumber<uint16_t>(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int luaSpellCharges(lua_State* L)
{
	// spell:charges(charges)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			lua_pushnumber(L, spell->getCharges());
		} else {
			spell->setCharges(tfs::lua::getNumber<uint32_t>(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int luaSpellAllowFarUse(lua_State* L)
{
	// spell:allowFarUse(bool)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getAllowFarUse());
		} else {
			spell->setAllowFarUse(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int luaSpellBlockWalls(lua_State* L)
{
	// spell:blockWalls(bool)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getCheckLineOfSight());
		} else {
			spell->setCheckLineOfSight(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int luaSpellCheckFloor(lua_State* L)
{
	// spell:checkFloor(bool)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			tfs::lua::pushBoolean(L, spell->getCheckFloor());
		} else {
			spell->setCheckFloor(tfs::lua::getBoolean(L, 2));
			tfs::lua::pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaCreateAction(lua_State* L)
{
	// Action()
	if (tfs::lua::getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "Actions can only be registered in the Scripts interface.");
		lua_pushnil(L);
		return 1;
	}

	Action* action = new Action(tfs::lua::getScriptEnv()->getScriptInterface());
	if (action) {
		action->fromLua = true;
		tfs::lua::pushUserdata<Action>(L, action);
		tfs::lua::setMetatable(L, -1, "Action");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaActionOnUse(lua_State* L)
{
	// action:onUse(callback)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		if (!action->loadCallback()) {
			tfs::lua::pushBoolean(L, false);
			return 1;
		}
		action->scripted = true;
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaActionRegister(lua_State* L)
{
	// action:register()
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		if (!action->isScripted()) {
			tfs::lua::pushBoolean(L, false);
			return 1;
		}
		tfs::lua::pushBoolean(L, g_actions->registerLuaEvent(action));
		action->clearActionIdRange();
		action->clearItemIdRange();
		action->clearUniqueIdRange();
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaActionItemId(lua_State* L)
{
	// action:id(ids)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				action->addItemId(tfs::lua::getNumber<uint32_t>(L, 2 + i));
			}
		} else {
			action->addItemId(tfs::lua::getNumber<uint32_t>(L, 2));
		}
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaActionActionId(lua_State* L)
{
	// action:aid(aids)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				action->addActionId(tfs::lua::getNumber<uint32_t>(L, 2 + i));
			}
		} else {
			action->addActionId(tfs::lua::getNumber<uint32_t>(L, 2));
		}
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaActionUniqueId(lua_State* L)
{
	// action:uid(uids)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				action->addUniqueId(tfs::lua::getNumber<uint32_t>(L, 2 + i));
			}
		} else {
			action->addUniqueId(tfs::lua::getNumber<uint32_t>(L, 2));
		}
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaActionAllowFarUse(lua_State* L)
{
	// action:allowFarUse(bool)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		action->setAllowFarUse(tfs::lua::getBoolean(L, 2));
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaActionBlockWalls(lua_State* L)
{
	// action:blockWalls(bool)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		action->setCheckLineOfSight(tfs::lua::getBoolean(L, 2));
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int luaActionCheckFloor(lua_State* L)
{
	// action:checkFloor(bool)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		action->setCheckFloor(tfs::lua::getBoolean(L, 2));
		tfs::lua::pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

} // namespace

#ifndef LUAJIT_VERSION
const luaL_Reg LuaScriptInterface::luaBitReg[] = {
    //{"tobit", LuaScriptInterface::luaBitToBit},
    {"bnot", luaBitNot},
    {"band", luaBitAnd},
    {"bor", luaBitOr},
    {"bxor", luaBitXor},
    {"lshift", luaBitLeftShift},
    {"rshift", luaBitRightShift},
    //{"arshift", luaBitArithmeticalRightShift},
    //{"rol", luaBitRotateLeft},
    //{"ror", luaBitRotateRight},
    //{"bswap", luaBitSwapEndian},
    //{"tohex", luaBitToHex},
    {nullptr, nullptr}};
#endif

const luaL_Reg LuaScriptInterface::luaConfigManagerTable[] = {{"getString", luaConfigManagerGetString},
                                                              {"getNumber", luaConfigManagerGetNumber},
                                                              {"getBoolean", luaConfigManagerGetBoolean},
                                                              {nullptr, nullptr}};

const luaL_Reg LuaScriptInterface::luaDatabaseTable[] = {{"query", luaDatabaseExecute},
                                                         {"asyncQuery", luaDatabaseAsyncExecute},
                                                         {"storeQuery", luaDatabaseStoreQuery},
                                                         {"asyncStoreQuery", luaDatabaseAsyncStoreQuery},
                                                         {"escapeString", luaDatabaseEscapeString},
                                                         {"escapeBlob", luaDatabaseEscapeBlob},
                                                         {"lastInsertId", luaDatabaseLastInsertId},
                                                         {"tableExists", luaDatabaseTableExists},
                                                         {nullptr, nullptr}};

const luaL_Reg LuaScriptInterface::luaResultTable[] = {
    {"getNumber", luaResultGetNumber}, {"getString", luaResultGetString}, {"getStream", luaResultGetStream},
    {"next", luaResultNext},           {"free", luaResultFree},           {nullptr, nullptr}};

// Metatables
#define registerEnum(value) \
	{ \
		std::string enumName = #value; \
		registerGlobalVariable(enumName.substr(enumName.find_last_of(':') + 1), value); \
	}
#define registerEnumIn(tableName, value) \
	{ \
		std::string enumName = #value; \
		registerVariable(tableName, enumName.substr(enumName.find_last_of(':') + 1), value); \
	}

void LuaScriptInterface::registerFunctions()
{
	// doPlayerAddItem(uid, itemid, <optional: default: 1> count/subtype)
	// doPlayerAddItem(cid, itemid, <optional: default: 1> count, <optional: default: 1> canDropOnMap, <optional:
	// default: 1>subtype) Returns uid of the created item
	lua_register(luaState, "doPlayerAddItem", luaDoPlayerAddItem);

	// isValidUID(uid)
	lua_register(luaState, "isValidUID", luaIsValidUID);

	// isDepot(uid)
	lua_register(luaState, "isDepot", luaIsDepot);

	// isMovable(uid)
	lua_register(luaState, "isMovable", luaIsMoveable);

	// doAddContainerItem(uid, itemid, <optional> count/subtype)
	lua_register(luaState, "doAddContainerItem", luaDoAddContainerItem);

	// getDepotId(uid)
	lua_register(luaState, "getDepotId", luaGetDepotId);

	// getWorldTime()
	lua_register(luaState, "getWorldTime", luaGetWorldTime);

	// getWorldLight()
	lua_register(luaState, "getWorldLight", luaGetWorldLight);

	// setWorldLight(level, color)
	lua_register(luaState, "setWorldLight", luaSetWorldLight);

	// getWorldUpTime()
	lua_register(luaState, "getWorldUpTime", luaGetWorldUpTime);

	// getSubTypeName(subType)
	lua_register(luaState, "getSubTypeName", luaGetSubTypeName);

	// createCombatArea({area}, <optional> {extArea})
	lua_register(luaState, "createCombatArea", luaCreateCombatArea);

	// doAreaCombat(cid, type, pos, area, min, max, effect[, origin = ORIGIN_SPELL[, blockArmor = false[, blockShield =
	// false[, ignoreResistances = false]]]])
	lua_register(luaState, "doAreaCombat", luaDoAreaCombat);

	// doTargetCombat(cid, target, type, min, max, effect[, origin = ORIGIN_SPELL[, blockArmor = false[, blockShield =
	// false[, ignoreResistances = false]]]])
	lua_register(luaState, "doTargetCombat", luaDoTargetCombat);

	// doChallengeCreature(cid, target[, force = false])
	lua_register(luaState, "doChallengeCreature", luaDoChallengeCreature);

	// addEvent(callback, delay, ...)
	lua_register(luaState, "addEvent", luaAddEvent);

	// stopEvent(eventid)
	lua_register(luaState, "stopEvent", luaStopEvent);

	// saveServer()
	lua_register(luaState, "saveServer", luaSaveServer);

	// cleanMap()
	lua_register(luaState, "cleanMap", luaCleanMap);

	// debugPrint(text)
	lua_register(luaState, "debugPrint", luaDebugPrint);

	// isInWar(cid, target)
	lua_register(luaState, "isInWar", luaIsInWar);

	// getWaypointPosition(name)
	lua_register(luaState, "getWaypointPositionByName", luaGetWaypointPositionByName);

	// sendChannelMessage(channelId, type, message)
	lua_register(luaState, "sendChannelMessage", luaSendChannelMessage);

	// sendGuildChannelMessage(guildId, type, message)
	lua_register(luaState, "sendGuildChannelMessage", luaSendGuildChannelMessage);

	// isScriptsInterface()
	lua_register(luaState, "isScriptsInterface", luaIsScriptsInterface);

#ifndef LUAJIT_VERSION
	// bit operations for Lua, based on bitlib project release 24
	// bit.bnot, bit.band, bit.bor, bit.bxor, bit.lshift, bit.rshift
	luaL_register(luaState, "bit", luaBitReg);
	lua_pop(luaState, 1);
#endif

	// configManager table
	luaL_register(luaState, "configManager", luaConfigManagerTable);
	lua_pop(luaState, 1);

	// db table
	luaL_register(luaState, "db", luaDatabaseTable);
	lua_pop(luaState, 1);

	// result table
	luaL_register(luaState, "result", luaResultTable);
	lua_pop(luaState, 1);

	/* New functions */
	// registerClass(className, baseClass, newFunction)
	// registerTable(tableName)
	// registerMethod(className, functionName, function)
	// registerMetaMethod(className, functionName, function)
	// registerGlobalMethod(functionName, function)
	// registerVariable(tableName, name, value)
	// registerGlobalVariable(name, value)
	// registerEnum(value)
	// registerEnumIn(tableName, value)

	// Enums
	registerEnum(ACCOUNT_TYPE_NORMAL);
	registerEnum(ACCOUNT_TYPE_TUTOR);
	registerEnum(ACCOUNT_TYPE_SENIORTUTOR);
	registerEnum(ACCOUNT_TYPE_GAMEMASTER);
	registerEnum(ACCOUNT_TYPE_COMMUNITYMANAGER);
	registerEnum(ACCOUNT_TYPE_GOD);

	registerEnum(AMMO_NONE);
	registerEnum(AMMO_BOLT);
	registerEnum(AMMO_ARROW);
	registerEnum(AMMO_SPEAR);
	registerEnum(AMMO_THROWINGSTAR);
	registerEnum(AMMO_THROWINGKNIFE);
	registerEnum(AMMO_STONE);
	registerEnum(AMMO_SNOWBALL);

	registerEnum(BUG_CATEGORY_MAP);
	registerEnum(BUG_CATEGORY_TYPO);
	registerEnum(BUG_CATEGORY_TECHNICAL);
	registerEnum(BUG_CATEGORY_OTHER);

	registerEnum(CALLBACK_PARAM_LEVELMAGICVALUE);
	registerEnum(CALLBACK_PARAM_SKILLVALUE);
	registerEnum(CALLBACK_PARAM_TARGETTILE);
	registerEnum(CALLBACK_PARAM_TARGETCREATURE);

	registerEnum(COMBAT_FORMULA_UNDEFINED);
	registerEnum(COMBAT_FORMULA_LEVELMAGIC);
	registerEnum(COMBAT_FORMULA_SKILL);
	registerEnum(COMBAT_FORMULA_DAMAGE);

	registerEnum(DIRECTION_NORTH);
	registerEnum(DIRECTION_EAST);
	registerEnum(DIRECTION_SOUTH);
	registerEnum(DIRECTION_WEST);
	registerEnum(DIRECTION_SOUTHWEST);
	registerEnum(DIRECTION_SOUTHEAST);
	registerEnum(DIRECTION_NORTHWEST);
	registerEnum(DIRECTION_NORTHEAST);

	registerEnum(COMBAT_NONE);
	registerEnum(COMBAT_PHYSICALDAMAGE);
	registerEnum(COMBAT_ENERGYDAMAGE);
	registerEnum(COMBAT_EARTHDAMAGE);
	registerEnum(COMBAT_FIREDAMAGE);
	registerEnum(COMBAT_UNDEFINEDDAMAGE);
	registerEnum(COMBAT_LIFEDRAIN);
	registerEnum(COMBAT_MANADRAIN);
	registerEnum(COMBAT_HEALING);
	registerEnum(COMBAT_DROWNDAMAGE);
	registerEnum(COMBAT_ICEDAMAGE);
	registerEnum(COMBAT_HOLYDAMAGE);
	registerEnum(COMBAT_DEATHDAMAGE);

	registerEnum(COMBAT_PARAM_TYPE);
	registerEnum(COMBAT_PARAM_EFFECT);
	registerEnum(COMBAT_PARAM_DISTANCEEFFECT);
	registerEnum(COMBAT_PARAM_BLOCKSHIELD);
	registerEnum(COMBAT_PARAM_BLOCKARMOR);
	registerEnum(COMBAT_PARAM_TARGETCASTERORTOPMOST);
	registerEnum(COMBAT_PARAM_CREATEITEM);
	registerEnum(COMBAT_PARAM_AGGRESSIVE);
	registerEnum(COMBAT_PARAM_DISPEL);
	registerEnum(COMBAT_PARAM_USECHARGES);

	registerEnum(CONDITION_NONE);
	registerEnum(CONDITION_POISON);
	registerEnum(CONDITION_FIRE);
	registerEnum(CONDITION_ENERGY);
	registerEnum(CONDITION_BLEEDING);
	registerEnum(CONDITION_HASTE);
	registerEnum(CONDITION_PARALYZE);
	registerEnum(CONDITION_OUTFIT);
	registerEnum(CONDITION_INVISIBLE);
	registerEnum(CONDITION_LIGHT);
	registerEnum(CONDITION_MANASHIELD);
	registerEnum(CONDITION_INFIGHT);
	registerEnum(CONDITION_DRUNK);
	registerEnum(CONDITION_EXHAUST_WEAPON);
	registerEnum(CONDITION_REGENERATION);
	registerEnum(CONDITION_SOUL);
	registerEnum(CONDITION_DROWN);
	registerEnum(CONDITION_MUTED);
	registerEnum(CONDITION_CHANNELMUTEDTICKS);
	registerEnum(CONDITION_YELLTICKS);
	registerEnum(CONDITION_ATTRIBUTES);
	registerEnum(CONDITION_FREEZING);
	registerEnum(CONDITION_DAZZLED);
	registerEnum(CONDITION_CURSED);
	registerEnum(CONDITION_EXHAUST_COMBAT);
	registerEnum(CONDITION_EXHAUST_HEAL);
	registerEnum(CONDITION_PACIFIED);
	registerEnum(CONDITION_SPELLCOOLDOWN);
	registerEnum(CONDITION_SPELLGROUPCOOLDOWN);
	registerEnum(CONDITION_ROOT);

	registerEnum(CONDITIONID_DEFAULT);
	registerEnum(CONDITIONID_COMBAT);
	registerEnum(CONDITIONID_HEAD);
	registerEnum(CONDITIONID_NECKLACE);
	registerEnum(CONDITIONID_BACKPACK);
	registerEnum(CONDITIONID_ARMOR);
	registerEnum(CONDITIONID_RIGHT);
	registerEnum(CONDITIONID_LEFT);
	registerEnum(CONDITIONID_LEGS);
	registerEnum(CONDITIONID_FEET);
	registerEnum(CONDITIONID_RING);
	registerEnum(CONDITIONID_AMMO);

	registerEnum(CONDITION_PARAM_OWNER);
	registerEnum(CONDITION_PARAM_TICKS);
	registerEnum(CONDITION_PARAM_DRUNKENNESS);
	registerEnum(CONDITION_PARAM_HEALTHGAIN);
	registerEnum(CONDITION_PARAM_HEALTHTICKS);
	registerEnum(CONDITION_PARAM_MANAGAIN);
	registerEnum(CONDITION_PARAM_MANATICKS);
	registerEnum(CONDITION_PARAM_DELAYED);
	registerEnum(CONDITION_PARAM_SPEED);
	registerEnum(CONDITION_PARAM_LIGHT_LEVEL);
	registerEnum(CONDITION_PARAM_LIGHT_COLOR);
	registerEnum(CONDITION_PARAM_SOULGAIN);
	registerEnum(CONDITION_PARAM_SOULTICKS);
	registerEnum(CONDITION_PARAM_MINVALUE);
	registerEnum(CONDITION_PARAM_MAXVALUE);
	registerEnum(CONDITION_PARAM_STARTVALUE);
	registerEnum(CONDITION_PARAM_TICKINTERVAL);
	registerEnum(CONDITION_PARAM_FORCEUPDATE);
	registerEnum(CONDITION_PARAM_SKILL_MELEE);
	registerEnum(CONDITION_PARAM_SKILL_FIST);
	registerEnum(CONDITION_PARAM_SKILL_CLUB);
	registerEnum(CONDITION_PARAM_SKILL_SWORD);
	registerEnum(CONDITION_PARAM_SKILL_AXE);
	registerEnum(CONDITION_PARAM_SKILL_DISTANCE);
	registerEnum(CONDITION_PARAM_SKILL_SHIELD);
	registerEnum(CONDITION_PARAM_SKILL_FISHING);
	registerEnum(CONDITION_PARAM_STAT_MAXHITPOINTS);
	registerEnum(CONDITION_PARAM_STAT_MAXMANAPOINTS);
	registerEnum(CONDITION_PARAM_STAT_MAGICPOINTS);
	registerEnum(CONDITION_PARAM_STAT_MAXHITPOINTSPERCENT);
	registerEnum(CONDITION_PARAM_STAT_MAXMANAPOINTSPERCENT);
	registerEnum(CONDITION_PARAM_STAT_MAGICPOINTSPERCENT);
	registerEnum(CONDITION_PARAM_PERIODICDAMAGE);
	registerEnum(CONDITION_PARAM_SKILL_MELEEPERCENT);
	registerEnum(CONDITION_PARAM_SKILL_FISTPERCENT);
	registerEnum(CONDITION_PARAM_SKILL_CLUBPERCENT);
	registerEnum(CONDITION_PARAM_SKILL_SWORDPERCENT);
	registerEnum(CONDITION_PARAM_SKILL_AXEPERCENT);
	registerEnum(CONDITION_PARAM_SKILL_DISTANCEPERCENT);
	registerEnum(CONDITION_PARAM_SKILL_SHIELDPERCENT);
	registerEnum(CONDITION_PARAM_SKILL_FISHINGPERCENT);
	registerEnum(CONDITION_PARAM_BUFF_SPELL);
	registerEnum(CONDITION_PARAM_SUBID);
	registerEnum(CONDITION_PARAM_FIELD);
	registerEnum(CONDITION_PARAM_DISABLE_DEFENSE);
	registerEnum(CONDITION_PARAM_SPECIALSKILL_CRITICALHITCHANCE);
	registerEnum(CONDITION_PARAM_SPECIALSKILL_CRITICALHITAMOUNT);
	registerEnum(CONDITION_PARAM_SPECIALSKILL_LIFELEECHCHANCE);
	registerEnum(CONDITION_PARAM_SPECIALSKILL_LIFELEECHAMOUNT);
	registerEnum(CONDITION_PARAM_SPECIALSKILL_MANALEECHCHANCE);
	registerEnum(CONDITION_PARAM_SPECIALSKILL_MANALEECHAMOUNT);
	registerEnum(CONDITION_PARAM_AGGRESSIVE);

	registerEnum(CONST_ME_NONE);
	registerEnum(CONST_ME_DRAWBLOOD);
	registerEnum(CONST_ME_LOSEENERGY);
	registerEnum(CONST_ME_POFF);
	registerEnum(CONST_ME_BLOCKHIT);
	registerEnum(CONST_ME_EXPLOSIONAREA);
	registerEnum(CONST_ME_EXPLOSIONHIT);
	registerEnum(CONST_ME_FIREAREA);
	registerEnum(CONST_ME_YELLOW_RINGS);
	registerEnum(CONST_ME_GREEN_RINGS);
	registerEnum(CONST_ME_HITAREA);
	registerEnum(CONST_ME_TELEPORT);
	registerEnum(CONST_ME_ENERGYHIT);
	registerEnum(CONST_ME_MAGIC_BLUE);
	registerEnum(CONST_ME_MAGIC_RED);
	registerEnum(CONST_ME_MAGIC_GREEN);
	registerEnum(CONST_ME_HITBYFIRE);
	registerEnum(CONST_ME_HITBYPOISON);
	registerEnum(CONST_ME_MORTAREA);
	registerEnum(CONST_ME_SOUND_GREEN);
	registerEnum(CONST_ME_SOUND_RED);
	registerEnum(CONST_ME_POISONAREA);
	registerEnum(CONST_ME_SOUND_YELLOW);
	registerEnum(CONST_ME_SOUND_PURPLE);
	registerEnum(CONST_ME_SOUND_BLUE);
	registerEnum(CONST_ME_SOUND_WHITE);
	registerEnum(CONST_ME_BUBBLES);
	registerEnum(CONST_ME_CRAPS);
	registerEnum(CONST_ME_GIFT_WRAPS);
	registerEnum(CONST_ME_FIREWORK_YELLOW);
	registerEnum(CONST_ME_FIREWORK_RED);
	registerEnum(CONST_ME_FIREWORK_BLUE);
	registerEnum(CONST_ME_STUN);
	registerEnum(CONST_ME_SLEEP);
	registerEnum(CONST_ME_WATERCREATURE);
	registerEnum(CONST_ME_GROUNDSHAKER);
	registerEnum(CONST_ME_HEARTS);
	registerEnum(CONST_ME_FIREATTACK);
	registerEnum(CONST_ME_ENERGYAREA);
	registerEnum(CONST_ME_SMALLCLOUDS);
	registerEnum(CONST_ME_HOLYDAMAGE);
	registerEnum(CONST_ME_BIGCLOUDS);
	registerEnum(CONST_ME_ICEAREA);
	registerEnum(CONST_ME_ICETORNADO);
	registerEnum(CONST_ME_ICEATTACK);
	registerEnum(CONST_ME_STONES);
	registerEnum(CONST_ME_SMALLPLANTS);
	registerEnum(CONST_ME_CARNIPHILA);
	registerEnum(CONST_ME_PURPLEENERGY);
	registerEnum(CONST_ME_YELLOWENERGY);
	registerEnum(CONST_ME_HOLYAREA);
	registerEnum(CONST_ME_BIGPLANTS);
	registerEnum(CONST_ME_CAKE);
	registerEnum(CONST_ME_GIANTICE);
	registerEnum(CONST_ME_WATERSPLASH);
	registerEnum(CONST_ME_PLANTATTACK);
	registerEnum(CONST_ME_TUTORIALARROW);
	registerEnum(CONST_ME_TUTORIALSQUARE);
	registerEnum(CONST_ME_MIRRORHORIZONTAL);
	registerEnum(CONST_ME_MIRRORVERTICAL);
	registerEnum(CONST_ME_SKULLHORIZONTAL);
	registerEnum(CONST_ME_SKULLVERTICAL);
	registerEnum(CONST_ME_ASSASSIN);
	registerEnum(CONST_ME_STEPSHORIZONTAL);
	registerEnum(CONST_ME_BLOODYSTEPS);
	registerEnum(CONST_ME_STEPSVERTICAL);
	registerEnum(CONST_ME_YALAHARIGHOST);
	registerEnum(CONST_ME_BATS);
	registerEnum(CONST_ME_SMOKE);
	registerEnum(CONST_ME_INSECTS);
	registerEnum(CONST_ME_DRAGONHEAD);
	registerEnum(CONST_ME_ORCSHAMAN);
	registerEnum(CONST_ME_ORCSHAMAN_FIRE);
	registerEnum(CONST_ME_THUNDER);
	registerEnum(CONST_ME_FERUMBRAS);
	registerEnum(CONST_ME_CONFETTI_HORIZONTAL);
	registerEnum(CONST_ME_CONFETTI_VERTICAL);
	registerEnum(CONST_ME_BLACKSMOKE);
	registerEnum(CONST_ME_REDSMOKE);
	registerEnum(CONST_ME_YELLOWSMOKE);
	registerEnum(CONST_ME_GREENSMOKE);
	registerEnum(CONST_ME_PURPLESMOKE);
	registerEnum(CONST_ME_EARLY_THUNDER);
	registerEnum(CONST_ME_RAGIAZ_BONECAPSULE);
	registerEnum(CONST_ME_CRITICAL_DAMAGE);
	registerEnum(CONST_ME_PLUNGING_FISH);
	registerEnum(CONST_ME_BLUECHAIN);
	registerEnum(CONST_ME_ORANGECHAIN);
	registerEnum(CONST_ME_GREENCHAIN);
	registerEnum(CONST_ME_PURPLECHAIN);
	registerEnum(CONST_ME_GREYCHAIN);
	registerEnum(CONST_ME_YELLOWCHAIN);
	registerEnum(CONST_ME_YELLOWSPARKLES);
	registerEnum(CONST_ME_FAEEXPLOSION);
	registerEnum(CONST_ME_FAECOMING);
	registerEnum(CONST_ME_FAEGOING);
	registerEnum(CONST_ME_BIGCLOUDSSINGLESPACE);
	registerEnum(CONST_ME_STONESSINGLESPACE);
	registerEnum(CONST_ME_BLUEGHOST);
	registerEnum(CONST_ME_POINTOFINTEREST);
	registerEnum(CONST_ME_MAPEFFECT);
	registerEnum(CONST_ME_PINKSPARK);
	registerEnum(CONST_ME_FIREWORK_GREEN);
	registerEnum(CONST_ME_FIREWORK_ORANGE);
	registerEnum(CONST_ME_FIREWORK_PURPLE);
	registerEnum(CONST_ME_FIREWORK_TURQUOISE);
	registerEnum(CONST_ME_THECUBE);
	registerEnum(CONST_ME_DRAWINK);
	registerEnum(CONST_ME_PRISMATICSPARKLES);
	registerEnum(CONST_ME_THAIAN);
	registerEnum(CONST_ME_THAIANGHOST);
	registerEnum(CONST_ME_GHOSTSMOKE);
	registerEnum(CONST_ME_FLOATINGBLOCK);
	registerEnum(CONST_ME_BLOCK);
	registerEnum(CONST_ME_ROOTING);
	registerEnum(CONST_ME_GHOSTLYSCRATCH);
	registerEnum(CONST_ME_GHOSTLYBITE);
	registerEnum(CONST_ME_BIGSCRATCHING);
	registerEnum(CONST_ME_SLASH);
	registerEnum(CONST_ME_BITE);
	registerEnum(CONST_ME_CHIVALRIOUSCHALLENGE);
	registerEnum(CONST_ME_DIVINEDAZZLE);
	registerEnum(CONST_ME_ELECTRICALSPARK);
	registerEnum(CONST_ME_PURPLETELEPORT);
	registerEnum(CONST_ME_REDTELEPORT);
	registerEnum(CONST_ME_ORANGETELEPORT);
	registerEnum(CONST_ME_GREYTELEPORT);
	registerEnum(CONST_ME_LIGHTBLUETELEPORT);
	registerEnum(CONST_ME_FATAL);
	registerEnum(CONST_ME_DODGE);
	registerEnum(CONST_ME_HOURGLASS);
	registerEnum(CONST_ME_FIREWORKSSTAR);
	registerEnum(CONST_ME_FIREWORKSCIRCLE);
	registerEnum(CONST_ME_FERUMBRAS_1);
	registerEnum(CONST_ME_GAZHARAGOTH);
	registerEnum(CONST_ME_MAD_MAGE);
	registerEnum(CONST_ME_HORESTIS);
	registerEnum(CONST_ME_DEVOVORGA);
	registerEnum(CONST_ME_FERUMBRAS_2);

	registerEnum(CONST_ANI_NONE);
	registerEnum(CONST_ANI_SPEAR);
	registerEnum(CONST_ANI_BOLT);
	registerEnum(CONST_ANI_ARROW);
	registerEnum(CONST_ANI_FIRE);
	registerEnum(CONST_ANI_ENERGY);
	registerEnum(CONST_ANI_POISONARROW);
	registerEnum(CONST_ANI_BURSTARROW);
	registerEnum(CONST_ANI_THROWINGSTAR);
	registerEnum(CONST_ANI_THROWINGKNIFE);
	registerEnum(CONST_ANI_SMALLSTONE);
	registerEnum(CONST_ANI_DEATH);
	registerEnum(CONST_ANI_LARGEROCK);
	registerEnum(CONST_ANI_SNOWBALL);
	registerEnum(CONST_ANI_POWERBOLT);
	registerEnum(CONST_ANI_POISON);
	registerEnum(CONST_ANI_INFERNALBOLT);
	registerEnum(CONST_ANI_HUNTINGSPEAR);
	registerEnum(CONST_ANI_ENCHANTEDSPEAR);
	registerEnum(CONST_ANI_REDSTAR);
	registerEnum(CONST_ANI_GREENSTAR);
	registerEnum(CONST_ANI_ROYALSPEAR);
	registerEnum(CONST_ANI_SNIPERARROW);
	registerEnum(CONST_ANI_ONYXARROW);
	registerEnum(CONST_ANI_PIERCINGBOLT);
	registerEnum(CONST_ANI_WHIRLWINDSWORD);
	registerEnum(CONST_ANI_WHIRLWINDAXE);
	registerEnum(CONST_ANI_WHIRLWINDCLUB);
	registerEnum(CONST_ANI_ETHEREALSPEAR);
	registerEnum(CONST_ANI_ICE);
	registerEnum(CONST_ANI_EARTH);
	registerEnum(CONST_ANI_HOLY);
	registerEnum(CONST_ANI_SUDDENDEATH);
	registerEnum(CONST_ANI_FLASHARROW);
	registerEnum(CONST_ANI_FLAMMINGARROW);
	registerEnum(CONST_ANI_SHIVERARROW);
	registerEnum(CONST_ANI_ENERGYBALL);
	registerEnum(CONST_ANI_SMALLICE);
	registerEnum(CONST_ANI_SMALLHOLY);
	registerEnum(CONST_ANI_SMALLEARTH);
	registerEnum(CONST_ANI_EARTHARROW);
	registerEnum(CONST_ANI_EXPLOSION);
	registerEnum(CONST_ANI_CAKE);
	registerEnum(CONST_ANI_TARSALARROW);
	registerEnum(CONST_ANI_VORTEXBOLT);
	registerEnum(CONST_ANI_PRISMATICBOLT);
	registerEnum(CONST_ANI_CRYSTALLINEARROW);
	registerEnum(CONST_ANI_DRILLBOLT);
	registerEnum(CONST_ANI_ENVENOMEDARROW);
	registerEnum(CONST_ANI_GLOOTHSPEAR);
	registerEnum(CONST_ANI_SIMPLEARROW);
	registerEnum(CONST_ANI_LEAFSTAR);
	registerEnum(CONST_ANI_DIAMONDARROW);
	registerEnum(CONST_ANI_SPECTRALBOLT);
	registerEnum(CONST_ANI_ROYALSTAR);
	registerEnum(CONST_ANI_WEAPONTYPE);

	registerEnum(CONST_PROP_BLOCKSOLID);
	registerEnum(CONST_PROP_HASHEIGHT);
	registerEnum(CONST_PROP_BLOCKPROJECTILE);
	registerEnum(CONST_PROP_BLOCKPATH);
	registerEnum(CONST_PROP_ISVERTICAL);
	registerEnum(CONST_PROP_ISHORIZONTAL);
	registerEnum(CONST_PROP_MOVEABLE);
	registerEnum(CONST_PROP_IMMOVABLEBLOCKSOLID);
	registerEnum(CONST_PROP_IMMOVABLEBLOCKPATH);
	registerEnum(CONST_PROP_IMMOVABLENOFIELDBLOCKPATH);
	registerEnum(CONST_PROP_NOFIELDBLOCKPATH);
	registerEnum(CONST_PROP_SUPPORTHANGABLE);

	registerEnum(CONST_SLOT_HEAD);
	registerEnum(CONST_SLOT_NECKLACE);
	registerEnum(CONST_SLOT_BACKPACK);
	registerEnum(CONST_SLOT_ARMOR);
	registerEnum(CONST_SLOT_RIGHT);
	registerEnum(CONST_SLOT_LEFT);
	registerEnum(CONST_SLOT_LEGS);
	registerEnum(CONST_SLOT_FEET);
	registerEnum(CONST_SLOT_RING);
	registerEnum(CONST_SLOT_AMMO);

	registerEnum(CREATURE_EVENT_NONE);
	registerEnum(CREATURE_EVENT_LOGIN);
	registerEnum(CREATURE_EVENT_LOGOUT);
	registerEnum(CREATURE_EVENT_THINK);
	registerEnum(CREATURE_EVENT_PREPAREDEATH);
	registerEnum(CREATURE_EVENT_DEATH);
	registerEnum(CREATURE_EVENT_KILL);
	registerEnum(CREATURE_EVENT_ADVANCE);
	registerEnum(CREATURE_EVENT_MODALWINDOW);
	registerEnum(CREATURE_EVENT_TEXTEDIT);
	registerEnum(CREATURE_EVENT_HEALTHCHANGE);
	registerEnum(CREATURE_EVENT_MANACHANGE);
	registerEnum(CREATURE_EVENT_EXTENDED_OPCODE);

	registerEnum(CREATURE_ID_MIN);
	registerEnum(CREATURE_ID_MAX);

	registerEnum(GAME_STATE_STARTUP);
	registerEnum(GAME_STATE_INIT);
	registerEnum(GAME_STATE_NORMAL);
	registerEnum(GAME_STATE_CLOSED);
	registerEnum(GAME_STATE_SHUTDOWN);
	registerEnum(GAME_STATE_CLOSING);
	registerEnum(GAME_STATE_MAINTAIN);

	registerEnum(MESSAGE_STATUS_DEFAULT);
	registerEnum(MESSAGE_STATUS_WARNING);
	registerEnum(MESSAGE_EVENT_ADVANCE);
	registerEnum(MESSAGE_STATUS_WARNING2);
	registerEnum(MESSAGE_STATUS_SMALL);
	registerEnum(MESSAGE_INFO_DESCR);
	registerEnum(MESSAGE_DAMAGE_DEALT);
	registerEnum(MESSAGE_DAMAGE_RECEIVED);
	registerEnum(MESSAGE_HEALED);
	registerEnum(MESSAGE_EXPERIENCE);
	registerEnum(MESSAGE_DAMAGE_OTHERS);
	registerEnum(MESSAGE_HEALED_OTHERS);
	registerEnum(MESSAGE_EXPERIENCE_OTHERS);
	registerEnum(MESSAGE_EVENT_DEFAULT);
	registerEnum(MESSAGE_LOOT);
	registerEnum(MESSAGE_TRADE);
	registerEnum(MESSAGE_GUILD);
	registerEnum(MESSAGE_PARTY_MANAGEMENT);
	registerEnum(MESSAGE_PARTY);
	registerEnum(MESSAGE_REPORT);
	registerEnum(MESSAGE_HOTKEY_PRESSED);
	registerEnum(MESSAGE_MARKET);
	registerEnum(MESSAGE_BEYOND_LAST);
	registerEnum(MESSAGE_TOURNAMENT_INFO);
	registerEnum(MESSAGE_ATTENTION);
	registerEnum(MESSAGE_BOOSTED_CREATURE);
	registerEnum(MESSAGE_OFFLINE_TRAINING);
	registerEnum(MESSAGE_TRANSACTION);

	registerEnum(CREATURETYPE_PLAYER);
	registerEnum(CREATURETYPE_MONSTER);
	registerEnum(CREATURETYPE_NPC);
	registerEnum(CREATURETYPE_SUMMON_OWN);
	registerEnum(CREATURETYPE_SUMMON_OTHERS);

	registerEnum(CLIENTOS_LINUX);
	registerEnum(CLIENTOS_WINDOWS);
	registerEnum(CLIENTOS_FLASH);
	registerEnum(CLIENTOS_OTCLIENT_LINUX);
	registerEnum(CLIENTOS_OTCLIENT_WINDOWS);
	registerEnum(CLIENTOS_OTCLIENT_MAC);

	registerEnum(FIGHTMODE_ATTACK);
	registerEnum(FIGHTMODE_BALANCED);
	registerEnum(FIGHTMODE_DEFENSE);

	registerEnum(ITEM_ATTRIBUTE_NONE);
	registerEnum(ITEM_ATTRIBUTE_ACTIONID);
	registerEnum(ITEM_ATTRIBUTE_UNIQUEID);
	registerEnum(ITEM_ATTRIBUTE_DESCRIPTION);
	registerEnum(ITEM_ATTRIBUTE_TEXT);
	registerEnum(ITEM_ATTRIBUTE_DATE);
	registerEnum(ITEM_ATTRIBUTE_WRITER);
	registerEnum(ITEM_ATTRIBUTE_NAME);
	registerEnum(ITEM_ATTRIBUTE_ARTICLE);
	registerEnum(ITEM_ATTRIBUTE_PLURALNAME);
	registerEnum(ITEM_ATTRIBUTE_WEIGHT);
	registerEnum(ITEM_ATTRIBUTE_ATTACK);
	registerEnum(ITEM_ATTRIBUTE_DEFENSE);
	registerEnum(ITEM_ATTRIBUTE_EXTRADEFENSE);
	registerEnum(ITEM_ATTRIBUTE_ARMOR);
	registerEnum(ITEM_ATTRIBUTE_HITCHANCE);
	registerEnum(ITEM_ATTRIBUTE_SHOOTRANGE);
	registerEnum(ITEM_ATTRIBUTE_OWNER);
	registerEnum(ITEM_ATTRIBUTE_DURATION);
	registerEnum(ITEM_ATTRIBUTE_DECAYSTATE);
	registerEnum(ITEM_ATTRIBUTE_CORPSEOWNER);
	registerEnum(ITEM_ATTRIBUTE_CHARGES);
	registerEnum(ITEM_ATTRIBUTE_FLUIDTYPE);
	registerEnum(ITEM_ATTRIBUTE_DOORID);
	registerEnum(ITEM_ATTRIBUTE_DECAYTO);
	registerEnum(ITEM_ATTRIBUTE_WRAPID);
	registerEnum(ITEM_ATTRIBUTE_STOREITEM);
	registerEnum(ITEM_ATTRIBUTE_ATTACK_SPEED);
	registerEnum(ITEM_ATTRIBUTE_OPENCONTAINER);

	registerEnum(ITEM_TYPE_DEPOT);
	registerEnum(ITEM_TYPE_MAILBOX);
	registerEnum(ITEM_TYPE_TRASHHOLDER);
	registerEnum(ITEM_TYPE_CONTAINER);
	registerEnum(ITEM_TYPE_DOOR);
	registerEnum(ITEM_TYPE_MAGICFIELD);
	registerEnum(ITEM_TYPE_TELEPORT);
	registerEnum(ITEM_TYPE_BED);
	registerEnum(ITEM_TYPE_KEY);
	registerEnum(ITEM_TYPE_RUNE);
	registerEnum(ITEM_TYPE_PODIUM);

	registerEnum(ITEM_GROUP_GROUND);
	registerEnum(ITEM_GROUP_CONTAINER);
	registerEnum(ITEM_GROUP_WEAPON);
	registerEnum(ITEM_GROUP_AMMUNITION);
	registerEnum(ITEM_GROUP_ARMOR);
	registerEnum(ITEM_GROUP_CHARGES);
	registerEnum(ITEM_GROUP_TELEPORT);
	registerEnum(ITEM_GROUP_MAGICFIELD);
	registerEnum(ITEM_GROUP_WRITEABLE);
	registerEnum(ITEM_GROUP_KEY);
	registerEnum(ITEM_GROUP_SPLASH);
	registerEnum(ITEM_GROUP_FLUID);
	registerEnum(ITEM_GROUP_DOOR);
	registerEnum(ITEM_GROUP_DEPRECATED);
	registerEnum(ITEM_GROUP_PODIUM);

	registerEnum(ITEM_BROWSEFIELD);
	registerEnum(ITEM_BAG);
	registerEnum(ITEM_SHOPPING_BAG);
	registerEnum(ITEM_GOLD_COIN);
	registerEnum(ITEM_PLATINUM_COIN);
	registerEnum(ITEM_CRYSTAL_COIN);
	registerEnum(ITEM_AMULETOFLOSS);
	registerEnum(ITEM_PARCEL);
	registerEnum(ITEM_LABEL);
	registerEnum(ITEM_FIREFIELD_PVP_FULL);
	registerEnum(ITEM_FIREFIELD_PVP_MEDIUM);
	registerEnum(ITEM_FIREFIELD_PVP_SMALL);
	registerEnum(ITEM_FIREFIELD_PERSISTENT_FULL);
	registerEnum(ITEM_FIREFIELD_PERSISTENT_MEDIUM);
	registerEnum(ITEM_FIREFIELD_PERSISTENT_SMALL);
	registerEnum(ITEM_FIREFIELD_NOPVP);
	registerEnum(ITEM_POISONFIELD_PVP);
	registerEnum(ITEM_POISONFIELD_PERSISTENT);
	registerEnum(ITEM_POISONFIELD_NOPVP);
	registerEnum(ITEM_ENERGYFIELD_PVP);
	registerEnum(ITEM_ENERGYFIELD_PERSISTENT);
	registerEnum(ITEM_ENERGYFIELD_NOPVP);
	registerEnum(ITEM_MAGICWALL);
	registerEnum(ITEM_MAGICWALL_PERSISTENT);
	registerEnum(ITEM_MAGICWALL_SAFE);
	registerEnum(ITEM_WILDGROWTH);
	registerEnum(ITEM_WILDGROWTH_PERSISTENT);
	registerEnum(ITEM_WILDGROWTH_SAFE);
	registerEnum(ITEM_DECORATION_KIT);

	registerEnum(WIELDINFO_NONE);
	registerEnum(WIELDINFO_LEVEL);
	registerEnum(WIELDINFO_MAGLV);
	registerEnum(WIELDINFO_VOCREQ);
	registerEnum(WIELDINFO_PREMIUM);

	registerEnum(PlayerFlag_CannotUseCombat);
	registerEnum(PlayerFlag_CannotAttackPlayer);
	registerEnum(PlayerFlag_CannotAttackMonster);
	registerEnum(PlayerFlag_CannotBeAttacked);
	registerEnum(PlayerFlag_CanConvinceAll);
	registerEnum(PlayerFlag_CanSummonAll);
	registerEnum(PlayerFlag_CanIllusionAll);
	registerEnum(PlayerFlag_CanSenseInvisibility);
	registerEnum(PlayerFlag_IgnoredByMonsters);
	registerEnum(PlayerFlag_NotGainInFight);
	registerEnum(PlayerFlag_HasInfiniteMana);
	registerEnum(PlayerFlag_HasInfiniteSoul);
	registerEnum(PlayerFlag_HasNoExhaustion);
	registerEnum(PlayerFlag_CannotUseSpells);
	registerEnum(PlayerFlag_CannotPickupItem);
	registerEnum(PlayerFlag_CanAlwaysLogin);
	registerEnum(PlayerFlag_CanBroadcast);
	registerEnum(PlayerFlag_CanEditHouses);
	registerEnum(PlayerFlag_CannotBeBanned);
	registerEnum(PlayerFlag_CannotBePushed);
	registerEnum(PlayerFlag_HasInfiniteCapacity);
	registerEnum(PlayerFlag_CanPushAllCreatures);
	registerEnum(PlayerFlag_CanTalkRedPrivate);
	registerEnum(PlayerFlag_CanTalkRedChannel);
	registerEnum(PlayerFlag_TalkOrangeHelpChannel);
	registerEnum(PlayerFlag_NotGainExperience);
	registerEnum(PlayerFlag_NotGainMana);
	registerEnum(PlayerFlag_NotGainHealth);
	registerEnum(PlayerFlag_NotGainSkill);
	registerEnum(PlayerFlag_SetMaxSpeed);
	registerEnum(PlayerFlag_SpecialVIP);
	registerEnum(PlayerFlag_NotGenerateLoot);
	registerEnum(PlayerFlag_IgnoreProtectionZone);
	registerEnum(PlayerFlag_IgnoreSpellCheck);
	registerEnum(PlayerFlag_IgnoreWeaponCheck);
	registerEnum(PlayerFlag_CannotBeMuted);
	registerEnum(PlayerFlag_IsAlwaysPremium);
	registerEnum(PlayerFlag_IgnoreYellCheck);
	registerEnum(PlayerFlag_IgnoreSendPrivateCheck);

	registerEnum(PODIUM_SHOW_PLATFORM);
	registerEnum(PODIUM_SHOW_OUTFIT);
	registerEnum(PODIUM_SHOW_MOUNT);

	registerEnum(PLAYERSEX_FEMALE);
	registerEnum(PLAYERSEX_MALE);

	registerEnum(REPORT_REASON_NAMEINAPPROPRIATE);
	registerEnum(REPORT_REASON_NAMEPOORFORMATTED);
	registerEnum(REPORT_REASON_NAMEADVERTISING);
	registerEnum(REPORT_REASON_NAMEUNFITTING);
	registerEnum(REPORT_REASON_NAMERULEVIOLATION);
	registerEnum(REPORT_REASON_INSULTINGSTATEMENT);
	registerEnum(REPORT_REASON_SPAMMING);
	registerEnum(REPORT_REASON_ADVERTISINGSTATEMENT);
	registerEnum(REPORT_REASON_UNFITTINGSTATEMENT);
	registerEnum(REPORT_REASON_LANGUAGESTATEMENT);
	registerEnum(REPORT_REASON_DISCLOSURE);
	registerEnum(REPORT_REASON_RULEVIOLATION);
	registerEnum(REPORT_REASON_STATEMENT_BUGABUSE);
	registerEnum(REPORT_REASON_UNOFFICIALSOFTWARE);
	registerEnum(REPORT_REASON_PRETENDING);
	registerEnum(REPORT_REASON_HARASSINGOWNERS);
	registerEnum(REPORT_REASON_FALSEINFO);
	registerEnum(REPORT_REASON_ACCOUNTSHARING);
	registerEnum(REPORT_REASON_STEALINGDATA);
	registerEnum(REPORT_REASON_SERVICEATTACKING);
	registerEnum(REPORT_REASON_SERVICEAGREEMENT);

	registerEnum(REPORT_TYPE_NAME);
	registerEnum(REPORT_TYPE_STATEMENT);
	registerEnum(REPORT_TYPE_BOT);

	registerEnum(VOCATION_NONE);

	registerEnum(SKILL_FIST);
	registerEnum(SKILL_CLUB);
	registerEnum(SKILL_SWORD);
	registerEnum(SKILL_AXE);
	registerEnum(SKILL_DISTANCE);
	registerEnum(SKILL_SHIELD);
	registerEnum(SKILL_FISHING);
	registerEnum(SKILL_MAGLEVEL);
	registerEnum(SKILL_LEVEL);

	registerEnum(SPECIALSKILL_CRITICALHITCHANCE);
	registerEnum(SPECIALSKILL_CRITICALHITAMOUNT);
	registerEnum(SPECIALSKILL_LIFELEECHCHANCE);
	registerEnum(SPECIALSKILL_LIFELEECHAMOUNT);
	registerEnum(SPECIALSKILL_MANALEECHCHANCE);
	registerEnum(SPECIALSKILL_MANALEECHAMOUNT);

	registerEnum(STAT_MAXHITPOINTS);
	registerEnum(STAT_MAXMANAPOINTS);
	registerEnum(STAT_SOULPOINTS);
	registerEnum(STAT_MAGICPOINTS);

	registerEnum(SKULL_NONE);
	registerEnum(SKULL_YELLOW);
	registerEnum(SKULL_GREEN);
	registerEnum(SKULL_WHITE);
	registerEnum(SKULL_RED);
	registerEnum(SKULL_BLACK);
	registerEnum(SKULL_ORANGE);

	registerEnum(FLUID_NONE);
	registerEnum(FLUID_WATER);
	registerEnum(FLUID_BLOOD);
	registerEnum(FLUID_BEER);
	registerEnum(FLUID_SLIME);
	registerEnum(FLUID_LEMONADE);
	registerEnum(FLUID_MILK);
	registerEnum(FLUID_MANA);
	registerEnum(FLUID_LIFE);
	registerEnum(FLUID_OIL);
	registerEnum(FLUID_URINE);
	registerEnum(FLUID_COCONUTMILK);
	registerEnum(FLUID_WINE);
	registerEnum(FLUID_MUD);
	registerEnum(FLUID_FRUITJUICE);
	registerEnum(FLUID_LAVA);
	registerEnum(FLUID_RUM);
	registerEnum(FLUID_SWAMP);
	registerEnum(FLUID_TEA);
	registerEnum(FLUID_MEAD);

	registerEnum(TALKTYPE_SAY);
	registerEnum(TALKTYPE_WHISPER);
	registerEnum(TALKTYPE_YELL);
	registerEnum(TALKTYPE_PRIVATE_FROM);
	registerEnum(TALKTYPE_PRIVATE_TO);
	registerEnum(TALKTYPE_CHANNEL_Y);
	registerEnum(TALKTYPE_CHANNEL_O);
	registerEnum(TALKTYPE_SPELL);
	registerEnum(TALKTYPE_PRIVATE_NP);
	registerEnum(TALKTYPE_PRIVATE_NP_CONSOLE);
	registerEnum(TALKTYPE_PRIVATE_PN);
	registerEnum(TALKTYPE_BROADCAST);
	registerEnum(TALKTYPE_CHANNEL_R1);
	registerEnum(TALKTYPE_PRIVATE_RED_FROM);
	registerEnum(TALKTYPE_PRIVATE_RED_TO);
	registerEnum(TALKTYPE_MONSTER_SAY);
	registerEnum(TALKTYPE_MONSTER_YELL);
	registerEnum(TALKTYPE_POTION);

	registerEnum(TEXTCOLOR_BLUE);
	registerEnum(TEXTCOLOR_LIGHTGREEN);
	registerEnum(TEXTCOLOR_LIGHTBLUE);
	registerEnum(TEXTCOLOR_MAYABLUE);
	registerEnum(TEXTCOLOR_DARKRED);
	registerEnum(TEXTCOLOR_LIGHTGREY);
	registerEnum(TEXTCOLOR_SKYBLUE);
	registerEnum(TEXTCOLOR_PURPLE);
	registerEnum(TEXTCOLOR_ELECTRICPURPLE);
	registerEnum(TEXTCOLOR_RED);
	registerEnum(TEXTCOLOR_PASTELRED);
	registerEnum(TEXTCOLOR_ORANGE);
	registerEnum(TEXTCOLOR_YELLOW);
	registerEnum(TEXTCOLOR_WHITE_EXP);
	registerEnum(TEXTCOLOR_NONE);

	registerEnum(TILESTATE_NONE);
	registerEnum(TILESTATE_PROTECTIONZONE);
	registerEnum(TILESTATE_NOPVPZONE);
	registerEnum(TILESTATE_NOLOGOUT);
	registerEnum(TILESTATE_PVPZONE);
	registerEnum(TILESTATE_FLOORCHANGE);
	registerEnum(TILESTATE_FLOORCHANGE_DOWN);
	registerEnum(TILESTATE_FLOORCHANGE_NORTH);
	registerEnum(TILESTATE_FLOORCHANGE_SOUTH);
	registerEnum(TILESTATE_FLOORCHANGE_EAST);
	registerEnum(TILESTATE_FLOORCHANGE_WEST);
	registerEnum(TILESTATE_TELEPORT);
	registerEnum(TILESTATE_MAGICFIELD);
	registerEnum(TILESTATE_MAILBOX);
	registerEnum(TILESTATE_TRASHHOLDER);
	registerEnum(TILESTATE_BED);
	registerEnum(TILESTATE_DEPOT);
	registerEnum(TILESTATE_BLOCKSOLID);
	registerEnum(TILESTATE_BLOCKPATH);
	registerEnum(TILESTATE_IMMOVABLEBLOCKSOLID);
	registerEnum(TILESTATE_IMMOVABLEBLOCKPATH);
	registerEnum(TILESTATE_IMMOVABLENOFIELDBLOCKPATH);
	registerEnum(TILESTATE_NOFIELDBLOCKPATH);
	registerEnum(TILESTATE_FLOORCHANGE_SOUTH_ALT);
	registerEnum(TILESTATE_FLOORCHANGE_EAST_ALT);
	registerEnum(TILESTATE_SUPPORTS_HANGABLE);

	registerEnum(WEAPON_NONE);
	registerEnum(WEAPON_SWORD);
	registerEnum(WEAPON_CLUB);
	registerEnum(WEAPON_AXE);
	registerEnum(WEAPON_SHIELD);
	registerEnum(WEAPON_DISTANCE);
	registerEnum(WEAPON_WAND);
	registerEnum(WEAPON_AMMO);
	registerEnum(WEAPON_QUIVER);

	registerEnum(WORLD_TYPE_NO_PVP);
	registerEnum(WORLD_TYPE_PVP);
	registerEnum(WORLD_TYPE_PVP_ENFORCED);

	// Use with container:addItem, container:addItemEx and possibly other functions.
	registerEnum(FLAG_NOLIMIT);
	registerEnum(FLAG_IGNOREBLOCKITEM);
	registerEnum(FLAG_IGNOREBLOCKCREATURE);
	registerEnum(FLAG_CHILDISOWNER);
	registerEnum(FLAG_PATHFINDING);
	registerEnum(FLAG_IGNOREFIELDDAMAGE);
	registerEnum(FLAG_IGNORENOTMOVEABLE);
	registerEnum(FLAG_IGNOREAUTOSTACK);

	// Use with itemType:getSlotPosition
	registerEnum(SLOTP_WHEREEVER);
	registerEnum(SLOTP_HEAD);
	registerEnum(SLOTP_NECKLACE);
	registerEnum(SLOTP_BACKPACK);
	registerEnum(SLOTP_ARMOR);
	registerEnum(SLOTP_RIGHT);
	registerEnum(SLOTP_LEFT);
	registerEnum(SLOTP_LEGS);
	registerEnum(SLOTP_FEET);
	registerEnum(SLOTP_RING);
	registerEnum(SLOTP_AMMO);
	registerEnum(SLOTP_DEPOT);
	registerEnum(SLOTP_TWO_HAND);

	// Use with combat functions
	registerEnum(ORIGIN_NONE);
	registerEnum(ORIGIN_CONDITION);
	registerEnum(ORIGIN_SPELL);
	registerEnum(ORIGIN_MELEE);
	registerEnum(ORIGIN_RANGED);
	registerEnum(ORIGIN_WAND);

	// Use with house:getAccessList, house:setAccessList
	registerEnum(GUEST_LIST);
	registerEnum(SUBOWNER_LIST);

	// Use with npc:setSpeechBubble
	registerEnum(SPEECHBUBBLE_NONE);
	registerEnum(SPEECHBUBBLE_NORMAL);
	registerEnum(SPEECHBUBBLE_TRADE);
	registerEnum(SPEECHBUBBLE_QUEST);
	registerEnum(SPEECHBUBBLE_COMPASS);
	registerEnum(SPEECHBUBBLE_NORMAL2);
	registerEnum(SPEECHBUBBLE_NORMAL3);
	registerEnum(SPEECHBUBBLE_HIRELING);

	// Use with player:addMapMark
	registerEnum(MAPMARK_TICK);
	registerEnum(MAPMARK_QUESTION);
	registerEnum(MAPMARK_EXCLAMATION);
	registerEnum(MAPMARK_STAR);
	registerEnum(MAPMARK_CROSS);
	registerEnum(MAPMARK_TEMPLE);
	registerEnum(MAPMARK_KISS);
	registerEnum(MAPMARK_SHOVEL);
	registerEnum(MAPMARK_SWORD);
	registerEnum(MAPMARK_FLAG);
	registerEnum(MAPMARK_LOCK);
	registerEnum(MAPMARK_BAG);
	registerEnum(MAPMARK_SKULL);
	registerEnum(MAPMARK_DOLLAR);
	registerEnum(MAPMARK_REDNORTH);
	registerEnum(MAPMARK_REDSOUTH);
	registerEnum(MAPMARK_REDEAST);
	registerEnum(MAPMARK_REDWEST);
	registerEnum(MAPMARK_GREENNORTH);
	registerEnum(MAPMARK_GREENSOUTH);

	// Use with Game.getReturnMessage
	registerEnum(RETURNVALUE_NOERROR);
	registerEnum(RETURNVALUE_NOTPOSSIBLE);
	registerEnum(RETURNVALUE_NOTENOUGHROOM);
	registerEnum(RETURNVALUE_PLAYERISPZLOCKED);
	registerEnum(RETURNVALUE_PLAYERISNOTINVITED);
	registerEnum(RETURNVALUE_CANNOTTHROW);
	registerEnum(RETURNVALUE_THEREISNOWAY);
	registerEnum(RETURNVALUE_DESTINATIONOUTOFREACH);
	registerEnum(RETURNVALUE_CREATUREBLOCK);
	registerEnum(RETURNVALUE_NOTMOVEABLE);
	registerEnum(RETURNVALUE_DROPTWOHANDEDITEM);
	registerEnum(RETURNVALUE_BOTHHANDSNEEDTOBEFREE);
	registerEnum(RETURNVALUE_CANONLYUSEONEWEAPON);
	registerEnum(RETURNVALUE_NEEDEXCHANGE);
	registerEnum(RETURNVALUE_CANNOTBEDRESSED);
	registerEnum(RETURNVALUE_PUTTHISOBJECTINYOURHAND);
	registerEnum(RETURNVALUE_PUTTHISOBJECTINBOTHHANDS);
	registerEnum(RETURNVALUE_TOOFARAWAY);
	registerEnum(RETURNVALUE_FIRSTGODOWNSTAIRS);
	registerEnum(RETURNVALUE_FIRSTGOUPSTAIRS);
	registerEnum(RETURNVALUE_CONTAINERNOTENOUGHROOM);
	registerEnum(RETURNVALUE_NOTENOUGHCAPACITY);
	registerEnum(RETURNVALUE_CANNOTPICKUP);
	registerEnum(RETURNVALUE_THISISIMPOSSIBLE);
	registerEnum(RETURNVALUE_DEPOTISFULL);
	registerEnum(RETURNVALUE_CREATUREDOESNOTEXIST);
	registerEnum(RETURNVALUE_CANNOTUSETHISOBJECT);
	registerEnum(RETURNVALUE_PLAYERWITHTHISNAMEISNOTONLINE);
	registerEnum(RETURNVALUE_NOTREQUIREDLEVELTOUSERUNE);
	registerEnum(RETURNVALUE_YOUAREALREADYTRADING);
	registerEnum(RETURNVALUE_THISPLAYERISALREADYTRADING);
	registerEnum(RETURNVALUE_YOUMAYNOTLOGOUTDURINGAFIGHT);
	registerEnum(RETURNVALUE_DIRECTPLAYERSHOOT);
	registerEnum(RETURNVALUE_NOTENOUGHLEVEL);
	registerEnum(RETURNVALUE_NOTENOUGHMAGICLEVEL);
	registerEnum(RETURNVALUE_NOTENOUGHMANA);
	registerEnum(RETURNVALUE_NOTENOUGHSOUL);
	registerEnum(RETURNVALUE_YOUAREEXHAUSTED);
	registerEnum(RETURNVALUE_YOUCANNOTUSEOBJECTSTHATFAST);
	registerEnum(RETURNVALUE_PLAYERISNOTREACHABLE);
	registerEnum(RETURNVALUE_CANONLYUSETHISRUNEONCREATURES);
	registerEnum(RETURNVALUE_ACTIONNOTPERMITTEDINPROTECTIONZONE);
	registerEnum(RETURNVALUE_YOUMAYNOTATTACKTHISPLAYER);
	registerEnum(RETURNVALUE_YOUMAYNOTATTACKAPERSONINPROTECTIONZONE);
	registerEnum(RETURNVALUE_YOUMAYNOTATTACKAPERSONWHILEINPROTECTIONZONE);
	registerEnum(RETURNVALUE_YOUMAYNOTATTACKTHISCREATURE);
	registerEnum(RETURNVALUE_YOUCANONLYUSEITONCREATURES);
	registerEnum(RETURNVALUE_CREATUREISNOTREACHABLE);
	registerEnum(RETURNVALUE_TURNSECUREMODETOATTACKUNMARKEDPLAYERS);
	registerEnum(RETURNVALUE_YOUNEEDPREMIUMACCOUNT);
	registerEnum(RETURNVALUE_YOUNEEDTOLEARNTHISSPELL);
	registerEnum(RETURNVALUE_YOURVOCATIONCANNOTUSETHISSPELL);
	registerEnum(RETURNVALUE_YOUNEEDAWEAPONTOUSETHISSPELL);
	registerEnum(RETURNVALUE_PLAYERISPZLOCKEDLEAVEPVPZONE);
	registerEnum(RETURNVALUE_PLAYERISPZLOCKEDENTERPVPZONE);
	registerEnum(RETURNVALUE_ACTIONNOTPERMITTEDINANOPVPZONE);
	registerEnum(RETURNVALUE_YOUCANNOTLOGOUTHERE);
	registerEnum(RETURNVALUE_YOUNEEDAMAGICITEMTOCASTSPELL);
	registerEnum(RETURNVALUE_NAMEISTOOAMBIGUOUS);
	registerEnum(RETURNVALUE_CANONLYUSEONESHIELD);
	registerEnum(RETURNVALUE_NOPARTYMEMBERSINRANGE);
	registerEnum(RETURNVALUE_YOUARENOTTHEOWNER);
	registerEnum(RETURNVALUE_TRADEPLAYERFARAWAY);
	registerEnum(RETURNVALUE_YOUDONTOWNTHISHOUSE);
	registerEnum(RETURNVALUE_TRADEPLAYERALREADYOWNSAHOUSE);
	registerEnum(RETURNVALUE_TRADEPLAYERHIGHESTBIDDER);
	registerEnum(RETURNVALUE_YOUCANNOTTRADETHISHOUSE);
	registerEnum(RETURNVALUE_YOUDONTHAVEREQUIREDPROFESSION);
	registerEnum(RETURNVALUE_YOUCANNOTUSETHISBED);

	registerEnum(RELOAD_TYPE_ALL);
	registerEnum(RELOAD_TYPE_ACTIONS);
	registerEnum(RELOAD_TYPE_CHAT);
	registerEnum(RELOAD_TYPE_CONFIG);
	registerEnum(RELOAD_TYPE_CREATURESCRIPTS);
	registerEnum(RELOAD_TYPE_EVENTS);
	registerEnum(RELOAD_TYPE_GLOBAL);
	registerEnum(RELOAD_TYPE_GLOBALEVENTS);
	registerEnum(RELOAD_TYPE_ITEMS);
	registerEnum(RELOAD_TYPE_MONSTERS);
	registerEnum(RELOAD_TYPE_MOUNTS);
	registerEnum(RELOAD_TYPE_MOVEMENTS);
	registerEnum(RELOAD_TYPE_NPCS);
	registerEnum(RELOAD_TYPE_QUESTS);
	registerEnum(RELOAD_TYPE_RAIDS);
	registerEnum(RELOAD_TYPE_SCRIPTS);
	registerEnum(RELOAD_TYPE_SPELLS);
	registerEnum(RELOAD_TYPE_TALKACTIONS);
	registerEnum(RELOAD_TYPE_WEAPONS);

	registerEnum(ZONE_PROTECTION);
	registerEnum(ZONE_NOPVP);
	registerEnum(ZONE_PVP);
	registerEnum(ZONE_NOLOGOUT);
	registerEnum(ZONE_NORMAL);

	registerEnum(MAX_LOOTCHANCE);

	registerEnum(SPELL_INSTANT);
	registerEnum(SPELL_RUNE);

	registerEnum(MONSTERS_EVENT_THINK);
	registerEnum(MONSTERS_EVENT_APPEAR);
	registerEnum(MONSTERS_EVENT_DISAPPEAR);
	registerEnum(MONSTERS_EVENT_MOVE);
	registerEnum(MONSTERS_EVENT_SAY);

	registerEnum(DECAYING_FALSE);
	registerEnum(DECAYING_TRUE);
	registerEnum(DECAYING_PENDING);

	// _G
	registerGlobalVariable("INDEX_WHEREEVER", INDEX_WHEREEVER);
	registerGlobalBoolean("VIRTUAL_PARENT", true);

	registerGlobalMethod("isType", luaIsType);
	registerGlobalMethod("rawgetmetatable", luaRawGetMetatable);

	// configKeys
	registerTable("configKeys");

	registerEnumIn("configKeys", ConfigManager::ALLOW_CHANGEOUTFIT);
	registerEnumIn("configKeys", ConfigManager::ONE_PLAYER_ON_ACCOUNT);
	registerEnumIn("configKeys", ConfigManager::AIMBOT_HOTKEY_ENABLED);
	registerEnumIn("configKeys", ConfigManager::REMOVE_RUNE_CHARGES);
	registerEnumIn("configKeys", ConfigManager::REMOVE_WEAPON_AMMO);
	registerEnumIn("configKeys", ConfigManager::REMOVE_WEAPON_CHARGES);
	registerEnumIn("configKeys", ConfigManager::REMOVE_POTION_CHARGES);
	registerEnumIn("configKeys", ConfigManager::EXPERIENCE_FROM_PLAYERS);
	registerEnumIn("configKeys", ConfigManager::FREE_PREMIUM);
	registerEnumIn("configKeys", ConfigManager::REPLACE_KICK_ON_LOGIN);
	registerEnumIn("configKeys", ConfigManager::ALLOW_CLONES);
	registerEnumIn("configKeys", ConfigManager::BIND_ONLY_GLOBAL_ADDRESS);
	registerEnumIn("configKeys", ConfigManager::OPTIMIZE_DATABASE);
	registerEnumIn("configKeys", ConfigManager::MARKET_PREMIUM);
	registerEnumIn("configKeys", ConfigManager::EMOTE_SPELLS);
	registerEnumIn("configKeys", ConfigManager::STAMINA_SYSTEM);
	registerEnumIn("configKeys", ConfigManager::WARN_UNSAFE_SCRIPTS);
	registerEnumIn("configKeys", ConfigManager::CONVERT_UNSAFE_SCRIPTS);
	registerEnumIn("configKeys", ConfigManager::CLASSIC_EQUIPMENT_SLOTS);
	registerEnumIn("configKeys", ConfigManager::CLASSIC_ATTACK_SPEED);
	registerEnumIn("configKeys", ConfigManager::SERVER_SAVE_NOTIFY_MESSAGE);
	registerEnumIn("configKeys", ConfigManager::SERVER_SAVE_NOTIFY_DURATION);
	registerEnumIn("configKeys", ConfigManager::SERVER_SAVE_CLEAN_MAP);
	registerEnumIn("configKeys", ConfigManager::SERVER_SAVE_CLOSE);
	registerEnumIn("configKeys", ConfigManager::SERVER_SAVE_SHUTDOWN);
	registerEnumIn("configKeys", ConfigManager::ONLINE_OFFLINE_CHARLIST);

	registerEnumIn("configKeys", ConfigManager::MAP_NAME);
	registerEnumIn("configKeys", ConfigManager::HOUSE_RENT_PERIOD);
	registerEnumIn("configKeys", ConfigManager::SERVER_NAME);
	registerEnumIn("configKeys", ConfigManager::OWNER_NAME);
	registerEnumIn("configKeys", ConfigManager::OWNER_EMAIL);
	registerEnumIn("configKeys", ConfigManager::URL);
	registerEnumIn("configKeys", ConfigManager::LOCATION);
	registerEnumIn("configKeys", ConfigManager::IP);
	registerEnumIn("configKeys", ConfigManager::WORLD_TYPE);
	registerEnumIn("configKeys", ConfigManager::MYSQL_HOST);
	registerEnumIn("configKeys", ConfigManager::MYSQL_USER);
	registerEnumIn("configKeys", ConfigManager::MYSQL_PASS);
	registerEnumIn("configKeys", ConfigManager::MYSQL_DB);
	registerEnumIn("configKeys", ConfigManager::MYSQL_SOCK);
	registerEnumIn("configKeys", ConfigManager::DEFAULT_PRIORITY);
	registerEnumIn("configKeys", ConfigManager::MAP_AUTHOR);

	registerEnumIn("configKeys", ConfigManager::SQL_PORT);
	registerEnumIn("configKeys", ConfigManager::MAX_PLAYERS);
	registerEnumIn("configKeys", ConfigManager::PZ_LOCKED);
	registerEnumIn("configKeys", ConfigManager::DEFAULT_DESPAWNRANGE);
	registerEnumIn("configKeys", ConfigManager::DEFAULT_DESPAWNRADIUS);
	registerEnumIn("configKeys", ConfigManager::DEFAULT_WALKTOSPAWNRADIUS);
	registerEnumIn("configKeys", ConfigManager::REMOVE_ON_DESPAWN);
	registerEnumIn("configKeys", ConfigManager::RATE_EXPERIENCE);
	registerEnumIn("configKeys", ConfigManager::RATE_SKILL);
	registerEnumIn("configKeys", ConfigManager::RATE_LOOT);
	registerEnumIn("configKeys", ConfigManager::RATE_MAGIC);
	registerEnumIn("configKeys", ConfigManager::RATE_SPAWN);
	registerEnumIn("configKeys", ConfigManager::HOUSE_PRICE);
	registerEnumIn("configKeys", ConfigManager::KILLS_TO_RED);
	registerEnumIn("configKeys", ConfigManager::KILLS_TO_BLACK);
	registerEnumIn("configKeys", ConfigManager::MAX_MESSAGEBUFFER);
	registerEnumIn("configKeys", ConfigManager::ACTIONS_DELAY_INTERVAL);
	registerEnumIn("configKeys", ConfigManager::EX_ACTIONS_DELAY_INTERVAL);
	registerEnumIn("configKeys", ConfigManager::KICK_AFTER_MINUTES);
	registerEnumIn("configKeys", ConfigManager::PROTECTION_LEVEL);
	registerEnumIn("configKeys", ConfigManager::DEATH_LOSE_PERCENT);
	registerEnumIn("configKeys", ConfigManager::STATUSQUERY_TIMEOUT);
	registerEnumIn("configKeys", ConfigManager::FRAG_TIME);
	registerEnumIn("configKeys", ConfigManager::WHITE_SKULL_TIME);
	registerEnumIn("configKeys", ConfigManager::GAME_PORT);
	registerEnumIn("configKeys", ConfigManager::LOGIN_PORT);
	registerEnumIn("configKeys", ConfigManager::STATUS_PORT);
	registerEnumIn("configKeys", ConfigManager::STAIRHOP_DELAY);
	registerEnumIn("configKeys", ConfigManager::MARKET_OFFER_DURATION);
	registerEnumIn("configKeys", ConfigManager::CHECK_EXPIRED_MARKET_OFFERS_EACH_MINUTES);
	registerEnumIn("configKeys", ConfigManager::MAX_MARKET_OFFERS_AT_A_TIME_PER_PLAYER);
	registerEnumIn("configKeys", ConfigManager::EXP_FROM_PLAYERS_LEVEL_RANGE);
	registerEnumIn("configKeys", ConfigManager::MAX_PACKETS_PER_SECOND);
	registerEnumIn("configKeys", ConfigManager::PLAYER_CONSOLE_LOGS);
	registerEnumIn("configKeys", ConfigManager::TWO_FACTOR_AUTH);
	registerEnumIn("configKeys", ConfigManager::STAMINA_REGEN_MINUTE);
	registerEnumIn("configKeys", ConfigManager::STAMINA_REGEN_PREMIUM);

	// os
	registerMethod("os", "mtime", luaSystemTime);

	// table
	registerMethod("table", "create", luaTableCreate);
	registerMethod("table", "pack", luaTablePack);

	// Game
	registerTable("Game");

	registerMethod("Game", "getSpectators", luaGameGetSpectators);
	registerMethod("Game", "getPlayers", luaGameGetPlayers);
	registerMethod("Game", "getNpcs", luaGameGetNpcs);
	registerMethod("Game", "getMonsters", luaGameGetMonsters);
	registerMethod("Game", "loadMap", luaGameLoadMap);

	registerMethod("Game", "getExperienceStage", luaGameGetExperienceStage);
	registerMethod("Game", "getExperienceForLevel", luaGameGetExperienceForLevel);
	registerMethod("Game", "getMonsterCount", luaGameGetMonsterCount);
	registerMethod("Game", "getPlayerCount", luaGameGetPlayerCount);
	registerMethod("Game", "getNpcCount", luaGameGetNpcCount);
	registerMethod("Game", "getMonsterTypes", luaGameGetMonsterTypes);
	registerMethod("Game", "getCurrencyItems", luaGameGetCurrencyItems);
	registerMethod("Game", "getItemTypeByClientId", luaGameGetItemTypeByClientId);
	registerMethod("Game", "getMountIdByLookType", luaGameGetMountIdByLookType);

	registerMethod("Game", "getTowns", luaGameGetTowns);
	registerMethod("Game", "getHouses", luaGameGetHouses);
	registerMethod("Game", "getOutfits", luaGameGetOutfits);
	registerMethod("Game", "getMounts", luaGameGetMounts);

	registerMethod("Game", "getGameState", luaGameGetGameState);
	registerMethod("Game", "setGameState", luaGameSetGameState);

	registerMethod("Game", "getWorldType", luaGameGetWorldType);
	registerMethod("Game", "setWorldType", luaGameSetWorldType);

	registerMethod("Game", "getItemAttributeByName", luaGameGetItemAttributeByName);
	registerMethod("Game", "getReturnMessage", luaGameGetReturnMessage);

	registerMethod("Game", "createItem", luaGameCreateItem);
	registerMethod("Game", "createContainer", luaGameCreateContainer);
	registerMethod("Game", "createMonster", luaGameCreateMonster);
	registerMethod("Game", "createNpc", luaGameCreateNpc);
	registerMethod("Game", "createTile", luaGameCreateTile);
	registerMethod("Game", "createMonsterType", luaGameCreateMonsterType);

	registerMethod("Game", "startRaid", luaGameStartRaid);

	registerMethod("Game", "getClientVersion", luaGameGetClientVersion);

	registerMethod("Game", "reload", luaGameReload);

	registerMethod("Game", "getAccountStorageValue", luaGameGetAccountStorageValue);
	registerMethod("Game", "setAccountStorageValue", luaGameSetAccountStorageValue);
	registerMethod("Game", "saveAccountStorageValues", luaGameSaveAccountStorageValues);

	// Variant
	registerClass("Variant", "", luaVariantCreate);

	registerMethod("Variant", "getNumber", luaVariantGetNumber);
	registerMethod("Variant", "getString", luaVariantGetString);
	registerMethod("Variant", "getPosition", luaVariantGetPosition);

	// Position
	registerClass("Position", "", luaPositionCreate);
	registerMetaMethod("Position", "__add", luaPositionAdd);
	registerMetaMethod("Position", "__sub", luaPositionSub);
	registerMetaMethod("Position", "__eq", luaPositionCompare);

	registerMethod("Position", "getDistance", luaPositionGetDistance);
	registerMethod("Position", "isSightClear", luaPositionIsSightClear);

	registerMethod("Position", "sendMagicEffect", luaPositionSendMagicEffect);
	registerMethod("Position", "sendDistanceEffect", luaPositionSendDistanceEffect);

	// Tile

	// NetworkMessage
	registerClass("NetworkMessage", "", luaNetworkMessageCreate);
	registerMetaMethod("NetworkMessage", "__eq", tfs::lua::luaUserdataCompare);
	registerMetaMethod("NetworkMessage", "__gc", luaNetworkMessageDelete);
	registerMethod("NetworkMessage", "delete", luaNetworkMessageDelete);

	registerMethod("NetworkMessage", "getByte", luaNetworkMessageGetByte);
	registerMethod("NetworkMessage", "getU16", luaNetworkMessageGetU16);
	registerMethod("NetworkMessage", "getU32", luaNetworkMessageGetU32);
	registerMethod("NetworkMessage", "getU64", luaNetworkMessageGetU64);
	registerMethod("NetworkMessage", "getString", luaNetworkMessageGetString);
	registerMethod("NetworkMessage", "getPosition", luaNetworkMessageGetPosition);

	registerMethod("NetworkMessage", "addByte", luaNetworkMessageAddByte);
	registerMethod("NetworkMessage", "addU16", luaNetworkMessageAddU16);
	registerMethod("NetworkMessage", "addU32", luaNetworkMessageAddU32);
	registerMethod("NetworkMessage", "addU64", luaNetworkMessageAddU64);
	registerMethod("NetworkMessage", "addString", luaNetworkMessageAddString);
	registerMethod("NetworkMessage", "addPosition", luaNetworkMessageAddPosition);
	registerMethod("NetworkMessage", "addDouble", luaNetworkMessageAddDouble);
	registerMethod("NetworkMessage", "addItem", luaNetworkMessageAddItem);
	registerMethod("NetworkMessage", "addItemId", luaNetworkMessageAddItemId);

	registerMethod("NetworkMessage", "reset", luaNetworkMessageReset);
	registerMethod("NetworkMessage", "seek", luaNetworkMessageSeek);
	registerMethod("NetworkMessage", "tell", luaNetworkMessageTell);
	registerMethod("NetworkMessage", "len", luaNetworkMessageLength);
	registerMethod("NetworkMessage", "skipBytes", luaNetworkMessageSkipBytes);
	registerMethod("NetworkMessage", "sendToPlayer", luaNetworkMessageSendToPlayer);

	// ModalWindow
	registerClass("ModalWindow", "", luaModalWindowCreate);
	registerMetaMethod("ModalWindow", "__eq", tfs::lua::luaUserdataCompare);
	registerMetaMethod("ModalWindow", "__gc", luaModalWindowDelete);
	registerMethod("ModalWindow", "delete", luaModalWindowDelete);

	registerMethod("ModalWindow", "getId", luaModalWindowGetId);
	registerMethod("ModalWindow", "getTitle", luaModalWindowGetTitle);
	registerMethod("ModalWindow", "getMessage", luaModalWindowGetMessage);

	registerMethod("ModalWindow", "setTitle", luaModalWindowSetTitle);
	registerMethod("ModalWindow", "setMessage", luaModalWindowSetMessage);

	registerMethod("ModalWindow", "getButtonCount", luaModalWindowGetButtonCount);
	registerMethod("ModalWindow", "getChoiceCount", luaModalWindowGetChoiceCount);

	registerMethod("ModalWindow", "addButton", luaModalWindowAddButton);
	registerMethod("ModalWindow", "addChoice", luaModalWindowAddChoice);

	registerMethod("ModalWindow", "getDefaultEnterButton", luaModalWindowGetDefaultEnterButton);
	registerMethod("ModalWindow", "setDefaultEnterButton", luaModalWindowSetDefaultEnterButton);

	registerMethod("ModalWindow", "getDefaultEscapeButton", luaModalWindowGetDefaultEscapeButton);
	registerMethod("ModalWindow", "setDefaultEscapeButton", luaModalWindowSetDefaultEscapeButton);

	registerMethod("ModalWindow", "hasPriority", luaModalWindowHasPriority);
	registerMethod("ModalWindow", "setPriority", luaModalWindowSetPriority);

	registerMethod("ModalWindow", "sendToPlayer", luaModalWindowSendToPlayer);

	// Item
	registerClass("Item", "", luaItemCreate);
	registerMetaMethod("Item", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Item", "isItem", luaItemIsItem);

	registerMethod("Item", "getParent", luaItemGetParent);
	registerMethod("Item", "getTopParent", luaItemGetTopParent);

	registerMethod("Item", "getId", luaItemGetId);

	registerMethod("Item", "clone", luaItemClone);
	registerMethod("Item", "split", luaItemSplit);
	registerMethod("Item", "remove", luaItemRemove);

	registerMethod("Item", "getUniqueId", luaItemGetUniqueId);
	registerMethod("Item", "getActionId", luaItemGetActionId);
	registerMethod("Item", "setActionId", luaItemSetActionId);

	registerMethod("Item", "getCount", luaItemGetCount);
	registerMethod("Item", "getCharges", luaItemGetCharges);
	registerMethod("Item", "getFluidType", luaItemGetFluidType);
	registerMethod("Item", "getWeight", luaItemGetWeight);
	registerMethod("Item", "getWorth", luaItemGetWorth);

	registerMethod("Item", "getSubType", luaItemGetSubType);

	registerMethod("Item", "getName", luaItemGetName);
	registerMethod("Item", "getPluralName", luaItemGetPluralName);
	registerMethod("Item", "getArticle", luaItemGetArticle);

	registerMethod("Item", "getPosition", luaItemGetPosition);
	registerMethod("Item", "getTile", luaItemGetTile);

	registerMethod("Item", "hasAttribute", luaItemHasAttribute);
	registerMethod("Item", "getAttribute", luaItemGetAttribute);
	registerMethod("Item", "setAttribute", luaItemSetAttribute);
	registerMethod("Item", "removeAttribute", luaItemRemoveAttribute);
	registerMethod("Item", "getCustomAttribute", luaItemGetCustomAttribute);
	registerMethod("Item", "setCustomAttribute", luaItemSetCustomAttribute);
	registerMethod("Item", "removeCustomAttribute", luaItemRemoveCustomAttribute);

	registerMethod("Item", "moveTo", luaItemMoveTo);
	registerMethod("Item", "transform", luaItemTransform);
	registerMethod("Item", "decay", luaItemDecay);

	registerMethod("Item", "getSpecialDescription", luaItemGetSpecialDescription);

	registerMethod("Item", "hasProperty", luaItemHasProperty);
	registerMethod("Item", "isLoadedFromMap", luaItemIsLoadedFromMap);

	registerMethod("Item", "setStoreItem", luaItemSetStoreItem);
	registerMethod("Item", "isStoreItem", luaItemIsStoreItem);

	registerMethod("Item", "setReflect", luaItemSetReflect);
	registerMethod("Item", "getReflect", luaItemGetReflect);

	registerMethod("Item", "setBoostPercent", luaItemSetBoostPercent);
	registerMethod("Item", "getBoostPercent", luaItemGetBoostPercent);

	// Container
	registerClass("Container", "Item", luaContainerCreate);
	registerMetaMethod("Container", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Container", "getSize", luaContainerGetSize);
	registerMethod("Container", "getCapacity", luaContainerGetCapacity);
	registerMethod("Container", "getEmptySlots", luaContainerGetEmptySlots);
	registerMethod("Container", "getItems", luaContainerGetItems);
	registerMethod("Container", "getItemHoldingCount", luaContainerGetItemHoldingCount);
	registerMethod("Container", "getItemCountById", luaContainerGetItemCountById);

	registerMethod("Container", "getItem", luaContainerGetItem);
	registerMethod("Container", "hasItem", luaContainerHasItem);
	registerMethod("Container", "addItem", luaContainerAddItem);
	registerMethod("Container", "addItemEx", luaContainerAddItemEx);
	registerMethod("Container", "getCorpseOwner", luaContainerGetCorpseOwner);

	// Teleport
	registerClass("Teleport", "Item", luaTeleportCreate);
	registerMetaMethod("Teleport", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Teleport", "getDestination", luaTeleportGetDestination);
	registerMethod("Teleport", "setDestination", luaTeleportSetDestination);

	// Podium
	registerClass("Podium", "Item", luaPodiumCreate);
	registerMetaMethod("Podium", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Podium", "getOutfit", luaPodiumGetOutfit);
	registerMethod("Podium", "setOutfit", luaPodiumSetOutfit);
	registerMethod("Podium", "hasFlag", luaPodiumHasFlag);
	registerMethod("Podium", "setFlag", luaPodiumSetFlag);
	registerMethod("Podium", "getDirection", luaPodiumGetDirection);
	registerMethod("Podium", "setDirection", luaPodiumSetDirection);

	// Creature
	registerClass("Creature", "", luaCreatureCreate);
	registerMetaMethod("Creature", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Creature", "getEvents", luaCreatureGetEvents);
	registerMethod("Creature", "registerEvent", luaCreatureRegisterEvent);
	registerMethod("Creature", "unregisterEvent", luaCreatureUnregisterEvent);

	registerMethod("Creature", "isRemoved", luaCreatureIsRemoved);
	registerMethod("Creature", "isCreature", luaCreatureIsCreature);
	registerMethod("Creature", "isInGhostMode", luaCreatureIsInGhostMode);
	registerMethod("Creature", "isHealthHidden", luaCreatureIsHealthHidden);
	registerMethod("Creature", "isMovementBlocked", luaCreatureIsMovementBlocked);
	registerMethod("Creature", "isImmune", luaCreatureIsImmune);

	registerMethod("Creature", "canSee", luaCreatureCanSee);
	registerMethod("Creature", "canSeeCreature", luaCreatureCanSeeCreature);
	registerMethod("Creature", "canSeeGhostMode", luaCreatureCanSeeGhostMode);
	registerMethod("Creature", "canSeeInvisibility", luaCreatureCanSeeInvisibility);

	registerMethod("Creature", "getParent", luaCreatureGetParent);

	registerMethod("Creature", "getId", luaCreatureGetId);
	registerMethod("Creature", "getName", luaCreatureGetName);

	registerMethod("Creature", "getTarget", luaCreatureGetTarget);
	registerMethod("Creature", "setTarget", luaCreatureSetTarget);

	registerMethod("Creature", "getFollowCreature", luaCreatureGetFollowCreature);
	registerMethod("Creature", "setFollowCreature", luaCreatureSetFollowCreature);

	registerMethod("Creature", "getMaster", luaCreatureGetMaster);
	registerMethod("Creature", "setMaster", luaCreatureSetMaster);

	registerMethod("Creature", "getLight", luaCreatureGetLight);
	registerMethod("Creature", "setLight", luaCreatureSetLight);

	registerMethod("Creature", "getSpeed", luaCreatureGetSpeed);
	registerMethod("Creature", "getBaseSpeed", luaCreatureGetBaseSpeed);
	registerMethod("Creature", "changeSpeed", luaCreatureChangeSpeed);

	registerMethod("Creature", "setDropLoot", luaCreatureSetDropLoot);
	registerMethod("Creature", "setSkillLoss", luaCreatureSetSkillLoss);

	registerMethod("Creature", "getPosition", luaCreatureGetPosition);
	registerMethod("Creature", "getTile", luaCreatureGetTile);
	registerMethod("Creature", "getDirection", luaCreatureGetDirection);
	registerMethod("Creature", "setDirection", luaCreatureSetDirection);

	registerMethod("Creature", "getHealth", luaCreatureGetHealth);
	registerMethod("Creature", "setHealth", luaCreatureSetHealth);
	registerMethod("Creature", "addHealth", luaCreatureAddHealth);
	registerMethod("Creature", "getMaxHealth", luaCreatureGetMaxHealth);
	registerMethod("Creature", "setMaxHealth", luaCreatureSetMaxHealth);
	registerMethod("Creature", "setHiddenHealth", luaCreatureSetHiddenHealth);
	registerMethod("Creature", "setMovementBlocked", luaCreatureSetMovementBlocked);

	registerMethod("Creature", "getSkull", luaCreatureGetSkull);
	registerMethod("Creature", "setSkull", luaCreatureSetSkull);

	registerMethod("Creature", "getOutfit", luaCreatureGetOutfit);
	registerMethod("Creature", "setOutfit", luaCreatureSetOutfit);

	registerMethod("Creature", "getCondition", luaCreatureGetCondition);
	registerMethod("Creature", "addCondition", luaCreatureAddCondition);
	registerMethod("Creature", "removeCondition", luaCreatureRemoveCondition);
	registerMethod("Creature", "hasCondition", luaCreatureHasCondition);

	registerMethod("Creature", "remove", luaCreatureRemove);
	registerMethod("Creature", "teleportTo", luaCreatureTeleportTo);
	registerMethod("Creature", "say", luaCreatureSay);

	registerMethod("Creature", "getDamageMap", luaCreatureGetDamageMap);

	registerMethod("Creature", "getSummons", luaCreatureGetSummons);

	registerMethod("Creature", "getDescription", luaCreatureGetDescription);

	registerMethod("Creature", "getPathTo", luaCreatureGetPathTo);
	registerMethod("Creature", "move", luaCreatureMove);

	registerMethod("Creature", "getZone", luaCreatureGetZone);

	// Player

	// Monster
	registerClass("Monster", "Creature", luaMonsterCreate);
	registerMetaMethod("Monster", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Monster", "isMonster", luaMonsterIsMonster);

	registerMethod("Monster", "getType", luaMonsterGetType);

	registerMethod("Monster", "rename", luaMonsterRename);

	registerMethod("Monster", "getSpawnPosition", luaMonsterGetSpawnPosition);
	registerMethod("Monster", "isInSpawnRange", luaMonsterIsInSpawnRange);

	registerMethod("Monster", "isIdle", luaMonsterIsIdle);
	registerMethod("Monster", "setIdle", luaMonsterSetIdle);

	registerMethod("Monster", "isTarget", luaMonsterIsTarget);
	registerMethod("Monster", "isOpponent", luaMonsterIsOpponent);
	registerMethod("Monster", "isFriend", luaMonsterIsFriend);

	registerMethod("Monster", "addFriend", luaMonsterAddFriend);
	registerMethod("Monster", "removeFriend", luaMonsterRemoveFriend);
	registerMethod("Monster", "getFriendList", luaMonsterGetFriendList);
	registerMethod("Monster", "getFriendCount", luaMonsterGetFriendCount);

	registerMethod("Monster", "addTarget", luaMonsterAddTarget);
	registerMethod("Monster", "removeTarget", luaMonsterRemoveTarget);
	registerMethod("Monster", "getTargetList", luaMonsterGetTargetList);
	registerMethod("Monster", "getTargetCount", luaMonsterGetTargetCount);

	registerMethod("Monster", "selectTarget", luaMonsterSelectTarget);
	registerMethod("Monster", "searchTarget", luaMonsterSearchTarget);

	registerMethod("Monster", "isWalkingToSpawn", luaMonsterIsWalkingToSpawn);
	registerMethod("Monster", "walkToSpawn", luaMonsterWalkToSpawn);

	// Npc
	registerClass("Npc", "Creature", luaNpcCreate);
	registerMetaMethod("Npc", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Npc", "isNpc", luaNpcIsNpc);

	registerMethod("Npc", "setMasterPos", luaNpcSetMasterPos);

	registerMethod("Npc", "getSpeechBubble", luaNpcGetSpeechBubble);
	registerMethod("Npc", "setSpeechBubble", luaNpcSetSpeechBubble);

	// Guild
	registerClass("Guild", "", luaGuildCreate);
	registerMetaMethod("Guild", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Guild", "getId", luaGuildGetId);
	registerMethod("Guild", "getName", luaGuildGetName);
	registerMethod("Guild", "getMembersOnline", luaGuildGetMembersOnline);

	registerMethod("Guild", "addRank", luaGuildAddRank);
	registerMethod("Guild", "getRankById", luaGuildGetRankById);
	registerMethod("Guild", "getRankByLevel", luaGuildGetRankByLevel);

	registerMethod("Guild", "getMotd", luaGuildGetMotd);
	registerMethod("Guild", "setMotd", luaGuildSetMotd);

	// Group
	registerClass("Group", "", luaGroupCreate);
	registerMetaMethod("Group", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Group", "getId", luaGroupGetId);
	registerMethod("Group", "getName", luaGroupGetName);
	registerMethod("Group", "getFlags", luaGroupGetFlags);
	registerMethod("Group", "getAccess", luaGroupGetAccess);
	registerMethod("Group", "getMaxDepotItems", luaGroupGetMaxDepotItems);
	registerMethod("Group", "getMaxVipEntries", luaGroupGetMaxVipEntries);
	registerMethod("Group", "hasFlag", luaGroupHasFlag);

	// Vocation
	registerClass("Vocation", "", luaVocationCreate);
	registerMetaMethod("Vocation", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Vocation", "getId", luaVocationGetId);
	registerMethod("Vocation", "getClientId", luaVocationGetClientId);
	registerMethod("Vocation", "getName", luaVocationGetName);
	registerMethod("Vocation", "getDescription", luaVocationGetDescription);

	registerMethod("Vocation", "getRequiredSkillTries", luaVocationGetRequiredSkillTries);
	registerMethod("Vocation", "getRequiredManaSpent", luaVocationGetRequiredManaSpent);

	registerMethod("Vocation", "getCapacityGain", luaVocationGetCapacityGain);

	registerMethod("Vocation", "getHealthGain", luaVocationGetHealthGain);
	registerMethod("Vocation", "getHealthGainTicks", luaVocationGetHealthGainTicks);
	registerMethod("Vocation", "getHealthGainAmount", luaVocationGetHealthGainAmount);

	registerMethod("Vocation", "getManaGain", luaVocationGetManaGain);
	registerMethod("Vocation", "getManaGainTicks", luaVocationGetManaGainTicks);
	registerMethod("Vocation", "getManaGainAmount", luaVocationGetManaGainAmount);

	registerMethod("Vocation", "getMaxSoul", luaVocationGetMaxSoul);
	registerMethod("Vocation", "getSoulGainTicks", luaVocationGetSoulGainTicks);

	registerMethod("Vocation", "getAttackSpeed", luaVocationGetAttackSpeed);
	registerMethod("Vocation", "getBaseSpeed", luaVocationGetBaseSpeed);

	registerMethod("Vocation", "getDemotion", luaVocationGetDemotion);
	registerMethod("Vocation", "getPromotion", luaVocationGetPromotion);

	registerMethod("Vocation", "allowsPvp", luaVocationAllowsPvp);

	// Town
	registerClass("Town", "", luaTownCreate);
	registerMetaMethod("Town", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Town", "getId", luaTownGetId);
	registerMethod("Town", "getName", luaTownGetName);
	registerMethod("Town", "getTemplePosition", luaTownGetTemplePosition);

	// House
	registerClass("House", "", luaHouseCreate);
	registerMetaMethod("House", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("House", "getId", luaHouseGetId);
	registerMethod("House", "getName", luaHouseGetName);
	registerMethod("House", "getTown", luaHouseGetTown);
	registerMethod("House", "getExitPosition", luaHouseGetExitPosition);

	registerMethod("House", "getRent", luaHouseGetRent);
	registerMethod("House", "setRent", luaHouseSetRent);

	registerMethod("House", "getPaidUntil", luaHouseGetPaidUntil);
	registerMethod("House", "setPaidUntil", luaHouseSetPaidUntil);

	registerMethod("House", "getPayRentWarnings", luaHouseGetPayRentWarnings);
	registerMethod("House", "setPayRentWarnings", luaHouseSetPayRentWarnings);

	registerMethod("House", "getOwnerName", luaHouseGetOwnerName);
	registerMethod("House", "getOwnerGuid", luaHouseGetOwnerGuid);
	registerMethod("House", "setOwnerGuid", luaHouseSetOwnerGuid);
	registerMethod("House", "startTrade", luaHouseStartTrade);

	registerMethod("House", "getBeds", luaHouseGetBeds);
	registerMethod("House", "getBedCount", luaHouseGetBedCount);

	registerMethod("House", "getDoors", luaHouseGetDoors);
	registerMethod("House", "getDoorCount", luaHouseGetDoorCount);
	registerMethod("House", "getDoorIdByPosition", luaHouseGetDoorIdByPosition);

	registerMethod("House", "getTiles", luaHouseGetTiles);
	registerMethod("House", "getItems", luaHouseGetItems);
	registerMethod("House", "getTileCount", luaHouseGetTileCount);

	registerMethod("House", "canEditAccessList", luaHouseCanEditAccessList);
	registerMethod("House", "getAccessList", luaHouseGetAccessList);
	registerMethod("House", "setAccessList", luaHouseSetAccessList);

	registerMethod("House", "kickPlayer", luaHouseKickPlayer);

	registerMethod("House", "save", luaHouseSave);

	// ItemType
	registerClass("ItemType", "", luaItemTypeCreate);
	registerMetaMethod("ItemType", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("ItemType", "isCorpse", luaItemTypeIsCorpse);
	registerMethod("ItemType", "isDoor", luaItemTypeIsDoor);
	registerMethod("ItemType", "isContainer", luaItemTypeIsContainer);
	registerMethod("ItemType", "isFluidContainer", luaItemTypeIsFluidContainer);
	registerMethod("ItemType", "isMovable", luaItemTypeIsMovable);
	registerMethod("ItemType", "isRune", luaItemTypeIsRune);
	registerMethod("ItemType", "isStackable", luaItemTypeIsStackable);
	registerMethod("ItemType", "isReadable", luaItemTypeIsReadable);
	registerMethod("ItemType", "isWritable", luaItemTypeIsWritable);
	registerMethod("ItemType", "isBlocking", luaItemTypeIsBlocking);
	registerMethod("ItemType", "isGroundTile", luaItemTypeIsGroundTile);
	registerMethod("ItemType", "isMagicField", luaItemTypeIsMagicField);
	registerMethod("ItemType", "isUseable", luaItemTypeIsUseable);
	registerMethod("ItemType", "isPickupable", luaItemTypeIsPickupable);

	registerMethod("ItemType", "getType", luaItemTypeGetType);
	registerMethod("ItemType", "getGroup", luaItemTypeGetGroup);
	registerMethod("ItemType", "getId", luaItemTypeGetId);
	registerMethod("ItemType", "getClientId", luaItemTypeGetClientId);
	registerMethod("ItemType", "getName", luaItemTypeGetName);
	registerMethod("ItemType", "getPluralName", luaItemTypeGetPluralName);
	registerMethod("ItemType", "getArticle", luaItemTypeGetArticle);
	registerMethod("ItemType", "getDescription", luaItemTypeGetDescription);
	registerMethod("ItemType", "getSlotPosition", luaItemTypeGetSlotPosition);

	registerMethod("ItemType", "getCharges", luaItemTypeGetCharges);
	registerMethod("ItemType", "getFluidSource", luaItemTypeGetFluidSource);
	registerMethod("ItemType", "getCapacity", luaItemTypeGetCapacity);
	registerMethod("ItemType", "getWeight", luaItemTypeGetWeight);
	registerMethod("ItemType", "getWorth", luaItemTypeGetWorth);

	registerMethod("ItemType", "getHitChance", luaItemTypeGetHitChance);
	registerMethod("ItemType", "getShootRange", luaItemTypeGetShootRange);

	registerMethod("ItemType", "getAttack", luaItemTypeGetAttack);
	registerMethod("ItemType", "getAttackSpeed", luaItemTypeGetAttackSpeed);
	registerMethod("ItemType", "getDefense", luaItemTypeGetDefense);
	registerMethod("ItemType", "getExtraDefense", luaItemTypeGetExtraDefense);
	registerMethod("ItemType", "getArmor", luaItemTypeGetArmor);
	registerMethod("ItemType", "getWeaponType", luaItemTypeGetWeaponType);

	registerMethod("ItemType", "getElementType", luaItemTypeGetElementType);
	registerMethod("ItemType", "getElementDamage", luaItemTypeGetElementDamage);

	registerMethod("ItemType", "getTransformEquipId", luaItemTypeGetTransformEquipId);
	registerMethod("ItemType", "getTransformDeEquipId", luaItemTypeGetTransformDeEquipId);
	registerMethod("ItemType", "getDestroyId", luaItemTypeGetDestroyId);
	registerMethod("ItemType", "getDecayId", luaItemTypeGetDecayId);
	registerMethod("ItemType", "getRequiredLevel", luaItemTypeGetRequiredLevel);
	registerMethod("ItemType", "getAmmoType", luaItemTypeGetAmmoType);
	registerMethod("ItemType", "getCorpseType", luaItemTypeGetCorpseType);
	registerMethod("ItemType", "getClassification", luaItemTypeGetClassification);

	registerMethod("ItemType", "getAbilities", luaItemTypeGetAbilities);

	registerMethod("ItemType", "hasShowAttributes", luaItemTypeHasShowAttributes);
	registerMethod("ItemType", "hasShowCount", luaItemTypeHasShowCount);
	registerMethod("ItemType", "hasShowCharges", luaItemTypeHasShowCharges);
	registerMethod("ItemType", "hasShowDuration", luaItemTypeHasShowDuration);
	registerMethod("ItemType", "hasAllowDistRead", luaItemTypeHasAllowDistRead);
	registerMethod("ItemType", "getWieldInfo", luaItemTypeGetWieldInfo);
	registerMethod("ItemType", "getDuration", luaItemTypeGetDuration);
	registerMethod("ItemType", "getLevelDoor", luaItemTypeGetLevelDoor);
	registerMethod("ItemType", "getRuneSpellName", luaItemTypeGetRuneSpellName);
	registerMethod("ItemType", "getVocationString", luaItemTypeGetVocationString);
	registerMethod("ItemType", "getMinReqLevel", luaItemTypeGetMinReqLevel);
	registerMethod("ItemType", "getMinReqMagicLevel", luaItemTypeGetMinReqMagicLevel);
	registerMethod("ItemType", "getMarketBuyStatistics", luaItemTypeGetMarketBuyStatistics);
	registerMethod("ItemType", "getMarketSellStatistics", luaItemTypeGetMarketSellStatistics);

	registerMethod("ItemType", "hasSubType", luaItemTypeHasSubType);

	registerMethod("ItemType", "isStoreItem", luaItemTypeIsStoreItem);

	// Combat
	registerClass("Combat", "", luaCombatCreate);
	registerMetaMethod("Combat", "__eq", tfs::lua::luaUserdataCompare);
	registerMetaMethod("Combat", "__gc", luaCombatDelete);
	registerMethod("Combat", "delete", luaCombatDelete);

	registerMethod("Combat", "setParameter", luaCombatSetParameter);
	registerMethod("Combat", "getParameter", luaCombatGetParameter);

	registerMethod("Combat", "setFormula", luaCombatSetFormula);

	registerMethod("Combat", "setArea", luaCombatSetArea);
	registerMethod("Combat", "addCondition", luaCombatAddCondition);
	registerMethod("Combat", "clearConditions", luaCombatClearConditions);
	registerMethod("Combat", "setCallback", luaCombatSetCallback);
	registerMethod("Combat", "setOrigin", luaCombatSetOrigin);

	registerMethod("Combat", "execute", luaCombatExecute);

	// Condition
	registerClass("Condition", "", luaConditionCreate);
	registerMetaMethod("Condition", "__eq", tfs::lua::luaUserdataCompare);
	registerMetaMethod("Condition", "__gc", luaConditionDelete);

	registerMethod("Condition", "getId", luaConditionGetId);
	registerMethod("Condition", "getSubId", luaConditionGetSubId);
	registerMethod("Condition", "getType", luaConditionGetType);
	registerMethod("Condition", "getIcons", luaConditionGetIcons);
	registerMethod("Condition", "getEndTime", luaConditionGetEndTime);

	registerMethod("Condition", "clone", luaConditionClone);

	registerMethod("Condition", "getTicks", luaConditionGetTicks);
	registerMethod("Condition", "setTicks", luaConditionSetTicks);

	registerMethod("Condition", "setParameter", luaConditionSetParameter);
	registerMethod("Condition", "getParameter", luaConditionGetParameter);

	registerMethod("Condition", "setFormula", luaConditionSetFormula);
	registerMethod("Condition", "setOutfit", luaConditionSetOutfit);

	registerMethod("Condition", "addDamage", luaConditionAddDamage);

	// Outfit
	registerClass("Outfit", "", luaOutfitCreate);
	registerMetaMethod("Outfit", "__eq", luaOutfitCompare);

	// MonsterType
	registerClass("MonsterType", "", luaMonsterTypeCreate);
	registerMetaMethod("MonsterType", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("MonsterType", "isAttackable", luaMonsterTypeIsAttackable);
	registerMethod("MonsterType", "isChallengeable", luaMonsterTypeIsChallengeable);
	registerMethod("MonsterType", "isConvinceable", luaMonsterTypeIsConvinceable);
	registerMethod("MonsterType", "isSummonable", luaMonsterTypeIsSummonable);
	registerMethod("MonsterType", "isIgnoringSpawnBlock", luaMonsterTypeIsIgnoringSpawnBlock);
	registerMethod("MonsterType", "isIllusionable", luaMonsterTypeIsIllusionable);
	registerMethod("MonsterType", "isHostile", luaMonsterTypeIsHostile);
	registerMethod("MonsterType", "isPushable", luaMonsterTypeIsPushable);
	registerMethod("MonsterType", "isHealthHidden", luaMonsterTypeIsHealthHidden);
	registerMethod("MonsterType", "isBoss", luaMonsterTypeIsBoss);

	registerMethod("MonsterType", "canPushItems", luaMonsterTypeCanPushItems);
	registerMethod("MonsterType", "canPushCreatures", luaMonsterTypeCanPushCreatures);

	registerMethod("MonsterType", "canWalkOnEnergy", luaMonsterTypeCanWalkOnEnergy);
	registerMethod("MonsterType", "canWalkOnFire", luaMonsterTypeCanWalkOnFire);
	registerMethod("MonsterType", "canWalkOnPoison", luaMonsterTypeCanWalkOnPoison);

	registerMethod("MonsterType", "name", luaMonsterTypeName);
	registerMethod("MonsterType", "nameDescription", luaMonsterTypeNameDescription);

	registerMethod("MonsterType", "health", luaMonsterTypeHealth);
	registerMethod("MonsterType", "maxHealth", luaMonsterTypeMaxHealth);
	registerMethod("MonsterType", "runHealth", luaMonsterTypeRunHealth);
	registerMethod("MonsterType", "experience", luaMonsterTypeExperience);
	registerMethod("MonsterType", "skull", luaMonsterTypeSkull);

	registerMethod("MonsterType", "combatImmunities", luaMonsterTypeCombatImmunities);
	registerMethod("MonsterType", "conditionImmunities", luaMonsterTypeConditionImmunities);

	registerMethod("MonsterType", "getAttackList", luaMonsterTypeGetAttackList);
	registerMethod("MonsterType", "addAttack", luaMonsterTypeAddAttack);

	registerMethod("MonsterType", "getDefenseList", luaMonsterTypeGetDefenseList);
	registerMethod("MonsterType", "addDefense", luaMonsterTypeAddDefense);

	registerMethod("MonsterType", "getElementList", luaMonsterTypeGetElementList);
	registerMethod("MonsterType", "addElement", luaMonsterTypeAddElement);

	registerMethod("MonsterType", "getVoices", luaMonsterTypeGetVoices);
	registerMethod("MonsterType", "addVoice", luaMonsterTypeAddVoice);

	registerMethod("MonsterType", "getLoot", luaMonsterTypeGetLoot);
	registerMethod("MonsterType", "addLoot", luaMonsterTypeAddLoot);

	registerMethod("MonsterType", "getCreatureEvents", luaMonsterTypeGetCreatureEvents);
	registerMethod("MonsterType", "registerEvent", luaMonsterTypeRegisterEvent);

	registerMethod("MonsterType", "eventType", luaMonsterTypeEventType);
	registerMethod("MonsterType", "onThink", luaMonsterTypeEventOnCallback);
	registerMethod("MonsterType", "onAppear", luaMonsterTypeEventOnCallback);
	registerMethod("MonsterType", "onDisappear", luaMonsterTypeEventOnCallback);
	registerMethod("MonsterType", "onMove", luaMonsterTypeEventOnCallback);
	registerMethod("MonsterType", "onSay", luaMonsterTypeEventOnCallback);

	registerMethod("MonsterType", "getSummonList", luaMonsterTypeGetSummonList);
	registerMethod("MonsterType", "addSummon", luaMonsterTypeAddSummon);

	registerMethod("MonsterType", "maxSummons", luaMonsterTypeMaxSummons);

	registerMethod("MonsterType", "armor", luaMonsterTypeArmor);
	registerMethod("MonsterType", "defense", luaMonsterTypeDefense);
	registerMethod("MonsterType", "outfit", luaMonsterTypeOutfit);
	registerMethod("MonsterType", "race", luaMonsterTypeRace);
	registerMethod("MonsterType", "corpseId", luaMonsterTypeCorpseId);
	registerMethod("MonsterType", "manaCost", luaMonsterTypeManaCost);
	registerMethod("MonsterType", "baseSpeed", luaMonsterTypeBaseSpeed);
	registerMethod("MonsterType", "light", luaMonsterTypeLight);

	registerMethod("MonsterType", "staticAttackChance", luaMonsterTypeStaticAttackChance);
	registerMethod("MonsterType", "targetDistance", luaMonsterTypeTargetDistance);
	registerMethod("MonsterType", "yellChance", luaMonsterTypeYellChance);
	registerMethod("MonsterType", "yellSpeedTicks", luaMonsterTypeYellSpeedTicks);
	registerMethod("MonsterType", "changeTargetChance", luaMonsterTypeChangeTargetChance);
	registerMethod("MonsterType", "changeTargetSpeed", luaMonsterTypeChangeTargetSpeed);

	// Loot
	registerClass("Loot", "", luaCreateLoot);
	registerMetaMethod("Loot", "__gc", luaDeleteLoot);
	registerMethod("Loot", "delete", luaDeleteLoot);

	registerMethod("Loot", "setId", luaLootSetId);
	registerMethod("Loot", "setMaxCount", luaLootSetMaxCount);
	registerMethod("Loot", "setSubType", luaLootSetSubType);
	registerMethod("Loot", "setChance", luaLootSetChance);
	registerMethod("Loot", "setActionId", luaLootSetActionId);
	registerMethod("Loot", "setDescription", luaLootSetDescription);
	registerMethod("Loot", "addChildLoot", luaLootAddChildLoot);

	// MonsterSpell
	registerClass("MonsterSpell", "", luaCreateMonsterSpell);
	registerMetaMethod("MonsterSpell", "__gc", luaDeleteMonsterSpell);
	registerMethod("MonsterSpell", "delete", luaDeleteMonsterSpell);

	registerMethod("MonsterSpell", "setType", luaMonsterSpellSetType);
	registerMethod("MonsterSpell", "setScriptName", luaMonsterSpellSetScriptName);
	registerMethod("MonsterSpell", "setChance", luaMonsterSpellSetChance);
	registerMethod("MonsterSpell", "setInterval", luaMonsterSpellSetInterval);
	registerMethod("MonsterSpell", "setRange", luaMonsterSpellSetRange);
	registerMethod("MonsterSpell", "setCombatValue", luaMonsterSpellSetCombatValue);
	registerMethod("MonsterSpell", "setCombatType", luaMonsterSpellSetCombatType);
	registerMethod("MonsterSpell", "setAttackValue", luaMonsterSpellSetAttackValue);
	registerMethod("MonsterSpell", "setNeedTarget", luaMonsterSpellSetNeedTarget);
	registerMethod("MonsterSpell", "setNeedDirection", luaMonsterSpellSetNeedDirection);
	registerMethod("MonsterSpell", "setCombatLength", luaMonsterSpellSetCombatLength);
	registerMethod("MonsterSpell", "setCombatSpread", luaMonsterSpellSetCombatSpread);
	registerMethod("MonsterSpell", "setCombatRadius", luaMonsterSpellSetCombatRadius);
	registerMethod("MonsterSpell", "setCombatRing", luaMonsterSpellSetCombatRing);
	registerMethod("MonsterSpell", "setConditionType", luaMonsterSpellSetConditionType);
	registerMethod("MonsterSpell", "setConditionDamage", luaMonsterSpellSetConditionDamage);
	registerMethod("MonsterSpell", "setConditionSpeedChange", luaMonsterSpellSetConditionSpeedChange);
	registerMethod("MonsterSpell", "setConditionDuration", luaMonsterSpellSetConditionDuration);
	registerMethod("MonsterSpell", "setConditionDrunkenness", luaMonsterSpellSetConditionDrunkenness);
	registerMethod("MonsterSpell", "setConditionTickInterval", luaMonsterSpellSetConditionTickInterval);
	registerMethod("MonsterSpell", "setCombatShootEffect", luaMonsterSpellSetCombatShootEffect);
	registerMethod("MonsterSpell", "setCombatEffect", luaMonsterSpellSetCombatEffect);
	registerMethod("MonsterSpell", "setOutfit", luaMonsterSpellSetOutfit);

	// Party
	registerClass("Party", "", luaPartyCreate);
	registerMetaMethod("Party", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Party", "disband", luaPartyDisband);

	registerMethod("Party", "getLeader", luaPartyGetLeader);
	registerMethod("Party", "setLeader", luaPartySetLeader);

	registerMethod("Party", "getMembers", luaPartyGetMembers);
	registerMethod("Party", "getMemberCount", luaPartyGetMemberCount);

	registerMethod("Party", "getInvitees", luaPartyGetInvitees);
	registerMethod("Party", "getInviteeCount", luaPartyGetInviteeCount);

	registerMethod("Party", "addInvite", luaPartyAddInvite);
	registerMethod("Party", "removeInvite", luaPartyRemoveInvite);

	registerMethod("Party", "addMember", luaPartyAddMember);
	registerMethod("Party", "removeMember", luaPartyRemoveMember);

	registerMethod("Party", "isSharedExperienceActive", luaPartyIsSharedExperienceActive);
	registerMethod("Party", "isSharedExperienceEnabled", luaPartyIsSharedExperienceEnabled);
	registerMethod("Party", "shareExperience", luaPartyShareExperience);
	registerMethod("Party", "setSharedExperience", luaPartySetSharedExperience);

	// Spells
	registerClass("Spell", "", luaSpellCreate);
	registerMetaMethod("Spell", "__eq", tfs::lua::luaUserdataCompare);

	registerMethod("Spell", "onCastSpell", luaSpellOnCastSpell);
	registerMethod("Spell", "register", luaSpellRegister);
	registerMethod("Spell", "name", luaSpellName);
	registerMethod("Spell", "id", luaSpellId);
	registerMethod("Spell", "group", luaSpellGroup);
	registerMethod("Spell", "cooldown", luaSpellCooldown);
	registerMethod("Spell", "groupCooldown", luaSpellGroupCooldown);
	registerMethod("Spell", "level", luaSpellLevel);
	registerMethod("Spell", "magicLevel", luaSpellMagicLevel);
	registerMethod("Spell", "mana", luaSpellMana);
	registerMethod("Spell", "manaPercent", luaSpellManaPercent);
	registerMethod("Spell", "soul", luaSpellSoul);
	registerMethod("Spell", "range", luaSpellRange);
	registerMethod("Spell", "isPremium", luaSpellPremium);
	registerMethod("Spell", "isEnabled", luaSpellEnabled);
	registerMethod("Spell", "needTarget", luaSpellNeedTarget);
	registerMethod("Spell", "needWeapon", luaSpellNeedWeapon);
	registerMethod("Spell", "needLearn", luaSpellNeedLearn);
	registerMethod("Spell", "isSelfTarget", luaSpellSelfTarget);
	registerMethod("Spell", "isBlocking", luaSpellBlocking);
	registerMethod("Spell", "isAggressive", luaSpellAggressive);
	registerMethod("Spell", "isPzLock", luaSpellPzLock);
	registerMethod("Spell", "vocation", luaSpellVocation);

	// only for InstantSpell
	registerMethod("Spell", "words", luaSpellWords);
	registerMethod("Spell", "needDirection", luaSpellNeedDirection);
	registerMethod("Spell", "hasParams", luaSpellHasParams);
	registerMethod("Spell", "hasPlayerNameParam", luaSpellHasPlayerNameParam);
	registerMethod("Spell", "needCasterTargetOrDirection", luaSpellNeedCasterTargetOrDirection);
	registerMethod("Spell", "isBlockingWalls", luaSpellIsBlockingWalls);

	// only for RuneSpells
	registerMethod("Spell", "runeLevel", luaSpellRuneLevel);
	registerMethod("Spell", "runeMagicLevel", luaSpellRuneMagicLevel);
	registerMethod("Spell", "runeId", luaSpellRuneId);
	registerMethod("Spell", "charges", luaSpellCharges);
	registerMethod("Spell", "allowFarUse", luaSpellAllowFarUse);
	registerMethod("Spell", "blockWalls", luaSpellBlockWalls);
	registerMethod("Spell", "checkFloor", luaSpellCheckFloor);

	// Action
	registerClass("Action", "", luaCreateAction);
	registerMethod("Action", "onUse", luaActionOnUse);
	registerMethod("Action", "register", luaActionRegister);
	registerMethod("Action", "id", luaActionItemId);
	registerMethod("Action", "aid", luaActionActionId);
	registerMethod("Action", "uid", luaActionUniqueId);
	registerMethod("Action", "allowFarUse", luaActionAllowFarUse);
	registerMethod("Action", "blockWalls", luaActionBlockWalls);
	registerMethod("Action", "checkFloor", luaActionCheckFloor);

	// TalkAction

	// MoveEvent

	tfs::lua::importModules(*this);
}

#undef registerEnum
#undef registerEnumIn

//
LuaEnvironment::LuaEnvironment() : LuaScriptInterface("Main Interface") {}

LuaEnvironment::~LuaEnvironment()
{
	delete testInterface;
	closeState();
}

bool LuaEnvironment::initState()
{
	luaState = luaL_newstate();
	if (!luaState) {
		return false;
	}

	luaL_openlibs(luaState);
	registerFunctions();

	runningEventId = EVENT_ID_USER;
	return true;
}

bool LuaEnvironment::reInitState()
{
	// TODO: get children, reload children
	closeState();
	return initState();
}

bool LuaEnvironment::closeState()
{
	if (!luaState) {
		return false;
	}

	for (const auto& combatEntry : combatIdMap) {
		clearCombatObjects(combatEntry.first);
	}

	for (const auto& areaEntry : areaIdMap) {
		clearAreaObjects(areaEntry.first);
	}

	for (auto& timerEntry : timerEvents) {
		LuaTimerEventDesc timerEventDesc = std::move(timerEntry.second);
		for (int32_t parameter : timerEventDesc.parameters) {
			luaL_unref(luaState, LUA_REGISTRYINDEX, parameter);
		}
		luaL_unref(luaState, LUA_REGISTRYINDEX, timerEventDesc.function);
	}

	combatIdMap.clear();
	areaIdMap.clear();
	timerEvents.clear();
	cacheFiles.clear();

	lua_close(luaState);
	luaState = nullptr;
	return true;
}

LuaScriptInterface* LuaEnvironment::getTestInterface()
{
	if (!testInterface) {
		testInterface = new LuaScriptInterface("Test Interface");
		testInterface->initState();
	}
	return testInterface;
}

Combat_ptr LuaEnvironment::getCombatObject(uint32_t id) const
{
	auto it = combatMap.find(id);
	if (it == combatMap.end()) {
		return nullptr;
	}
	return it->second;
}

Combat_ptr LuaEnvironment::createCombatObject(LuaScriptInterface* interface)
{
	Combat_ptr combat = std::make_shared<Combat>();
	combatMap[++lastCombatId] = combat;
	combatIdMap[interface].push_back(lastCombatId);
	return combat;
}

void LuaEnvironment::clearCombatObjects(LuaScriptInterface* interface)
{
	auto it = combatIdMap.find(interface);
	if (it == combatIdMap.end()) {
		return;
	}

	for (uint32_t id : it->second) {
		auto itt = combatMap.find(id);
		if (itt != combatMap.end()) {
			combatMap.erase(itt);
		}
	}
	it->second.clear();
}

AreaCombat* LuaEnvironment::getAreaObject(uint32_t id) const
{
	auto it = areaMap.find(id);
	if (it == areaMap.end()) {
		return nullptr;
	}
	return it->second;
}

uint32_t LuaEnvironment::createAreaObject(LuaScriptInterface* interface)
{
	areaMap[++lastAreaId] = new AreaCombat;
	areaIdMap[interface].push_back(lastAreaId);
	return lastAreaId;
}

void LuaEnvironment::clearAreaObjects(LuaScriptInterface* interface)
{
	auto it = areaIdMap.find(interface);
	if (it == areaIdMap.end()) {
		return;
	}

	for (uint32_t id : it->second) {
		auto itt = areaMap.find(id);
		if (itt != areaMap.end()) {
			delete itt->second;
			areaMap.erase(itt);
		}
	}
	it->second.clear();
}

void LuaEnvironment::executeTimerEvent(uint32_t eventIndex)
{
	auto it = timerEvents.find(eventIndex);
	if (it == timerEvents.end()) {
		return;
	}

	LuaTimerEventDesc timerEventDesc = std::move(it->second);
	timerEvents.erase(it);

	// push function
	lua_rawgeti(luaState, LUA_REGISTRYINDEX, timerEventDesc.function);

	// push parameters
	for (auto parameter : boost::adaptors::reverse(timerEventDesc.parameters)) {
		lua_rawgeti(luaState, LUA_REGISTRYINDEX, parameter);
	}

	// call the function
	if (tfs::lua::reserveScriptEnv()) {
		tfs::lua::ScriptEnvironment* env = tfs::lua::getScriptEnv();
		env->setTimerEvent();
		env->setScriptId(timerEventDesc.scriptId, this);
		callFunction(timerEventDesc.parameters.size());
	} else {
		std::cout << "[Error - LuaScriptInterface::executeTimerEvent] Call stack overflow" << std::endl;
	}

	// free resources
	luaL_unref(luaState, LUA_REGISTRYINDEX, timerEventDesc.function);
	for (auto parameter : timerEventDesc.parameters) {
		luaL_unref(luaState, LUA_REGISTRYINDEX, parameter);
	}
}
