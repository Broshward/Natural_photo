# Минимальная версия CMake, требуемая для IDF
cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# Подключение базовой логики сборки ESP-IDF
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Имя вашего проекта
project(greenhouse_photo)


