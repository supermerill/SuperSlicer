
# This ensures dependencies don't use SDK features which are not available in the version specified by Deployment target
# That can happen when one uses a recent SDK but specifies an older Deployment target
set(DEP_WERRORS_SDK "-Werror=partial-availability -Werror=unguarded-availability -Werror=unguarded-availability-new")

set(DEP_CMAKE_OPTS
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    "-DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}"
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=${DEP_OSX_TARGET}"
    "-DCMAKE_CXX_FLAGS=${DEP_WERRORS_SDK}"
    "-DCMAKE_C_FLAGS=${DEP_WERRORS_SDK}"
    "-DCMAKE_FIND_FRAMEWORK=LAST"
    "-DCMAKE_FIND_APPBUNDLE=LAST"
)

include("deps-unix-common.cmake")

find_package(CURL QUIET)
if (NOT CURL_FOUND)
    message(WARNING "No CURL dev package found in system, building static library. Mac SDK should include CURL from at least version 10.12. Check your SDK installation.")
endif ()

if (IS_CROSS_COMPILE)
    if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm")
        set(_build_arch aarch64)
    elseif (${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86_64")
        set(_build_arch x86_64)
    endif()

    if (${CMAKE_OSX_ARCHITECTURES} MATCHES "arm")
        message(STATUS "Compiling slic3r for macos arm64.")
        set(_host_arch aarch64)
        set(_arch_flags "-arch arm64")
    elseif (${CMAKE_OSX_ARCHITECTURES} MATCHES "x86_64")
        message(STATUS "Compiling slic3r for macos x86_64.")
        set(_host_arch x86_64)
        set(_arch_flags "-arch x86_64")
    endif()
    set(_boost_linkflags "linkflags=${_arch_flags}")
    set(_build_tgt --build=${_build_arch}-apple-darwin --host=${_host_arch}-apple-darwin)
    set(_env_curl env "CFLAGS=${_arch_flags}")
    message(STATUS "TEST _build_arch = ${_build_arch}")
    message(STATUS "TEST _host_arch = ${_host_arch}")
    message(STATUS "TEST _boost_linkflags = ${_boost_linkflags}")
    message(STATUS "TEST _build_tgt = '${_build_tgt}'")
    message(STATUS "TEST _env_curl = ${_env_curl}")
endif ()

# ExternalProject_Add(dep_boost
#     EXCLUDE_FROM_ALL 1
#     URL "https://github.com/supermerill/SuperSlicer_deps/releases/download/0.4/boost_1_70_0.tar.gz"
#     URL_HASH SHA256=882b48708d211a5f48e60b0124cf5863c1534cd544ecd0664bb534a4b5d506e9
#     BUILD_IN_SOURCE 1
#     CONFIGURE_COMMAND ./bootstrap.sh
#         --with-toolset=clang
#         --with-libraries=system,iostreams,filesystem,thread,log,locale,regex
#         "--prefix=${DESTDIR}/usr/local"
#     BUILD_COMMAND ./b2
#         -j ${NPROC}
#         --reconfigure
#         toolset=clang
#         link=static
#         variant=release
#         threading=multi
#         boost.locale.icu=off
#         "cflags=-fPIC ${_arch_flags} -mmacosx-version-min=${DEP_OSX_TARGET}"
#         "cxxflags=-fPIC ${_arch_flags} -mmacosx-version-min=${DEP_OSX_TARGET}"
#         "mflags=-fPIC ${_arch_flags} -mmacosx-version-min=${DEP_OSX_TARGET}"
#         "mmflags=-fPIC ${_arch_flags} -mmacosx-version-min=${DEP_OSX_TARGET}"
#         ${_boost_linkflags}
#         install
#     INSTALL_COMMAND ""   # b2 does that already
# )

# ExternalProject_Add(dep_libcurl
#     EXCLUDE_FROM_ALL 1
#     URL "https://curl.haxx.se/download/curl-7.58.0.tar.gz"
#     URL_HASH SHA256=cc245bf9a1a42a45df491501d97d5593392a03f7b4f07b952793518d97666115
#     BUILD_IN_SOURCE 1
#     CONFIGURE_COMMAND ${_env_curl} ./configure
#         ${_build_tgt}
#         --enable-static
#         --disable-shared
#         "--with-ssl=${DESTDIR}/usr/local"
#         --with-pic
#         --enable-ipv6
#         --enable-versioned-symbols
#         --enable-threaded-resolver
#         --with-darwinssl
#         --without-ssl     # disables OpenSSL
#         --disable-ldap
#         --disable-ldaps
#         --disable-manual
#         --disable-rtsp
#         --disable-dict
#         --disable-telnet
#         --disable-pop3
#         --disable-imap
#         --disable-smb
#         --disable-smtp
#         --disable-gopher
#         --without-gssapi
#         --without-libpsl
#         --without-libidn2
#         --without-gnutls
#         --without-polarssl
#         --without-mbedtls
#         --without-cyassl
#         --without-nss
#         --without-axtls
#         --without-brotli
#         --without-libmetalink
#         --without-libssh
#         --without-libssh2
#         --without-librtmp
#         --without-nghttp2
#         --without-zsh-functions-dir
#     BUILD_COMMAND make "-j${NPROC}"
#     INSTALL_COMMAND make install "DESTDIR=${DESTDIR}"
# )

# add_dependencies(dep_openvdb dep_boost)