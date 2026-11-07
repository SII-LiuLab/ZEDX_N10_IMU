# ZED X 内置 IMU / N10 联合采集

ZED X 内置 BMI088 驱动适配、双目图像与 IMU 联合采集工具，以及实际设备验证数据。

**这是已完成图像驱动配置的 N10 的 IMU 补充包，不是通用 Jetson 驱动或完整刷机包。** 本仓库保留原交付文件和校验值；不包含设备登录密码、厂商下载账号或原始开发包中的其他厂商资料。

## 适用配置

开发前请阅读 [开发依赖与需提供的资料](docs/开发依赖与资料清单.md)：列明必需文档、厂商包、匹配 BSP/OOT 源码、图像基础交付及授权获取要求，不包含任何访问凭据。

- 天准 N10，板型 `B25EJ21_007`，原厂定制 `6.8.12-rt-tegra` / L4T R38.2 内核。
- ZED X 接 CAM-3/V7；双目 `/dev/video4`、`/dev/video5`。
- 内置 BMI088：I²C bus 7，陀螺仪 `0x3b`、加速度计 `0x3c`。
- 依赖此前已安装并验证的相机模块、设备树，以及 `load_zedx_n10.sh` 加载脚本和 SHW 参考工程，按实际部署位置配置。这些基础组件不在本 IMU 补充包中。
- 安装器检查原有模块哈希，不匹配时拒绝安装；不要通过删除检查来安装到其他系统。

## 验证结果

两轮各 45 秒联合采集，第二轮在完整重新初始化后执行：

| 项目 | 第一轮 | 第二轮 |
| --- | ---: | ---: |
| IMU 样本 | 8,993 | 8,996 |
| IMU 稳定段速率 | 约 199.925 Hz | 约 199.925 Hz |
| 图像 | 每目 1,212 帧，约 30 fps | 每目 1,212 帧，约 30 fps |
| 图像错误帧 / 帧序号断点 | 0 / 0 | 0 / 0 |
| IMU I²C 错误 / 环形缓冲区丢样 / FIFO 溢出观测 | 0 / 0 / 0 | 0 / 0 / 0 |

已验证完整重新加载、原模块回退、正常中断、再次采集，以及忙碌状态下拒绝第二消费者和拒绝重新初始化。

**验证边界：**通过的是采集链路及软件时间配对，不是曝光时刻、动态视觉—惯性时差或绝对 UTC 标定。图像时间是 VI SOF；IMU 时间是主机单调时钟下的软件估计。最近邻样本时间差约 2.76 ms 的上限不能解释为实际传感器同步精度。没有长期压力测试或开机自启验收。

## 使用

克隆仓库，普通 Git 即可，无需 Git LFS。仓库包含实际 PNG/RAW 和完整 IMU CSV，体积约 173 MiB（不含 Git 历史）。

```bash
git clone git@github.com:Ericonaldo/ZEDX_N10_IMU.git
cd ZEDX_N10_IMU
```

仅在上述已配置好的 N10 上安装：

```bash
bash scripts/install_imu_on_n10.sh --check
sudo bash scripts/install_imu_on_n10.sh --install
```

已安装的当前 N10 不需要重复安装。停止所有采集程序后，在已安装补充包的根目录初始化并启动新一轮记录：

```bash
sudo bash scripts/load_zedx_imu_n10.sh --full
sudo bash scripts/capture_joint_n10.sh "$PWD/ouputs/joint-new-01"
```

输出目录必须不存在；下一次采集更换目录名。采集完毕会停止 IMU 采样。原 NVIDIA 模块及启动文件不覆盖，没有新增自动启动服务。禁止执行系统整体升级或直接断电。

完整操作、数据字段、坐标轴与单位、构建及回退见 [使用指南](docs/使用指南.md)；统计结果与边界见上文 [验证结果](#验证结果)。

## 文件结构

```text
driver/                    两个适配后内核模块
source/                    内核模块及采集程序源码，保留原许可声明
scripts/                   安装、初始化、采集、导出与统计工具
validation/joint-01/       第一轮联合采集及 5 对图像/RAW
validation/joint-02/       重新初始化后的第二轮联合采集
validation/imu-*/          IMU 冒烟、中断及重新启动测试
validation/system-final.txt 最终驱动状态和校验记录
```

图像、原始帧与 IMU 可以通过眼别和帧序号追溯：`images/pairs.json` → `frame-imu-pairs.csv` → `imu/imu.csv`。RAW 使用校验值与偏移定位，不能用文件修改时间作拍摄时间。

原交付包中的 `SHA256SUMS.txt` 对应原交付文件，`RUNTIME-SHA256SUMS.txt` 对应安装组件；后续新增的 Git 文档和仓库配置不在原交付清单内。当前仓库保留安装组件清单，在仓库根目录检查：

```bash
shasum -a 256 -c RUNTIME-SHA256SUMS.txt
```

Linux 上可将 `shasum -a 256` 换成 `sha256sum`。请在验证数据副本上重新运行分析工具，以保留原始证据文件。

## 三相机离散采集

新增两台 SHW5G 与一台 ZED X Mini 的并行采集、独立启停、间隔保存和校验回传。三相机接线为 CAM-3 V6/V7 SHW、V8 ZED，与上文原单 ZED + IMU 的 CAM-3/V7 配置不同。

| 接口 | 相机 | 输出 |
| --- | --- | --- |
| CAM-3 V6 / V7 | SENSING SHW5G，ST VB1940 | 每台 2560×1984 RGB |
| CAM-3 V8 | Stereolabs ZED X Mini，双 AR0234，SN53004639 | 1920×1200 校正左右 RGB、米制深度和置信度 |

SHW 使用型号对应的 NVIDIA Argus ISP；ZED 使用官方 SDK 5.4.1 / L4T38.2、内置 V4 采集后端和 NEURAL 深度模式。两台 SHW 为单目 RGB，相机各自不输出深度。

在开发电脑的仓库根目录执行，使用已配置的 SSH 别名 `thor`：

```bash
python3 scripts/capture_remote_n10.py record --initialize --cameras shw_v6 shw_v7 zed --name three-camera-session --frames 600 --save-every 60
```

每路预热 90 帧后接收 600 帧，每 60 帧保存一组，共各 10 组离散样本，逐帧时间戳保留。采集结束后自动回传，并进行完整会话 SHA256 校验。

`--initialize` 只在全部相机和 IMU 空闲时使用，设备重启后初始化一次，后续运行省略。当前三相机接线使用混合驱动初始化，不执行上文原单相机配置的 `--full` 命令。

用 `--cameras` 选择独立运行的相机集合；省略时采集全部三路。不同集合可以在不同终端并行执行，使用不同会话名：

```bash
python3 scripts/capture_remote_n10.py record --cameras zed --name zed-session
python3 scripts/capture_remote_n10.py record --cameras shw_v6 shw_v7 --name shw-session
```

Ctrl+C 只停止该命令管理的相机；同一相机不能重复占用。已保存会话可续传：

```bash
python3 scripts/capture_remote_n10.py fetch three-camera-session
```

深度以 float32 米制数据保存，保留 NaN/Inf。三路并行不表示同步曝光；间隔保存不表示无丢帧全帧录像。三相机入口不导出独立 IMU 数据流。

## 三相机实时预览

新增电脑浏览器实时预览，复用上述相机采集、型号 ISP 和官方 RGB-D 处理。以下命令在开发电脑的仓库根目录执行：

```bash
python3 scripts/capture_remote_n10.py live
```

默认打开 `http://127.0.0.1:8844/`，显示两路 SHW RGB 和 ZED RGB、深度。默认预览宽度 960，发布上限 10 fps；SHW 各为 960×744，ZED RGB 与深度各为 960×600。

选择单路或多路相机：

```bash
python3 scripts/capture_remote_n10.py live --cameras zed
python3 scripts/capture_remote_n10.py live --cameras shw_v6
python3 scripts/capture_remote_n10.py live --cameras shw_v6 shw_v7
```

调整预览尺寸、帧率和本地端口：

```bash
python3 scripts/capture_remote_n10.py live --cameras shw_v7 zed --preview-width 640 --preview-fps 15 --port 8845
```

ZED 的 RGB 与深度来自同一次 SDK 采集并成对显示；SHW 仅显示 RGB。页面显示各路状态和帧率，支持放大画面。纯预览不录制原始数据，只发布最新画面，网络变慢时不累计旧帧。

不同相机集合可以分别实时预览或离散采集；多个预览会话使用不同的 `--port`。已有相机运行时，启动其他路省略 `--initialize`。同一相机需要从预览切换到录制时，先停止其预览会话。

Ctrl+C 停止本次会话并释放相机；关闭或刷新浏览器不停止终端会话。深度显示图仅用于观察，距离数值检查使用原始米制深度。

完整型号、参数、处理方式和并行操作见 [三相机使用指南](docs/三相机使用指南.md)；测试设置与结果见 [三相机验收说明](docs/三相机验收说明.md)。
