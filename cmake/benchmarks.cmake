# Hotel, social, media microservices, gomaxprocs, stress-ng, and the runtime
# wrappers. Invoked from the top-level CMakeLists.
# Train Ticket is a Maven tree and is built only when java and mvn are on PATH.
# Perturb is Python. The Jaeger all-in-one under benchmarks/runtime/jaeger/bin is
# already built; its sources are not in the tree.

find_program(GO_EXECUTABLE go REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

set(HOTEL ${CMAKE_SOURCE_DIR}/benchmarks/deathstar-hotelReservations)
set(SOCIAL ${CMAKE_SOURCE_DIR}/benchmarks/deathstar-socialNetwork)
set(RUNTIME ${CMAKE_SOURCE_DIR}/benchmarks/runtime)

# --- Go: hotel services and both operators ---------------------------------

file(GLOB HOTEL_CMD_DIRS LIST_DIRECTORIES true ${HOTEL}/cmd/*)
set(HOTEL_BINS ${HOTEL}/bin/loadgen)
foreach(dir ${HOTEL_CMD_DIRS})
  if(IS_DIRECTORY ${dir})
    get_filename_component(name ${dir} NAME)
    list(APPEND HOTEL_BINS ${HOTEL}/bin/${name})
  endif()
endforeach()

file(GLOB_RECURSE HOTEL_GO CONFIGURE_DEPENDS
  ${HOTEL}/operate/*.go ${HOTEL}/cmd/*.go ${HOTEL}/services/*.go
  ${HOTEL}/boot/*.go ${HOTEL}/tracing/*.go ${HOTEL}/dialer/*.go
  ${HOTEL}/data/*.go ${HOTEL}/tls/*.go ${HOTEL}/tune/*.go ${HOTEL}/cachepolicy/*.go)
add_custom_command(
  OUTPUT ${HOTEL_BINS}
  COMMAND ${CMAKE_SOURCE_DIR}/cmake/build-hotel.sh ${HOTEL} ${HOTEL}/bin
  DEPENDS ${CMAKE_SOURCE_DIR}/cmake/build-hotel.sh ${HOTEL}/go.mod ${HOTEL}/go.sum ${HOTEL_GO}
  COMMENT "go build hotel"
  VERBATIM)
add_custom_target(hotel ALL DEPENDS ${HOTEL_BINS})

set(GOPERATE_BINS ${SOCIAL}/goperate/bin/goperate ${SOCIAL}/goperate/bin/loadgen)
file(GLOB GOPERATE_GO CONFIGURE_DEPENDS ${SOCIAL}/goperate/*.go)
add_custom_command(
  OUTPUT ${GOPERATE_BINS}
  COMMAND ${CMAKE_SOURCE_DIR}/cmake/build-goperate.sh ${SOCIAL}/goperate
  DEPENDS ${CMAKE_SOURCE_DIR}/cmake/build-goperate.sh ${SOCIAL}/goperate/go.mod ${SOCIAL}/goperate/go.sum ${GOPERATE_GO}
  COMMENT "go build goperate"
  VERBATIM)
add_custom_target(goperate ALL DEPENDS ${GOPERATE_BINS})

set(GOMAX ${CMAKE_SOURCE_DIR}/benchmarks/gomaxprocs)
set(GOMAX_BINS ${GOMAX}/bin/server ${GOMAX}/bin/operate)
file(GLOB_RECURSE GOMAX_GO CONFIGURE_DEPENDS ${GOMAX}/cmd/*.go ${GOMAX}/operate/*.go)
add_custom_command(
  OUTPUT ${GOMAX_BINS}
  COMMAND ${CMAKE_SOURCE_DIR}/cmake/build-gomaxprocs.sh ${GOMAX}
  DEPENDS ${CMAKE_SOURCE_DIR}/cmake/build-gomaxprocs.sh ${GOMAX}/go.mod ${GOMAX_GO}
  COMMENT "go build gomaxprocs"
  VERBATIM)
add_custom_target(gomaxprocs ALL DEPENDS ${GOMAX_BINS})

# stress-ng's Makefile writes the binary and its objects into its own tree.
set(STRESS ${CMAKE_SOURCE_DIR}/benchmarks/stress-ng)
file(GLOB_RECURSE STRESS_SRC CONFIGURE_DEPENDS ${STRESS}/*.c ${STRESS}/*.h ${STRESS}/Makefile)
list(FILTER STRESS_SRC EXCLUDE REGEX "/configs/")
add_custom_command(
  OUTPUT ${STRESS}/stress-ng
  # config.h is generated for this host. `make` refreshes it when it is missing.
  # A config.h copied from another machine can define headers this libc does not
  # ship (termio.h); delete it and re-run the build if compile errors name one.
  COMMAND make -C ${STRESS} -j
  DEPENDS ${STRESS_SRC}
  COMMENT "make stress-ng"
  VERBATIM)
add_custom_target(stress_ng ALL DEPENDS ${STRESS}/stress-ng)

# --- Runtime wrappers (the real servers are local binaries they exec) ------

function(runtime_wrapper target src outdir outname)
  add_executable(${target} ${src})
  set_target_properties(${target} PROPERTIES
    OUTPUT_NAME ${outname}
    RUNTIME_OUTPUT_DIRECTORY ${outdir})
  target_compile_options(${target} PRIVATE -Wall -Wextra -O2)
endfunction()

runtime_wrapper(runtime_mongod ${RUNTIME}/mongodb/mongod.c ${RUNTIME}/mongodb mongod)
runtime_wrapper(runtime_redis ${RUNTIME}/redis/redis-server.c ${RUNTIME}/redis redis-server)
runtime_wrapper(runtime_memcached ${RUNTIME}/memcached/memcached-server.c ${RUNTIME}/memcached memcached-server)
foreach(port 11211 11212 11213 11214 11311 11312 11313)
  if(NOT EXISTS ${RUNTIME}/memcached/${port})
    file(CREATE_LINK memcached-server ${RUNTIME}/memcached/${port} SYMBOLIC)
  endif()
endforeach()
runtime_wrapper(runtime_rabbitmq ${RUNTIME}/rabbitmq/rabbitmq-server.c ${RUNTIME}/rabbitmq rabbitmq-server)

# --- Social network C++ services -------------------------------------------

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
cmake_policy(SET CMP0169 OLD)
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_LINTING OFF CACHE BOOL "" FORCE)
set(BUILD_MOCKTRACER OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_API_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_SSL_SUPPORT OFF CACHE BOOL "" FORCE)
set(REDIS_PLUS_PLUS_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(REDIS_PLUS_PLUS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(REDIS_PLUS_PLUS_USE_TLS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(opentracing
  GIT_REPOSITORY https://github.com/opentracing/opentracing-cpp.git
  GIT_TAG v1.6.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(opentracing)

FetchContent_Declare(hiredis
  GIT_REPOSITORY https://github.com/redis/hiredis.git
  GIT_TAG v1.2.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(hiredis)
# The generated hiredis-config.cmake points at the install prefix, which this
# build never installs into. Keep find_package(hiredis) from picking it up.
unset(hiredis_DIR CACHE)

# Populate only: these upstream projects either force a package manager (Jaeger
# uses Hunter), search for an installed hiredis (redis-plus-plus), or search for
# a library that does not exist until after the build (SimpleAmqpClient).
macro(mercury_populate name)
  FetchContent_GetProperties(${name})
  if(NOT ${name}_POPULATED)
    FetchContent_Populate(${name})
  endif()
endmacro()

FetchContent_Declare(redispp
  GIT_REPOSITORY https://github.com/sewenew/redis-plus-plus.git
  GIT_TAG 1.3.13
  GIT_SHALLOW TRUE)
mercury_populate(redispp)
set(_hiredis_inc ${CMAKE_BINARY_DIR}/hiredis-include)
file(MAKE_DIRECTORY ${_hiredis_inc})
if(NOT EXISTS ${_hiredis_inc}/hiredis)
  file(CREATE_LINK ${hiredis_SOURCE_DIR} ${_hiredis_inc}/hiredis SYMBOLIC)
endif()
set(_redis_features ${CMAKE_BINARY_DIR}/redis-features/sw/redis++)
file(MAKE_DIRECTORY ${_redis_features})
file(WRITE ${_redis_features}/hiredis_features.h
  "#define REDIS_PLUS_PLUS_HAS_redisEnableKeepAliveWithInterval\n")
set(_rpp ${redispp_SOURCE_DIR}/src/sw/redis++)
add_library(redispp_static STATIC
  ${_rpp}/command.cpp ${_rpp}/command_options.cpp ${_rpp}/connection.cpp
  ${_rpp}/connection_pool.cpp ${_rpp}/crc16.cpp ${_rpp}/errors.cpp
  ${_rpp}/pipeline.cpp ${_rpp}/redis.cpp ${_rpp}/redis_cluster.cpp
  ${_rpp}/redis_uri.cpp ${_rpp}/reply.cpp ${_rpp}/sentinel.cpp
  ${_rpp}/shards.cpp ${_rpp}/shards_pool.cpp ${_rpp}/subscriber.cpp
  ${_rpp}/transaction.cpp ${_rpp}/patterns/redlock.cpp)
target_include_directories(redispp_static PUBLIC
  ${redispp_SOURCE_DIR}/src
  ${_rpp}/no_tls ${_rpp}/cxx17
  ${CMAKE_BINARY_DIR}/redis-features
  ${_hiredis_inc})
target_link_libraries(redispp_static PUBLIC hiredis Threads::Threads)
target_compile_features(redispp_static PUBLIC cxx_std_17)
# HomeTimelineService calls RedisCluster::get_shards_pool(). redis-plus-plus keeps
# the pool private; 1.3 stores it in a unique_ptr, so the accessor returns the pointer.
file(READ ${_rpp}/redis_cluster.h _cluster_h)
if(NOT _cluster_h MATCHES "get_shards_pool")
  string(REPLACE "    ShardsPoolUPtr _pool;"
    "public:\n    ShardsPool *get_shards_pool() { return _pool.get(); }\nprivate:\n    ShardsPoolUPtr _pool;"
    _cluster_h "${_cluster_h}")
  file(WRITE ${_rpp}/redis_cluster.h "${_cluster_h}")
endif()

FetchContent_Declare(rabbitmq-c
  GIT_REPOSITORY https://github.com/alanxz/rabbitmq-c.git
  GIT_TAG v0.15.0
  GIT_SHALLOW TRUE)
# rabbitmq-c force-writes CMAKE_BUILD_TYPE=Release when it is empty, which would
# apply -O3 -DNDEBUG to mercury. Give it a type for its own targets only.
set(_mercury_build_type "${CMAKE_BUILD_TYPE}")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE None)
endif()
FetchContent_MakeAvailable(rabbitmq-c)
if(NOT _mercury_build_type)
  unset(CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "" CACHE STRING "Choose the type of build." FORCE)
endif()

FetchContent_Declare(jaeger_cpp
  GIT_REPOSITORY https://github.com/jaegertracing/jaeger-client-cpp.git
  GIT_TAG v0.9.0
  GIT_SHALLOW TRUE)
mercury_populate(jaeger_cpp)

FetchContent_Declare(cpp_jwt
  GIT_REPOSITORY https://github.com/arun11299/cpp-jwt.git
  GIT_TAG v1.5.1
  GIT_SHALLOW TRUE)
mercury_populate(cpp_jwt)

FetchContent_Declare(simple_amqp
  GIT_REPOSITORY https://github.com/alanxz/SimpleAmqpClient.git
  GIT_TAG v2.5.1
  GIT_SHALLOW TRUE)
mercury_populate(simple_amqp)

FetchContent_Declare(cpp_redis
  GIT_REPOSITORY https://github.com/cpp-redis/cpp_redis.git
  GIT_TAG 4.3.1
  GIT_SHALLOW TRUE)
mercury_populate(cpp_redis)

FetchContent_Declare(tacopie
  GIT_REPOSITORY https://github.com/cpp-redis/tacopie.git
  GIT_TAG 3.2.0
  GIT_SHALLOW TRUE)
mercury_populate(tacopie)

# Boost 1.91 ships boost_system as headers only, so it is not a component.
cmake_policy(SET CMP0167 NEW)
cmake_policy(SET CMP0169 OLD)
find_package(Boost REQUIRED COMPONENTS log log_setup program_options chrono thread filesystem)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(yaml-cpp REQUIRED)
pkg_check_modules(MONGOC REQUIRED IMPORTED_TARGET mongoc2)
pkg_check_modules(LIBMEMCACHED REQUIRED IMPORTED_TARGET libmemcached)
pkg_check_modules(THRIFT_PC REQUIRED IMPORTED_TARGET thrift)
find_library(AMQPCPP_LIB amqpcpp REQUIRED)
find_library(EVENT_LIB event REQUIRED)

if(NOT TARGET rabbitmq-static)
  message(FATAL_ERROR "rabbitmq-c did not produce the rabbitmq-static target")
endif()

add_library(SimpleAmqpClient STATIC
  ${simple_amqp_SOURCE_DIR}/src/AmqpException.cpp
  ${simple_amqp_SOURCE_DIR}/src/Channel.cpp
  ${simple_amqp_SOURCE_DIR}/src/ChannelImpl.cpp
  ${simple_amqp_SOURCE_DIR}/src/BasicMessage.cpp
  ${simple_amqp_SOURCE_DIR}/src/AmqpLibraryException.cpp
  ${simple_amqp_SOURCE_DIR}/src/AmqpResponseLibraryException.cpp
  ${simple_amqp_SOURCE_DIR}/src/Envelope.cpp
  ${simple_amqp_SOURCE_DIR}/src/MessageReturnedException.cpp
  ${simple_amqp_SOURCE_DIR}/src/Table.cpp
  ${simple_amqp_SOURCE_DIR}/src/TableImpl.cpp)
target_include_directories(SimpleAmqpClient PUBLIC
  ${simple_amqp_SOURCE_DIR}/src
  ${rabbitmq-c_SOURCE_DIR}/librabbitmq
  ${rabbitmq-c_BINARY_DIR}/librabbitmq)
target_link_libraries(SimpleAmqpClient PUBLIC rabbitmq-static Boost::chrono)
target_compile_features(SimpleAmqpClient PUBLIC cxx_std_11)

set(_saved_project_version "${PROJECT_VERSION}")
set(PROJECT_VERSION 0.9.0)
set(JAEGERTRACING_WITH_YAML_CPP 1)
file(MAKE_DIRECTORY ${jaeger_cpp_BINARY_DIR}/src/jaegertracing)
configure_file(
  ${jaeger_cpp_SOURCE_DIR}/src/jaegertracing/Constants.h.in
  ${jaeger_cpp_BINARY_DIR}/src/jaegertracing/Constants.h
  @ONLY)
set(PROJECT_VERSION "${_saved_project_version}")

file(GLOB_RECURSE JAEGER_SRC ${jaeger_cpp_SOURCE_DIR}/src/jaegertracing/*.cpp)
list(FILTER JAEGER_SRC EXCLUDE REGEX "Test\\.cpp$|/testutils/")
add_library(jaeger_client STATIC ${JAEGER_SRC})
# /usr/local/include holds an older Thrift than libthrift.so. CMake drops a plain
# -I/usr/include, so point at the system headers through a directory of our own.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/system-thrift)
if(NOT EXISTS ${CMAKE_BINARY_DIR}/system-thrift/thrift)
  file(CREATE_LINK /usr/include/thrift ${CMAKE_BINARY_DIR}/system-thrift/thrift SYMBOLIC)
endif()

target_include_directories(jaeger_client BEFORE PUBLIC
  ${CMAKE_BINARY_DIR}/system-thrift
  ${jaeger_cpp_SOURCE_DIR}/src
  ${jaeger_cpp_BINARY_DIR}/src
  ${opentracing_SOURCE_DIR}/include
  ${opentracing_BINARY_DIR}/include
  ${opentracing_SOURCE_DIR}/3rd_party/include)
target_link_libraries(jaeger_client PUBLIC
  opentracing-static PkgConfig::THRIFT_PC yaml-cpp nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(jaeger_client PRIVATE -Wno-deprecated-declarations -Wno-unused-parameter)
target_compile_features(jaeger_client PUBLIC cxx_std_14)

file(GLOB_RECURSE CPP_REDIS_SRC ${cpp_redis_SOURCE_DIR}/sources/*.cpp)
file(GLOB_RECURSE TACOPIE_SRC ${tacopie_SOURCE_DIR}/sources/*.cpp)
list(FILTER CPP_REDIS_SRC EXCLUDE REGEX "/windows/")
list(FILTER TACOPIE_SRC EXCLUDE REGEX "/windows/")
add_library(tacopie STATIC ${TACOPIE_SRC})
target_include_directories(tacopie PUBLIC ${tacopie_SOURCE_DIR}/includes)
target_compile_features(tacopie PUBLIC cxx_std_11)
target_compile_options(tacopie PRIVATE -Wno-deprecated)
add_library(cpp_redis STATIC ${CPP_REDIS_SRC})
target_include_directories(cpp_redis PUBLIC ${cpp_redis_SOURCE_DIR}/includes ${tacopie_SOURCE_DIR}/includes)
target_link_libraries(cpp_redis PUBLIC tacopie Threads::Threads)
target_compile_features(cpp_redis PUBLIC cxx_std_11)
# Header lambdas capture this with [=], which C++20 deprecates.
target_compile_options(cpp_redis PRIVATE -Wno-deprecated)

# libmongoc 1 headers were <mongoc.h> and <bson.h>, and bson_as_json. libmongoc 2
# nests the headers and renamed that function. Wrappers sit ahead of the system
# includes so the services keep compiling.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/bsoncompat ${CMAKE_BINARY_DIR}/mongocompat/bson)
file(WRITE ${CMAKE_BINARY_DIR}/bsoncompat/bson.h "#include <bson/bson.h>\n")
file(WRITE ${CMAKE_BINARY_DIR}/bsoncompat/mongoc.h "#include <mongoc/mongoc.h>\n")
file(WRITE ${CMAKE_BINARY_DIR}/mongocompat/bson/bson.h
"#include_next <bson/bson.h>\n#define bson_as_json bson_as_legacy_extended_json\n")

file(GLOB SOCIAL_THRIFT_SRC ${SOCIAL}/gen-cpp/*.cpp)
add_library(social_thrift STATIC ${SOCIAL_THRIFT_SRC})
target_include_directories(social_thrift BEFORE PUBLIC ${CMAKE_BINARY_DIR}/system-thrift ${SOCIAL}/gen-cpp)
target_link_libraries(social_thrift PUBLIC PkgConfig::THRIFT_PC)
target_compile_options(social_thrift PRIVATE -Wno-unused-parameter -Wno-deprecated-declarations)
target_compile_features(social_thrift PUBLIC cxx_std_17)
# Thrift 0.22 still inherits std::iterator, which libstdc++ deprecates. The
# project default is C++20, so pin the services to C++17 and drop that warning.
set_target_properties(social_thrift PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

add_library(social_runtime INTERFACE)
target_link_libraries(social_runtime INTERFACE
  social_thrift jaeger_client SimpleAmqpClient redispp_static
  PkgConfig::MONGOC PkgConfig::LIBMEMCACHED
  Boost::log Boost::log_setup Boost::program_options Boost::thread Boost::filesystem
  OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
target_include_directories(social_runtime BEFORE INTERFACE
  ${CMAKE_BINARY_DIR}/system-thrift
  ${CMAKE_BINARY_DIR}/mongocompat
  ${SOCIAL}/src ${SOCIAL}/gen-cpp ${SOCIAL}/third_party/PicoSHA2
  ${cpp_jwt_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/bsoncompat)
target_compile_definitions(social_runtime INTERFACE BOOST_ALL_DYN_LINK BOOST_LOG_DYN_LINK)
target_compile_features(social_runtime INTERFACE cxx_std_17)

function(social_bin name)
  cmake_parse_arguments(S "" "" "SRC;LIBS" ${ARGN})
  add_executable(${name} ${S_SRC})
  target_link_libraries(${name} PRIVATE social_runtime ${S_LIBS})
  target_compile_options(${name} PRIVATE -O2 -Wno-unused-parameter -Wno-deprecated-declarations)
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${SOCIAL}/bin
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
endfunction()

social_bin(TextService SRC ${SOCIAL}/src/TextService/TextService.cpp)
social_bin(UniqueIdService SRC ${SOCIAL}/src/UniqueIdService/UniqueIdService.cpp)
social_bin(UserService SRC ${SOCIAL}/src/UserService/UserService.cpp)
social_bin(SocialGraphService SRC ${SOCIAL}/src/SocialGraphService/SocialGraphService.cpp)
social_bin(PostStorageService SRC ${SOCIAL}/src/PostStorageService/PostStorageService.cpp)
social_bin(UserTimelineService SRC ${SOCIAL}/src/UserTimelineService/UserTimelineService.cpp)
social_bin(ComposePostService SRC ${SOCIAL}/src/ComposePostService/ComposePostService.cpp)
social_bin(UserMentionService SRC ${SOCIAL}/src/UserMentionService/UserMentionService.cpp)
social_bin(UrlShortenService SRC ${SOCIAL}/src/UrlShortenService/UrlShortenService.cpp)
social_bin(MediaService SRC ${SOCIAL}/src/MediaService/MediaService.cpp)
social_bin(HomeTimelineService SRC ${SOCIAL}/src/HomeTimelineService/HomeTimelineService.cpp)
social_bin(WriteHomeTimelineService
  SRC ${SOCIAL}/src/WriteHomeTimelineService/WriteHomeTimelineService.cpp
  LIBS cpp_redis ${AMQPCPP_LIB} ${EVENT_LIB})

add_executable(social_operate
  ${SOCIAL}/operate/operate.cpp ${SOCIAL}/operate/routes.cpp ${SOCIAL}/operate/graph.cpp)
target_include_directories(social_operate BEFORE PRIVATE ${CMAKE_BINARY_DIR}/system-thrift ${SOCIAL}/operate ${SOCIAL}/gen-cpp)
target_link_libraries(social_operate PRIVATE social_thrift Threads::Threads)
target_compile_options(social_operate PRIVATE -O2 -Wno-unused-parameter -Wno-deprecated-declarations)
set_target_properties(social_operate PROPERTIES
  OUTPUT_NAME operate
  RUNTIME_OUTPUT_DIRECTORY ${SOCIAL}/operate
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)

# --- Media microservices ---------------------------------------------------
# Upstream cmake looks for libjaegertracing.so, libmongoc-1.0 and a Thrift
# installed under /usr/local. Build the services here against the same
# libraries as the social network. Generated code is Thrift 0.12 and includes
# <thrift/stdcxx.h> and names apache::thrift::stdcxx, which the Thrift that
# ships with this system no longer has. The shim below provides that header.

set(MEDIA ${CMAKE_SOURCE_DIR}/benchmarks/deathstar-mediaMicroservices)
set(MEDIA_GEN ${MEDIA}/gen-cpp)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/media-compat/thrift)
file(WRITE ${CMAKE_BINARY_DIR}/media-compat/thrift/stdcxx.h
"#pragma once
#include <memory>
namespace apache { namespace thrift { namespace stdcxx {
using std::shared_ptr;
using std::weak_ptr;
using std::enable_shared_from_this;
template <class T, class... Args>
std::shared_ptr<T> make_shared(Args&&... args) {
  return std::make_shared<T>(std::forward<Args>(args)...);
}
}}}
")

function(media_bin name)
  cmake_parse_arguments(M "" "" "SRC" ${ARGN})
  add_executable(media_${name} ${M_SRC})
  target_link_libraries(media_${name} PRIVATE
    jaeger_client PkgConfig::THRIFT_PC nlohmann_json::nlohmann_json
    PkgConfig::MONGOC PkgConfig::LIBMEMCACHED
    Boost::log Boost::log_setup
    OpenSSL::SSL OpenSSL::Crypto
    cpp_redis Threads::Threads)
  target_include_directories(media_${name} BEFORE PRIVATE
    ${CMAKE_BINARY_DIR}/media-compat
    ${CMAKE_BINARY_DIR}/system-thrift
    ${CMAKE_BINARY_DIR}/mongocompat
    ${MEDIA}/src ${MEDIA_GEN}
    ${cpp_jwt_SOURCE_DIR}/include
    ${cpp_redis_SOURCE_DIR}/includes
    ${tacopie_SOURCE_DIR}/includes
    ${CMAKE_BINARY_DIR}/bsoncompat)
  target_compile_definitions(media_${name} PRIVATE BOOST_ALL_DYN_LINK BOOST_LOG_DYN_LINK)
  target_compile_options(media_${name} PRIVATE
    -O2 -Wno-unused-parameter -Wno-deprecated-declarations)
  target_compile_features(media_${name} PRIVATE cxx_std_17)
  set_target_properties(media_${name} PROPERTIES
    OUTPUT_NAME ${name}
    RUNTIME_OUTPUT_DIRECTORY ${MEDIA}/bin
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
endfunction()

media_bin(CastInfoService SRC
  ${MEDIA}/src/CastInfoService/CastInfoService.cpp
  ${MEDIA_GEN}/CastInfoService.cpp ${MEDIA_GEN}/media_service_types.cpp)
media_bin(ComposeReviewService SRC
  ${MEDIA}/src/ComposeReviewService/ComposeReviewService.cpp
  ${MEDIA_GEN}/ComposeReviewService.cpp ${MEDIA_GEN}/media_service_types.cpp
  ${MEDIA_GEN}/ReviewStorageService.cpp ${MEDIA_GEN}/UserReviewService.cpp
  ${MEDIA_GEN}/MovieReviewService.cpp)
media_bin(MovieIdService SRC
  ${MEDIA}/src/MovieIdService/MovieIdService.cpp
  ${MEDIA_GEN}/MovieIdService.cpp ${MEDIA_GEN}/RatingService.cpp
  ${MEDIA_GEN}/ComposeReviewService.cpp ${MEDIA_GEN}/media_service_types.cpp)
media_bin(MovieInfoService SRC
  ${MEDIA}/src/MovieInfoService/MovieInfoService.cpp
  ${MEDIA_GEN}/MovieInfoService.cpp ${MEDIA_GEN}/media_service_types.cpp)
media_bin(MovieReviewService SRC
  ${MEDIA}/src/MovieReviewService/MovieReviewService.cpp
  ${MEDIA_GEN}/MovieReviewService.cpp ${MEDIA_GEN}/ReviewStorageService.cpp
  ${MEDIA_GEN}/media_service_types.cpp)
media_bin(PageService SRC
  ${MEDIA}/src/PageService/PageService.cpp
  ${MEDIA_GEN}/MovieReviewService.cpp ${MEDIA_GEN}/PageService.cpp
  ${MEDIA_GEN}/CastInfoService.cpp ${MEDIA_GEN}/PlotService.cpp
  ${MEDIA_GEN}/MovieInfoService.cpp ${MEDIA_GEN}/media_service_types.cpp)
media_bin(PlotService SRC
  ${MEDIA}/src/PlotService/PlotService.cpp
  ${MEDIA_GEN}/PlotService.cpp ${MEDIA_GEN}/media_service_types.cpp)
media_bin(RatingService SRC
  ${MEDIA}/src/RatingService/RatingService.cpp
  ${MEDIA_GEN}/RatingService.cpp ${MEDIA_GEN}/ComposeReviewService.cpp
  ${MEDIA_GEN}/media_service_types.cpp)
media_bin(ReviewStorageService SRC
  ${MEDIA}/src/ReviewStorageService/ReviewStorageService.cpp
  ${MEDIA_GEN}/ReviewStorageService.cpp ${MEDIA_GEN}/media_service_types.cpp)
media_bin(MediaTextService SRC
  ${MEDIA}/src/TextService/TextService.cpp
  ${MEDIA_GEN}/TextService.cpp ${MEDIA_GEN}/ComposeReviewService.cpp
  ${MEDIA_GEN}/media_service_types.cpp)
media_bin(MediaUniqueIdService SRC
  ${MEDIA}/src/UniqueIdService/UniqueIdService.cpp
  ${MEDIA_GEN}/UniqueIdService.cpp ${MEDIA_GEN}/ComposeReviewService.cpp
  ${MEDIA_GEN}/media_service_types.cpp)
media_bin(MediaUserService SRC
  ${MEDIA}/src/UserService/UserService.cpp
  ${MEDIA_GEN}/UserService.cpp ${MEDIA_GEN}/ComposeReviewService.cpp
  ${MEDIA_GEN}/media_service_types.cpp)
media_bin(UserReviewService SRC
  ${MEDIA}/src/UserReviewService/UserReviewService.cpp
  ${MEDIA_GEN}/UserReviewService.cpp ${MEDIA_GEN}/ReviewStorageService.cpp
  ${MEDIA_GEN}/media_service_types.cpp)

# OUTPUT_NAME follows the source tree. Text, unique-id and user collide with
# the social network target names, so those three use a media_ prefix above
# and the binary name is set here.
set_target_properties(media_MediaTextService PROPERTIES OUTPUT_NAME TextService)
set_target_properties(media_MediaUniqueIdService PROPERTIES OUTPUT_NAME UniqueIdService)
set_target_properties(media_MediaUserService PROPERTIES OUTPUT_NAME UserService)

add_custom_target(benchmarks)
add_dependencies(benchmarks
  hotel goperate gomaxprocs stress_ng
  runtime_mongod runtime_redis runtime_memcached runtime_rabbitmq
  TextService UniqueIdService UserService SocialGraphService PostStorageService
  UserTimelineService ComposePostService UserMentionService UrlShortenService
  MediaService HomeTimelineService WriteHomeTimelineService social_operate
  media_CastInfoService media_ComposeReviewService media_MovieIdService
  media_MovieInfoService media_MovieReviewService media_PageService
  media_PlotService media_RatingService media_ReviewStorageService
  media_MediaTextService media_MediaUniqueIdService media_MediaUserService
  media_UserReviewService)

find_program(JAVA_EXECUTABLE java)
find_program(MVN_EXECUTABLE mvn)
if(JAVA_EXECUTABLE AND MVN_EXECUTABLE)
  add_custom_target(train-ticket
    COMMAND ${MVN_EXECUTABLE} -DskipTests package
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/benchmarks/train-ticket
    COMMENT "mvn package train-ticket")
  add_dependencies(benchmarks train-ticket)
else()
  message(STATUS "train-ticket not built: java and mvn are not both on PATH")
endif()
