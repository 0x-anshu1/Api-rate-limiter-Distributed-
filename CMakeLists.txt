cmake_minimum_required(VERSION 3.16)

project(rate_limiter)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer the system pkg-config metadata so headers and libraries stay ABI-compatible.
set(ENV{PKG_CONFIG_PATH} "/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig")

# Use pkg-config for all dependencies
find_package(PkgConfig REQUIRED)

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
    ${CMAKE_CURRENT_BINARY_DIR}
)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/google)
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/google/protobuf)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                /usr/include/google/protobuf
                ${CMAKE_CURRENT_BINARY_DIR}/google/protobuf
    )
endif()

if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/grpc)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                /usr/include/grpc
                ${CMAKE_CURRENT_BINARY_DIR}/grpc
    )
endif()

if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/grpcpp)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                /usr/include/grpcpp
                ${CMAKE_CURRENT_BINARY_DIR}/grpcpp
    )
endif()

find_program(PROTOC_EXECUTABLE NAMES protoc PATHS /usr/bin REQUIRED NO_DEFAULT_PATH)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin PATHS /usr/bin REQUIRED NO_DEFAULT_PATH)

set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/node.proto)
set(GENERATED_PROTO_SRCS
    ${CMAKE_CURRENT_BINARY_DIR}/node.pb.cc
    ${CMAKE_CURRENT_BINARY_DIR}/node.grpc.pb.cc
)
set(GENERATED_PROTO_HDRS
    ${CMAKE_CURRENT_BINARY_DIR}/node.pb.h
    ${CMAKE_CURRENT_BINARY_DIR}/node.grpc.pb.h
)

add_custom_command(
    OUTPUT ${GENERATED_PROTO_SRCS} ${GENERATED_PROTO_HDRS}
    COMMAND ${PROTOC_EXECUTABLE}
    ARGS
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
        --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
        --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
        ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "Generating gRPC/protobuf sources"
    VERBATIM
)

add_custom_target(generate_proto DEPENDS ${GENERATED_PROTO_SRCS} ${GENERATED_PROTO_HDRS})

# HTTP server (Crow)
set(HTTP_SOURCES
    src/main.cc
    ${GENERATED_PROTO_SRCS}
    src/limiter/token_bucket.cc
    src/manager/limiter_manager.cc
    src/cluster/hashing.cc
    src/cluster/cluster_manager.cc
    src/storage/token_bucket_store.cc
)

add_executable(rate_limiter ${HTTP_SOURCES})
add_dependencies(rate_limiter generate_proto)

# gRPC server
set(GRPC_SOURCES
    src/grpc_server.cc
    ${GENERATED_PROTO_SRCS}
    src/limiter/token_bucket.cc
    src/manager/limiter_manager.cc
    src/cluster/hashing.cc
    src/cluster/cluster_manager.cc
    src/storage/token_bucket_store.cc
)

add_executable(grpc_server ${GRPC_SOURCES})
add_dependencies(grpc_server generate_proto)

# gRPC client
set(GRPC_CLIENT_SOURCES
    src/grpc_client.cc
    ${GENERATED_PROTO_SRCS}
)

add_executable(grpc_client ${GRPC_CLIENT_SOURCES})
add_dependencies(grpc_client generate_proto)

# Use pkg-config for protobuf and gRPC
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc)

target_include_directories(rate_limiter PRIVATE ${PROTOBUF_INCLUDE_DIRS} ${GRPCPP_INCLUDE_DIRS} ${GRPC_INCLUDE_DIRS})
target_include_directories(grpc_server PRIVATE ${PROTOBUF_INCLUDE_DIRS} ${GRPCPP_INCLUDE_DIRS} ${GRPC_INCLUDE_DIRS})
target_include_directories(grpc_client PRIVATE ${PROTOBUF_INCLUDE_DIRS} ${GRPCPP_INCLUDE_DIRS} ${GRPC_INCLUDE_DIRS})
target_link_libraries(rate_limiter PRIVATE PkgConfig::PROTOBUF PkgConfig::GRPCPP PkgConfig::GRPC pthread)
target_link_libraries(grpc_server PRIVATE PkgConfig::PROTOBUF PkgConfig::GRPCPP PkgConfig::GRPC pthread)
target_link_libraries(grpc_client PRIVATE PkgConfig::PROTOBUF PkgConfig::GRPCPP PkgConfig::GRPC pthread)

# Link hiredis if available
if(Hiredis_FOUND)
    target_link_libraries(rate_limiter Hiredis::Hiredis)
    target_link_libraries(grpc_server Hiredis::Hiredis)
    target_link_libraries(grpc_client Hiredis::Hiredis)
    target_compile_definitions(rate_limiter HAVE_REDIS)
    target_compile_definitions(grpc_server HAVE_REDIS)
    target_compile_definitions(grpc_client HAVE_REDIS)
endif()
