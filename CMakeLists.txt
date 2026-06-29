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

set(_LLVM_ROOT_HINTS
    "$ENV{LLVM_ROOT}"
    "$ENV{LLVM_DIR}/../../.."
    "${LLVM_ROOT}"
    "${LLVM_DIR}/../../.."
    "/usr/lib/llvm-18"
    "/usr/lib/llvm-17"
    "/usr/lib/llvm-16"
    "/usr/lib/llvm-15"
    "/usr/lib/llvm-14"
    "/usr/lib/llvm-13"
    "/opt/homebrew/opt/llvm"
    "/usr/local/opt/llvm"
    "C:/Program Files/LLVM"
)

find_path(LIBCLANG_INCLUDE_DIR
    NAMES clang-c/Index.h
    HINTS ${_LLVM_ROOT_HINTS}
    PATH_SUFFIXES include
    DOC "Directory containing clang-c/Index.h"
)

find_library(LIBCLANG_LIBRARY
    NAMES clang libclang
    HINTS ${_LLVM_ROOT_HINTS}
    PATH_SUFFIXES lib lib64
    DOC "libclang C API library"
)

if(WIN32)
    find_file(LIBCLANG_RUNTIME
        NAMES libclang.dll clang.dll
        HINTS ${_LLVM_ROOT_HINTS}
        PATH_SUFFIXES bin
        DOC "libclang runtime DLL"
    )
endif()

if(NOT LIBCLANG_INCLUDE_DIR OR NOT LIBCLANG_LIBRARY)
    message(FATAL_ERROR
        "libclang was not found. Install LLVM/Clang development files, or pass "
        "-DLIBCLANG_INCLUDE_DIR=/path/to/include and "
        "-DLIBCLANG_LIBRARY=/path/to/libclang."
    )
endif()

message(STATUS "Found libclang include dir: ${LIBCLANG_INCLUDE_DIR}")
message(STATUS "Found libclang library: ${LIBCLANG_LIBRARY}")
if(WIN32)
    if(LIBCLANG_RUNTIME)
        message(STATUS "Found libclang runtime: ${LIBCLANG_RUNTIME}")
    else()
        message(WARNING "Could not find libclang.dll. Add the LLVM bin directory to PATH before running predicate-expand.")
    endif()
endif()

add_executable(predicate-expand
    src/main.cpp
    src/ast/ASTBuilderDriver.cpp
    src/ast/ASTBuilder.cpp
    src/ast/VulOpRecognizer.cpp
    src/ast/VulTypeRecognizer.cpp
    src/eval/PredicateEvaluator.cpp
    src/ir/CanonicalIR.cpp
    src/ir/CFG.cpp
    src/ir/SSA.cpp
    src/ir/TypedDAG.cpp
    src/semantics/AliasGraph.cpp
    src/semantics/IntSemantics.cpp
    src/transform/LoopUnroll.cpp
    src/transform/Normalize.cpp
    src/transform/Predicate.cpp
    src/normalize/NormalizeDriver.cpp
    src/normalize/AliasLowering.cpp
    src/normalize/FunctionInline.cpp
    src/normalize/LambdaInline.cpp
    src/normalize/ArrayScalarize.cpp
    src/normalize/StructFlatten.cpp
    src/normalize/IntOpLowering.cpp
    src/normalize/LookupLowering.cpp
    src/normalize/AssignmentLowering.cpp
    src/normalize/DefaultTotalizationPass.cpp
    src/predicate/DefaultTotalizationPass.cpp
    src/predicate/OutputExpressionMap.cpp
    src/predicate/PredicateVerifier.cpp
    src/emitter/TextEmitter.cpp
    src/emitter/JsonEmitter.cpp
    src/emitter/SmtEmitter.cpp
)

target_include_directories(predicate-expand PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${LIBCLANG_INCLUDE_DIR}
)

target_link_libraries(predicate-expand PRIVATE ${LIBCLANG_LIBRARY})

target_compile_definitions(predicate-expand PRIVATE
    PREDICATE_EXPAND_BUILD_COMMIT="${PREDICATE_EXPAND_BUILD_COMMIT}"
)

if(MSVC)
    set_property(TARGET predicate-expand PROPERTY VS_GLOBAL_TrackFileAccess false)
    set_property(TARGET predicate-expand PROPERTY
        MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>"
    )
endif()

if(WIN32 AND LIBCLANG_RUNTIME)
    add_custom_command(TARGET predicate-expand POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${LIBCLANG_RUNTIME}
            $<TARGET_FILE_DIR:predicate-expand>
        COMMENT "Copying libclang runtime DLL"
        VERBATIM
    )
endif()
