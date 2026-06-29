cmake_minimum_required(VERSION 3.20)

project(rtlzz-backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(rtlzz-backend
    src/main.cpp
    src/BackendIR.cpp
    src/JsonParser.cpp
    src/PredicateJsonLoader.cpp
)

target_include_directories(rtlzz-backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
