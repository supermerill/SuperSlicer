
if (APPLE)
    add_cmake_project(TIFF
        URL https://gitlab.com/libtiff/libtiff/-/archive/v4.1.0/libtiff-v4.1.0.zip
        URL_HASH SHA256=c56edfacef0a60c0de3e6489194fcb2f24c03dbb550a8a7de5938642d045bd32
        CMAKE_ARGS
            -Dtiff-tools:BOOL=OFF
            -Dtiff-tests:BOOL=OFF
            -Dlzma:BOOL=OFF
            -Dwebp:BOOL=OFF
            -Djbig:BOOL=OFF
            -Dzstd:BOOL=OFF
            -Dpixarlog:BOOL=OFF
            -Dlibdeflate:BOOL=OFF
            -Dlerc:BOOL=OFF
    )
else ()
    add_cmake_project(TIFF
        URL https://gitlab.com/libtiff/libtiff/-/archive/v4.6.0/libtiff-v4.6.0.zip
        URL_HASH SHA256=5d652432123223338a6ee642a6499d98ebc5a702f8a065571e1001d4c08c37e6
        CMAKE_ARGS
            -Dtiff-tools:BOOL=OFF
            -Dtiff-tests:BOOL=OFF
            -Dlzma:BOOL=OFF
            -Dwebp:BOOL=OFF
            -Djbig:BOOL=OFF
            -Dzstd:BOOL=OFF
            -Dpixarlog:BOOL=OFF
            -Dlibdeflate:BOOL=OFF
            -Dlerc:BOOL=OFF
    )
endif ()


set(DEP_TIFF_DEPENDS ZLIB PNG JPEG OpenGL)
