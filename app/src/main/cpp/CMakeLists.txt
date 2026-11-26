cmake_minimum_required(VERSION 3.22.1)
project(dbxcomp LANGUAGES CXX)

find_package(oboe REQUIRED CONFIG)

add_library(dbxcomp SHARED audio_engine.cpp)

target_link_libraries(dbxcomp oboe::oboe log android)
