add_library(aegisflow_benchmark_core STATIC
    src/benchmark/login_frame_codec.cpp
    src/benchmark/load_generator.cpp
)
target_include_directories(aegisflow_benchmark_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(aegisflow_benchmark_core PUBLIC
    aegisflow_proto
    Threads::Threads
)

if(AEGISFLOW_BUILD_BENCHMARK)
    add_executable(benchmark_native tools/benchmark_native.cpp)
    target_link_libraries(benchmark_native PRIVATE aegisflow_benchmark_core)

    add_executable(benchmark_feature_store
        tools/benchmark_feature_store.cpp
        src/app/login_request_validator.cpp
        src/domain/ip_address.cpp
        src/feature/feature_store.cpp
    )
    target_include_directories(benchmark_feature_store PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(benchmark_feature_store PRIVATE aegisflow_proto)
endif()
