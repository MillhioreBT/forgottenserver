#include "../otpch.h"

#include "luaregister.h"

namespace tfs::lua {

using ModuleInit = std::function<void(LuaScriptInterface&)>;

static auto& getModules()
{
	static std::vector<std::pair<std::string_view, ModuleInit>> modules;
	return modules;
};

void registerModule(std::string_view moduleName, ModuleInit init) { getModules().emplace_back(moduleName, init); }

void importModules(LuaScriptInterface& lsi)
{
	for (auto [moduleName, init] : getModules()) {
		init(lsi);
	}
}

} // namespace tfs::lua
