set(_curl_platform_flags 
  -DENABLE_IPV6:BOOL=ON
  -DENABLE_VERSIONED_SYMBOLS:BOOL=ON
  -DENABLE_THREADED_RESOLVER:BOOL=ON

  # -DCURL_DISABLE_LDAP:BOOL=ON
  # -DCURL_DISABLE_LDAPS:BOOL=ON
  -DENABLE_MANUAL:BOOL=OFF
  # -DCURL_DISABLE_RTSP:BOOL=ON
  # -DCURL_DISABLE_DICT:BOOL=ON
  # -DCURL_DISABLE_TELNET:BOOL=ON
  # -DCURL_DISABLE_POP3:BOOL=ON
  # -DCURL_DISABLE_IMAP:BOOL=ON
  # -DCURL_DISABLE_SMB:BOOL=ON
  # -DCURL_DISABLE_SMTP:BOOL=ON
  # -DCURL_DISABLE_GOPHER:BOOL=ON
  -DHTTP_ONLY=ON

  -DCMAKE_USE_GSSAPI:BOOL=OFF
  -DCMAKE_USE_LIBSSH2:BOOL=OFF
  -DUSE_RTMP:BOOL=OFF
  -DUSE_NGHTTP2:BOOL=OFF
  -DUSE_MBEDTLS:BOOL=OFF
)

if (WIN32)
  set(_curl_platform_flags  ${_curl_platform_flags} -DCMAKE_USE_SCHANNEL=ON)
elseif (APPLE)
  set(_curl_platform_flags 
    
    ${_curl_platform_flags}

    -DCMAKE_USE_SECTRANSP:BOOL=ON 
    -DCMAKE_USE_OPENSSL:BOOL=OFF

    -DCURL_CA_PATH:STRING=none
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_curl_platform_flags 

    ${_curl_platform_flags}

    -DCMAKE_USE_OPENSSL:BOOL=ON

    -DCURL_CA_PATH:STRING=none
    -DCURL_CA_BUNDLE:STRING=none
    -DCURL_CA_FALLBACK:BOOL=ON
  )
endif ()

set(_patch_command "")
if (UNIX AND NOT APPLE)
  # On non-apple UNIX platforms, finding the location of OpenSSL certificates is necessary at runtime, as there is no standard location usable across platforms.
  # The OPENSSL_CERT_OVERRIDE flag is understood by PrusaSlicer and will trigger the search of certificates at initial application launch. 
  # Then ask the user for consent about the correctness of the found location.
  set (_patch_command echo set_target_properties(CURL::libcurl PROPERTIES INTERFACE_COMPILE_DEFINITIONS OPENSSL_CERT_OVERRIDE) >> CMake/curl-config.cmake.in)
endif ()

if (IS_CROSS_COMPILE AND APPLE)
# TODO
endif ()

prusaslicer_add_cmake_project(CURL
  # GIT_REPOSITORY      https://github.com/curl/curl.git
  # GIT_TAG             curl-7_75_0
  URL                 https://github.com/curl/curl/archive/refs/tags/curl-7_75_0.zip
  URL_HASH            SHA256=a63ae025bb0a14f119e73250f2c923f4bf89aa93b8d4fafa4a9f5353a96a765a
  DEPENDS             ${ZLIB_PKG}
  # PATCH_COMMAND       ${GIT_EXECUTABLE} checkout -f -- . && git clean -df && 
  #                     ${GIT_EXECUTABLE} apply --whitespace=fix ${CMAKE_CURRENT_LIST_DIR}/curl-mods.patch
  PATCH_COMMAND       "${_patch_command}"
  CMAKE_ARGS
    -DBUILD_TESTING:BOOL=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    ${_curl_platform_flags}
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_dependencies(dep_CURL dep_OpenSSL)
endif ()

if (MSVC)
    add_debug_dep(dep_CURL)
endif ()
