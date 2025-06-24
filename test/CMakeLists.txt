# 查找 libevent
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent)

# 查找 src/core 下所有源文件
file(GLOB CORE_SOURCES ${PROJECT_SOURCE_DIR}/src/core/*.c)
file(GLOB TEST_SOURCES *.c)

# 让 test 也能用 <core/xxx.h> 方式找头文件
include_directories(${PROJECT_SOURCE_DIR}/src)
include_directories(${LIBEVENT_INCLUDE_DIRS})

foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src} ${CORE_SOURCES})
    target_link_libraries(${test_name} ${LIBEVENT_LIBRARIES})
endforeach()