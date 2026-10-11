cmake_minimum_required(VERSION 3.16)
project(kfilefilter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

# Plain Qt on purpose: the KDE classes are reached through QMetaObject
# (inherits(), string signals), so no KIO symbol is linked and a KIO update
# cannot break the ABI this library depends on.
add_library(kfilefilter SHARED src/kfilefilter.cpp)
target_link_libraries(kfilefilter PRIVATE Qt6::Widgets)
set_target_properties(kfilefilter PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)

option(BUILD_SMOKE_TEST "Build the KFileWidget smoke test (needs KF6 KIO headers)" OFF)
if(BUILD_SMOKE_TEST)
    find_package(KF6KIO REQUIRED)
    add_executable(smoke tests/smoke.cpp)
    target_link_libraries(smoke PRIVATE Qt6::Widgets KF6::KIOFileWidgets)
endif()
