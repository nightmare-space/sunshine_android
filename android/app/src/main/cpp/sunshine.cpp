#include <jni.h>
#include <string>
#include "logging.h"
#include "config.h"
#include "nvhttp.h"
#include "globals.h"
#include "sunshine.h"
#include "stream.h"
#include "rtsp.h"
#include "audio.h"
#include "moonlight-common-c/src/input.h"
#include "video_colorspace.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <boost/endian/buffers.hpp>

using namespace std::literals;

extern "C" {

static std::unique_ptr<logging::deinit_t> deinit;
static JavaVM *jvm = nullptr;
static JNIEnv *g_env = nullptr;
static jclass sunshineServerClass = nullptr;
static audio::sample_queue_t samples = nullptr;

// 声明全局变量来存储音频录制状态
static std::thread audioRecordingThread;

/// Create a Java Integer object
jobject createJavaInt(JNIEnv *env, int value) {
    jclass integerClass = env->FindClass("java/lang/Integer");
    jmethodID integerConstructor = env->GetMethodID(integerClass, "<init>", "(I)V");
    return env->NewObject(integerClass, integerConstructor, value);
}
/// 一个封装好的函数，用于调用 Java 方法
/// A helper function to call Java methods
void invokeJavaFunction(
        const char *name,
        const char *sig
        ...
) {
    if (jvm == nullptr) {
        BOOST_LOG(error) << "JVM is null!"sv;
        return;
    }
    JNIEnv *env;
    jint result = jvm->AttachCurrentThread(&env, nullptr);
    if (result != JNI_OK) {
        BOOST_LOG(error) << "Cannot attach java thread!"sv;
        return;
    }

    jmethodID method = env->GetStaticMethodID(sunshineServerClass, name, sig);
    if (method == nullptr) {
        BOOST_LOG(error) << "Cannot find method "sv << name << " with signature "sv << sig;
        jvm->DetachCurrentThread();
        return;
    }
    va_list args;
    va_start(args, sig);
    env->CallStaticVoidMethodV(sunshineServerClass, method, args);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    jvm->DetachCurrentThread();
}
/// Create a Java Double object
jobject createJavaDouble(JNIEnv *env, double value) {
    jclass doubleClass = env->FindClass("java/lang/Double");
    jmethodID doubleConstructor = env->GetMethodID(doubleClass, "<init>", "(D)V");
    return env->NewObject(doubleClass, doubleConstructor, value);
}

jobject createHashMap(JNIEnv *env) {
    jclass hashMapClass = env->FindClass("java/util/HashMap");
    jmethodID hashMapConstructor = env->GetMethodID(hashMapClass, "<init>", "()V");
    return env->NewObject(hashMapClass, hashMapConstructor);
}

/// 将 C 中 的 int、double、std::string 转换为 Java 中的 Integer、Double、String
/// Convert int, double, and std::string in C to Integer, Double, and String in Java
jobject convertToJavaObject(JNIEnv *env, const std::any &value) {
    if (value.type() == typeid(int)) {
        return createJavaInt(env, std::any_cast<int>(value));
    } else if (value.type() == typeid(double)) {
        return createJavaDouble(env, std::any_cast<double>(value));
    } else {
        return env->NewStringUTF(std::any_cast<std::string>(value).c_str());
    }
}
void putValueInJavaHashMap(
        JNIEnv *env,
        jobject hashMap,
        const std::string &key,
        const std::any &value
) {
    jclass hashMapClass = env->FindClass("java/util/HashMap");
    const char *sig = "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;";
    jmethodID putMethod = env->GetMethodID(hashMapClass, "put", sig);

    jstring javaKey = env->NewStringUTF(key.c_str());
    jobject javaValue = convertToJavaObject(env, value);

    env->CallObjectMethod(hashMap, putMethod, javaKey, javaValue);

    env->DeleteLocalRef(javaKey);
    env->DeleteLocalRef(javaValue);
}

jobject convertMapToJavaHashMap(JNIEnv *env, const std::map<std::string, std::any> &cppMap) {
    jobject map = createHashMap(env);

    // Iterate over the C++ map and put entries into the HashMap
    for (const auto &entry: cppMap) {
        putValueInJavaHashMap(env, map, entry.first, entry.second);
    }

    return map;
}


JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_start(JNIEnv *env, jclass clazz) {
    env->GetJavaVM(&jvm);
    g_env = env;
    // Use the passed clazz parameter directly instead of looking up the class by name
    sunshineServerClass = (jclass) env->NewGlobalRef(clazz);
    if (sunshineServerClass == nullptr) {
        BOOST_LOG(error) << "Failed to create global reference for SunshineServer class"sv;
        return;
    }
    deinit = logging::init(1, "/dev/null");
    BOOST_LOG(info) << "Start sunshine server"sv;
    // log sunshineServerClass
    BOOST_LOG(info) << "sunshineServerClass: "sv << sunshineServerClass;
    mail::man = std::make_shared<safe::mail_raw_t>();
    task_pool.start(1);
    std::thread httpThread{nvhttp::start};
    rtsp_stream::rtpThread();
    httpThread.join();
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_setSunshineName(JNIEnv *env, jclass clazz,
                                                         jstring sunshine_name) {
    const char *str = env->GetStringUTFChars(sunshine_name, nullptr);
    config::nvhttp.sunshine_name = str;
    env->ReleaseStringUTFChars(sunshine_name, str);
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_setPkeyPath(JNIEnv *env, jclass clazz, jstring path) {
    const char *str = env->GetStringUTFChars(path, nullptr);
    config::nvhttp.pkey = str;
    env->ReleaseStringUTFChars(path, str);
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_setCertPath(JNIEnv *env, jclass clazz, jstring path) {
    const char *str = env->GetStringUTFChars(path, nullptr);
    config::nvhttp.cert = str;
    env->ReleaseStringUTFChars(path, str);
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_setFileStatePath(JNIEnv *env, jclass clazz,
                                                          jstring path) {
    const char *str = env->GetStringUTFChars(path, nullptr);
    config::nvhttp.file_state = str;
    env->ReleaseStringUTFChars(path, str);
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_submitPin(JNIEnv *env, jclass clazz, jstring pin) {
    const char *pinStr = env->GetStringUTFChars(pin, nullptr);
    nvhttp::pin(pinStr, "some-moonlight");
    env->ReleaseStringUTFChars(pin, pinStr);
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_SunshineServer_cleanup(JNIEnv *env, jclass clazz) {
    if (sunshineServerClass != nullptr) {
        env->DeleteGlobalRef(sunshineServerClass);
        sunshineServerClass = nullptr;
    }
    // 其他清理工作...
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_postAudioSample(JNIEnv *env, jclass clazz,
                                                         jfloatArray audioData,
                                                         jint sampleCount) {
    // 获取 Java 浮点数组的元素
    jfloat *audioBuffer = env->GetFloatArrayElements(audioData, nullptr);
    if (audioBuffer == nullptr) {
        BOOST_LOG(error) << "无法获取音频数据缓冲区"sv;
        return;
    }

    // 将 Java 浮点数组转换为 std::vector<float>
    std::vector<float> audioSamples(audioBuffer, audioBuffer + sampleCount);

    // 将音频数据传递给 Sunshine 的音频处理系统
    if (samples) {
        samples->raise(std::move(audioSamples));
    } else {
        BOOST_LOG(error) << "音频样本队列未初始化"sv;
    }

    // 释放 Java 数组
    env->ReleaseFloatArrayElements(audioData, audioBuffer, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_startAudioRecording(JNIEnv *env, jclass clazz,
                                                             jobject audioRecord,
                                                             jint framesPerPacket) {
    // 创建 AudioRecord 的全局引用，以便在线程中使用
    jobject globalAudioRecord = env->NewGlobalRef(audioRecord);
    if (globalAudioRecord == nullptr) {
        BOOST_LOG(error) << "无法创建 AudioRecord 的全局引用"sv;
        return;
    }

    // 获取 AudioRecord 类和方法 ID
    jclass audioRecordClass = env->GetObjectClass(globalAudioRecord);
    if (audioRecordClass == nullptr) {
        BOOST_LOG(error) << "无法获取 AudioRecord 类"sv;
        env->DeleteGlobalRef(globalAudioRecord);
        return;
    }
    jmethodID readMethod = env->GetMethodID(audioRecordClass, "read", "([FIII)I");

    if (!readMethod) {
        BOOST_LOG(error) << "无法获取 AudioRecord 方法"sv;
        env->DeleteGlobalRef(globalAudioRecord);
        return;
    }

    // 设置活动标志并启动录制线程
    audioRecordingThread = std::thread([globalAudioRecord, readMethod, framesPerPacket, env]() {
        JNIEnv *threadEnv;
        jint result = jvm->AttachCurrentThread(&threadEnv, nullptr);
        if (result != JNI_OK) {
            BOOST_LOG(error) << "无法将音频线程附加到 JVM"sv;
            return;
        }

        // 创建缓冲区
        jfloatArray buffer = threadEnv->NewFloatArray(framesPerPacket * 2); // 立体声，每帧两个通道

        try {
            while (true) {
                // 读取音频数据
                jint samplesRead = threadEnv->CallIntMethod(globalAudioRecord, readMethod, buffer,
                                                            0, framesPerPacket * 2, 0);

                if (samplesRead > 0) {
                    // 获取缓冲区数据
                    jfloat *audioData = threadEnv->GetFloatArrayElements(buffer, nullptr);
                    if (audioData) {
                        // 将音频数据转换为 std::vector<float>
                        std::vector<float> audioSamples(audioData, audioData + samplesRead);

                        // 将音频数据传递给 Sunshine 的音频处理系统
                        if (samples) {
                            samples->raise(std::move(audioSamples));
                        }

                        // 释放缓冲区
                        threadEnv->ReleaseFloatArrayElements(buffer, audioData, JNI_ABORT);
                    }
                }
            }
        } catch (...) {
            BOOST_LOG(error) << "音频录制过程中发生异常"sv;
        }

        // 分离线程
        jvm->DetachCurrentThread();
    });
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_enableH265(JNIEnv *env, jclass clazz) {
    video::active_hevc_mode = 2;
}

}

AMediaFormat *createFormat(const video::config_t &config) {
    bool isHdr = false;
    video::sunshine_colorspace_t colorspace = colorspace_from_client_config(config, isHdr);
    AMediaFormat *format = AMediaFormat_new();
    bool isHevc = config.videoFormat == 1;
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, isHevc ? "video/hevc" : "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, config.width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, config.height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, config.bitrate * 1000);
#if __ANDROID_API__ >= 28
    // AMEDIAFORMAT_KEY_OPERATING_RATE was introduced in API 28 (Android 9)
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_OPERATING_RATE, config.framerate);
#endif
#if __ANDROID_API__ >= 28
    // AMEDIAFORMAT_KEY_CAPTURE_RATE was introduced in API 28 (Android 9)
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CAPTURE_RATE, config.framerate);
#endif
    // AMEDIAFORMAT_KEY_FRAME_RATE
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, config.framerate);
    // max-fps-to-encoder
    AMediaFormat_setInt32(format, "max-fps-to-encoder", config.framerate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 100000);
    // COLOR_FormatSurface
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 2130708361);
#if __ANDROID_API__ >= 28
    // AMEDIAFORMAT_KEY_LATENCY was introduced in API 28 (Android 9)
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_LATENCY, 0);
#endif
#if __ANDROID_API__ >= 28
    // AMEDIAFORMAT_KEY_COMPLEXITY was introduced in API 28 (Android 9)
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COMPLEXITY, 10);
#endif
    // max-bframes
    AMediaFormat_setInt32(format, "max-bframes", 0);
    if (isHevc) {
#if __ANDROID_API__ >= 28
        if (colorspace.bit_depth == 10) {
            // HEVCProfileMain10
            AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PROFILE, 2);
        } else {
            // HEVCProfileMain
            AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PROFILE, 1);
        }
        // HEVCMainTierLevel51
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_LEVEL, 65536);
#endif
    } else {
#if __ANDROID_API__ >= 28
        // HIGH profile
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PROFILE, 0x08);
        // AVCLevel42
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_LEVEL, 0x2000);
#endif
        AMediaFormat_setInt32(format, "vendor.qti-ext-enc-low-latency.enable", 1);
    }
#if __ANDROID_API__ >= 28
    switch (colorspace.colorspace) {
        case video::colorspace_e::rec601:
            // COLOR_STANDARD_BT601_NTSC
            AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD, 4);
            break;
        case video::colorspace_e::rec709:
            // COLOR_STANDARD_BT709
            AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD, 1);
            break;
        case video::colorspace_e::bt2020:
        case video::colorspace_e::bt2020sdr:
            // COLOR_STANDARD_BT2020
            AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD, 6);
            break;
    }
    // 1=FULL, 2=LIMITED
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_RANGE, colorspace.full_range ? 1 : 2);
    if (isHdr) {
        // COLOR_TRANSFER_ST2084
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_TRANSFER, 6);
    } else {
        // COLOR_TRANSFER_SDR_VIDEO
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_TRANSFER, 3);
    }
#endif
    return format;
}


namespace sunshine_callbacks {


    void callJavaOnPinRequested() {
        invokeJavaFunction("onPinRequested", "()V");
    }

    void createVirtualDisplay(
            JNIEnv *env,
            jint width,
            jint height,
            jint frameRate,
            jint packetDuration,
            jobject surface,
            jboolean shouldMute
    ) {
        invokeJavaFunction(
                "createVirtualDisplay",
                "(IIIILandroid/view/Surface;Z)V",
                width,
                height,
                frameRate,
                packetDuration,
                surface,
                shouldMute
        );
    }

    void stopVirtualDisplay() {
        invokeJavaFunction("stopVirtualDisplay", "()V");
    }


    void captureVideoLoop(void *channel_data, safe::mail_t mail, const video::config_t &config,
                          const audio::config_t &audioConfig) {
        JNIEnv *env;
        jint result = jvm->AttachCurrentThread(&env, nullptr);
        if (result != JNI_OK) {
            BOOST_LOG(error) << "无法附加到 Java 线程"sv;
            return;
        }
        auto shutdown_event = mail->event<bool>(mail::shutdown);
        auto idr_events = mail->event<bool>(mail::idr);

        BOOST_LOG(info) << "Client requested video configuration:";
        // 分辨率
        BOOST_LOG(info) << "  - Resolution: " << config.width << "x" << config.height;
        // 帧率
        BOOST_LOG(info) << "  - Frame rate: " << config.framerate << " fps";
        // 视频格式
        const char *videoFormatStr = "Unknown";
        if (config.videoFormat == 0) videoFormatStr = "H.264";
        else if (config.videoFormat == 1) videoFormatStr = "HEVC";
        else if (config.videoFormat == 2) videoFormatStr = "AV1";
        BOOST_LOG(info) << "  - Video format: " << videoFormatStr;
        // 比特率
        BOOST_LOG(info) << "  - Bitrate: " << config.bitrate << " kbps";
        // 色度采样
        BOOST_LOG(info) << "  - Chroma sampling: " << (config.chromaSamplingType == 1 ? "YUV 4:4:4" : "YUV 4:2:0");
        // 动态范围
        BOOST_LOG(info) << "  - Dynamic range: " << (config.dynamicRange ? "HDR (10-bit)" : "SDR (8-bit)");
        // 编码器色彩空间模式
        BOOST_LOG(info) << "  - Encoder color space mode: 0x" << std::hex << config.encoderCscMode << std::dec;
        // 每帧切片数
        BOOST_LOG(info) << "  - Slices per frame: " << config.slicesPerFrame;
        // Max number of reference frames
        BOOST_LOG(info) << "  - Number of reference frames: " << config.numRefFrames;
        BOOST_LOG(info) << "  - Intra refresh: " << (config.enableIntraRefresh ? "Enabled" : "Disabled");

        // Get and log detailed colorspace information
        bool isHdr = false;
        video::sunshine_colorspace_t colorspace = colorspace_from_client_config(config, isHdr);
        BOOST_LOG(info) << "Colorspace configuration:";
        const char *colorspaceStr = "Unknown";
        if (colorspace.colorspace == video::colorspace_e::rec601) colorspaceStr = "Rec.601";
        else if (colorspace.colorspace == video::colorspace_e::rec709) colorspaceStr = "Rec.709";
        else if (colorspace.colorspace == video::colorspace_e::bt2020sdr) colorspaceStr = "BT.2020 SDR";
        else if (colorspace.colorspace == video::colorspace_e::bt2020) colorspaceStr = "BT.2020 HDR";
        BOOST_LOG(info) << "  - Colorspace: " << colorspaceStr;
        BOOST_LOG(info) << "  - Bit depth: " << colorspace.bit_depth << "-bit";
        BOOST_LOG(info) << "  - Color range: " << (colorspace.full_range ? "Full (0-255)" : "Limited (16-235)");
        BOOST_LOG(info) << "  - HDR mode: " << (isHdr ? "Enabled" : "Disabled");

        AMediaFormat *format = createFormat(config);
#if __ANDROID_API__ >= 28
        int32_t colorStandard = 0, colorRange = 0, colorTransfer = 0;

        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD, &colorStandard);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_RANGE, &colorRange);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_TRANSFER, &colorTransfer);

        BOOST_LOG(info) << "Final media format color configuration:"sv;
        BOOST_LOG(info) << "  - COLOR_STANDARD: "sv << colorStandard;
        BOOST_LOG(info) << "  - COLOR_RANGE: "sv << colorRange << (colorRange == 1 ? " (FULL)" : " (LIMITED)");
        BOOST_LOG(info) << "  - COLOR_TRANSFER: "sv << colorTransfer;
#endif

        // Create encoder
        const char *mimeType = config.videoFormat == 1 ? "video/hevc" : "video/avc";
        AMediaCodec *codec = AMediaCodec_createEncoderByType(mimeType);
        if (!codec) {
            BOOST_LOG(error) << "Failed to create encoder";
            AMediaFormat_delete(format);
            return;
        }

        // Configure encoder
        media_status_t status = AMediaCodec_configure(codec, format, nullptr, nullptr,
                                                      AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        if (status != AMEDIA_OK) {
            BOOST_LOG(error) << "Failed to configure encoder, error code: " << status;
            AMediaCodec_delete(codec);
            AMediaFormat_delete(format);
            return;
        }

        // Get input Surface
        ANativeWindow *inputSurface;
        status = AMediaCodec_createInputSurface(codec, &inputSurface);
        if (status != AMEDIA_OK) {
            BOOST_LOG(error) << "Failed to create input Surface, error code: " << status;
            AMediaCodec_delete(codec);
            AMediaFormat_delete(format);
            return;
        }

        // Convert ANativeWindow to Java Surface object
        jobject javaSurface = ANativeWindow_toSurface(env, inputSurface);
        if (javaSurface == nullptr) {
            BOOST_LOG(error) << "Failed to convert ANativeWindow to Surface";
            ANativeWindow_release(inputSurface);
            AMediaCodec_delete(codec);
            AMediaFormat_delete(format);
            jvm->DetachCurrentThread();
            return;
        }

        bool shouldMute = true;
        if (audioConfig.flags[audio::config_t::HOST_AUDIO]) {
            BOOST_LOG(info) << "Audio config: Sound will be played on host (Sunshine server)"sv;
            shouldMute = false;
        } else {
            BOOST_LOG(info) << "Audio config: Sound will be played on client (Moonlight)"sv;
        }

        // Create virtual display with audio configuration
        createVirtualDisplay(env, config.width, config.height, 120, audioConfig.packetDuration, javaSurface, shouldMute);

        // Start encoder
        status = AMediaCodec_start(codec);
        if (status != AMEDIA_OK) {
            BOOST_LOG(error) << "Failed to start encoder, error code: "sv << status;
            env->DeleteLocalRef(javaSurface);
            jvm->DetachCurrentThread();
            ANativeWindow_release(inputSurface);
            AMediaCodec_delete(codec);
            AMediaFormat_delete(format);
            return;
        }

        // Initialize variables for encoding loop
        std::vector<uint8_t> codecConfigData;
        int64_t frameIndex = 0;

        // Encoding loop
        while (!shutdown_event->peek()) {
            bool requested_idr_frame = false;
            if (idr_events->peek()) {
                requested_idr_frame = true;
                idr_events->pop();
            }

            if (requested_idr_frame) {
                // Request IDR frame
                AMediaFormat *params = AMediaFormat_new();
                AMediaFormat_setInt32(params, "request-sync", 0);
                media_status_t status = AMediaCodec_setParameters(codec, params);
                if (status != AMEDIA_OK) {
                    BOOST_LOG(warning) << "Failed to request IDR frame, error code: " << status;
                } else {
                    BOOST_LOG(info) << "IDR frame requested";
                }
                AMediaFormat_delete(params);
            }

            // Dequeue output buffer with 1 second timeout
            AMediaCodecBufferInfo bufferInfo;
            ssize_t outputBufferIndex = AMediaCodec_dequeueOutputBuffer(codec, &bufferInfo, -1);

            if (outputBufferIndex >= 0) {
                // Valid output buffer received
                size_t bufferSize = bufferInfo.size;
                uint8_t *buffer = nullptr;
                size_t out_size = 0;

                // Get buffer data
                buffer = AMediaCodec_getOutputBuffer(codec, outputBufferIndex, &out_size);
                if (buffer != nullptr) {
                    if (bufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                        // Codec configuration data (SPS/PPS)
                        BOOST_LOG(info) << "Received codec configuration data, size: " << bufferSize;
                        codecConfigData.assign(buffer, buffer + bufferSize);
                        BOOST_LOG(info) << "Saved complete codec configuration data, size: " << codecConfigData.size();
                    } else {
                        // Regular encoded frame
                        bool isKeyFrame = (bufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
                        BOOST_LOG(verbose) << "Received " << (isKeyFrame ? "key frame" : "regular frame") << ", size: " << bufferSize;
                        frameIndex++;

                        if (isKeyFrame) {
                            // For key frames, prepend codec configuration data
                            if (!codecConfigData.empty()) {
                                std::vector<uint8_t> frameData;
                                frameData.insert(frameData.end(), codecConfigData.begin(), codecConfigData.end());
                                frameData.insert(frameData.end(), buffer, buffer + bufferSize);
                                BOOST_LOG(verbose) << "Sending key frame (with config data), total size: " << frameData.size();
                                stream::postFrame(std::move(frameData), frameIndex, true, channel_data);
                            } else {
                                BOOST_LOG(error) << "No codec configuration data, cannot send complete key frame";
                            }
                        } else {
                            std::vector<uint8_t> frameData;
                            frameData.insert(frameData.end(), buffer, buffer + bufferSize);
                            stream::postFrame(std::move(frameData), frameIndex, false, channel_data);
                        }
                    }
                }

                // Release output buffer
                AMediaCodec_releaseOutputBuffer(codec, outputBufferIndex, false);
            } else if (outputBufferIndex == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                BOOST_LOG(verbose) << "Encoder timeout, waiting for output buffer";
                continue;
            } else if (outputBufferIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                // Output format changed
                AMediaFormat *format = AMediaCodec_getOutputFormat(codec);
                BOOST_LOG(info) << "Encoder output format changed";
                AMediaFormat_delete(format);
            } else if (outputBufferIndex == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                // Output buffers changed
                BOOST_LOG(info) << "Encoder output buffers changed";
                // 在较新的 NDK 版本中，这个事件通常可以忽略，因为 AMediaCodec_getOutputBuffer 会自动处理缓冲区变化
            } else {
                // Error
                BOOST_LOG(error) << "Encoder error, error code: " << outputBufferIndex;
                break;
            }
        }

        stopVirtualDisplay();
        // 停止编码器
        AMediaCodec_stop(codec);

        // 清理资源
        ANativeWindow_release(inputSurface);
        AMediaCodec_delete(codec);
        AMediaFormat_delete(format);

        // 清理 Java Surface 引用
        jvm->DetachCurrentThread();
    }

    void captureAudioLoop(void *channel_data, safe::mail_t mail, const audio::config_t &config) {
        samples = std::make_shared<audio::sample_queue_t::element_type>(30);
        encodeThread(samples, config, channel_data);
    }

    float from_netfloat(netfloat f) {
        return boost::endian::endian_load<float, sizeof(float), boost::endian::order::little>(f);
    }

    void callJavaOnTouch(SS_TOUCH_PACKET *touchPacket) {
        if (jvm == nullptr) {
            BOOST_LOG(error) << "JVM 指针为空"sv;
            return;
        }

        if (sunshineServerClass == nullptr) {
            BOOST_LOG(error) << "SunshineServer 类引用为空"sv;
            return;
        }

        JNIEnv *env;
        jint result = jvm->AttachCurrentThread(&env, nullptr);
        if (result != JNI_OK) {
            BOOST_LOG(error) << "无法附加到 Java 线程"sv;
            return;
        }

        jmethodID handleTouchPacketMethod = env->GetStaticMethodID(sunshineServerClass,
                                                                   "handleTouchPacket",
                                                                   "(IIIFFFFF)V");
        if (handleTouchPacketMethod == nullptr) {
            BOOST_LOG(error) << "找不到 handleTouchPacket 方法"sv;
            jvm->DetachCurrentThread();
            return;
        }

        // 从 SS_TOUCH_PACKET 结构体中提取字段并传递给 Java 方法
        env->CallStaticVoidMethod(sunshineServerClass, handleTouchPacketMethod,
                                  static_cast<int>(touchPacket->eventType),
                                  static_cast<int>(touchPacket->rotation),
                                  static_cast<int>(touchPacket->pointerId),
                                  from_netfloat(touchPacket->x),
                                  from_netfloat(touchPacket->y),
                                  from_netfloat(touchPacket->pressureOrDistance),
                                  from_netfloat(touchPacket->contactAreaMajor),
                                  from_netfloat(touchPacket->contactAreaMinor));

        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }

        jvm->DetachCurrentThread();
    }
}