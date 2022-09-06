#ifndef TFS_LUAREGISTER
#define TFS_LUAREGISTER

class LuaScriptInterface;

namespace tfs::lua {

void registerModule(std::string_view moduleName, std::function<void(LuaScriptInterface&)> init);
void importModules(LuaScriptInterface& lsi);

} // namespace tfs::lua

#define registerLuaModule(moduleName, init) \
	static auto __module__ = ([]() { \
		tfs::lua::registerModule(moduleName, init); \
		return nullptr; \
	})();

#endif
