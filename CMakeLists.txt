cmake_minimum_required(VERSION 3.16)

project(SigmodContest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

Include(FetchContent)

# --------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------

FetchContent_Declare(
    Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.8.0.tar.gz
)
FetchContent_MakeAvailable(Catch2)

FetchContent_Declare(
    abseil
    URL https://github.com/abseil/abseil-cpp/releases/download/20240722.1/abseil-cpp-20240722.1.tar.gz
)
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_ENABLE_INSTALL ON)
FetchContent_MakeAvailable(abseil)

FetchContent_Declare(
    re2
    URL https://github.com/google/re2/releases/download/2024-07-02/re2-2024-07-02.tar.gz
)
FetchContent_MakeAvailable(re2)

FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

FetchContent_Declare(
    sql-parser
    URL https://github.com/a858438680/sql-parser/archive/refs/tags/win-port-2.tar.gz
)
set(HSQL_ENABLE_WERROR OFF)
FetchContent_MakeAvailable(sql-parser)

FetchContent_Declare(
    range-v3
    URL https://github.com/ericniebler/range-v3/archive/refs/tags/0.12.0.tar.gz
)
FetchContent_MakeAvailable(range-v3)

FetchContent_Declare(
    fmtlib
    URL https://github.com/fmtlib/fmt/releases/download/11.1.3/fmt-11.1.3.zip
)
FetchContent_MakeAvailable(fmtlib)

FetchContent_Declare(
    hopscotch_map
    GIT_REPOSITORY https://github.com/Tessil/hopscotch-map.git
    GIT_TAG master
)
FetchContent_MakeAvailable(hopscotch_map)

FetchContent_Declare(
    duckdb
    URL https://github.com/duckdb/duckdb/archive/refs/tags/v1.2.1.tar.gz
)
set(ENABLE_SANITIZER OFF)
set(ENABLE_UBSAN OFF)
FetchContent_MakeAvailable(duckdb)


# --------------------------------------------------------------------
# SOURCE FILTERING – ΚΡΑΤΑΜΕ ΜΟΝΟ τον executeGeneric.cpp
# --------------------------------------------------------------------

file(GLOB SIGMODPC_SRC
    CONFIGURE_DEPENDS
    "src/*.cpp"
)

# Remove ALL old executors
list(FILTER SIGMODPC_SRC EXCLUDE REGEX "src/execute_.*\\.cpp")

# Remove old LM implementation .cpp (we only use the header)
list(FILTER SIGMODPC_SRC EXCLUDE REGEX "src/late_materialization\\.cpp")

# Make sure columnar.cpp is kept (required LM bridge)
# No filter needed — columnar.cpp stays

# Add our unified executor
list(APPEND SIGMODPC_SRC src/executeGeneric.cpp)


# --------------------------------------------------------------------
# Compiler Options
# --------------------------------------------------------------------

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ppc|powerpc|ppc64|ppc64le")
    add_compile_options(-march=native)
endif()


# --------------------------------------------------------------------
# Targets
# --------------------------------------------------------------------

add_executable(run ${SIGMODPC_SRC} tests/read_sql.cpp)
add_executable(unit_tests ${SIGMODPC_SRC} tests/unit_tests.cpp)
add_executable(build_database tests/build_database.cpp)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(build_cache ${SIGMODPC_SRC} tests/read_sql.cpp)
    add_executable(fast ${SIGMODPC_SRC} tests/read_sql.cpp)

    target_link_libraries(build_cache PRIVATE re2 fmt range-v3 nlohmann_json::nlohmann_json sqlparser duckdb)
    target_compile_definitions(build_cache PRIVATE TEAMOPT_USE_DUCKDB TEAMOPT_BUILD_CACHE)

    target_link_libraries(fast PRIVATE re2 fmt range-v3 nlohmann_json::nlohmann_json sqlparser)
    target_include_directories(build_cache PRIVATE include ${hopscotch_map_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include)
    target_include_directories(fast PRIVATE include ${hopscotch_map_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include)
endif()

target_link_libraries(run PRIVATE re2 fmt range-v3 nlohmann_json::nlohmann_json sqlparser duckdb)
target_compile_definitions(run PRIVATE TEAMOPT_USE_DUCKDB)
target_link_libraries(build_database PRIVATE fmt duckdb)
target_link_libraries(unit_tests PRIVATE re2 range-v3 fmt Catch2::Catch2WithMain)

add_executable(software_tester ${SIGMODPC_SRC} tests/software_tester.cpp)
target_link_libraries(software_tester PRIVATE re2 range-v3 fmt Catch2::Catch2WithMain)
target_compile_definitions(software_tester PRIVATE TEAMOPT_BUILD_CACHE)

target_include_directories(software_tester PRIVATE include ${hopscotch_map_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include)
target_include_directories(run PRIVATE include ${hopscotch_map_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include)
target_include_directories(build_database PRIVATE include ${hopscotch_map_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include)
target_include_directories(unit_tests PRIVATE include ${hopscotch_map_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include)