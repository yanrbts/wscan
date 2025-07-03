# 查找 libevent
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent)

# --- 新增部分开始 ---
# 查找 libevent 的 OpenSSL 扩展库
# `evhttp_connection_base_ssl_new` 函数位于此库中，需要它来支持 HTTPS。
pkg_check_modules(LIBEVENT_OPENSSL REQUIRED libevent_openssl)
# --- 新增部分结束 ---

# 查找 OpenSSL 库
find_package(OpenSSL REQUIRED) # 确保 OpenSSL 库也被找到

# 查找 libcurl 库
find_package(CURL REQUIRED)

# 查找 src/core 下所有源文件
file(GLOB CORE_SOURCES ${PROJECT_SOURCE_DIR}/src/core/*.c)
file(GLOB TEST_SOURCES *.c)

# 让 test 也能用 <core/xxx.h> 方式找头文件
include_directories(${PROJECT_SOURCE_DIR}/src)
include_directories(${LIBEVENT_INCLUDE_DIRS})
# --- 新增部分开始 ---
# 添加 libevent_openssl 的头文件路径，确保编译器能找到相关定义
include_directories(${LIBEVENT_OPENSSL_INCLUDE_DIRS})
# 添加 OpenSSL 的头文件路径
include_directories(${OPENSSL_INCLUDE_DIR})
# 添加 libcurl 的头文件路径
include_directories(${CURL_INCLUDE_DIRS})
# --- 新增部分结束 ---
add_definitions(-DLOG_USE_COLOR)
foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src} ${CORE_SOURCES})
    
    # 链接库：现在需要把 libevent_openssl 也加进去
    # 链接顺序很重要：依赖其他库的库（如 libevent_openssl）应放在其所依赖的库（如 libevent 和 OpenSSL）之前。
    target_link_libraries(${test_name} 
        ${LIBEVENT_OPENSSL_LIBRARIES} # 确保链接到 libevent 的 OpenSSL 模块
        ${LIBEVENT_LIBRARIES}         # 核心 Libevent 库
        ${OPENSSL_LIBRARIES}          # OpenSSL 库
        ${CURL_LIBRARIES}             # libcurl 库
    )
endforeach()