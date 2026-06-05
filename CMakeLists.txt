cmake_minimum_required(VERSION 3.0)
project(sylar)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(cmake/utils.cmake)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

set(CMAKE_CXX_FLAGS "$ENV{CXXFLAGS} -rdynamic -fPIC -std=c++17 -Wall -Wno-deprecated -Werror -Wno-unused-function -Wno-builtin-macro-redefined -Wno-deprecated-declarations")
set(CMAKE_C_FLAGS "$ENV{CFLAGS} -rdynamic -fPIC -std=c17 -Wall -Wno-deprecated -Werror -Wno-unused-function -Wno-builtin-macro-redefined -Wno-deprecated-declarations")

enable_language(ASM)

include_directories(.)
link_directories(/usr/lib64)

add_compile_options(-Wno-error -Wno-error=address -Wno-error=use-after-free)

message(STATUS "Standard include directories: ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}")

find_package(OpenSSL REQUIRED)
if (OPENSSL_FOUND)
    include_directories(${OPENSSL_INCLUDE_DIR})
endif ()

find_package(ZLIB REQUIRED)
if (ZLIB_FOUND)
    include_directories(${ZLIB_INCLUDE_DIR})
endif ()

set(LIB_SRC
        sylar/address.cc
        sylar/bytearray.cc
        sylar/config.cc
        sylar/dns.cc
        sylar/env.cc
        sylar/fd_manager.cc
        sylar/fiber.cc
        sylar/fcontext/fcontext.S
        sylar/hook.cc
        sylar/http/http.cc
        sylar/http/http_connection.cc
        sylar/http/http_parser.cc
        sylar/http/http_session.cc
        sylar/http/http_server.cc
        sylar/http/servlet.cc
        sylar/http/servlets/status_servlet.cc
        sylar/http/session_data.cc
        sylar/http/ws_connection.cc
        sylar/http/ws_session.cc
        sylar/http/ws_server.cc
        sylar/http/ws_servlet.cc
        sylar/iomanager.cc
        sylar/log.cc
        sylar/mutex.cc
        sylar/scheduler.cc
        sylar/socket.cc
        sylar/stream.cc
        sylar/streams/socket_stream.cc
        sylar/streams/zlib_stream.cc
        sylar/tcp_server.cc
        sylar/thread.cc
        sylar/timer.cc
        sylar/util.cc
        sylar/util/hash_util.cc
)

ragelmaker(sylar/http/http11_parser.rl LIB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/sylar/http)
ragelmaker(sylar/http/httpclient_parser.rl LIB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/sylar/http)
ragelmaker(sylar/uri.rl LIB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/sylar)

message(STATUS ${LIB_SRC})

add_library(sylar STATIC ${LIB_SRC})
target_compile_options(sylar PRIVATE
        -Wno-error=builtin-macro-redefined
)
force_redefine_file_macro_for_sources(sylar)

set(LIBS
        "-Wl,--whole-archive"
        sylar
        "-Wl,--no-whole-archive"
        dl
        pthread
        tbb
        yaml-cpp
        ${ZLIB_LIBRARIES}
        ${OPENSSL_LIBRARIES}
)

sylar_add_executable(my_http_server "samples/my_http_server.cc" sylar "${LIBS}")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)
