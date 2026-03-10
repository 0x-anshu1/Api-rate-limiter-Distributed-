cmake_minimum_required(VERSION 3.16)

project(rate_limiter)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find hiredis for Redis (optional)
find_package(Hiredis QUIET)
if(NOT Hiredis_FOUND)
    message(STATUS "Hiredis not found - Redis support disabled")
endif()

# Include directories
include_directories(
    src
    src/limiter
    src/manager
    Third-Party/Crow/include
)

# Source files
set(SOURCES
    src/main.cc
    src/limiter/token_bucket.cc
    src/manager/limiter_manager.cc
    src/cluster/hashing.cc
)

# Build executable
add_executable(rate_limiter ${SOURCES})

# Link hiredis if available
if(Hiredis_FOUND)
    target_link_libraries(rate_limiter Hiredis::Hiredis)
    target_compile_definitions(rate_limiter HAVE_REDIS)
endif()

# Crow needs pthread
target_link_libraries(rate_limiter pthread)
