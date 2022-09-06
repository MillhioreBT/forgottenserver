#ifndef TFS_LUAREGISTER
#define TFS_LUAREGISTER

class LuaScriptInterface;

namespace tfs::lua {

void registerModule(std::function<void(LuaScriptInterface&)> init);
void importModules(LuaScriptInterface& lsi);

} // namespace tfs::lua

#define registerLuaModule(init) \
	static auto __module__ = ([]() { \
		tfs::lua::registerModule(init); \
		return nullptr; \
	})();

#endif
