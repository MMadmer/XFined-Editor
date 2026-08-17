# LZO 2.10 is vendored verbatim; keep this list explicit so auxiliary codecs
# from the upstream source tree cannot silently enter the runtime ABI.
set(LZO_ROOT "${XRAY_SOURCE}/External/LZO")
set(LZO_SRC
    "${LZO_ROOT}/src/lzo_init.c"
    "${LZO_ROOT}/src/lzo1x_1.c"
    "${LZO_ROOT}/src/lzo1x_9x.c"
    "${LZO_ROOT}/src/lzo1x_d1.c"
    "${LZO_ROOT}/src/lzo1x_d2.c"
    "${LZO_ROOT}/src/lzo1x_d3.c"
)

add_library(LZO STATIC ${LZO_SRC})
set_target_properties(LZO PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${XRAY_LIB}")
target_compile_definitions(LZO PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(LZO
    PUBLIC "${LZO_ROOT}/include"
    PRIVATE "${LZO_ROOT}/src"
)
target_compile_options(LZO PRIVATE /W0 /Oi /Gy /arch:AVX)
if (XRAY_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(LZO PRIVATE /GL)
endif()

configure_file("${LZO_ROOT}/COPYING" "${XRAY_BIN}/LZO_LICENSE.txt" COPYONLY)
