# ponytail: vendor library for OMS order and portfolio management
add_library(vendor OBJECT
    order_instance.cpp
    PortfolioOrderManager.cpp
)

set_target_properties(vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(vendor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/Convex
)
