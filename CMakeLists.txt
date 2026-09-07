cmake_minimum_required(VERSION 3.13.0)
project(StrategyDevelopment CXX)


add_definitions(-std=c++20)

set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

if(CMAKE_COMPILER_IS_GNUCXX)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra    -Wno-strict-overflow -Wno-unknown-pragmas")
endif()

# Portable (AMD/.45) build flags. For Intel oc20, swap back to: -mtune=haswell -march=ivybridge
# set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Wall -s -g0 -O3 -mtune=haswell -march=ivybridge")
# set(CMAKE_CXX_FLAGS_DEBUG   "${CMAKE_CXX_FLAGS_DEBUG} -Wall -ggdb -O0 -g3 -mtune=haswell -march=ivybridge")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Wall -s -g0 -O3 -mtune=generic -march=x86-64-v2")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wall -ggdb -O0 -g3 -mtune=generic -march=x86-64-v2")



include_directories(
    ${CMAKE_SOURCE_DIR}/vendor/minix/include
    ${CMAKE_SOURCE_DIR}/vendor/minix
)
link_directories(${CMAKE_SOURCE_DIR}/lib)

add_subdirectory(vendor)
add_subdirectory(Convex)
