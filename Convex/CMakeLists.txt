# ponytail: our strategy code linked with vendor objects
add_library(SampleAlgo SHARED MinixStrategy.cpp RatioLeg/RatioLegStrategy.cpp)

set_target_properties(
  SampleAlgo
  PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
             RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
             ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# find_package(fmt REQUIRED)

target_include_directories(
  SampleAlgo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                     ${CMAKE_CURRENT_SOURCE_DIR}/RatioLeg
                     ${PROJECT_SOURCE_DIR}
                     ${PROJECT_SOURCE_DIR}/vendor)

# target_link_libraries(SampleAlgo AlgoBase #                   fmt::fmt
# -static-libstdc++)

find_package(PkgConfig REQUIRED)
# pkg_check_modules(LIBPQXX REQUIRED libpqxx) find_package(PostgreSQL REQUIRED)

# target_include_directories(SampleAlgo PRIVATE ${LIBPQXX_INCLUDE_DIRS}
# ${PostgreSQL_INCLUDE_DIRS})

# Use dynamic linking with proper library order
target_link_libraries(SampleAlgo PRIVATE AlgoBase vendor)

# Let the linker resolve dependencies automatically
target_link_options(SampleAlgo PRIVATE "-Wl,-rpath,\$ORIGIN")

target_compile_options(SampleAlgo PRIVATE -D__STDC_FORMAT_MACROS)
target_compile_definitions(SampleAlgo PRIVATE FMT_HEADER_ONLY)
