# ponytail: internal minix platform library for OMS order and portfolio
# management
add_library(minix OBJECT src/order_instance.cpp src/PortfolioOrderManager.cpp)

set_target_properties(minix PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(
  minix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include
               ${PROJECT_SOURCE_DIR}/vendor)
