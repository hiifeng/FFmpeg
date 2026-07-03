/*
 * AV3A (AVS3 audio) decoder bridge for FFmpeg 4.2 (jellyfin exoplayer-ffmpeg-extension 2.19.1+1)
 *
 * 本文件是 libAVS3AudioDec.so 的 FFmpeg AVCodec 适配层，不包含任何 AV3A 解码算法本身，
 * 算法完全在运行时通过 dlopen/dlsym 调用 libAVS3AudioDec.so 里导出的四个函数完成：
 *   avs3_create_decoder / avs3_destroy_decoder / parse_header / avs3_decode
 *
 * 与当虹官方参考实现（针对 FFmpeg 6.1 编写）的差异说明：
 *   1. 6.1 用 FFCodec + codec_internal.h 的新式注册方式，本文件改为 4.2 的扁平 AVCodec
 *      结构体 + 旧式 .decode 回调（int (*)(AVCodecContext*, void*, int*, AVPacket*)）。
 *   2. 6.1 用 AVChannelLayout/avctx->ch_layout 新声道 API，本文件改为 4.2 的
 *      avctx->channels（int）+ avctx->channel_layout（uint64_t）旧声道 API。
 *   3. 不解引用 AVS3DecoderHandle 内部结构体的任何字段（channelNumConfig / numObjsOutput /
 *      isMixedContent / totalBitrate / outputFs / numChansOutput / hMetadataDec 等）。
 *      原因：用于生成本文件的参考头文件（Sourcecodeforplayer 仓库）经核实与本项目实际
 *      使用的 libAVS3AudioDec.so 并非同一构建（Avs3InitDecoder 导出符号签名不同），
 *      内部结构体内存布局不保证一致，解引用有 ABI 不匹配导致读脏内存/崩溃的风险。
 *      因此 AVS3DecoderHandle 在本文件中被当作纯不透明指针，只通过导出函数交互。
 *   4. 声道数 / 采样率不依赖 AV3A 解码器自身在解码后报告的值，而是在 avcodec_open2()
 *      时就直接使用 avctx->channels / avctx->sample_rate ——这两个值由上层
 *      ffmpeg_jni.cc::createContext() 从 ExoPlayer 侧的 Format（即 Av3aReader 对
 *      AV3A 原始帧头的解析结果）写入，独立于本解码器、且已验证可靠。
 *      对应改动：ffmpeg_jni.cc 的 createContext() 需要把 AV_CODEC_ID_AV3A 加进
 *      "直接使用 rawSampleRate/rawChannelCount" 的判断分支（目前只对 PCM_MULAW/
 *      PCM_ALAW 生效）。这是另一个文件的改动，不在本文件范围内。
 *   5. 不包含双耳渲染（RenderPCM / libav3a_binaural_render.so）相关代码，与本阶段
 *      "能正常输出 PCM" 的目标无关。
 *   6. 不包含"解码器内部配置中途变化（声道配置/对象数/混合类型/总码率）→重置解码器"
 *      的健壮性检查，原因同第 3 点（该检查需要读 AVS3DecoderHandle 内部字段）。
 *      如果后续发现实际素材存在流中途切声道配置的情况，需要单独评估如何在不解引用
 *      内部结构体的前提下实现等价检测（例如：让 libAVS3AudioDec.so 一侧新增一个
 *      "配置是否变化" 的导出查询函数，而不是让调用方直接读内部结构体）。
 *
 * 缓冲区累积 / parse_header→avs3_decode 调用循环的整体结构照抄当虹参考实现，
 * 未重新设计，以规避此前自研 av3a_jni.cpp 反复调试未成功的边界处理类 bug。
 *
 * TODO（不在本文件范围内，需在集成/联调阶段处理）：
 *   dlopen("libAVS3AudioDec.so", RTLD_LAZY) 使用裸 soname、不带路径。在 API 19~23 的
 *   Android 上，dlopen 裸文件名只有在该 .so 已经被加载进当前进程（例如 Java 层提前
 *   System.loadLibrary 过，或作为某个 DT_NEEDED 依赖被自动加载）时才能找到，否则会
 *   失败。需要确认 App 侧在 FFmpeg 解码器被打开之前，libAVS3AudioDec.so 已经以某种
 *   方式被加载到进程里（比如保留一个极小的 JNI_OnLoad 触发点提前 loadLibrary），
 *   或者把这里的 dlopen 调用改成传入 ApplicationInfo.nativeLibraryDir 拼出的绝对路径。
 */

#include <dlfcn.h>
#include <string.h>

#include "avcodec.h"
#include "internal.h"
#include "libavutil/channel_layout.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"

/* ------------------------------------------------------------------------
 * 与 libAVS3AudioDec.so 交互所需的最小声明。
 * 只声明四个导出函数的原型和两个协议级返回码常量，不引入任何内部结构体定义。
 * AVS3DecoderHandle 在本文件中始终作为不透明指针（void*）使用，不做解引用。
 * ------------------------------------------------------------------------ */

typedef void *AVS3DecoderHandle;

typedef AVS3DecoderHandle (*PFavs3_create_decoder)(void);
typedef void (*PFavs3_destroy_decoder)(AVS3DecoderHandle hAvs3Dec);
typedef int (*PFparse_header)(AVS3DecoderHandle hAvs3Dec, unsigned char *pData,
                              int nLenIn, int isInitFrame, int *pnLenConsumed,
                              unsigned short *crc);
typedef int (*PFavs3_decode)(AVS3DecoderHandle hAvs3Dec, unsigned char *pDataIN,
                             int nLenIn, unsigned char *pDataOut, int *pnLenOut,
                             int *pnLenConsumed);

/* 来自 libAVS3AudioDec.so 的协议级返回码（parse_header/avs3_decode 共用），
 * 值本身是调用协议的一部分，和内部结构体布局无关，故直接在此定义，
 * 不去 include 上游的 avs3_cnst_com.h（避免引入该头文件里其它可能有 ABI
 * 假设的定义）。 */
#define AVS3_TRUE            1
#define AVS3_DATA_NOT_ENOUGH 2

/* 解码一帧最多产出的 PCM 字节数上限：16 声道 * 1024 samples * 2 bytes(S16) */
#define MAX_AV3A_PCM_SIZE (16 * 1024 * 2)

/* 单次 avs3_decode 调用最多允许连续无输出多少次后，判定解码器状态异常并重置 */
#define MAX_ERROR_RETRY 50

typedef struct ARCDAv3aContext {
    AVCodecContext *avctx;

    void *handle; /* dlopen("libAVS3AudioDec.so") 句柄 */
    PFavs3_create_decoder avs3_create_decoder;
    PFavs3_destroy_decoder avs3_destroy_decoder;
    PFparse_header parse_header;
    PFavs3_decode avs3_decode;

    AVS3DecoderHandle hAvs3;

    int firstFrame;

    /* 输入侧累积缓冲区：一个 AVPacket 可能不足一帧，也可能包含多帧，
     * 需要自己维护跨 packet 的字节流，逻辑照抄当虹参考实现 */
    uint8_t *inBuf;
    unsigned long inBufSize;
    unsigned long inBufLen;

    /* 单帧解码输出的暂存区，大小固定为 MAX_AV3A_PCM_SIZE */
    uint8_t *outBuf;

    unsigned long errorCounter;
} ARCDAv3aContext;

static av_cold int arcdav3a_reset_decoder(AVCodecContext *avctx);

static av_cold int arcdav3a_load_library(AVCodecContext *avctx) {
    ARCDAv3aContext *h = avctx->priv_data;

    h->handle = dlopen("libAVS3AudioDec.so", RTLD_LAZY);
    if (!h->handle) {
        av_log(avctx, AV_LOG_ERROR, "libarcdav3a: dlopen libAVS3AudioDec.so failed: %s\n",
               dlerror());
        return AVERROR(EFAULT);
    }

    h->avs3_create_decoder = (PFavs3_create_decoder)dlsym(h->handle, "avs3_create_decoder");
    h->avs3_destroy_decoder = (PFavs3_destroy_decoder)dlsym(h->handle, "avs3_destroy_decoder");
    h->parse_header = (PFparse_header)dlsym(h->handle, "parse_header");
    h->avs3_decode = (PFavs3_decode)dlsym(h->handle, "avs3_decode");

    if (!h->avs3_create_decoder || !h->avs3_destroy_decoder || !h->parse_header ||
        !h->avs3_decode) {
        av_log(avctx, AV_LOG_ERROR, "libarcdav3a: dlsym failed to resolve avs3 decoder API\n");
        return AVERROR(EFAULT);
    }

    return 0;
}

static av_cold int arcdav3a_decode_init(AVCodecContext *avctx) {
    ARCDAv3aContext *h = avctx->priv_data;
    int ret;

    h->avctx = avctx;

    ret = arcdav3a_load_library(avctx);
    if (ret < 0)
        return ret;

    h->hAvs3 = h->avs3_create_decoder();
    if (!h->hAvs3) {
        av_log(avctx, AV_LOG_ERROR, "libarcdav3a: avs3_create_decoder failed\n");
        return AVERROR(EFAULT);
    }

    h->outBuf = av_malloc(MAX_AV3A_PCM_SIZE);
    if (!h->outBuf)
        return AVERROR(ENOMEM);

    /* 声道数/采样率固定使用容器层（Av3aReader）已经解析好、经由
     * ffmpeg_jni.cc::createContext() 写入 avctx 的值，本解码器不再自行判定。
     * avctx->channels / avctx->channel_layout 此时应已经是非零的有效值。 */
    if (avctx->channels <= 0) {
        av_log(avctx, AV_LOG_ERROR,
               "libarcdav3a: avctx->channels not set before opening decoder "
               "(check ffmpeg_jni.cc createContext AV3A special-case)\n");
        return AVERROR(EINVAL);
    }
    if (!avctx->channel_layout)
        avctx->channel_layout = av_get_default_channel_layout(avctx->channels);

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;

    h->firstFrame = 1;
    h->inBuf = NULL;
    h->inBufSize = 0;
    h->inBufLen = 0;
    h->errorCounter = 0;

    return 0;
}

static av_cold int arcdav3a_reset_decoder(AVCodecContext *avctx) {
    ARCDAv3aContext *h = avctx->priv_data;

    if (h->hAvs3)
        h->avs3_destroy_decoder(h->hAvs3);
    h->hAvs3 = h->avs3_create_decoder();
    if (!h->hAvs3) {
        av_log(avctx, AV_LOG_ERROR, "libarcdav3a: avs3_create_decoder failed on reset\n");
        return AVERROR(EFAULT);
    }

    h->firstFrame = 1;
    h->inBufLen = 0;
    h->errorCounter = 0;
    return 0;
}

static av_cold void arcdav3a_decode_flush(AVCodecContext *avctx) {
    arcdav3a_reset_decoder(avctx);
}

/*
 * 核心解码循环：把一个 AVPacket 的数据追加进累积缓冲区，反复调用
 * parse_header + avs3_decode 尽可能多地产出完整帧，直到剩余数据不足一帧为止。
 * 整体结构（缓冲区扩容策略、pos/consumed 推进方式、parse_header 失败时的重试
 * 逻辑、错误计数重置策略）照抄当虹参考实现 dav3a_decode_frame，仅去掉了依赖
 * AVS3DecoderHandle 内部字段的"配置变化检测→重置"分支。
 *
 * 返回值：写入 h->outBuf 的总字节数（可能是本次调用内多帧拼接的结果），
 * 若没有产出数据返回 0，出错返回负数。
 */
static int arcdav3a_decode_buffered(AVCodecContext *avctx) {
    ARCDAv3aContext *h = avctx->priv_data;
    int outIndex = 0;
    unsigned long pos = 0;

    do {
        int consumed = 0;
        int ret;

        while ((ret = h->parse_header(h->hAvs3, h->inBuf + pos,
                                       (int)(h->inBufLen - pos), h->firstFrame,
                                       &consumed, NULL)) != AVS3_TRUE) {
            if (ret == AVS3_DATA_NOT_ENOUGH) {
                if (pos + consumed < h->inBufLen)
                    pos += consumed;
                else
                    pos = h->inBufLen;
                goto out;
            }
            if (pos + consumed < h->inBufLen) {
                pos += consumed;
            } else {
                pos = h->inBufLen;
                goto out;
            }
        }

        if (pos + consumed < h->inBufLen) {
            pos += consumed;
        } else {
            pos = h->inBufLen;
            goto out;
        }

        if (h->firstFrame)
            h->firstFrame = 0;

        {
            int outLen = 0;
            ret = h->avs3_decode(h->hAvs3, h->inBuf + pos, (int)(h->inBufLen - pos),
                                 h->outBuf + outIndex, &outLen, &consumed);
            pos += consumed;

            if (ret != AVS3_TRUE || outLen <= 0) {
                h->errorCounter++;
                if (h->errorCounter > MAX_ERROR_RETRY) {
                    av_log(avctx, AV_LOG_WARNING,
                           "libarcdav3a: avs3_decode kept failing, resetting decoder\n");
                    h->errorCounter = 0;
                    arcdav3a_reset_decoder(avctx);
                    outIndex = 0;
                    break;
                }
            } else {
                h->errorCounter = 0;
            }

            if (ret != AVS3_TRUE)
                break;

            if (outIndex + outLen > MAX_AV3A_PCM_SIZE) {
                av_log(avctx, AV_LOG_ERROR, "libarcdav3a: decoded PCM exceeds buffer, dropping\n");
                break;
            }
            outIndex += outLen;
        }
    } while (h->inBufLen > pos);

out:
    /* 把还没消费完的残余数据移到缓冲区开头，供下次 packet 追加 */
    if (pos > 0 && pos < h->inBufLen) {
        memmove(h->inBuf, h->inBuf + pos, h->inBufLen - pos);
        h->inBufLen -= pos;
    } else if (pos >= h->inBufLen) {
        h->inBufLen = 0;
    }

    return outIndex;
}

static int arcdav3a_append_input(AVCodecContext *avctx, const uint8_t *data, int size) {
    ARCDAv3aContext *h = avctx->priv_data;

    if (!h->inBuf) {
        h->inBufSize = (unsigned long)size * 2;
        h->inBuf = av_malloc(h->inBufSize);
    } else if (h->inBufLen + size > h->inBufSize) {
        uint8_t *tmp;
        h->inBufSize = h->inBufLen + (unsigned long)size * 2;
        tmp = av_realloc(h->inBuf, h->inBufSize);
        if (!tmp) {
            av_freep(&h->inBuf);
            h->inBufSize = 0;
            h->inBufLen = 0;
            return AVERROR(ENOMEM);
        }
        h->inBuf = tmp;
    }
    if (!h->inBuf) {
        av_log(avctx, AV_LOG_ERROR, "libarcdav3a: input buffer allocation failed\n");
        return AVERROR(ENOMEM);
    }

    memcpy(h->inBuf + h->inBufLen, data, size);
    h->inBufLen += size;
    return 0;
}

static int arcdav3a_decode_frame(AVCodecContext *avctx, void *data, int *got_frame_ptr,
                                 AVPacket *avpkt) {
    ARCDAv3aContext *h = avctx->priv_data;
    AVFrame *frm = data;
    int outLen;
    int ret;

    *got_frame_ptr = 0;

    if (avpkt->size <= 0)
        return avpkt->size;

    ret = arcdav3a_append_input(avctx, avpkt->data, avpkt->size);
    if (ret < 0)
        return ret;

    outLen = arcdav3a_decode_buffered(avctx);
    if (outLen < 0)
        return outLen;
    if (outLen == 0)
        return avpkt->size;

    frm->nb_samples = outLen / (avctx->channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16));
    frm->format = AV_SAMPLE_FMT_S16;
    frm->channels = avctx->channels;
    frm->channel_layout = avctx->channel_layout;
    frm->sample_rate = avctx->sample_rate;

    ret = ff_get_buffer(avctx, frm, 0);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "libarcdav3a: ff_get_buffer failed\n");
        return ret;
    }

    memcpy(frm->data[0], h->outBuf, outLen);
    *got_frame_ptr = 1;

    return avpkt->size;
}

static av_cold int arcdav3a_decode_close(AVCodecContext *avctx) {
    ARCDAv3aContext *h = avctx->priv_data;

    if (!h)
        return 0;

    if (h->hAvs3 && h->avs3_destroy_decoder)
        h->avs3_destroy_decoder(h->hAvs3);
    h->hAvs3 = NULL;

    if (h->handle)
        dlclose(h->handle);
    h->handle = NULL;

    av_freep(&h->inBuf);
    av_freep(&h->outBuf);
    h->inBufSize = 0;
    h->inBufLen = 0;

    return 0;
}

AVCodec ff_libarcdav3a_decoder = {
    .name           = "av3a",
    .long_name      = NULL_IF_CONFIG_SMALL("AV3A (AVS3 audio) decoder via libAVS3AudioDec"),
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_AV3A,
    .priv_data_size = sizeof(ARCDAv3aContext),
    .init           = arcdav3a_decode_init,
    .decode         = arcdav3a_decode_frame,
    .close          = arcdav3a_decode_close,
    .flush          = arcdav3a_decode_flush,
    .capabilities   = AV_CODEC_CAP_CHANNEL_CONF | AV_CODEC_CAP_DR1,
};
