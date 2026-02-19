cmake_minimum_required(VERSION 3.16)

project(SigmodContest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

Include(FetchContent)

# Minimal third-party dependencies required for the build and tests.
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
    range-v3
    URL https://github.com/ericniebler/range-v3/archive/refs/tags/0.12.0.tar.gz
)
FetchContent_MakeAvailable(range-v3)

FetchContent_Declare(
    fmtlib
    URL https://github.com/fmtlib/fmt/releases/download/11.1.3/fmt-11.1.3.zip
)
FetchContent_MakeAvailable(fmtlib)

# Tessil hopscotch-map is not required for minimal builds; removed.

FetchContent_Declare(
    sql-parser
    URL https://github.com/a858438680/sql-parser/archive/refs/tags/win-port-2.tar.gz
)
set(HSQL_ENABLE_WERROR OFF)
FetchContent_MakeAvailable(sql-parser)

# Collect project sources and select the execute implementation.
file(GLOB SIGMODPC_SRC CONFIGURE_DEPENDS "src/*.cpp")
# Only the default execute implementation is used in this minimal build.
set(EXECUTE_FILE "src/execute_default.cpp")
list(APPEND SIGMODPC_SRC ${EXECUTE_FILE})

# Compiler tuning: enable -march=native on non-Power architectures.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ppc|powerpc|ppc64|ppc64le")
    add_compile_options(-march=native)
endif()

# Primary runtime executable used by README: `fast`
add_executable(fast ${SIGMODPC_SRC} tests/read_sql.cpp)
target_link_libraries(fast PRIVATE re2 fmt range-v3 nlohmann_json::nlohmann_json sqlparser)
target_include_directories(fast PRIVATE include)

# Lightweight software tester (unit/integration tests)
add_executable(software_tester ${SIGMODPC_SRC}
    tests/software_tester.cpp
    tests/software_tester/work_stealing_tests.cpp
    tests/software_tester/bloom_filter_tests.cpp
    tests/software_tester/late_materialization_tests.cpp
    tests/software_tester/columnar_tests.cpp
    tests/software_tester/zero_copy_int32_tests.cpp
    tests/software_tester/hashtable_algorithms_tests.cpp
    tests/software_tester/indexing_optimization_tests.cpp
)
target_link_libraries(software_tester PRIVATE re2 range-v3 fmt Catch2::Catch2WithMain)
target_include_directories(software_tester PRIVATE include)
