cmake_minimum_required(VERSION 3.20)

project(AegisFlow
    VERSION 1.0
    DESCRIPTION "AegisFlow C++实时风控决策引擎"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AEGISFLOW_BUILD_PROJECT
    "Build the AegisFlow service and operational clients" ON)
option(AEGISFLOW_BUILD_BENCHMARK
    "Build native load and FeatureStore benchmark clients" OFF)
option(AEGISFLOW_BUILD_TESTS
    "Build focused module tests" OFF)

if(NOT AEGISFLOW_BUILD_PROJECT AND
   NOT AEGISFLOW_BUILD_BENCHMARK AND
   NOT AEGISFLOW_BUILD_TESTS)
    message(FATAL_ERROR
        "No build group selected. Enable AEGISFLOW_BUILD_PROJECT, "
        "AEGISFLOW_BUILD_BENCHMARK, or AEGISFLOW_BUILD_TESTS."
    )
endif()

set(AEGISFLOW_SANITIZER "none" CACHE STRING
    "Sanitizer instrumentation: none, address, leak, or thread"
)
set_property(CACHE AEGISFLOW_SANITIZER PROPERTY STRINGS
    none address leak thread
)

set(AEGISFLOW_SUPPORTED_SANITIZERS none address leak thread)
list(FIND AEGISFLOW_SUPPORTED_SANITIZERS "${AEGISFLOW_SANITIZER}"
    AEGISFLOW_SANITIZER_INDEX
)
if(AEGISFLOW_SANITIZER_INDEX EQUAL -1)
    message(FATAL_ERROR
        "Unsupported AEGISFLOW_SANITIZER='${AEGISFLOW_SANITIZER}'. "
        "Choose one of: none, address, leak, thread."
    )
endif()

if(NOT AEGISFLOW_SANITIZER STREQUAL "none")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "AEGISFLOW_SANITIZER requires GCC or Clang; found "
            "'${CMAKE_CXX_COMPILER_ID}'."
        )
    endif()
    if(AEGISFLOW_SANITIZER STREQUAL "address")
        set(AEGISFLOW_SANITIZER_FLAG "-fsanitize=address,undefined")
        add_compile_options(-fno-sanitize-recover=all)
    elseif(AEGISFLOW_SANITIZER STREQUAL "leak")
        set(AEGISFLOW_SANITIZER_FLAG "-fsanitize=leak")
    elseif(AEGISFLOW_SANITIZER STREQUAL "thread")
        set(AEGISFLOW_SANITIZER_FLAG "-fsanitize=thread")
    endif()
    add_compile_options("${AEGISFLOW_SANITIZER_FLAG}" -fno-omit-frame-pointer)
    add_link_options("${AEGISFLOW_SANITIZER_FLAG}")
    message(STATUS "AegisFlow sanitizer: ${AEGISFLOW_SANITIZER}")
endif()

find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)

set(PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/login.proto)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

add_library(aegisflow_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})
target_include_directories(aegisflow_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(aegisflow_proto PUBLIC protobuf::libprotobuf)

if(AEGISFLOW_BUILD_PROJECT OR AEGISFLOW_BUILD_TESTS)
    find_path(MYSQL_INCLUDE_DIR
        NAMES mysql.h
        PATHS
            /usr/include/mysql
            /usr/include/mariadb
            /usr/local/include/mysql
            /usr/local/include/mariadb
    )
    find_library(MYSQLCLIENT_LIBRARY NAMES mysqlclient mariadb)
    if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQLCLIENT_LIBRARY)
        message(FATAL_ERROR
            "MySQL client library not found. Install libmysqlclient-dev or "
            "libmariadb-dev."
        )
    endif()

    find_path(HIREDIS_INCLUDE_DIR NAMES hiredis/hiredis.h)
    find_library(HIREDIS_LIBRARY NAMES hiredis)
    if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
        message(FATAL_ERROR
            "hiredis is required. Install libhiredis-dev."
        )
    endif()

    include(cmake/AegisFlowServiceSources.cmake)
endif()

if(AEGISFLOW_BUILD_PROJECT)
    include(cmake/AegisFlowProject.cmake)
endif()

if(AEGISFLOW_BUILD_BENCHMARK OR AEGISFLOW_BUILD_TESTS)
    include(cmake/AegisFlowBenchmark.cmake)
endif()

if(AEGISFLOW_BUILD_TESTS)
    include(cmake/AegisFlowTests.cmake)
endif()
