cmake_minimum_required(VERSION 3.10)

project(WorkProject)

set(CMAKE_CXX_STANDARD 23)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS ON)

FIND_PACKAGE(Boost COMPONENTS program_options REQUIRED )

find_package(OpenSSL REQUIRED)

add_executable(WorkProject project.cpp)

if(MSVC)
    target_compile_options(WorkProject PRIVATE /W4 /WX)
endif()

target_compile_options(WorkProject PRIVATE /FA)

target_link_libraries(WorkProject PRIVATE Boost::program_options)

target_link_libraries(WorkProject PRIVATE OpenSSL::Crypto)