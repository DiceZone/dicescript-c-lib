cmake_minimum_required(VERSION 3.16)
project(dicescript-c-lib VERSION 0.1.0 LANGUAGES C)

option(DICESCRIPT_BUILD_TESTS "Build DiceScript C99 tests" ON)
option(DICESCRIPT_BUILD_EXAMPLE "Build DiceScript C99 example" ON)

add_library(dicescript STATIC
    src/dicescript.c
    src/dicescript_vm.c
    src/dicescript_vm_parser.c
    src/dicescript_parser.c
)
add_library(dicescript::dicescript ALIAS dicescript)

target_compile_features(dicescript PUBLIC c_std_99)
target_include_directories(dicescript
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE src
)

if(MSVC)
    target_compile_options(dicescript PRIVATE /utf-8 /W4)
    set_source_files_properties(src/dicescript_parser.c PROPERTIES COMPILE_OPTIONS "/W0")
    set_source_files_properties(src/dicescript_vm_parser.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
    target_compile_options(dicescript PRIVATE -Wall -Wextra -Wpedantic)
    set_source_files_properties(src/dicescript_parser.c PROPERTIES COMPILE_OPTIONS "-w")
    set_source_files_properties(src/dicescript_vm_parser.c PROPERTIES COMPILE_OPTIONS "-w")
    target_link_libraries(dicescript PUBLIC m)
endif()

if(DICESCRIPT_BUILD_EXAMPLE)
    add_executable(dicescript-example example/main.c)
    target_link_libraries(dicescript-example PRIVATE dicescript::dicescript)
endif()

if(DICESCRIPT_BUILD_TESTS)
    enable_testing()
    add_executable(dicescript-tests tests/test_dicescript.c)
    target_link_libraries(dicescript-tests PRIVATE dicescript::dicescript)
    if(MSVC)
        target_compile_options(dicescript-tests PRIVATE /utf-8)
    endif()
    add_test(NAME dicescript-tests COMMAND dicescript-tests)
endif()

install(TARGETS dicescript EXPORT dicescript-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT dicescript-targets
    FILE dicescript-targets.cmake
    NAMESPACE dicescript::
    DESTINATION lib/cmake/dicescript
)
