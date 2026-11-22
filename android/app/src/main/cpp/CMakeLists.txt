cmake_minimum_required(VERSION 3.22)
project(slate_android LANGUAGES CXX)
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")
add_library(slate_android SHARED document_jni.cpp
        "${ROOT}/mobile/core/slate_document.cpp" "${ROOT}/src/document.cpp")
target_compile_features(slate_android PRIVATE cxx_std_20)
target_include_directories(slate_android PRIVATE "${ROOT}/mobile/core" "${ROOT}/src")
target_compile_options(slate_android PRIVATE -Wall -Wextra)
