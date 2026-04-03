cmake_minimum_required(VERSION 3.20)
project(AndroidCamStreamer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/deps/ffmpeg")
set(SOFTCAM_ROOT "${CMAKE_SOURCE_DIR}/deps/softcam")

add_executable(${PROJECT_NAME} main.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE 
    "${FFMPEG_ROOT}/include"
    "${SOFTCAM_ROOT}"
)

target_link_directories(${PROJECT_NAME} PRIVATE 
    "${FFMPEG_ROOT}/lib"
    "${SOFTCAM_ROOT}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    avcodec
    avformat
    avutil
    swscale
    softcam
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${FFMPEG_ROOT}/bin"
    $<TARGET_FILE_DIR:${PROJECT_NAME}>
)