add_executable(AegisFlow src/main.cpp ${AEGISFLOW_SERVICE_SOURCES})
target_include_directories(AegisFlow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HIREDIS_INCLUDE_DIR}
    ${MYSQL_INCLUDE_DIR}
)
target_link_libraries(AegisFlow PRIVATE
    aegisflow_proto
    ${HIREDIS_LIBRARY}
    ${MYSQLCLIENT_LIBRARY}
    Threads::Threads
)

add_executable(send_event tools/send_event.cpp)
target_include_directories(send_event PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(send_event PRIVATE aegisflow_proto)

add_executable(
    manage_blacklist
    tools/manage_blacklist.cpp
    src/config/config.cpp
    src/domain/ip_address.cpp
    src/risk/blacklist_mutation.cpp
    src/risk/login_policy_chain.cpp
    src/storage/blacklist_redis_store.cpp
    src/storage/mysql_dao.cpp
    src/storage/redis_connection.cpp
    src/tools/manage_blacklist_command.cpp
)
target_include_directories(manage_blacklist PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HIREDIS_INCLUDE_DIR}
    ${MYSQL_INCLUDE_DIR}
)
target_link_libraries(manage_blacklist PRIVATE
    ${HIREDIS_LIBRARY}
    ${MYSQLCLIENT_LIBRARY}
)
