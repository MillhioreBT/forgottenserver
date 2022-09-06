#include "../otpch.h"

#include "luaregister.h"

namespace tfs::lua {

using ModuleInit = std::function<void(LuaScriptInterface&)>;

static std::vector<ModuleInit> modules = {};

void registerModule(ModuleInit init) { modules.push_back(init); }

void importModules(LuaScriptInterface& lsi)
{
	for (auto module : modules) {
		module(lsi);
	}
}

} // namespace tfs::lua
