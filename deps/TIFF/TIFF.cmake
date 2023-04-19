find_package(OpenGL QUIET REQUIRED)


if (APPLE)
    message(STATUS "Compiling TIFF for macos ${CMAKE_SYSTEM_VERSION}.")
    if (${CMAKE_OSX_ARCHITECTURES} MATCHES "arm")
        prusaslicer_add_cmake_project(TIFF
            URL https://gitlab.com/libtiff/libtiff/-/archive/v4.3.0/libtiff-v4.3.0.zip
            URL_HASH SHA256=a1db6826ea1b8b08eaf168973abb29b8a143fb744c1b24cc6967cb12f5e6e81f
            DEPENDS ${ZLIB_PKG} ${PNG_PKG} dep_JPEG
            CMAKE_ARGS
                -Dlzma:BOOL=OFF
                -Dwebp:BOOL=OFF
                -Djbig:BOOL=OFF
                -Dzstd:BOOL=OFF
                -Dpixarlog:BOOL=OFF
        )

    else()

        prusaslicer_add_cmake_project(TIFF
            URL https://gitlab.com/libtiff/libtiff/-/archive/v4.3.0/libtiff-v4.3.0.zip
            URL_HASH SHA256=4fca1b582c88319f3ad6ecd5b46320eadaf5eb4ef6f6c32d44caaae4a03d0726
            DEPENDS ${ZLIB_PKG} ${PNG_PKG} dep_JPEG
            CMAKE_ARGS
                -Dlzma:BOOL=OFF
                -Dwebp:BOOL=OFF
                -Djbig:BOOL=OFF
                -Dzstd:BOOL=OFF
                -Dpixarlog:BOOL=OFF
        )

    endif()

else()

prusaslicer_add_cmake_project(TIFF
    URL https://gitlab.com/libtiff/libtiff/-/archive/v4.1.0/libtiff-v4.1.0.zip
    URL_HASH SHA256=c56edfacef0a60c0de3e6489194fcb2f24c03dbb550a8a7de5938642d045bd32
    DEPENDS ${ZLIB_PKG} ${PNG_PKG} dep_JPEG
    CMAKE_ARGS
        -Dlzma:BOOL=OFF
        -Dwebp:BOOL=OFF
        -Djbig:BOOL=OFF
        -Dzstd:BOOL=OFF
        -Dpixarlog:BOOL=OFF
)

endif()
