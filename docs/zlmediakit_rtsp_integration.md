# 进程内 ZLMediaKit RTSP 服务

项目不再用 FFmpeg 把编码流推给另一个 RTSP 进程。现在的数据路径是：

```text
MosaicComposer
  -> RkMppEncoder（输出 Annex-B H.264）
  -> ZlMediaPublisher（拆分 NALU、90 kHz 时间戳转毫秒）
  -> ZLMediaKit 媒体源
  -> RTSP 客户端（TCP 或 UDP，可多客户端）
```

ZLMediaKit 根据送入的 SPS/PPS 创建 H.264 track 和 SDP。后续 IDR/P 帧进入同一
媒体源；RTSP 的 `DESCRIBE`、`SETUP`、`PLAY`、`PAUSE` 和 `TEARDOWN` 由
ZLMediaKit 处理。某个客户端 `PAUSE` 只暂停该客户端的发送，编码和媒体源仍继续
运行，其他客户端不受影响。

## 构建

默认使用同级目录中的 ZLMediaKit：

```bash
cmake -S . -B build
cmake --build build -j4
cmake --install build
```

如果目录不同，可指定：

```bash
cmake -S . -B build \
  -DZLMEDIAKIT_ROOT=/path/to/ZLMediaKit \
  -DZLMEDIAKIT_API_LIBRARY=/path/to/libmk_api.so
```

安装目录会包含 `lib/libmk_api.so`，程序的运行库路径为 `$ORIGIN/lib`。

## 运行和播放

默认监听所有网卡的 8554 端口，拼图流为：

```text
rtsp://<开发板IP>:8554/live/mosaic
```

可以用环境变量换端口：

```bash
ZLMEDIAKIT_RTSP_PORT=18554 ./rknn_yolov5_demo <yolo.rknn> <face.rknn>
```

播放器也要改用相同端口。`127.0.0.1` 只适用于开发板本机；其他机器应填写开发板
IP。

同一端口不能同时由独立 `MediaServer` 和本程序监听。切换到进程内服务前应停止
旧 `MediaServer`，或者通过 `ZLMEDIAKIT_RTSP_PORT` 给本程序换一个端口。

## 独立冒烟测试

测试程序不会访问 RKNN、MPP 编码器或摄像头，可单独检查 ZLMediaKit、H.264、
SDP 和 RTSP 播放链路：

```bash
cmake -S . -B build -DBUILD_ZLMEDIAKIT_SMOKE_TEST=ON
cmake --build build --target zlmedia_rtsp_smoke -j4
./build/zlmedia_rtsp_smoke test.h264 240
```

测试地址为 `rtsp://127.0.0.1:18554/live/smoke`。
