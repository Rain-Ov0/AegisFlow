cmake_minimum_required(VERSION 3.20)

project(AegisFlow VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf REQUIRED)

find_path(MYSQL_INCLUDE_DIR
    NAMES mysql.h
    PATHS
        /usr/include/mysql
        /usr/include/mariadb
        /usr/local/include/mysql
        /usr/local/include/mariadb
)

find_library(MYSQLCLIENT_LIBRARY
    NAMES mysqlclient mariadb
)

if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQLCLIENT_LIBRARY)
    message(FATAL_ERROR "MySQL client library not found. Please install libmysqlclient-dev or libmariadb-dev.")
endif()

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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_lexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/decision_aggregator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_validator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/mysql_dao.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

target_include_directories(AegisFlow PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include/
    ${MYSQL_INCLUDE_DIR}
)

target_link_libraries(AegisFlow PRIVATE
    protobuf::libprotobuf
    ${MYSQLCLIENT_LIBRARY}
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

add_executable(benchmark_feature_store
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark_feature_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/feature/feature_store.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

target_include_directories(benchmark_feature_store PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include/
)

target_link_libraries(benchmark_feature_store PRIVATE
    protobuf::libprotobuf
)

enable_testing()

function(add_aegisflow_test target)
    add_executable(${target} ${ARGN})

    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include/
    )

    target_link_libraries(${target} PRIVATE
        protobuf::libprotobuf
    )

    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_aegisflow_test(test_sliding_counter
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sliding_counter.cpp
)

add_aegisflow_test(test_recent_action_window
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_recent_action_window.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

add_aegisflow_test(test_user_state
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_user_state.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

add_aegisflow_test(test_feature_store
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_feature_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/feature/feature_store.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

add_aegisflow_test(test_sliding_distinct
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sliding_distinct.cpp
)

add_aegisflow_test(test_count_min_sketch
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_count_min_sketch.cpp
)

add_aegisflow_test(test_space_saving_topk
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_space_saving_topk.cpp
)

add_aegisflow_test(test_feature_store_ip_device
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_feature_store_ip_device.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/feature/feature_store.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

add_aegisflow_test(test_feature_store_attack_flow
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_feature_store_attack_flow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/feature/feature_store.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

add_aegisflow_test(test_rule_lexer
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rule_lexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_lexer.cpp
)

add_aegisflow_test(test_rule_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rule_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_lexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_parser.cpp
)

add_aegisflow_test(test_rule_engine
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rule_engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_lexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_engine.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

add_aegisflow_test(test_decision_aggregator
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_decision_aggregator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/decision_aggregator.cpp
)

add_aegisflow_test(test_risk_service_integration
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_risk_service_integration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/risk_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/feature/feature_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/worker_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_lexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/decision_aggregator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_validator.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

target_compile_definitions(test_risk_service_integration PRIVATE
    AEGISFLOW_TEST_RULE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/config/rules.dsl"
)

add_aegisflow_test(test_rule_validator
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rule_validator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_lexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rule/rule_validator.cpp
)