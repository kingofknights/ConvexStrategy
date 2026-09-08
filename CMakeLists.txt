cmake_minimum_required(VERSION 3.13.0)
project(StrategyDevelopment CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX)
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} -Wall -Wextra    -Wno-strict-overflow -Wno-unknown-pragmas"
  )
endif()

set(CMAKE_CXX_FLAGS_RELEASE
    "${CMAKE_CXX_FLAGS_RELEASE} -Wall -s -g0 -O3 -mtune=generic -march=x86-64-v2"
)
set(CMAKE_CXX_FLAGS_DEBUG
    "${CMAKE_CXX_FLAGS_DEBUG} -Wall -ggdb -O0 -g3 -mtune=generic -march=x86-64-v2"
)

include_directories(${CMAKE_SOURCE_DIR}/vendor/minix/include)
include_directories(${CMAKE_SOURCE_DIR}/vendor/minix)
link_directories(${CMAKE_SOURCE_DIR}/lib)

add_subdirectory(vendor SYSTEM)
add_subdirectory(Convex)
