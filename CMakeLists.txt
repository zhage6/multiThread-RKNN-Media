cmake_minimum_required(VERSION 3.10)

project(rknn_yolov5_demo)
set(CMAKE_C_COMPILER "aarch64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "aarch64-linux-gnu-g++") #使用交叉编译
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-pthread")

# skip 3rd-party lib dependencies
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-shlib-undefined")

# install target and libraries
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/rknn_yolov5_demo_${CMAKE_SYSTEM_NAME})

set(CMAKE_SKIP_INSTALL_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")

# rknn api
set(RKNN_API_PATH ${CMAKE_SOURCE_DIR}/../../runtime/RK3588/${CMAKE_SYSTEM_NAME}/librknn_api)
set(LIB_ARCH aarch64)
set(RKNN_RT_LIB ${CMAKE_SOURCE_DIR}/include/librknnrt.so)

#rga
set(RGA_PATH ${CMAKE_SOURCE_DIR}/include/3rdparty/rga/RK3588)
set(RGA_LIB ${RGA_PATH}/lib/Linux//${LIB_ARCH}/librga.so)

#mpp
set(MPP_PATH /usr/include/rockchip)
set(MPP_LIB /usr/lib/aarch64-linux-gnu/librockchip_mpp.so)



include_directories(${RKNN_API_PATH}/include)
include_directories(${CMAKE_SOURCE_DIR}/include/3rdparty)
include_directories(${RGA_PATH}/include)
include_directories(${MPP_PATH})

# opencv
set(OpenCV_DIR "${CMAKE_SOURCE_DIR}/include/3rdparty/opencv_arm64/lib/cmake/opencv4")
find_package(OpenCV REQUIRED)

include_directories(${OpenCV_INCLUDE_DIRS})

# Embedded ZLMediaKit RTSP server
set(ZLMEDIAKIT_ROOT "${CMAKE_SOURCE_DIR}/../ZLMediaKit" CACHE PATH
    "Path to the ZLMediaKit source tree")
set(ZLMEDIAKIT_API_INCLUDE "${ZLMEDIAKIT_ROOT}/api/include")
set(ZLMEDIAKIT_API_LIBRARY
    "${ZLMEDIAKIT_ROOT}/release/linux/Debug/libmk_api.so"
    CACHE FILEPATH "Path to the ZLMediaKit C API shared library")

if(NOT EXISTS "${ZLMEDIAKIT_API_INCLUDE}/mk_mediakit.h")
    message(FATAL_ERROR
        "ZLMediaKit API headers not found under ${ZLMEDIAKIT_API_INCLUDE}")
endif()
if(NOT EXISTS "${ZLMEDIAKIT_API_LIBRARY}")
    message(FATAL_ERROR
        "ZLMediaKit C API library not found: ${ZLMEDIAKIT_API_LIBRARY}")
endif()

add_library(zlmediakit_api SHARED IMPORTED)
set_target_properties(zlmediakit_api PROPERTIES
    IMPORTED_LOCATION "${ZLMEDIAKIT_API_LIBRARY}")

get_filename_component(
    ZLMEDIAKIT_API_LIBRARY_DIR "${ZLMEDIAKIT_API_LIBRARY}" DIRECTORY)
set(CMAKE_BUILD_RPATH "${ZLMEDIAKIT_API_LIBRARY_DIR}")


# rknn_yolov5_demo
include_directories( ${CMAKE_SOURCE_DIR}/include)


add_executable(rknn_yolov5_demo
        src/main.cc
        src/postprocess.cc
        src/preprocess.cc
        src/rkYolov5s.cc
        src/MppDecoder.cpp
        src/dma_alloc.cpp
        src/MppEncoder.cpp
        src/StreamChannel.cpp
        src/MosaicComposer.cpp
        src/ZlMediaPublisher.cpp
        src/FrameResultAggregator.cpp
        src/MultiModelPipeline.cpp
)

target_include_directories(rknn_yolov5_demo PRIVATE
    "${ZLMEDIAKIT_API_INCLUDE}")

target_link_libraries(rknn_yolov5_demo
  ${RKNN_RT_LIB}
  ${OpenCV_LIBS}
  ${RGA_LIB}
  ${MPP_LIB}
  zlmediakit_api
)

option(BUILD_ZLMEDIAKIT_SMOKE_TEST
    "Build the standalone embedded RTSP smoke test" OFF)
if(BUILD_ZLMEDIAKIT_SMOKE_TEST)
    add_executable(zlmedia_rtsp_smoke EXCLUDE_FROM_ALL
        tests/zlmedia_rtsp_smoke.cpp
        src/ZlMediaPublisher.cpp)
    target_include_directories(zlmedia_rtsp_smoke PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${ZLMEDIAKIT_API_INCLUDE}")
    target_link_libraries(zlmedia_rtsp_smoke
        zlmediakit_api
        pthread)
endif()


# install target and libraries
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/rknn_yolov5_demo_${CMAKE_SYSTEM_NAME})
install(TARGETS rknn_yolov5_demo DESTINATION ./)
install(PROGRAMS ${RKNN_RT_LIB} DESTINATION lib)
install(PROGRAMS ${RGA_LIB} DESTINATION lib)
install(PROGRAMS ${MPP_LIB} DESTINATION lib)
install(PROGRAMS ${ZLMEDIAKIT_API_LIBRARY} DESTINATION lib)
install(DIRECTORY model DESTINATION ./)
