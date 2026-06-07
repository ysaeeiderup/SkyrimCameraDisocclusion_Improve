cmake_minimum_required(VERSION 3.21)

# Local VS Code build environment adaptation entrypoint.
project(skyrimcameradisocclusion VERSION 0.1.0 LANGUAGES CXX)

if(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_FOLDER}/Data")
endif()

if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}")
endif()

set(PLUGIN_SOURCES
    src/main.cpp
    src/Hook.cpp
    src/Settings.cpp
)

option(USE_LOCAL_COMMONLIBSSE "Build against local CommonLibSSE checkout" ON)
set(LOCAL_COMMONLIBSSE_DIR "${CMAKE_CURRENT_LIST_DIR}/../CommonLibSSE" CACHE PATH "Path to CommonLibSSE checkout")

if(USE_LOCAL_COMMONLIBSSE AND EXISTS "${LOCAL_COMMONLIBSSE_DIR}/CMakeLists.txt")
    set(SKYRIM_SUPPORT_AE ON CACHE BOOL "Enable AE support in local CommonLibSSE" FORCE)
    set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "Enable Xbyak trampoline support in local CommonLibSSE" FORCE)
    add_subdirectory("${LOCAL_COMMONLIBSSE_DIR}" CommonLibSSE EXCLUDE_FROM_ALL)

    add_library(${PROJECT_NAME} SHARED ${PLUGIN_SOURCES})
    target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MANUAL_SKSE_PLUGIN_METADATA=1 SKYRIM_SUPPORT_AE=1 COMMONLIBSSE_LEGACY_API=1)
    message(STATUS "Using local CommonLibSSE checkout: ${LOCAL_COMMONLIBSSE_DIR}")
else()
    find_package(CommonLibSSE CONFIG REQUIRED)
    add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${PLUGIN_SOURCES})
    message(STATUS "Using package-managed CommonLibSSE")
endif()

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(DEFINED OUTPUT_FOLDER)
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")

    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )

    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/dist/SKSE/Plugins/skyrimcameradisocclusion.ini")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CMAKE_CURRENT_LIST_DIR}/dist/SKSE/Plugins/skyrimcameradisocclusion.ini"
                    "${DLL_FOLDER}/skyrimcameradisocclusion.ini"
            VERBATIM
        )
    endif()

endif()
