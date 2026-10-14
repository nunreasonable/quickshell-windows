qt_add_executable(quickshell main.cpp)

if (WIN32)
	set_target_properties(quickshell PROPERTIES OUTPUT_NAME qs)

	qt_add_executable(quickshell-gui main.cpp)
	set_target_properties(quickshell-gui PROPERTIES OUTPUT_NAME qsw WIN32_EXECUTABLE ON)
	target_compile_definitions(quickshell-gui PRIVATE QS_WINDOWS_GUI_EXE)
	target_sources(quickshell PRIVATE quickshell.rc)
	target_sources(quickshell-gui PRIVATE quickshell.rc)

	install(TARGETS quickshell quickshell-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
	install(TARGETS quickshell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

add_subdirectory(build)
add_subdirectory(launch)
add_subdirectory(core)
add_subdirectory(debug)
add_subdirectory(ipc)
add_subdirectory(window)
add_subdirectory(io)
add_subdirectory(widgets)
add_subdirectory(ui)
add_subdirectory(windowmanager)

if (CRASH_HANDLER AND NOT WIN32)
	add_subdirectory(crash)
endif()

if (DBUS)
	add_subdirectory(dbus)
endif()

if (WAYLAND)
	add_subdirectory(wayland)
endif()

if (X11)
	add_subdirectory(x11)
endif()

if (I3)
	add_subdirectory(x11/i3)
endif()

if (WINDOWS_BACKEND)
	add_subdirectory(windows)
endif()

add_subdirectory(services)

if (BLUETOOTH)
	add_subdirectory(bluetooth)
endif()

if (NETWORK)
	add_subdirectory(network)
endif()

if (WIN32)
	get_target_property(QS_EXE_LIBRARIES quickshell LINK_LIBRARIES)
	target_link_libraries(quickshell-gui PRIVATE ${QS_EXE_LIBRARIES})
endif()
