cmake_minimum_required(VERSION 3.28)

project(recast-obs-plugin VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- OBS SDK ---
# OBS_SDK_DIR should point to the extracted OBS source (for headers).
# OBS_LIB_DIR should point to the directory containing obs.lib and
# obs-frontend-api.lib (generated from installed OBS DLLs).
if(NOT OBS_SDK_DIR)
	set(OBS_SDK_DIR "$ENV{OBS_SDK_DIR}" CACHE PATH "Path to OBS source tree")
endif()
if(NOT OBS_LIB_DIR)
	set(OBS_LIB_DIR "$ENV{OBS_LIB_DIR}" CACHE PATH "Path to obs.lib / obs-frontend-api.lib")
endif()

# --- Find Qt6 ---
find_package(Qt6 REQUIRED COMPONENTS Widgets Network WebSockets)
qt6_standard_project_setup()

# --- Create imported targets for OBS ---
add_library(OBS::libobs STATIC IMPORTED)
set_target_properties(OBS::libobs PROPERTIES
	IMPORTED_LOCATION "${OBS_LIB_DIR}/obs.lib"
	INTERFACE_INCLUDE_DIRECTORIES "${OBS_SDK_DIR}/libobs"
)

add_library(OBS::obs-frontend-api STATIC IMPORTED)
set_target_properties(OBS::obs-frontend-api PROPERTIES
	IMPORTED_LOCATION "${OBS_LIB_DIR}/obs-frontend-api.lib"
	INTERFACE_INCLUDE_DIRECTORIES "${OBS_SDK_DIR}/frontend/api"
)

# --- Plugin target ---
add_library(recast-obs-plugin MODULE
	# Core C files
	src/plugin-main.c
	src/recast-output.c
	src/recast-protocol.c
	src/recast-config.c
	src/recast-scene-model.c

	# Shared C++ widgets (kept from v2)
	src/recast-platform-icons.cpp
	src/recast-source-tree.cpp

	# New: extracted preview widget
	src/recast-preview-widget.cpp

	# New: vertical canvas singleton
	src/recast-vertical.cpp

	# New: always-present vertical docks
	src/recast-vertical-preview.cpp
	src/recast-vertical-scenes.cpp
	src/recast-vertical-sources.cpp

	# New: multistream dock
	src/recast-multistream.cpp

	# New: platform auth, chat, and events
	src/recast-auth.cpp
	src/recast-chat.cpp
	src/recast-events.cpp

	# New: top-level UI setup
	src/recast-ui.cpp
)

target_include_directories(recast-obs-plugin PRIVATE src)

target_link_libraries(recast-obs-plugin PRIVATE
	OBS::libobs
	OBS::obs-frontend-api
	Qt6::Widgets
	Qt6::Network
	Qt6::WebSockets
)

# Qt MOC for C++ headers with Q_OBJECT
set_target_properties(recast-obs-plugin PROPERTIES
	AUTOMOC ON
	AUTOUIC ON
	AUTORCC ON
	PREFIX ""
)

# --- Platform-specific settings ---
if(WIN32)
	set_target_properties(recast-obs-plugin PROPERTIES
		SUFFIX ".dll"
	)
elseif(APPLE)
	set_target_properties(recast-obs-plugin PROPERTIES
		SUFFIX ".dylib"
	)
else()
	set_target_properties(recast-obs-plugin PROPERTIES
		SUFFIX ".so"
	)
endif()

# --- Install ---
if(WIN32)
	set(_plugin_dest "obs-plugins/64bit")
	set(_data_dest "data/obs-plugins/recast-obs-plugin")
elseif(APPLE)
	set(_plugin_dest "obs-plugins")
	set(_data_dest "data/obs-plugins/recast-obs-plugin")
else()
	set(_plugin_dest "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
	set(_data_dest "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/recast-obs-plugin")
endif()

install(TARGETS recast-obs-plugin DESTINATION "${_plugin_dest}")
install(DIRECTORY data/ DESTINATION "${_data_dest}")
