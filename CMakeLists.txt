cmake_minimum_required(VERSION 3.16)
project(evk4_pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(MetavisionSDK 5.2.0 EXACT REQUIRED COMPONENTS base core stream)
find_package(MetavisionHAL 5.2.0 EXACT REQUIRED)
find_package(pybind11 CONFIG REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_library(evk4_native STATIC src/tensor_builder.cpp src/pipeline.cpp)
target_include_directories(evk4_native PUBLIC include)
target_link_libraries(evk4_native PUBLIC MetavisionSDK::core MetavisionSDK::stream Metavision::HAL)
target_compile_options(evk4_native PRIVATE -Wall -Wextra)

pybind11_add_module(_evk4_pipeline src/bindings.cpp)
target_link_libraries(_evk4_pipeline PRIVATE evk4_native)

include(CTest)
if(BUILD_TESTING)
  add_executable(tensor_test tests/tensor_test.cpp)
  target_link_libraries(tensor_test PRIVATE evk4_native)
  add_test(NAME tensor_semantics COMMAND tensor_test)

  add_executable(make_fixture tests/make_fixture.cpp)
  target_link_libraries(make_fixture PRIVATE MetavisionSDK::stream)

  add_test(NAME python_integration
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -v)
  set_tests_properties(python_integration PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}/python:${CMAKE_BINARY_DIR}")
endif()
