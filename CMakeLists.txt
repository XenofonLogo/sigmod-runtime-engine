cmake_minimum_required(VERSION 3.16)

project(SigmodContest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

Include(FetchContent)

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

# -----------------------------
# Add Tessil Hopscotch Map
# -----------------------------
FetchContent_Declare(
    hopscotch_map
    GIT_REPOSITORY https://github.com/Tessil/hopscotch-map.git
    GIT_TAG master
)
FetchContent_MakeAvailable(hopscotch_map)

# -----------------------------
# Add Libcuckoo Cuckoo Map
# -----------------------------
FetchContent_Declare(
    libcuckoo
    GIT_REPOSITORY https://github.com/efficient/libcuckoo.git
    GIT_TAG master
)
FetchContent_MakeAvailable(libcuckoo)

# -----------------------------
# Add DuckDB
# -----------------------------
FetchContent_Declare(
    duckdb
    URL https://github.com/duckdb/duckdb/archive/refs/tags/v1.2.1.tar.gz
)
set(ENABLE_SANITIZER OFF)
set(ENABLE_UBSAN OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc|powerpc|ppc64|ppc64le")
    message("Disabling jemalloc extension of DuckDB on Power.")
    set(SKIP_EXTENSIONS jemalloc)
endif()
FetchContent_MakeAvailable(duckdb)

# ------------------------------------------------------------
# Choose which execute implementation to compile
# ------------------------------------------------------------
file(GLOB SIGMODPC_SRC
    CONFIGURE_DEPENDS
    "src/*.cpp"
)

file(GLOB SIGMODPC_EXECUTE_FILES
    CONFIGURE_DEPENDS
    "src/execute_*.cpp"
)
list(REMOVE_ITEM SIGMODPC_SRC ${SIGMODPC_EXECUTE_FILES})

# Allow selecting implementation (default: robinhood)
set(EXECUTE_IMPL "robinhood" CACHE STRING "Which execute variant: robinhood (default), hopscotch, cuckoo")

if(EXECUTE_IMPL STREQUAL "robinhood")
    set(EXECUTE_FILE "src/execute_robinhood.cpp")
    set(EXECUTE_INCLUDES ${hopscotch_map_SOURCE_DIR}/include)
elseif(EXECUTE_IMPL STREQUAL "hopscotch")
    set(EXECUTE_FILE "src/execute_hopscotch.cpp")
    set(EXECUTE_INCLUDES ${hopscotch_map_SOURCE_DIR}/include)
elseif(EXECUTE_IMPL STREQUAL "cuckoo")
    set(EXECUTE_FILE "src/execute_cuckoo.cpp")
    set(EXECUTE_INCLUDES ${libcuckoo_SOURCE_DIR})
else()
    message(WARNING "Unknown EXECUTE_IMPL '${EXECUTE_IMPL}', defaulting to 'robinhood'.")
    set(EXECUTE_FILE "src/execute_robinhood.cpp")
    set(EXECUTE_INCLUDES ${hopscotch_map_SOURCE_DIR}/include)
endif()

message(STATUS "✅ Building with EXECUTE_IMPL=${EXECUTE_IMPL} (${EXECUTE_FILE})")

list(APPEND SIGMODPC_SRC ${EXECUTE_FILE})

# ------------------------------------------------------------
# Compiler optimizations
# ------------------------------------------------------------
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ppc|powerpc|ppc64|ppc64le")
    add_compile_options(-march=native)
endif()

# ------------------------------------------------------------
# Targets
# ------------------------------------------------------------
add_executable(run ${SIGMODPC_SRC} tests/read_sql.cpp)
add_executable(unit_tests ${SIGMODPC_SRC} tests/unit_tests.cpp)
add_executable(build_database tests/build_database.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(run PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(build_database PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(unit_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    add_executable(build_cache ${SIGMODPC_SRC} tests/read_sql.cpp)
    add_executable(fast ${SIGMODPC_SRC} tests/read_sql.cpp)

    target_link_libraries(build_cache PRIVATE re2 fmt range-v3 nlohmann_json::nlohmann_json sqlparser duckdb)
    target_compile_definitions(build_cache PRIVATE TEAMOPT_USE_DUCKDB TEAMOPT_BUILD_CACHE)

    target_link_libraries(fast PRIVATE re2 fmt range-v3 nlohmann_json::nlohmann_json sqlparser)

    # ✅ Only include what's needed for the chosen variant
    target_include_directories(build_cache PRIVATE include ${EXECUTE_INCLUDES})
    target_include_directories(fast PRIVATE include ${EXECUTE_INCLUDES})
endif()

target_link_libraries(run PRIVATE re2 fmt range-v3 nlohmann_json::nlohmann_json sqlparser duckdb)
target_compile_definitions(run PRIVATE TEAMOPT_USE_DUCKDB)
target_link_libraries(build_database PRIVATE fmt duckdb)
target_link_libraries(unit_tests PRIVATE re2 range-v3 fmt Catch2::Catch2WithMain)

target_include_directories(run PRIVATE include)
target_include_directories(build_database PRIVATE include)
target_include_directories(unit_tests PRIVATE include)

# -----------------------------
# Include Hopscotch headers
# -----------------------------
target_include_directories(run PRIVATE ${hopscotch_map_SOURCE_DIR}/include)
target_include_directories(unit_tests PRIVATE ${hopscotch_map_SOURCE_DIR}/include)
target_include_directories(build_database PRIVATE ${hopscotch_map_SOURCE_DIR}/include)

if(TARGET build_cache)
    target_include_directories(build_cache PRIVATE ${hopscotch_map_SOURCE_DIR}/include)
endif()
if(TARGET fast)
    target_include_directories(fast PRIVATE ${hopscotch_map_SOURCE_DIR}/include)
endif()
