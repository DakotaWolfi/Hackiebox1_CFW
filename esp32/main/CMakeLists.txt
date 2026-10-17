# SPDX-License-Identifier: GPL-3.0-or-later
#
# The shared code of the repository (../../src) with this platform's part
# (this directory). The port headers here come before ../../src, so that a
# shared module includes this platform's version of them.
set(TOP ${CMAKE_CURRENT_LIST_DIR}/../..)
set(OPUS ${TOP}/third_party/opus-1.1)
set(OPUS_CELT bands celt celt_decoder cwrs entcode entdec entenc kiss_fft laplace mathops mdct modes pitch celt_lpc quant_bands rate vq)

file(GLOB SHARED ${TOP}/src/*.c)
file(GLOB OWN ${CMAKE_CURRENT_LIST_DIR}/*.c)
set(OPUS_SRC ${OPUS}/src/opus.c ${OPUS}/src/opus_decoder.c ${TOP}/third_party/opus_glue/silk_stub.c)
foreach(f ${OPUS_CELT})
    list(APPEND OPUS_SRC ${OPUS}/celt/${f}.c)
endforeach()
set(FATFS_SRC ${TOP}/src/fatfs/ff.c ${TOP}/third_party/fatfs/ffunicode.c)

# The setup page as it is, with its own version; a copy in the
# build directory keeps the embedded symbol name, made again when it changes.
configure_file(${TOP}/card/cfw/setup.htm ${CMAKE_CURRENT_BINARY_DIR}/setup.htm COPYONLY)

idf_component_register(
    SRCS ${OWN} ${SHARED} ${OPUS_SRC} ${FATFS_SRC}
    INCLUDE_DIRS . ${TOP}/src ${TOP}/third_party/fatfs ${OPUS}/include
                 ${IDF_PATH}/components/freertos/FreeRTOS-Kernel/include/freertos
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash app_update esp_partition spi_flash
                  wear_levelling mbedtls lwip driver esp_adc esp_timer esp_app_format esp_driver_i2s
                  esp_driver_spi esp_driver_i2c esp_driver_gpio esp_driver_ledc esp_driver_sdmmc sdmmc
    EMBED_FILES ${CMAKE_CURRENT_BINARY_DIR}/setup.htm)

# Build identity (version.h), made again at every build.
set(BUILD_H ${CMAKE_BINARY_DIR}/cfw_build.h)
add_custom_target(cfw_build_id ALL
    COMMAND ${CMAKE_COMMAND} -DTOP=${TOP} -DOUT=${BUILD_H} -P ${CMAKE_CURRENT_LIST_DIR}/build_id.cmake
    BYPRODUCTS ${BUILD_H})
add_dependencies(${COMPONENT_LIB} cfw_build_id)
if(NOT CFW_DEBUG)
    set(CFW_DEBUG 1)
endif()
if(NOT CFW_START_STEP)
    set(CFW_START_STEP 8)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CFW_VERSION="${CFW_VERSION}" CFW_DEBUG=${CFW_DEBUG} CFW_START_STEP=${CFW_START_STEP}
    CFW_HIBERNATE=0 CFW_LOG_UART=0 CFW_ESP32=1)

# Own code warning-free; vendored code as it is.
set_source_files_properties(${OPUS_SRC} PROPERTIES COMPILE_OPTIONS
    "-w;-O2;-DFIXED_POINT;-DDISABLE_FLOAT_API;-DOPUS_BUILD;-DVAR_ARRAYS;-DHAVE_LRINTF;-I${OPUS}/celt;-I${OPUS}/silk;-I${OPUS}/silk/fixed;-I${OPUS}")
set_source_files_properties(${FATFS_SRC} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${OWN} ${SHARED} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter;-include;${BUILD_H}")
