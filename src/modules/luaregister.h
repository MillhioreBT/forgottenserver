#ifndef TFS_LUAREGISTER
#define TFS_LUAREGISTER

class LuaScriptInterface;

namespace tfs::lua {

void registerModule(std::string_view moduleName, std::function<void(LuaScriptInterface&)> init);
void importModules(LuaScriptInterface& lsi);

} // namespace tfs::lua

#define registerEnum(lsi, value) \
	{ \
		std::string_view enumName = #value; \
		lsi.registerGlobalVariable(enumName.substr(enumName.find_last_of(':') + 1), value); \
	}

#define registerLuaModule(moduleName, init) \
	static auto __module__ = ([]() { \
		tfs::lua::registerModule(moduleName, init); \
		return nullptr; \
	})();

#endif
