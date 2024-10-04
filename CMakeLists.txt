# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(detector)

#mqtt authentication
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "${project_dir}/certs/esp32.crt" TEXT)
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "${project_dir}/certs/esp32.key" TEXT)
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "${project_dir}/certs/ca.crt" TEXT)
