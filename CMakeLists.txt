# Copyright (c) 2018-2019 Snowflake Computing, Inc. All rights reserved.
#
# CMakeList for Snowflake Client
#
cmake_minimum_required(VERSION 2.8)
project(snowflakeclient)

# Enabling tests by Ctest. Don't use INCLUDE(Ctest) as
# we don't need Dart and other tools.
enable_testing()

add_definitions(-DLOG_USE_COLOR)

option(BUILD_TESTS "True if build tests" on)
option(MOCK "True if mock should be used" off)
set(OPENSSL_VERSION_NUMBER  0x11100000L)
# Developers can uncomment this to enable mock builds on their local VMs
#set(MOCK TRUE)

if (MOCK)
    set(MOCK_OBJECT_WRAPPER_FLAGS -Wl,--wrap=http_perform)
    add_definitions(-DMOCK_ENABLED)
else()
    set(MOCK_OBJECT_WRAPPER_FLAGS )
endif ()

if (UNIX AND NOT APPLE)
    set(LINUX TRUE)
endif ()

if (LINUX)
    set(PLATFORM linux)
    message("Platform: Linux")
endif ()
if (APPLE)
    set(PLATFORM darwin)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.12" CACHE STRING "Minimum OS X deployment version")
    message("Platform: Apple OSX")
endif ()
if ("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    set(PLATFORM win64)
    message("Platform: Windows 64bit")
endif ()
if ("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
    set(PLATFORM win32)
    message("Platform: Windows 32bit")
endif ()

set(CMAKE_VERBOSE_MAKEFILE ON)
if (UNIX)
    # Linux and OSX
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 -std=gnu99 -g -fPIC -Werror ${MOCK_OBJECT_WRAPPER_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -std=gnu++11 -fPIC -Werror ${MOCK_OBJECT_WRAPPER_FLAGS}")
endif ()
if (LINUX)
    # Linux. MacOS doesn't require pthread option
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
endif ()

if (LINUX)
    # Profiler for Linux
    if (NOT "$ENV{BUILD_WITH_PROFILE_OPTION}" STREQUAL "")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pg")
    endif ()

    # Code coverage for Linux
    if (NOT "$ENV{BUILD_WITH_GCOV_OPTION}" STREQUAL "")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -gp -fprofile-arcs -ftest-coverage")
    endif ()
endif ()

set(SOURCE_FILES
        include/snowflake/basic_types.h
        include/snowflake/client.h
        include/snowflake/logger.h
        include/snowflake/version.h
        include/snowflake/platform.h
        lib/client.c
        lib/constants.h
        lib/cJSON.h
        lib/cJSON.c
        lib/logger.c
        lib/lib_common.h
        lib/paramstore.c
        lib/paramstore.h
        lib/arraylist.h
        lib/arraylist.c
        lib/treemap.h
        lib/treemap.c
        lib/rbtree.h
        lib/rbtree.c
        lib/memory.h
        lib/memory.c
        lib/connection.h
        lib/connection.c
        lib/constants.h
        lib/results.h
        lib/results.c
        lib/platform.c
        lib/uuid4.c
        lib/basic_types.c
        lib/error.h
        lib/error.c
        lib/client_int.h
        lib/chunk_downloader.h
        lib/chunk_downloader.c
        lib/mock_http_perform.h
        lib/http_perform.c)

set (SOURCE_FILES_PUT_GET
        cpp/EncryptionProvider.cpp
        cpp/FileCompressionType.cpp
        cpp/FileCompressionType.hpp
        cpp/FileMetadata.hpp
        cpp/FileMetadataInitializer.cpp
        cpp/FileMetadataInitializer.hpp
        cpp/FileTransferAgent.cpp
        cpp/FileTransferAgent.hpp
        cpp/FileTransferExecutionResult.cpp
        cpp/FileTransferExecutionResult.hpp
        cpp/IFileTransferAgent.cpp
        cpp/IStorageClient.hpp
        cpp/SnowflakeS3Client.hpp
        cpp/SnowflakeS3Client.cpp
        cpp/SnowflakeAzureClient.hpp
        cpp/SnowflakeAzureClient.cpp
        cpp/SnowflakeTransferException.cpp
        cpp/StatementPutGet.hpp
        cpp/StatementPutGet.cpp
        cpp/StorageClientFactory.hpp
        cpp/StorageClientFactory.cpp
        cpp/RemoteStorageRequestOutcome.hpp
        cpp/util/Base64.hpp
        cpp/util/Base64.cpp
        cpp/util/ByteArrayStreamBuf.cpp
        cpp/util/ByteArrayStreamBuf.hpp
        cpp/util/CompressionUtil.cpp
        cpp/util/CompressionUtil.hpp
        cpp/util/Proxy.hpp
        cpp/util/Proxy.cpp
        cpp/util/ThreadPool.hpp
        cpp/util/SnowflakeCommon.hpp
        cpp/crypto/CryptoTypes.hpp
        cpp/crypto/Cryptor.hpp
        cpp/crypto/CipherContext.hpp
        cpp/crypto/CipherContext.cpp
        cpp/crypto/Cryptor.cpp
        cpp/crypto/CipherStreamBuf.cpp
        cpp/crypto/CipherStreamBuf.hpp
        cpp/crypto/HashContext.cpp
        cpp/crypto/HashContext.hpp
        cpp/jwt/Jwt.cpp
        cpp/jwt/Jwt.hpp
        cpp/jwt/Signer.cpp
        cpp/jwt/Signer.hpp
        cpp/jwt/Util.cpp
        cpp/jwt/Util.hpp
        cpp/jwt/ClaimSet.hpp
        cpp/jwt/ClaimSet.cpp
        cpp/jwt/Header.cpp
        cpp/jwt/Header.hpp
        cpp/logger/ISFLogger.cpp
        cpp/logger/SFAwsLogger.cpp
        cpp/logger/SFAwsLogger.hpp
        cpp/logger/SFLogger.cpp
        cpp/logger/SFLogger.hpp
        include/snowflake/IFileTransferAgent.hpp
        include/snowflake/ISFLogger.hpp
        include/snowflake/IStatementPutGet.hpp
        include/snowflake/ITransferResult.hpp
        include/snowflake/PutGetParseResponse.hpp
        include/snowflake/SnowflakeTransferException.hpp
        include/snowflake/IJwt.hpp
        include/snowflake/IBase64.hpp
        )

set(SOURCE_FILES_CPP_WRAPPER
        include/snowflake/Connection.hpp
        include/snowflake/Statement.hpp
        include/snowflake/Column.hpp
        include/snowflake/Param.hpp
        include/snowflake/Exceptions.hpp
        include/snowflake/jwtWrapper.h
        cpp/lib/Exceptions.cpp
        cpp/lib/Connection.cpp
        cpp/lib/Statement.cpp
        cpp/lib/Column.cpp
        cpp/jwt/jwtWrapper.cpp)

if (LINUX)
    # Linux
    find_library(CURL_LIB libcurl.a PATHS deps-build/${PLATFORM}/curl/lib/ REQUIRED)
    find_library(SSL_LIB libssl.a PATHS deps-build/${PLATFORM}/openssl/lib/ REQUIRED)
    find_library(CRYPTO_LIB libcrypto.a PATHS deps-build/${PLATFORM}/openssl/lib/ REQUIRED)
    find_library(AWS_CORE_LIB libaws-cpp-sdk-core.a PATHS deps-build/${PLATFORM}/aws/lib64/ REQUIRED)
    find_library(AWS_S3_LIB libaws-cpp-sdk-s3.a PATHS deps-build/${PLATFORM}/aws/lib64/ REQUIRED)
    find_library(AZURE_STORAGE_LITE_LIB libazure-storage-lite.a PATHS deps-build/${PLATFORM}/azure/lib/ REQUIRED)
    find_library(UUID_LIB libuuid.a PATHS deps-build/${PLATFORM}/uuid/lib/ REQUIRED)
    if(MOCK)
        find_library(CMOCKA_LIB libcmocka.a PATHS deps-build/${PLATFORM}/cmocka/lib/ REQUIRED)
    endif()
endif ()

if (APPLE)
    find_library(CURL_LIB libcurl.a PATHS deps-build/${PLATFORM}/curl/lib/ REQUIRED)
    find_library(SSL_LIB libssl.a PATHS deps-build/${PLATFORM}/openssl/lib/ REQUIRED)
    find_library(CRYPTO_LIB libcrypto.a PATHS deps-build/${PLATFORM}/openssl/lib/ REQUIRED)
    find_library(AWS_CORE_LIB libaws-cpp-sdk-core.a PATHS deps-build/${PLATFORM}/aws/lib/ REQUIRED)
    find_library(AWS_S3_LIB libaws-cpp-sdk-s3.a PATHS deps-build/${PLATFORM}/aws/lib/ REQUIRED)
    find_library(AZURE_STORAGE_LITE_LIB libazure-storage-lite.a PATHS deps-build/${PLATFORM}/azure/lib/ REQUIRED)
    if(MOCK)
        find_library(CMOCKA_LIB libcmocka.a PATHS deps-build/${PLATFORM}/cmocka/lib/ REQUIRED)
    endif()
endif()

if (WIN32)
    # Windows
    option(DYNAMIC_RUNTIME "Dynamic runtime" ON)
    set(VSDIR "vs15" CACHE STRING "Used to specify visual studio version of libsnowflakeclient dependecies")
    add_definitions(-D_CRT_SECURE_NO_DEPRECATE)
    find_library(CURL_LIB libcurl_a.lib PATHS deps-build/${PLATFORM}/${VSDIR}/curl/lib/ REQUIRED)
    find_library(SSL_LIB libssl_a.lib PATHS deps-build/${PLATFORM}/${VSDIR}/openssl/lib/ REQUIRED)
    find_library(CRYPTO_LIB libcrypto_a.lib PATHS deps-build/${PLATFORM}/${VSDIR}/openssl/lib/ REQUIRED)
    find_library(ZLIB_LIB zlib_a.lib PATHS deps-build/${PLATFORM}/${VSDIR}/zlib/lib/ REQUIRED)
    find_library(AWS_CORE_LIB aws-cpp-sdk-core.lib PATHS deps-build/${PLATFORM}/${VSDIR}/aws/lib/ REQUIRED)
    find_library(AWS_S3_LIB aws-cpp-sdk-s3.lib PATHS deps-build/${PLATFORM}/${VSDIR}/aws/lib/ REQUIRED)
    find_library(AZURE_STORAGE_LITE_LIB azure-storage-lite.lib PATHS deps-build/${PLATFORM}/${VSDIR}/azure/lib/ REQUIRED)
    if(MOCK)
        find_library(CMOCKA_LIB cmocka_a.lib PATHS deps-build/${PLATFORM}/${VSDIR}/cmocka/lib/ REQUIRED)
    endif()
endif ()

if (LINUX)
    include_directories(
            deps-build/${PLATFORM}/curl/include
            deps-build/${PLATFORM}/openssl/include
            deps-build/${PLATFORM}/zlib/include
            deps-build/${PLATFORM}/aws/include
            deps-build/${PLATFORM}/azure/include
            deps-build/${PLATFORM}/uuid/include
            deps-build/${PLATFORM}/cmocka/include
            include
            lib)
endif()

if (APPLE)
    include_directories(
            deps-build/${PLATFORM}/curl/include
            deps-build/${PLATFORM}/openssl/include
            deps-build/${PLATFORM}/zlib/include
            deps-build/${PLATFORM}/aws/include
            deps-build/${PLATFORM}/azure/include
            deps-build/${PLATFORM}/cmocka/include
            include
            lib)
endif()

if (WIN32)
    include_directories(
            deps-build/${PLATFORM}/${VSDIR}/curl/include
            deps-build/${PLATFORM}/${VSDIR}/openssl/include
            deps-build/${PLATFORM}/${VSDIR}/zlib/include
            deps-build/${PLATFORM}/${VSDIR}/aws/include
            deps-build/${PLATFORM}/${VSDIR}/cmocka/include
	        deps-build/${PLATFORM}/${VSDIR}/azure/include
            include
            lib)
endif()

message("libcurl is located at " ${CURL_LIB})
message("libssl is located at " ${SSL_LIB})
message("libcrypto is located at " ${CRYPTO_LIB})
message("libaws-cpp-sdk-core is located at " ${AWS_CORE_LIB})
message("libaws-cpp-sdk-s3 is located at " ${AWS_S3_LIB})
message("libazure-storage-lite.a is located at " ${AZURE_STORAGE_LITE_LIB})
message("libuuid is located at " ${UUID_LIB})
if (MOCK)
    message("libcmocka is located at " ${CMOCKA_LIB})
endif ()

add_library(snowflakeclient STATIC ${SOURCE_FILES} ${SOURCE_FILES_PUT_GET} ${SOURCE_FILES_CPP_WRAPPER})

set_target_properties(snowflakeclient PROPERTIES LINKER_LANGUAGE CXX)
set_property(TARGET snowflakeclient PROPERTY C_STANDARD 99)
#set (CMAKE_CXX_STANDARD 11)

if(LINUX)
    link_directories(
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/curl/lib"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/openssl/lib"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/zlib/lib"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/aws/lib64"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/azure/lib"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/uuid/lib"
    )
    if(MOCK)
        link_directories("${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/cmocka/lib")
    endif()
elseif(WIN32)
    link_directories(
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/${VSDIR}/curl/lib"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/${VSDIR}/openssl/lib"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/${VSDIR}/aws/lib"
    )
    if(MOCK)
        link_directories("${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/${VSDIR}/cmocka/lib")
    endif()
elseif()
    link_directories(
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/curl/lib"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/openssl/lib"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/aws/lib"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/azure/lib"
    )
    if(MOCK)
        link_directories("${CMAKE_CURRENT_SOURCE_DIR}/deps-build/${PLATFORM}/cmocka/lib")
    endif()
endif()

if (LINUX)
    # Linux
    target_link_libraries(snowflakeclient rt dl z)
endif ()
if (APPLE)
    # OSX. no librt is required.
    target_link_libraries(snowflakeclient dl z)
endif ()
if (WIN32)
    # Windows
    target_link_libraries(snowflakeclient)
    if (DYNAMIC_RUNTIME)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MD")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MDd")
    else()
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
    endif()
endif ()

add_subdirectory(examples)

if (BUILD_TESTS)
    add_subdirectory(tests)
endif()
