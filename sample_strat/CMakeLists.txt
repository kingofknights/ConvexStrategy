add_library(SampleAlgo SHARED MinixStrategy.cpp Ratio2Leg/Ratio2LegStrategy.cpp
                              Ratio3Leg/Ratio3LegStrategy.cpp
                              Ratio4Leg/Ratio4LegStrategy.cpp
                              Ratio5Leg/Ratio5LegStrategy.cpp
                              Ratio6Leg/Ratio6LegStrategy.cpp
                              Butterfly/ButterflyStrategy.cpp
                              ConversionReversal/ConversionReversalStrategy.cpp
                              BoxSpread/BoxSpreadStrategy.cpp
                              order_instance.cpp
                              PortfolioOrderManager.cpp)

set_target_properties(
  SampleAlgo
  PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
             RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
             ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# find_package(fmt REQUIRED)

target_include_directories(SampleAlgo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                              ${CMAKE_CURRENT_SOURCE_DIR}/Ratio2Leg
                                              ${CMAKE_CURRENT_SOURCE_DIR}/Ratio3Leg
                                              ${CMAKE_CURRENT_SOURCE_DIR}/Ratio4Leg
                                              ${CMAKE_CURRENT_SOURCE_DIR}/Ratio5Leg
                                              ${CMAKE_CURRENT_SOURCE_DIR}/Ratio6Leg
                                              ${CMAKE_CURRENT_SOURCE_DIR}/Butterfly
                                              ${CMAKE_CURRENT_SOURCE_DIR}/ConversionReversal
                                              ${CMAKE_CURRENT_SOURCE_DIR}/BoxSpread
                                              ${PROJECT_SOURCE_DIR})

# target_link_libraries(SampleAlgo AlgoBase #                   fmt::fmt
# -static-libstdc++)

find_package(PkgConfig REQUIRED)
# pkg_check_modules(LIBPQXX REQUIRED libpqxx) find_package(PostgreSQL REQUIRED)

# target_include_directories(SampleAlgo PRIVATE ${LIBPQXX_INCLUDE_DIRS}
# ${PostgreSQL_INCLUDE_DIRS})

# Use dynamic linking with proper library order
target_link_libraries(SampleAlgo PRIVATE AlgoBase)

# Let the linker resolve dependencies automatically
target_link_options(SampleAlgo PRIVATE "-Wl,-rpath,\$ORIGIN")

target_compile_options(SampleAlgo PRIVATE -D__STDC_FORMAT_MACROS)
target_compile_definitions(SampleAlgo PRIVATE FMT_HEADER_ONLY)
