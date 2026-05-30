cmake_minimum_required(VERSION 3.20)

project(AegisFlow VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf REQUIRED)

set(PROTO_FILES 
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/event.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/decision.proto
)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

add_executable(AegisFlow 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

target_include_directories(AegisFlow PRIVATE 
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(AegisFlow PRIVATE
    protobuf::libprotobuf
)