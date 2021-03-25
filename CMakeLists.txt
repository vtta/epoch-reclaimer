cmake_minimum_required(VERSION 3.11)
project(epoch-reclaimer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-long-long -pedantic -fPIC -march=native")
set(CMAKE_ENABLE_COMPILE_COMMANDS "ON")
add_definitions(-Wno-deprecated-declarations)

set(PMDK_LIB_PATH "/opt/local/lib" CACHE STRING "PMDK lib install path")
set(PMDK_HEADER_PATH "/opt/local/include" CACHE STRING "PMDK header install path")
message("-- Build with persistent memory support, PMDK lib path:" ${PMDK_LIB_PATH} ", PMDK header path: " ${PMDK_HEADER_PATH})
include_directories(${PMDK_HEADER_PATH})
add_library(pmemobj STATIC IMPORTED)
set_property(TARGET pmemobj PROPERTY IMPORTED_LOCATION ${PMDK_LIB_PATH}/libpmemobj.a)
add_definitions(-DPMEM)

add_library(epoch_reclaimer epoch_manager.cpp garbage_list.cpp tls_thread.cpp)
target_link_libraries(epoch_reclaimer pmemobj)