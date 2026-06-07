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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log/logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config/config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/http_server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/risk_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/feature/feature_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/worker_pool.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

target_include_directories(AegisFlow PRIVATE 
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include/
)

target_link_libraries(AegisFlow PRIVATE
    protobuf::libprotobuf
)

add_executable(send_event
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/send_event.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

target_include_directories(send_event PRIVATE 
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(send_event PRIVATE
    protobuf::libprotobuf
)

# enable_testing()

# add_executable(test_sliding_counter
#     ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sliding_counter.cpp
# )
# target_include_directories(test_sliding_counter PRIVATE 
#     ${CMAKE_CURRENT_SOURCE_DIR}/include/
# )

# add_test(NAME test_sliding_counter COMMAND test_sliding_counter)

# add_executable(test_recent_action_window
#     ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_recent_action_window.cpp
#     ${PROTO_SRCS}
#     ${PROTO_HDRS}
# )

# target_include_directories(test_recent_action_window PRIVATE 
#     ${CMAKE_CURRENT_BINARY_DIR}
#     ${CMAKE_CURRENT_SOURCE_DIR}/include/
# )

# target_link_libraries(test_recent_action_window PRIVATE
#     protobuf::libprotobuf
# )

# add_test(NAME test_recent_action_window COMMAND test_recent_action_window)


# add_executable(test_user_state
#     ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_user_state.cpp
#     ${PROTO_SRCS}
#     ${PROTO_HDRS}
# )

# target_include_directories(test_user_state PRIVATE
#     ${CMAKE_CURRENT_BINARY_DIR}
#     ${CMAKE_CURRENT_SOURCE_DIR}/include/
# )

# target_link_libraries(test_user_state PRIVATE
#     protobuf::libprotobuf
# )

# add_test(NAME test_user_state COMMAND test_user_state)