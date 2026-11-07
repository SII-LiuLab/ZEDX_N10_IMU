// Live SHW acquisition using the sensor's factory NVIDIA ISP tuning.
#include <Argus/Argus.h>
#include <EGLStream/EGLStream.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <csignal>
#include <chrono>
#include <future>
#ifdef N10_ENABLE_PREVIEW
#include <EGLStream/NV/ImageNativeBuffer.h>
#include <nvbufsurface.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#endif
#include "camera_transition_lock.h"
#include "live_preview.h"
using namespace Argus;
namespace fs = std::filesystem;
static volatile std::sig_atomic_t stopping = 0;
static void stop_capture(int) { stopping = 1; }
static void ok(Status s, const char* what) {
    if (s != STATUS_OK) throw std::runtime_error(std::string(what)+": "+std::to_string(s));
}
template<class I, class T> I* api(T* p) {
    auto* i=interface_cast<I>(p);
    if (!i) throw std::runtime_error("Missing Argus interface");
    return i;
}
// EGLStream buffers may be block-linear even when mapBuffer() succeeds.
// Copy into one reusable pitch-linear surface before CPU colour conversion.
#ifdef N10_ENABLE_PREVIEW
class PreviewSurface {
    int fd = -1;
    NvBufSurface* surface = nullptr;
    bool mapped = false;
public:
    ~PreviewSurface() {
        if (mapped) NvBufSurfaceUnMap(surface,0,-1);
        if (surface) NvBufSurfaceDestroy(surface);
    }
    cv::Mat bgr(EGLStream::Image* image, int width, int height) {
        auto native=api<EGLStream::NV::IImageNativeBuffer>(image);
        if (fd < 0) {
            fd=native->createNvBuffer(Size2D<uint32_t>(width,height),NVBUF_COLOR_FORMAT_NV12,NVBUF_LAYOUT_PITCH);
            if (fd < 0 || NvBufSurfaceFromFd(fd,reinterpret_cast<void**>(&surface)))
                throw std::runtime_error("Cannot create pitch-linear preview buffer");
            if (NvBufSurfaceMap(surface,0,-1,NVBUF_MAP_READ))
                throw std::runtime_error("Cannot map pitch-linear preview buffer");
            mapped=true;
        } else {
            ok(native->copyToNvBuffer(fd),"copy pitch-linear preview");
        }
        if (NvBufSurfaceSyncForCpu(surface,0,-1)) throw std::runtime_error("Cannot synchronize preview buffer");
        const auto& p=surface->surfaceList[0];
        cv::Mat y(height,width,CV_8UC1,p.mappedAddr.addr[0],p.planeParams.pitch[0]);
        cv::Mat uv(height/2,width/2,CV_8UC2,p.mappedAddr.addr[1],p.planeParams.pitch[1]);
        cv::Mat rgb;
        cv::cvtColorTwoPlane(y,uv,rgb,cv::COLOR_YUV2BGR_NV12);
        return rgb;
    }
};
#endif
int main(int argc, char** argv) {
    CameraTransitionLock transition;
    try {
        if (argc != 5) throw std::runtime_error("Usage: capture_shw_n10 CAMERA_INDEX NEW_OUTPUT_DIR FRAMES SAVE_EVERY");
        unsigned camera=std::stoul(argv[1]), count=std::stoul(argv[3]), every=std::stoul(argv[4]);
        fs::path output(argv[2]);
        LivePreview preview(output);
#ifdef N10_ENABLE_PREVIEW
        if (!preview.enabled) throw std::runtime_error("Live SHW executable requires N10_LIVE");
#else
        if (preview.enabled) throw std::runtime_error("Use capture_shw_live_n10 for live preview");
#endif
        if ((camera!=2 && camera!=3) || (!count && !preview.enabled) || count>100000 || !every)
            throw std::runtime_error("Invalid camera/frame arguments");
        if (!fs::create_directory(output)) throw std::runtime_error("Output exists");
        std::signal(SIGTERM,stop_capture); std::signal(SIGINT,stop_capture);
        UniqueObj<CameraProvider> provider(CameraProvider::create());
        auto cp=api<ICameraProvider>(provider.get());
        std::vector<CameraDevice*> devices; ok(cp->getCameraDevices(&devices),"enumerate");
        if(camera>=devices.size()) throw std::runtime_error("Missing SHW camera");
        std::vector<SensorMode*> modes;
        ok(api<ICameraProperties>(devices[camera])->getAllSensorModes(&modes),"modes");
        if(modes.empty() || api<ISensorMode>(modes[0])->getResolution()!=Size2D<uint32_t>(2560,1984))
            throw std::runtime_error("Unexpected SHW mode");
        UniqueObj<CaptureSession> session(cp->createCaptureSession(devices[camera]));
        auto cs=api<ICaptureSession>(session.get());
        UniqueObj<OutputStreamSettings> settings(cs->createOutputStreamSettings(STREAM_TYPE_EGL));
        auto oes=api<IEGLOutputStreamSettings>(settings.get());
        ok(oes->setPixelFormat(PIXEL_FMT_YCbCr_420_888),"YUV");
        ok(oes->setResolution(preview.enabled ? Size2D<uint32_t>(preview.width,preview.height(2560,1984))
                                             : Size2D<uint32_t>(2560,1984)),"resolution");
        ok(oes->setMetadataEnable(true),"metadata");
        UniqueObj<OutputStream> stream(cs->createOutputStream(settings.get()));
        UniqueObj<EGLStream::FrameConsumer> consumer(EGLStream::FrameConsumer::create(stream.get()));
        auto fc=api<EGLStream::IFrameConsumer>(consumer.get());
        UniqueObj<Request> request(cs->createRequest(CAPTURE_INTENT_VIDEO_RECORD));
        auto rq=api<IRequest>(request.get()); ok(rq->enableOutputStream(stream.get()),"stream");
        auto ss=api<ISourceSettings>(rq->getSourceSettings());
        ok(ss->setSensorMode(modes[0]),"mode");
        ok(ss->setFrameDurationRange(Range<uint64_t>(33333333)),"frame duration");
        ok(ss->setExposureTimeRange(Range<uint64_t>(7500000)),"exposure");
        ok(ss->setGainRange(Range<float>(1)),"analog gain");
        auto ac=api<IAutoControlSettings>(rq->getAutoControlSettings());
        ok(ac->setIspDigitalGainRange(Range<float>(1)),"digital gain");
        ok(ac->setAwbMode(AWB_MODE_AUTO),"AWB");
        std::ofstream frames;
        if (!preview.enabled) {
            frames.open(output/"frames.csv");
            frames<<"index,timestamp_ns,host_monotonic_ns,exposure_ns,analog_gain,awb_cct,saved\n";
        }
        ok(cs->repeat(request.get()),"repeat");
        struct RelockOnExit {
            CameraTransitionLock& lock;
            ~RelockOnExit() { lock.lock(); }
        } relock{transition};
        unsigned long long acquired=0;
#ifdef N10_ENABLE_PREVIEW
        PreviewSurface preview_surface;
        std::future<void> preview_writer;
#endif
        for(unsigned long long n=0; (preview.enabled || n<count+90) && !stopping; ++n) {
            UniqueObj<EGLStream::Frame> frame(fc->acquireFrame(10000000000ULL));
            auto fi=api<EGLStream::IFrame>(frame.get());
            if(n<90) continue;
            if(n==90) transition.unlock();
            auto md=api<ICaptureMetadata>(api<EGLStream::IArgusCaptureMetadata>(frame.get())->getMetadata());
            auto host=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#ifdef N10_ENABLE_PREVIEW
            if (preview.due() && (!preview_writer.valid() ||
                                 preview_writer.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
                if (preview_writer.valid()) preview_writer.get();
                cv::Mat rgb=preview_surface.bgr(fi->getImage(),preview.width,preview.height(2560,1984));
                auto timestamp=md->getSensorTimestamp();
                auto received=acquired+1;
                preview_writer=std::async(std::launch::async,[rgb,&preview,timestamp,host,received]() {
                    std::vector<unsigned char> jpeg;
                    if(!cv::imencode(".jpg",rgb,jpeg,{cv::IMWRITE_JPEG_QUALITY,80}))
                        throw std::runtime_error("Cannot encode live JPEG");
                    preview.publish_bytes({std::string(jpeg.begin(),jpeg.end())},timestamp,host,received);
                });
            }
#endif
            const bool saved=!preview.enabled && acquired%every==0;
            if(saved) {
                auto jpeg=api<EGLStream::IImageJPEG>(fi->getImage());
                fs::path path=output/(std::to_string(acquired)+"-rgb.jpg");
                ok(jpeg->writeJPEG(path.c_str()),"JPEG");
                fs::copy_file(path,output/"latest-tmp.jpg",fs::copy_options::overwrite_existing);
                fs::rename(output/"latest-tmp.jpg",output/"latest.jpg");
            }
            if (!preview.enabled) {
                frames<<acquired<<','<<md->getSensorTimestamp()<<','<<host<<','<<md->getSensorExposureTime()<<','<<md->getSensorAnalogGain()<<','<<md->getAwbCct()<<','<<saved<<'\n';
                frames.flush(); if(!frames) throw std::runtime_error("Metadata write failed");
            }
            if(!acquired) { std::ofstream ready(output/"ready"); ready<<md->getSensorTimestamp()<<'\n'; }
            ++acquired;
        }
#ifdef N10_ENABLE_PREVIEW
        if (preview_writer.valid()) preview_writer.get();
#endif
        transition.lock();
        cs->stopRepeat(); ok(cs->waitForIdle(10000000000ULL),"idle");
        std::ofstream complete(output/"complete.json");
        complete<<"{\"backend\":\"live-argus-isp\",\"camera_index\":"<<camera<<",\"frames\":"<<acquired<<"}\n";
        return complete ? 0 : 1;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
