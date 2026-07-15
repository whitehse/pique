cmake_minimum_required(VERSION 3.20)
project(libpqwire LANGUAGES C VERSION 0.1.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

add_library(pqwire STATIC src/pqwire.c src/scram.c)

target_include_directories(pqwire PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

include(GNUInstallDirs)
install(TARGETS pqwire EXPORT pqwire-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT pqwire-targets FILE pqwire-config.cmake NAMESPACE pqwire:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pqwire)

enable_testing()
add_executable(pqwire_test tests/test_pqwire.c)
target_link_libraries(pqwire_test PRIVATE pqwire)
add_test(NAME pqwire_dialectic COMMAND pqwire_test)

add_executable(pqwire_pipeline_recover tests/test_pipeline_recover.c)
target_link_libraries(pqwire_pipeline_recover PRIVATE pqwire)
add_test(NAME pipeline_recover COMMAND pqwire_pipeline_recover)

# Placeholder for examples and more tests as development progresses
option(BUILD_EXAMPLES "Build example programs" OFF)
if(BUILD_EXAMPLES)
    # Add examples later, e.g., using libuv or io_uring
endif()
