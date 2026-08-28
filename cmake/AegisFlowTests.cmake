enable_testing()

function(aegisflow_test_executable target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${MYSQL_INCLUDE_DIR}
    )
    target_link_libraries(${target} PRIVATE
        aegisflow_proto
        Threads::Threads
    )
endfunction()

aegisflow_test_executable(test_array_view
    tests/base/array_view_test.cpp
)
add_test(NAME test_array_view COMMAND test_array_view)

aegisflow_test_executable(test_cancellation
    tests/runtime/cancellation_test.cpp
)
add_test(NAME test_cancellation COMMAND test_cancellation)

aegisflow_test_executable(test_app_config
    tests/config/app_config_test.cpp
    src/config/config.cpp
    src/risk/login_policy_chain.cpp
)
add_test(NAME test_app_config COMMAND test_app_config)

aegisflow_test_executable(test_length_prefixed_codec
    tests/protocol/length_prefixed_codec_test.cpp
    src/net/length_prefixed_codec.cpp
)
add_test(NAME test_length_prefixed_codec COMMAND test_length_prefixed_codec)

aegisflow_test_executable(test_login_protocol
    tests/protocol/login_protocol_test.cpp
    src/benchmark/login_frame_codec.cpp
)
add_test(NAME test_login_protocol COMMAND test_login_protocol)

aegisflow_test_executable(test_login_request_validator
    tests/app/login_request_validator_test.cpp
    src/app/login_request_validator.cpp
    src/domain/ip_address.cpp
)
add_test(NAME test_login_request_validator COMMAND test_login_request_validator)

aegisflow_test_executable(test_login_policy
    tests/risk/login_policy_test.cpp
    src/app/login_request_validator.cpp
    src/domain/ip_address.cpp
    src/risk/login_policy_chain.cpp
)
add_test(NAME test_login_policy COMMAND test_login_policy)

aegisflow_test_executable(test_blacklist_candidate_generator
    tests/risk/blacklist_candidate_generator_test.cpp
    src/app/login_request_validator.cpp
    src/domain/ip_address.cpp
    src/risk/blacklist_candidate_generator.cpp
    src/risk/blacklist_mutation.cpp
)
add_test(NAME test_blacklist_candidate_generator
    COMMAND test_blacklist_candidate_generator)

aegisflow_test_executable(test_blacklist_candidate_queue
    tests/app/blacklist_candidate_queue_test.cpp
    src/app/blacklist_candidate_queue.cpp
    src/domain/ip_address.cpp
    src/risk/blacklist_mutation.cpp
)
add_test(NAME test_blacklist_candidate_queue
    COMMAND test_blacklist_candidate_queue)

aegisflow_test_executable(test_login_business_handler
    tests/app/login_business_handler_test.cpp
    src/app/blacklist_candidate_queue.cpp
    src/app/login_request_validator.cpp
    src/app/risk_service.cpp
    src/domain/ip_address.cpp
    src/feature/feature_store.cpp
    src/net/login_business_handler.cpp
    src/risk/blacklist_candidate_generator.cpp
    src/risk/blacklist_manager.cpp
    src/risk/blacklist_mutation.cpp
    src/risk/blacklist_snapshot.cpp
    src/risk/login_policy_chain.cpp
)
add_test(NAME test_login_business_handler COMMAND test_login_business_handler)

aegisflow_test_executable(test_sliding_window
    tests/feature/sliding_window_test.cpp
)
add_test(NAME test_sliding_window COMMAND test_sliding_window)

aegisflow_test_executable(test_feature_store
    tests/feature/feature_store_test.cpp
    src/app/login_request_validator.cpp
    src/domain/ip_address.cpp
    src/feature/feature_store.cpp
)
add_test(NAME test_feature_store COMMAND test_feature_store)

aegisflow_test_executable(test_feature_state_maintenance
    tests/app/feature_state_maintenance_test.cpp
    src/app/feature_state_maintenance.cpp
    src/feature/feature_store.cpp
    src/runtime/bounded_worker_pool.cpp
)
add_test(
    NAME test_feature_state_maintenance
    COMMAND test_feature_state_maintenance
)

aegisflow_test_executable(test_benchmark_metrics
    tests/benchmark/benchmark_metrics_test.cpp
)
target_link_libraries(test_benchmark_metrics PRIVATE aegisflow_benchmark_core)
add_test(NAME test_benchmark_metrics COMMAND test_benchmark_metrics)

aegisflow_test_executable(test_async_logger
    tests/log/async_logger_test.cpp
    src/log/logger.cpp
)
add_test(NAME test_async_logger COMMAND test_async_logger)
set_tests_properties(test_async_logger PROPERTIES TIMEOUT 20)

aegisflow_test_executable(test_mysql_blacklist
    tests/storage/mysql_blacklist_test.cpp
    src/domain/ip_address.cpp
    src/risk/blacklist_mutation.cpp
    src/storage/mysql_dao.cpp
)
target_link_libraries(test_mysql_blacklist PRIVATE ${MYSQLCLIENT_LIBRARY})
add_test(NAME test_mysql_blacklist COMMAND test_mysql_blacklist)

aegisflow_test_executable(test_redis_blacklist_store
    tests/storage/redis_blacklist_store_test.cpp
    src/domain/ip_address.cpp
    src/risk/blacklist_mutation.cpp
    src/storage/blacklist_redis_store.cpp
    src/storage/redis_connection.cpp
)
target_include_directories(test_redis_blacklist_store PRIVATE
    ${HIREDIS_INCLUDE_DIR})
target_link_libraries(test_redis_blacklist_store PRIVATE ${HIREDIS_LIBRARY})
add_test(NAME test_redis_blacklist_store COMMAND test_redis_blacklist_store)
add_test(NAME test_redis_blacklist_store_integration
    COMMAND test_redis_blacklist_store --integration)
set_tests_properties(test_redis_blacklist_store_integration PROPERTIES
    SKIP_REGULAR_EXPRESSION "\\[SKIP\\]")

aegisflow_test_executable(test_blacklist_cache_bootstrap
    tests/app/blacklist_cache_bootstrap_test.cpp
    src/app/blacklist_cache_bootstrap.cpp
    src/domain/ip_address.cpp
    src/log/logger.cpp
    src/risk/blacklist_mutation.cpp
    src/storage/blacklist_redis_store.cpp
    src/storage/mysql_dao.cpp
    src/storage/redis_connection.cpp
)
target_include_directories(test_blacklist_cache_bootstrap PRIVATE
    ${HIREDIS_INCLUDE_DIR})
target_link_libraries(test_blacklist_cache_bootstrap PRIVATE
    ${HIREDIS_LIBRARY}
    ${MYSQLCLIENT_LIBRARY}
)
add_test(NAME test_blacklist_cache_bootstrap COMMAND test_blacklist_cache_bootstrap)
set_tests_properties(test_blacklist_cache_bootstrap PROPERTIES
    SKIP_REGULAR_EXPRESSION "\\[SKIP\\]")

aegisflow_test_executable(test_blacklist_maintenance
    tests/app/blacklist_maintenance_test.cpp
    src/app/blacklist_candidate_queue.cpp
    src/app/blacklist_maintenance.cpp
    src/app/blacklist_maintenance_backend.cpp
    src/domain/ip_address.cpp
    src/log/logger.cpp
    src/risk/blacklist_manager.cpp
    src/risk/blacklist_mutation.cpp
    src/risk/blacklist_snapshot.cpp
    src/runtime/bounded_worker_pool.cpp
    src/storage/blacklist_redis_store.cpp
    src/storage/mysql_dao.cpp
    src/storage/redis_connection.cpp
)
target_include_directories(test_blacklist_maintenance PRIVATE
    ${HIREDIS_INCLUDE_DIR})
target_link_libraries(test_blacklist_maintenance PRIVATE
    ${HIREDIS_LIBRARY}
    ${MYSQLCLIENT_LIBRARY}
)
add_test(NAME test_blacklist_maintenance COMMAND test_blacklist_maintenance)
add_test(NAME test_blacklist_maintenance_integration
    COMMAND test_blacklist_maintenance --integration)
set_tests_properties(test_blacklist_maintenance_integration PROPERTIES
    SKIP_REGULAR_EXPRESSION "\\[SKIP\\]")

aegisflow_test_executable(test_manage_blacklist
    tests/tools/manage_blacklist_test.cpp
    src/domain/ip_address.cpp
    src/risk/blacklist_mutation.cpp
    src/storage/blacklist_redis_store.cpp
    src/tools/manage_blacklist_command.cpp
)
add_test(NAME test_manage_blacklist COMMAND test_manage_blacklist)

aegisflow_test_executable(test_benchmark_reset
    tests/benchmark/benchmark_reset_test.cpp
)
add_test(NAME test_benchmark_reset
    COMMAND test_benchmark_reset
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pressure_test.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/config/benchmark_native.conf
)

aegisflow_test_executable(test_bounded_worker_pool
    tests/runtime/bounded_worker_pool_test.cpp
    src/runtime/bounded_worker_pool.cpp
)
add_test(NAME test_bounded_worker_pool COMMAND test_bounded_worker_pool)

aegisflow_test_executable(test_timer
    tests/runtime/timer_test.cpp
    src/timer/timer.cpp
    src/timer/timer_core.cpp
)
add_test(NAME test_timer COMMAND test_timer)

aegisflow_test_executable(test_session
    tests/net/session_test.cpp
    src/net/length_prefixed_codec.cpp
    src/net/session.cpp
)
add_test(NAME test_session COMMAND test_session)

aegisflow_test_executable(test_event_loop
    tests/net/event_loop_test.cpp
    src/net/completion_router.cpp
    src/net/event_loop.cpp
    src/net/length_prefixed_codec.cpp
    src/net/loop_mailbox.cpp
    src/net/session.cpp
    src/runtime/bounded_worker_pool.cpp
)
add_test(NAME test_event_loop COMMAND test_event_loop)
set_tests_properties(test_event_loop PROPERTIES TIMEOUT 20)

aegisflow_test_executable(test_lifecycle
    tests/runtime/lifecycle_test.cpp
    ${AEGISFLOW_SERVICE_SOURCES}
)
target_include_directories(test_lifecycle PRIVATE ${HIREDIS_INCLUDE_DIR})
target_link_libraries(test_lifecycle PRIVATE
    ${HIREDIS_LIBRARY}
    ${MYSQLCLIENT_LIBRARY}
)
add_test(NAME test_lifecycle COMMAND test_lifecycle)
add_test(NAME test_lifecycle_integration COMMAND test_lifecycle --integration)
set_tests_properties(test_lifecycle test_lifecycle_integration PROPERTIES
    TIMEOUT 30)
set_tests_properties(test_lifecycle_integration PROPERTIES
    SKIP_REGULAR_EXPRESSION "\\[SKIP\\]")
