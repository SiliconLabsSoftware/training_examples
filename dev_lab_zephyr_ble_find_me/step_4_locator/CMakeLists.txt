# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_find_me_4_locator)
add_compile_definitions(CMAKE_PROJECT_NAME="${PROJECT_NAME}")

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE
  ${app_sources}
  )
