#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <cstdio>

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libswscale/swscale.h>
}

#include "softcam.h"

const int W = 1280;
const int H = 720;

std::queue<uint8_t> adbBuffer;
std::mutex bufferMutex;
bool isRunning = true;
std::vector<uint8_t> cached_headers;

void socketReaderThread(SOCKET sock) {
    uint8_t temp[65536]; 
    
    char deviceName[64];
    int received = recv(sock, deviceName, 64, 0);
    if (received > 0) {
        std::cout << "Connected to device: " << deviceName << std::endl;
    }

    while (isRunning) {
        int n = recv(sock, (char*)temp, sizeof(temp), 0);
        if (n > 0) {
            std::lock_guard<std::mutex> lock(bufferMutex);
            for (int i = 0; i < n; ++i) {
                adbBuffer.push(temp[i]);
            }
        } else if (n == 0 || n == SOCKET_ERROR) {
            std::cerr << "Socket connection lost." << std::endl;
            isRunning = false;
            break;
        }
    }
}

int main() {
    std::cout << "--- Auto-initializing ADB and scrcpy-server ---" << std::endl;

    system("adb push scrcpy-server /data/local/tmp/scrcpy-server");

    system("adb forward tcp:1234 localabstract:scrcpy");

    std::string startServerCmd = "start /b adb shell CLASSPATH=/data/local/tmp/scrcpy-server app_process / com.genymobile.scrcpy.Server 2.3.1 video_source=camera camera_id=0 max_size=1280 tunnel_forward=true audio=false control=false";
    system(startServerCmd.c_str());

    std::this_thread::sleep_for(std::chrono::seconds(2));

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return -1;
    }

    scCamera cam = scCreateCamera(W, H, 60.0f);
    if (!cam) {
        std::cerr << "Could not create camera instance." << std::endl;
        return -1;
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->flags |= AV_CODEC_FLAG_LOW_DELAY;
    c->flags2 |= AV_CODEC_FLAG2_CHUNKS;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", "ultrafast", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
    avcodec_open2(c, codec, &opts);
    av_dict_free(&opts);

    AVCodecParserContext* parser = av_parser_init(codec->id);
    AVFrame* frame = av_frame_alloc();
    AVFrame* frameRGB = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();

    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, W, H, 1);
    uint8_t* bufferRGB = (uint8_t*)av_malloc(numBytes);
    av_image_fill_arrays(frameRGB->data, frameRGB->linesize, bufferRGB, AV_PIX_FMT_RGB24, W, H, 1);

    SwsContext* sws_ctx = sws_getContext(
        W, H, AV_PIX_FMT_YUV420P,
        W, H, AV_PIX_FMT_BGR24,
        SWS_BILINEAR, NULL, NULL, NULL
    );
    
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    std::cout << "Connecting to scrcpy-server (localhost:1234)..." << std::endl;
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "Connect failed! Run 'adb forward tcp:1234 localabstract:scrcpy' first." << std::endl;
        return -1;
    }

    const char* version = "2.3.1";
    send(sock, version, (int)strlen(version), 0);

    std::thread reader(socketReaderThread, sock);
    std::vector<uint8_t> decodeWorkBuffer;

    std::cout << "Waiting for Discord to connect..." << std::endl;
    scWaitForConnection(cam);

    while (isRunning) {
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            while (!adbBuffer.empty()) {
                decodeWorkBuffer.push_back(adbBuffer.front());
                adbBuffer.pop();
            }
        }

        if (decodeWorkBuffer.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        uint8_t* data = decodeWorkBuffer.data();
        int size = (int)decodeWorkBuffer.size();

        while (size > 0) {
            int len = av_parser_parse2(parser, c, &pkt->data, &pkt->size, data, size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            data += len;
            size -= len;

            if (pkt->size > 0) {
                int ret = avcodec_send_packet(c, pkt);
                
                if (ret < 0 && !cached_headers.empty()) {
                    AVPacket* sps_pkt = av_packet_alloc();
                    sps_pkt->data = cached_headers.data();
                    sps_pkt->size = (int)cached_headers.size();
                    avcodec_send_packet(c, sps_pkt);
                    av_packet_free(&sps_pkt);
                    ret = avcodec_send_packet(c, pkt);
                }

                if (ret >= 0) {
                    while (avcodec_receive_frame(c, frame) >= 0) {
                        if (parser->key_frame == 1 && cached_headers.empty()) {
                             cached_headers.assign(pkt->data, pkt->data + pkt->size);
                        }
                        sws_scale(sws_ctx, frame->data, frame->linesize, 0, H, frameRGB->data, frameRGB->linesize);
                        scSendFrame(cam, frameRGB->data[0]);
                    }
                }
            }
        }
        decodeWorkBuffer.clear();
    }

    isRunning = false;
    closesocket(sock);
    WSACleanup();
    if (reader.joinable()) reader.join();
    
    av_parser_close(parser);
    sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    av_frame_free(&frameRGB);
    av_packet_free(&pkt);
    av_free(bufferRGB);
    avcodec_free_context(&c);
    scDeleteCamera(cam);

    return 0;
}