cmake_minimum_required(VERSION 3.10)

project(WorkProject)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS ON)

FIND_PACKAGE(Boost COMPONENTS program_options REQUIRED )

add_executable(WorkProject project.cpp)

target_link_libraries(WorkProject PRIVATE Boost::program_options)