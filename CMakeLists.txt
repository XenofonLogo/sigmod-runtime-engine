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

# Include all sources from /src directory. CONFIGURE_DEPENDS can be unreliable.
# Try re-running cmake in case changes are not recognized.
file(GLOB SIGMODPC_SRC
    CONFIGURE_DEPENDS
    "src/*.cpp"
)

# Remove any execute_*.cpp files so we don't compile multiple implementations
file(GLOB SIGMODPC_EXECUTE_FILES
    CONFIGURE_DEPENDS
    "src/execute_*.cpp"
)
list(REMOVE_ITEM SIGMODPC_SRC ${SIGMODPC_EXECUTE_FILES})

# Allow choosing which execute implementation to append (default: robinhood)
set(EXECUTE_IMPL "robinhood" CACHE STRING "Which execute variant: robinhood, hopscotch, cuckoo")
if(EXECUTE_IMPL STREQUAL "default")
    set(EXECUTE_FILE "src/execute_default.cpp")
elseif(EXECUTE_IMPL STREQUAL "unchained")
    set(EXECUTE_FILE "src/execute_unchained_hashtable.cpp")
else()
    message(WARNING "Unknown EXECUTE_IMPL '${EXECUTE_IMPL}', executing default implementation.")
    set(EXECUTE_FILE "src/execute_default.cpp")
endif()

list(APPEND SIGMODPC_SRC ${EXECUTE_FILE})

# Enable server-specific compiler optimizations.
# Use march=native for all but Power servers, which results in the following error:
#    clang++-18: error: unsupported option '-march=' for target 'powerpc64le-unknown-linux-gnu'
# This flag works on other Power systems, but for now, we disable march=native on all Power machines.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ppc|powerpc|ppc64|ppc64le")
    add_compile_options(-march=native)
endif()

# Targets
add_executable(run ${SIGMODPC_SRC} tests/read_sql.cpp)
add_executable(build_database tests/build_database.cpp)

# For all executables
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(run PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(build_database PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(build_cache ${SIGMODPC_SRC} tests/read_sql.cpp)
    add_executable(fast ${SIGMODPC_SRC} tests/read_sql.cpp)

    # --- ΠΡΟΣΘΗΚΗ ΓΙΑ ΤΟ LEADERBOARD ---
    # Δημιουργία του executable 'leaderboard' ως ακριβές αντίγραφο του 'fast'
    add_executable(leaderboard ${SIGMODPC_SRC} tests/read_sql.cpp)
    target_link_libraries(leaderboard PRIVATE re2 fmt range-v3 nlohmann_json::nlohmann_json sqlparser)
    target_include_directories(leaderboard PRIVATE include ${hopscotch_map_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include)
    # -----------------------------------

    target_link_libraries(build_cache PRIVATE re2 fmt range-v3 nlohmann_json::nlohmann_json sqlparser duckdb)
    target_compile_definitions(build_cache PRIVATE TEAMOPT_USE_DUCKDB TEAMOPT_BUILD_CACHE)
    target_link_libraries(fast PRIVATE re2 fmt range-v3 nlohmann_json::nlohmann_json sqlparser)
    target_include_directories(build_cache PRIVATE include ${hopscotch_map_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include)
    target_include_directories(fast PRIVATE include ${hopscotch_map_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include)
endif()

target_link_libraries(run PRIVATE re2 fmt range-v3 nlohmann_json::nlohmann_json sqlparser duckdb)
target_compile_definitions(run PRIVATE TEAMOPT_USE_DUCKDB)
target_link_libraries(build_database PRIVATE fmt duckdb)

# -----------------------------
# Software Tester
# -----------------------------
add_executable(software_tester ${SIGMODPC_SRC} 
    tests/software_tester.cpp
    tests/software_tester/partitioned_build_tests.cpp
    tests/software_tester/work_stealing_tests.cpp
    tests/software_tester/bloom_filter_tests.cpp
    tests/software_tester/late_materialization_tests.cpp
    tests/software_tester/columnar_tests.cpp
    tests/software_tester/zero_copy_int32_tests.cpp
    tests/software_tester/hashtable_algorithms_tests.cpp
    tests/software_tester/indexing_optimization_tests.cpp
)
target_link_libraries(software_tester PRIVATE re2 range-v3 fmt Catch2::Catch2WithMain)
target_compile_definitions(software_tester PRIVATE TEAMOPT_BUILD_CACHE)
target_include_directories(software_tester PRIVATE include ${hopscotch_map_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include)

target_include_directories(run PRIVATE include ${hopscotch_map_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include)
target_include_directories(build_database PRIVATE include ${hopscotch_map_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include)
