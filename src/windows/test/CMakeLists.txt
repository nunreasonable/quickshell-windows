cmake_minimum_required(VERSION 3.20)
project(qs_windows_panel_animation_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Quick Qml Test)
qt_standard_project_setup(REQUIRES 6.11)

set(QS_WINDOWS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")

qt_add_executable(panel_animation_test
	panel_animation_test.cpp
	${QS_WINDOWS_SRC}/panel_animation.cpp
	${QS_WINDOWS_SRC}/layer_rules.cpp
)

target_include_directories(panel_animation_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(panel_animation_test PRIVATE Qt6::Quick Qt6::Qml Qt6::Test)
