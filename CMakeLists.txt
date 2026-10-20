cmake_minimum_required(VERSION 3.5)
# Explicit layer roots: IDF still sees normal component names in REQUIRES.
set(COMPONENTS main)
set(COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/main"
    "${CMAKE_CURRENT_LIST_DIR}/components/application"
    "${CMAKE_CURRENT_LIST_DIR}/components/services"
    "${CMAKE_CURRENT_LIST_DIR}/components/domain"
    "${CMAKE_CURRENT_LIST_DIR}/components/protocol"
    "${CMAKE_CURRENT_LIST_DIR}/components/drivers"
    "${CMAKE_CURRENT_LIST_DIR}/components/contracts"
    "$ENV{IDF_PATH}/components"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "1.3.0")
project(0_helloworld)
