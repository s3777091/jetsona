#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

std::atomic<bool> running{true};

void stop_handler(int) { running.store(false); }

int env_int(const char *name, int fallback, int minimum, int maximum) {
    const char *value = std::getenv(name);
    if (!value || !value[0]) return fallback;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (!end || *end || parsed < minimum || parsed > maximum) return fallback;
    return static_cast<int>(parsed);
}

std::string framebuffer_format(const fb_var_screeninfo &vinfo) {
    if (vinfo.bits_per_pixel != 32 || vinfo.green.offset != 8) return {};
    // The scanout alpha byte is unused. Advertising it as padding gives
    // nvvidconv the widely supported BGRx/RGBx input caps on JetPack 4.x.
    if (vinfo.red.offset == 16 && vinfo.blue.offset == 0) return "BGRx";
    if (vinfo.red.offset == 0 && vinfo.blue.offset == 16) return "RGBx";
    return {};
}

bool print_bus_error(GstElement *pipeline) {
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_pop_filtered(
        bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    gst_object_unref(bus);
    if (!message) return false;

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        std::fprintf(stderr, "fb-webrtc: pipeline error: %s\n",
                     error ? error->message : "unknown error");
        if (debug && debug[0]) std::fprintf(stderr, "fb-webrtc: %s\n", debug);
        if (error) g_error_free(error);
        g_free(debug);
    } else {
        std::fprintf(stderr, "fb-webrtc: pipeline ended\n");
    }
    gst_message_unref(message);
    return true;
}

int run_x11_pipeline(const char *rtsp_url, int fps, int bitrate, int key_interval) {
    const char *display = std::getenv("DISPLAY");
    if (!display || !display[0]) display = ":0";
    const int width = env_int("JETSON_WEBRTC_WIDTH", 800, 64, 4096);
    const int height = env_int("JETSON_WEBRTC_HEIGHT", 480, 64, 2160);

    char description[4096];
    std::snprintf(
        description, sizeof(description),
        "ximagesrc display-name=\"%s\" use-damage=false show-pointer=true "
        "! video/x-raw,width=%d,height=%d,framerate=%d/1 "
        "! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream "
        "! nvvidconv "
        "! video/x-raw(memory:NVMM),format=NV12 "
        "! nvv4l2h264enc maxperf-enable=true preset-level=1 control-rate=1 "
        "bitrate=%d iframeinterval=%d insert-sps-pps=true profile=0 "
        "! h264parse config-interval=-1 "
        "! video/x-h264,profile=baseline "
        "! rtspclientsink protocols=tcp location=\"%s\"",
        display, width, height, fps, bitrate, key_interval, rtsp_url);

    GError *parse_error = nullptr;
    GstElement *pipeline = gst_parse_launch(description, &parse_error);
    if (!pipeline) {
        std::fprintf(stderr, "x11-webrtc: pipeline parse: %s\n",
                     parse_error ? parse_error->message : "unknown error");
        if (parse_error) g_error_free(parse_error);
        return 2;
    }
    if (parse_error) {
        std::fprintf(stderr, "x11-webrtc: pipeline warning: %s\n", parse_error->message);
        g_error_free(parse_error);
    }
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::fprintf(stderr, "x11-webrtc: pipeline failed to start on %s\n", display);
        gst_object_unref(pipeline);
        return 2;
    }

    std::fprintf(stderr,
                 "x11-webrtc: streaming display %s at %dx%d, %d fps, %.2f Mbps\n",
                 display, width, height, fps, bitrate / 1000000.0);
    int result = 0;
    while (running.load()) {
        if (print_bus_error(pipeline)) {
            result = 2;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return result;
}

}  // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    gst_init(&argc, &argv);

    const char *device = std::getenv("JETSON_WEBRTC_FRAMEBUFFER");
    if (!device || !device[0]) device = "/dev/fb0";
    const char *rtsp_url = std::getenv("JETSON_WEBRTC_RTSP_URL");
    if (!rtsp_url || !rtsp_url[0]) {
        std::fprintf(stderr, "fb-webrtc: JETSON_WEBRTC_RTSP_URL is required\n");
        return 2;
    }
    const int fps = env_int("JETSON_WEBRTC_FPS", 30, 5, 60);
    const int bitrate = env_int("JETSON_WEBRTC_BITRATE", 2500000, 250000, 12000000);
    const int key_interval = env_int("JETSON_WEBRTC_KEY_INTERVAL", fps, 5, 300);
    const char *source_mode = std::getenv("JETSON_WEBRTC_SOURCE");
    if (source_mode && std::strcmp(source_mode, "x11") == 0)
        return run_x11_pipeline(rtsp_url, fps, bitrate, key_interval);

    const int fd = open(device, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        std::fprintf(stderr, "fb-webrtc: open %s: %s\n", device, std::strerror(errno));
        return 2;
    }

    fb_var_screeninfo vinfo{};
    fb_fix_screeninfo finfo{};
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        std::fprintf(stderr, "fb-webrtc: framebuffer information: %s\n",
                     std::strerror(errno));
        close(fd);
        return 2;
    }

    const std::string format = framebuffer_format(vinfo);
    if (format.empty()) {
        std::fprintf(stderr,
                     "fb-webrtc: unsupported framebuffer layout: %u bpp r=%u g=%u b=%u a=%u\n",
                     vinfo.bits_per_pixel, vinfo.red.offset, vinfo.green.offset,
                     vinfo.blue.offset, vinfo.transp.offset);
        close(fd);
        return 2;
    }

    const size_t map_size = finfo.smem_len;
    const auto *framebuffer = static_cast<const unsigned char *>(
        mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd, 0));
    if (framebuffer == MAP_FAILED) {
        std::fprintf(stderr, "fb-webrtc: mmap %s: %s\n", device, std::strerror(errno));
        close(fd);
        return 2;
    }

    const int width = static_cast<int>(vinfo.xres);
    const int height = static_cast<int>(vinfo.yres);
    const size_t packed_stride = static_cast<size_t>(width) * 4;
    const size_t packed_size = packed_stride * static_cast<size_t>(height);
    const size_t source_offset =
        static_cast<size_t>(vinfo.yoffset) * finfo.line_length +
        static_cast<size_t>(vinfo.xoffset) * 4;

    char description[4096];
    std::snprintf(
        description, sizeof(description),
        "appsrc name=fb_src is-live=true block=false format=time "
        "caps=video/x-raw,format=%s,width=%d,height=%d,framerate=%d/1 "
        "! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream "
        "! nvvidconv "
        "! video/x-raw(memory:NVMM),format=NV12 "
        "! nvv4l2h264enc maxperf-enable=true preset-level=1 control-rate=1 "
        "bitrate=%d iframeinterval=%d insert-sps-pps=true profile=0 "
        "! h264parse config-interval=-1 "
        "! video/x-h264,profile=baseline "
        "! rtspclientsink protocols=tcp location=\"%s\"",
        format.c_str(), width, height, fps, bitrate, key_interval, rtsp_url);

    GError *parse_error = nullptr;
    GstElement *pipeline = gst_parse_launch(description, &parse_error);
    if (!pipeline) {
        std::fprintf(stderr, "fb-webrtc: pipeline parse: %s\n",
                     parse_error ? parse_error->message : "unknown error");
        if (parse_error) g_error_free(parse_error);
        munmap(const_cast<unsigned char *>(framebuffer), map_size);
        close(fd);
        return 2;
    }
    if (parse_error) {
        std::fprintf(stderr, "fb-webrtc: pipeline warning: %s\n", parse_error->message);
        g_error_free(parse_error);
    }

    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "fb_src");
    if (!source) {
        std::fprintf(stderr, "fb-webrtc: appsrc was not created\n");
        gst_object_unref(pipeline);
        munmap(const_cast<unsigned char *>(framebuffer), map_size);
        close(fd);
        return 2;
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::fprintf(stderr, "fb-webrtc: pipeline failed to start\n");
        gst_object_unref(source);
        gst_object_unref(pipeline);
        munmap(const_cast<unsigned char *>(framebuffer), map_size);
        close(fd);
        return 2;
    }

    std::fprintf(stderr,
                 "fb-webrtc: streaming %dx%d %s at %d fps, %.2f Mbps\n",
                 width, height, format.c_str(), fps, bitrate / 1000000.0);

    const auto frame_period = std::chrono::nanoseconds(1000000000LL / fps);
    auto deadline = std::chrono::steady_clock::now();
    guint64 frame_number = 0;
    int result = 0;
    while (running.load()) {
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, packed_size, nullptr);
        if (!buffer) {
            std::fprintf(stderr, "fb-webrtc: unable to allocate video buffer\n");
            result = 2;
            break;
        }

        GstMapInfo map{};
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            gst_buffer_unref(buffer);
            result = 2;
            break;
        }
        for (int y = 0; y < height; ++y) {
            const size_t source_row = source_offset +
                static_cast<size_t>(y) * finfo.line_length;
            if (source_row + packed_stride > map_size) {
                std::memset(map.data + static_cast<size_t>(y) * packed_stride, 0,
                            packed_stride);
            } else {
                std::memcpy(map.data + static_cast<size_t>(y) * packed_stride,
                            framebuffer + source_row, packed_stride);
            }
        }
        gst_buffer_unmap(buffer, &map);

        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(frame_number, GST_SECOND, fps);
        GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, fps);
        ++frame_number;

        const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
        if (flow != GST_FLOW_OK) {
            std::fprintf(stderr, "fb-webrtc: appsrc stopped (%s)\n",
                         gst_flow_get_name(flow));
            result = 2;
            break;
        }
        if (print_bus_error(pipeline)) {
            result = 2;
            break;
        }

        deadline += frame_period;
        const auto now = std::chrono::steady_clock::now();
        if (deadline > now) std::this_thread::sleep_until(deadline);
        else if (now - deadline > frame_period * 2) deadline = now;
    }

    gst_app_src_end_of_stream(GST_APP_SRC(source));
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(source);
    gst_object_unref(pipeline);
    munmap(const_cast<unsigned char *>(framebuffer), map_size);
    close(fd);
    return result;
}
