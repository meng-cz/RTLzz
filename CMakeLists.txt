cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()

project(predicate-expand LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PREDICATE_EXPAND_BUILD_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        RESULT_VARIABLE _GPEF_GIT_RESULT
        OUTPUT_VARIABLE _GPEF_GIT_COMMIT
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_GPEF_GIT_RESULT EQUAL 0 AND NOT _GPEF_GIT_COMMIT STREQUAL "")
        set(PREDICATE_EXPAND_BUILD_COMMIT "${_GPEF_GIT_COMMIT}")
    endif()
endif()
message(STATUS "Predicate expand build commit: ${PREDICATE_EXPAND_BUILD_COMMIT}")

set(_LLVM18_ROOT_HINTS
    "$ENV{LLVM18_ROOT}"
    "$ENV{LLVM_ROOT}"
    "${LLVM18_ROOT}"
    "${LLVM_ROOT}"
    "/usr/lib/llvm-18"
    "/opt/homebrew/opt/llvm@18"
    "/opt/homebrew/opt/llvm"
    "C:/Program Files/LLVM"
)

find_package(LLVM REQUIRED CONFIG
    HINTS ${_LLVM18_ROOT_HINTS}
    PATH_SUFFIXES lib/cmake/llvm cmake
)
find_package(Clang REQUIRED CONFIG
    HINTS ${_LLVM18_ROOT_HINTS}
    PATH_SUFFIXES lib/cmake/clang cmake
)

if(NOT LLVM_VERSION_MAJOR EQUAL 18)
    message(FATAL_ERROR
        "RTLzz requires LLVM/Clang 18.x for the Clang C++ API frontend, "
        "but CMake found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}."
    )
endif()

if(NOT TARGET libclang)
    message(FATAL_ERROR
        "Clang package at ${Clang_DIR} does not export the libclang target. "
        "Install the LLVM/Clang 18 development packages."
    )
endif()

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}: ${LLVM_DIR}")
message(STATUS "Found Clang package: ${Clang_DIR}")

if(WIN32)
    find_file(LIBCLANG_RUNTIME
        NAMES libclang.dll clang.dll
        HINTS ${_LLVM18_ROOT_HINTS}
        PATH_SUFFIXES bin
        DOC "libclang runtime DLL"
    )
endif()
if(WIN32)
    if(LIBCLANG_RUNTIME)
        message(STATUS "Found libclang runtime: ${LIBCLANG_RUNTIME}")
    else()
        message(WARNING "Could not find libclang.dll. Add the LLVM bin directory to PATH before running predicate-expand.")
    endif()
endif()

file(GLOB_RECURSE PREDICATE_EXPAND_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
)

add_library(predicate-expand-objects OBJECT ${PREDICATE_EXPAND_SOURCES})
add_library(predicate-expand::objects ALIAS predicate-expand-objects)

target_include_directories(predicate-expand-objects PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

target_link_libraries(predicate-expand-objects PUBLIC
    libclang
    clangTooling
    clangFrontend
    clangAST
    clangBasic
)

target_compile_definitions(predicate-expand-objects PRIVATE
    PREDICATE_EXPAND_BUILD_COMMIT="${PREDICATE_EXPAND_BUILD_COMMIT}"
)

if(MSVC)
    set_property(TARGET predicate-expand-objects PROPERTY VS_GLOBAL_TrackFileAccess false)
    set_property(TARGET predicate-expand-objects PROPERTY
        MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>"
    )
endif()

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    function(configure_debug_executable target_name)
        if(MSVC)
            set_property(TARGET ${target_name} PROPERTY VS_GLOBAL_TrackFileAccess false)
            set_property(TARGET ${target_name} PROPERTY
                MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>"
            )
        endif()
    endfunction()

    function(configure_testv2_executable target_name)
        target_link_libraries(${target_name} PRIVATE predicate-expand-objects)
        set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/testv2"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/testv2"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/testv2"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/testv2"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/testv2"
        )
        configure_debug_executable(${target_name})
    endfunction()

    add_executable(predicate-expand main/main.cpp)
    target_link_libraries(predicate-expand PRIVATE predicate-expand-objects)
    configure_debug_executable(predicate-expand)

    add_custom_command(TARGET predicate-expand POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/third_party/vulsim/vullib"
            "$<TARGET_FILE_DIR:predicate-expand>/vullib"
        COMMENT "Copying default vullib directory"
        VERBATIM
    )

    file(GLOB TESTV2_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/testv2/*.cpp"
        "${PROJECT_SOURCE_DIR}/testv2/clang18/*.cpp"
    )
    foreach(test_source IN LISTS TESTV2_SOURCES)
        get_filename_component(test_stem "${test_source}" NAME_WE)
        string(REPLACE "_" "-" test_target "${test_stem}")
        add_executable(${test_target} "${test_source}")
        configure_testv2_executable(${test_target})
    endforeach()

    if(WIN32 AND LIBCLANG_RUNTIME)
        add_custom_command(TARGET predicate-expand POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${LIBCLANG_RUNTIME}
                $<TARGET_FILE_DIR:predicate-expand>
            COMMENT "Copying libclang runtime DLL"
            VERBATIM
        )
    endif()
endif()
