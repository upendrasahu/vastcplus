cmake_minimum_required(VERSION 3.10)

# Project name
project(VASTClient)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Include directories
include_directories(include)

# Add source files
file(GLOB SOURCES "src/*.cpp")

# Create the library as a shared object
add_library(vastclient SHARED ${SOURCES})

# Link the libcurl library
find_package(CURL REQUIRED)
target_link_libraries(vastclient PRIVATE CURL::libcurl)

# Specify installation rules
install(TARGETS vastclient
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include FILES_MATCHING PATTERN "*.h")

# Add executable for testing
add_executable(vastclient_test tests/main.cpp)
target_link_libraries(vastclient_test PRIVATE vastclient CURL::libcurl)