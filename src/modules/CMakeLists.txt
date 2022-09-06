set(tfs_modules_SRC
	${CMAKE_CURRENT_LIST_DIR}/creatureevent.cpp
	${CMAKE_CURRENT_LIST_DIR}/globalevent.cpp
	${CMAKE_CURRENT_LIST_DIR}/luaregister.cpp
	${CMAKE_CURRENT_LIST_DIR}/moveevent.cpp
	${CMAKE_CURRENT_LIST_DIR}/player.cpp
	${CMAKE_CURRENT_LIST_DIR}/talkaction.cpp
	${CMAKE_CURRENT_LIST_DIR}/tile.cpp
	${CMAKE_CURRENT_LIST_DIR}/weapons.cpp
	${CMAKE_CURRENT_LIST_DIR}/xml.cpp
	PARENT_SCOPE)

set(tfs_modules_HDR
	${CMAKE_CURRENT_LIST_DIR}/luaregister.h
	PARENT_SCOPE)
