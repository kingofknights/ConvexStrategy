project(ConvexMoss)

find_package(fmt REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PostgreSQL REQUIRED)
add_library(${PROJECT_NAME} SHARED MinixStrategy.cpp
                                   ManualOrder/ManualOrder.cpp
                                   RatioLeg/RatioLegStrategy.cpp
                                   SquareOff/SquareOff.cpp
                                   SquareOff/TradeDb.cpp)

target_include_directories(
  ${PROJECT_NAME}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/ManualOrder
          ${CMAKE_CURRENT_SOURCE_DIR}/RatioLeg
          ${CMAKE_CURRENT_SOURCE_DIR}/SquareOff
          ${PROJECT_SOURCE_DIR}
          ${PROJECT_SOURCE_DIR}/vendor/minix
          ${PROJECT_SOURCE_DIR}/vendor/minix/include
          ${PROJECT_SOURCE_DIR}/vendor)

target_link_libraries(${PROJECT_NAME} PRIVATE AlgoBase minix)
target_link_libraries(${PROJECT_NAME} PRIVATE fmt::fmt-header-only)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(${PROJECT_NAME} PRIVATE PostgreSQL::PostgreSQL)
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,-rpath,\$ORIGIN")

target_compile_options(${PROJECT_NAME} PRIVATE -D__STDC_FORMAT_MACROS)
