cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_VS_GLOBALS "TrackFileAccess=false;VcpkgEnabled=false")
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_VS_GLOBALS)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
project(predicate-expand LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(PREDICATE_EXPAND_REQUIRE_LIBCLANG "Fail configure when libclang is not available" OFF)

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

# --- Find libclang ---
# Configure with -DLLVM_DIR=... when LLVM is not in a standard CMake package
# location. This project uses the libclang C API, so ClangConfig.cmake is not
# required. Some Windows LLVM installs ship lib/cmake/llvm without a complete
# LLVMConfig.cmake, so the package is optional and explicit libclang discovery
# below is authoritative.
find_package(LLVM CONFIG QUIET)

if(LLVM_FOUND)
    set(_LLVM_HINT_INCLUDE ${LLVM_INCLUDE_DIRS})
    set(_LLVM_HINT_LIBRARY ${LLVM_LIBRARY_DIRS})
    set(_LLVM_HINT_PREFIX ${LLVM_INSTALL_PREFIX})
else()
    get_filename_component(_LLVM_HINT_PREFIX "${LLVM_DIR}/../../.." ABSOLUTE)
endif()

find_path(LIBCLANG_INCLUDE_DIR
    NAMES clang-c/Index.h
    HINTS
        ${_LLVM_HINT_INCLUDE}
        ${_LLVM_HINT_PREFIX}/include
        ${LLVM_DIR}/../../../include
        "$ENV{LLVM_DIR}/../../../include"
        "C:/Program Files/LLVM/include"
    DOC "Directory containing clang-c/Index.h"
)

find_library(LIBCLANG_LIBRARY
    NAMES libclang clang
    HINTS
        ${_LLVM_HINT_LIBRARY}
        ${_LLVM_HINT_PREFIX}/lib
        ${LLVM_DIR}/../../../lib
        "$ENV{LLVM_DIR}/../../../lib"
        "C:/Program Files/LLVM/lib"
    DOC "libclang C API library"
)

if(WIN32)
    find_file(LIBCLANG_RUNTIME
        NAMES libclang.dll clang.dll
        HINTS
            ${_LLVM_HINT_PREFIX}/bin
            ${LLVM_DIR}/../../../bin
            "$ENV{LLVM_DIR}/../../../bin"
            "C:/Program Files/LLVM/bin"
        DOC "libclang runtime DLL"
    )
endif()

if(LLVM_FOUND)
    message(STATUS "Found LLVM package: ${LLVM_PACKAGE_VERSION}")
else()
    message(STATUS "LLVM CMake package not found; using explicit libclang C API discovery")
endif()

set(HAS_LIBCLANG FALSE)
if(LIBCLANG_INCLUDE_DIR AND LIBCLANG_LIBRARY)
    set(HAS_LIBCLANG TRUE)
    message(STATUS "Found libclang include dir: ${LIBCLANG_INCLUDE_DIR}")
    message(STATUS "Found libclang library: ${LIBCLANG_LIBRARY}")
    if(WIN32 AND LIBCLANG_RUNTIME)
        message(STATUS "Found libclang runtime: ${LIBCLANG_RUNTIME}")
    elseif(WIN32)
        message(WARNING "Could not find libclang.dll. Add the LLVM bin directory to PATH before running libclang-based executables.")
    endif()
else()
    if(PREDICATE_EXPAND_REQUIRE_LIBCLANG)
        message(FATAL_ERROR "PREDICATE_EXPAND_REQUIRE_LIBCLANG=ON but clang-c/Index.h or libclang was not found")
    endif()
    message(WARNING "libclang was not found. Configuring UNIT-ONLY mode: predicate-expand CLI and e2e tests are disabled; predicate-expand-tests unit target remains available.")
endif()

function(copy_libclang_runtime target)
    if(WIN32 AND LIBCLANG_RUNTIME)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${LIBCLANG_RUNTIME}
                $<TARGET_FILE_DIR:${target}>
            COMMENT "Copying libclang runtime DLL for ${target}"
            VERBATIM
        )
    endif()
endfunction()

# --- Source files ---
set(SOURCES
    src/main.cpp
    src/ast/ASTBuilderDriver.cpp
    src/ast/ASTBuilder.cpp
    src/ast/VulOpRecognizer.cpp
    src/ast/VulTypeRecognizer.cpp
    src/eval/PredicateEvaluator.cpp
    src/ir/CanonicalIR.cpp
    src/semantics/AliasGraph.cpp
    src/semantics/IntSemantics.cpp
    src/ir/CFG.cpp
    src/ir/SSA.cpp
    src/ir/TypedDAG.cpp
    src/transform/LoopUnroll.cpp
    src/normalize/NormalizeDriver.cpp
    src/transform/Normalize.cpp
    src/normalize/AliasLowering.cpp
    src/normalize/FunctionInline.cpp
    src/normalize/LambdaInline.cpp
    src/normalize/ArrayScalarize.cpp
    src/normalize/StructFlatten.cpp
    src/normalize/IntOpLowering.cpp
    src/normalize/LookupLowering.cpp
    src/normalize/AssignmentLowering.cpp
    src/normalize/DefaultTotalizationPass.cpp
    src/transform/Predicate.cpp
    src/predicate/DefaultTotalizationPass.cpp
    src/predicate/OutputExpressionMap.cpp
    src/predicate/PredicateVerifier.cpp
    src/emitter/TextEmitter.cpp
    src/emitter/JsonEmitter.cpp
    src/emitter/SmtEmitter.cpp
)

if(HAS_LIBCLANG)
    add_executable(predicate-expand ${SOURCES})
    set_property(TARGET predicate-expand PROPERTY VS_GLOBAL_TrackFileAccess false)

    target_include_directories(predicate-expand PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${LLVM_INCLUDE_DIRS}
        ${LIBCLANG_INCLUDE_DIR}
    )

    target_link_libraries(predicate-expand PRIVATE ${LIBCLANG_LIBRARY})
    target_compile_definitions(predicate-expand PRIVATE PREDICATE_EXPAND_BUILD_COMMIT="${PREDICATE_EXPAND_BUILD_COMMIT}")
    copy_libclang_runtime(predicate-expand)
endif()

# --- Tests (standalone, no Clang dependency) ---
set(TEST_SOURCES
    tests/unit/test_basic.cpp
    src/ir/CFG.cpp
    src/ir/SSA.cpp
    src/ir/TypedDAG.cpp
    src/eval/PredicateEvaluator.cpp
    src/ir/CanonicalIR.cpp
    src/semantics/AliasGraph.cpp
    src/semantics/IntSemantics.cpp
    src/transform/LoopUnroll.cpp
    src/normalize/NormalizeDriver.cpp
    src/transform/Normalize.cpp
    src/normalize/AliasLowering.cpp
    src/normalize/FunctionInline.cpp
    src/normalize/LambdaInline.cpp
    src/normalize/ArrayScalarize.cpp
    src/normalize/StructFlatten.cpp
    src/normalize/IntOpLowering.cpp
    src/normalize/LookupLowering.cpp
    src/normalize/AssignmentLowering.cpp
    src/normalize/DefaultTotalizationPass.cpp
    src/transform/Predicate.cpp
    src/predicate/DefaultTotalizationPass.cpp
    src/predicate/OutputExpressionMap.cpp
    src/predicate/PredicateVerifier.cpp
    src/emitter/TextEmitter.cpp
    src/emitter/JsonEmitter.cpp
    src/emitter/SmtEmitter.cpp
)

add_executable(predicate-expand-tests ${TEST_SOURCES})
set_property(TARGET predicate-expand-tests PROPERTY VS_GLOBAL_TrackFileAccess false)

target_include_directories(predicate-expand-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_compile_definitions(predicate-expand-tests PRIVATE PREDICATE_EXPAND_BUILD_COMMIT="${PREDICATE_EXPAND_BUILD_COMMIT}")

if(HAS_LIBCLANG)
    add_executable(predicate-expand-e2e-tests
        tests/e2e/test_e2e.cpp
        src/ast/ASTBuilderDriver.cpp
        src/ast/ASTBuilder.cpp
        src/ast/VulOpRecognizer.cpp
        src/ast/VulTypeRecognizer.cpp
        src/eval/PredicateEvaluator.cpp
        src/ir/CanonicalIR.cpp
        src/semantics/AliasGraph.cpp
        src/semantics/IntSemantics.cpp
        src/ir/CFG.cpp
        src/ir/SSA.cpp
        src/ir/TypedDAG.cpp
        src/transform/LoopUnroll.cpp
        src/normalize/NormalizeDriver.cpp
        src/transform/Normalize.cpp
        src/normalize/AliasLowering.cpp
        src/normalize/FunctionInline.cpp
        src/normalize/LambdaInline.cpp
        src/normalize/ArrayScalarize.cpp
        src/normalize/StructFlatten.cpp
        src/normalize/IntOpLowering.cpp
        src/normalize/LookupLowering.cpp
        src/normalize/AssignmentLowering.cpp
        src/normalize/DefaultTotalizationPass.cpp
        src/transform/Predicate.cpp
        src/predicate/DefaultTotalizationPass.cpp
        src/predicate/OutputExpressionMap.cpp
        src/predicate/PredicateVerifier.cpp
        src/emitter/TextEmitter.cpp
        src/emitter/JsonEmitter.cpp
        src/emitter/SmtEmitter.cpp
    )
    set_property(TARGET predicate-expand-e2e-tests PROPERTY VS_GLOBAL_TrackFileAccess false)

    target_include_directories(predicate-expand-e2e-tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${LLVM_INCLUDE_DIRS}
        ${LIBCLANG_INCLUDE_DIR}
    )

    target_link_libraries(predicate-expand-e2e-tests PRIVATE ${LIBCLANG_LIBRARY})
    target_compile_definitions(predicate-expand-e2e-tests PRIVATE PREDICATE_EXPAND_BUILD_COMMIT="${PREDICATE_EXPAND_BUILD_COMMIT}")
    copy_libclang_runtime(predicate-expand-e2e-tests)
endif()

enable_testing()

add_test(NAME unit COMMAND predicate-expand-tests)
if(HAS_LIBCLANG)
    add_test(NAME e2e COMMAND predicate-expand-e2e-tests)
endif()

set_tests_properties(unit PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
if(HAS_LIBCLANG)
    set_tests_properties(e2e PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()

