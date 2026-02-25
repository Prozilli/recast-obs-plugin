cmake_minimum_required(VERSION 3.28)

project(recast-obs-plugin VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find OBS SDK ---
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

qt6_standard_project_setup()

# --- Plugin target ---
add_library(recast-obs-plugin MODULE
	src/plugin-main.c
	src/recast-output.c
	src/recast-service.c
	src/recast-config.c
	src/recast-dock.cpp
)

target_include_directories(recast-obs-plugin PRIVATE src)

target_link_libraries(recast-obs-plugin PRIVATE
	OBS::libobs
	OBS::obs-frontend-api
	Qt6::Widgets
	Qt6::Network
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
if(COMMAND install_obs_plugin_with_data)
	install_obs_plugin_with_data(recast-obs-plugin data)
else()
	# Fallback install rules
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
endif()
