#include "../otpch.h"

#include "luaregister.h"

namespace tfs::lua {

using ModuleInit = std::function<void(LuaScriptInterface&)>;

static auto& getModules()
{
	static std::vector<std::pair<std::string_view, ModuleInit>> modules;
	return modules;
};

void registerModule(std::string_view moduleName, ModuleInit init, const std::vector<std::string_view>& dependencies)
{
	auto& modules = getModules();

	auto hint = modules.begin();
	for (auto depName : dependencies) {
		if (auto it = find_if(hint, modules.end(), [=](auto module) { return module.first == depName; });
		    it != modules.end()) {
			hint = next(it);
		}
	}

	getModules().emplace(hint, moduleName, init);
}

void importModules(LuaScriptInterface& lsi)
{
	for (auto [moduleName, init] : getModules()) {
		init(lsi);
	}
}

} // namespace tfs::lua
