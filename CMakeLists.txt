cmake_minimum_required(VERSION 3.16)
project(kitty_a2ad VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# --- Dependencies -------------------------------------------------------------
# nlohmann_json
find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
    # Fallback: header-only from a known system path.
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
    if(NOT NLOHMANN_JSON_INCLUDE_DIR)
        message(FATAL_ERROR "nlohmann_json not found (apt install nlohmann-json3-dev)")
    endif()
else()
    find_package(nlohmann_json 3.10 REQUIRED)
endif()

# libcurl
find_package(CURL REQUIRED)

# SQLite3
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

# yaml-cpp
find_package(yaml-cpp REQUIRED)

# Threads
find_package(Threads REQUIRED)

# Native A2A gRPC binding. The checked-in proto is the normative v1 schema
# with documentation-only google.api annotations removed; wire field numbers,
# messages, and service definitions are unchanged.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
find_package(gRPC CONFIG REQUIRED)
set(A2A_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/proto/a2a.proto)
set(A2A_PROTO_SRC ${CMAKE_CURRENT_BINARY_DIR}/a2a.pb.cc)
set(A2A_PROTO_HDR ${CMAKE_CURRENT_BINARY_DIR}/a2a.pb.h)
set(A2A_GRPC_SRC ${CMAKE_CURRENT_BINARY_DIR}/a2a.grpc.pb.cc)
set(A2A_GRPC_HDR ${CMAKE_CURRENT_BINARY_DIR}/a2a.grpc.pb.h)
add_custom_command(
    OUTPUT ${A2A_PROTO_SRC} ${A2A_PROTO_HDR} ${A2A_GRPC_SRC} ${A2A_GRPC_HDR}
    COMMAND protobuf::protoc
    ARGS --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
         --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
         --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
         --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
         ${A2A_PROTO}
    DEPENDS ${A2A_PROTO}
    VERBATIM)

# --- Daemon library (everything except main) ---------------------------------
add_library(a2ad_core STATIC
    daemon/src/ControlPlane.cpp
    daemon/src/types.cpp
    daemon/src/Artifact.cpp
    daemon/src/AgentCard.cpp
    daemon/src/Config.cpp
    daemon/src/Credentials.cpp
    daemon/src/A2AClient.cpp
    daemon/src/Database.cpp
    daemon/src/IpcServer.cpp
    daemon/src/TaskManager.cpp
    daemon/src/HttpTransport.cpp
    daemon/src/GrpcTransport.cpp
    ${A2A_PROTO_SRC}
    ${A2A_GRPC_SRC}
)
target_include_directories(a2ad_core PUBLIC daemon/include PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(a2ad_core PUBLIC
    nlohmann_json::nlohmann_json
    CURL::libcurl
    SQLite3::SQLite3
    OpenSSL::Crypto
    yaml-cpp::yaml-cpp
    Threads::Threads
    protobuf::libprotobuf
    gRPC::grpc++
)

# --- Daemon executable --------------------------------------------------------
add_executable(a2ad daemon/src/main.cpp)
target_link_libraries(a2ad PRIVATE a2ad_core)

# --- Tests --------------------------------------------------------------------
enable_testing()

add_executable(a2ad_tests
    tests/daemon/control_plane_test.cpp
    tests/daemon/test_main.cpp
    tests/daemon/types_test.cpp
    tests/daemon/artifact_test.cpp
    tests/daemon/config_test.cpp
    tests/daemon/a2a_client_test.cpp
    tests/daemon/credentials_test.cpp
    tests/daemon/database_test.cpp
    tests/daemon/protocol_test.cpp
    tests/daemon/task_manager_test.cpp
    tests/daemon/grpc_transport_test.cpp
    tests/daemon/ipc_server_test.cpp
)
target_link_libraries(a2ad_tests PRIVATE a2ad_core gRPC::grpc++)
target_include_directories(a2ad_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(a2ad_tests PRIVATE A2AD_TESTING)

add_test(NAME unit COMMAND a2ad_tests)

# Integration test: daemon + mock A2A server over the real socket.
add_executable(a2ad_e2e tests/ipc/e2e.cpp)
target_include_directories(a2ad_e2e PRIVATE ${CMAKE_SOURCE_DIR}/daemon/include)
target_link_libraries(a2ad_e2e PRIVATE a2ad_core)
target_compile_definitions(a2ad_e2e PRIVATE A2AD_BIN="${CMAKE_BINARY_DIR}/a2ad")
add_test(NAME e2e COMMAND a2ad_e2e)
set_tests_properties(e2e PROPERTIES TIMEOUT 60)

# Integration test: the default double-fork mode retains a working IPC server.
add_executable(a2ad_daemon_mode_test tests/ipc/daemon_mode.cpp)
target_compile_definitions(a2ad_daemon_mode_test PRIVATE A2AD_BIN="${CMAKE_BINARY_DIR}/a2ad")
add_test(NAME daemon_mode COMMAND a2ad_daemon_mode_test)
set_tests_properties(daemon_mode PROPERTIES TIMEOUT 30)

install(TARGETS a2ad RUNTIME DESTINATION bin)
install(FILES kitten/a2a.py DESTINATION ${CMAKE_INSTALL_PREFIX}/share/kitty/kittens)
install(FILES config/agents.example.yaml DESTINATION ${CMAKE_INSTALL_PREFIX}/share/kitty-a2a)

# Optional direct-dependency inventory with source revision and binary SHA-256.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(sbom
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/generate_sbom.py
            --binary $<TARGET_FILE:a2ad> --output ${CMAKE_BINARY_DIR}/a2ad.cdx.json
            --dependency "curl=${CURL_VERSION_STRING}"
            --dependency "sqlite=${SQLite3_VERSION}"
            --dependency "openssl=${OPENSSL_VERSION}"
            --dependency "protobuf=${Protobuf_VERSION}"
            --dependency "grpc=${gRPC_VERSION}"
            --dependency "yaml-cpp=${yaml-cpp_VERSION}"
            --dependency "nlohmann-json=${nlohmann_json_VERSION}"
        DEPENDS a2ad VERBATIM)
endif()
if(Python3_Interpreter_FOUND)
    add_test(NAME control_plane_ipc
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/ipc/control_plane.py $<TARGET_FILE:a2ad>)
    set_tests_properties(control_plane_ipc PROPERTIES TIMEOUT 45)
endif()
