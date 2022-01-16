#ifdef EDITTIME
#undef __cplusplus
#define __cplusplus 201703L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string>
// #include <functional>
// #include <map>
// #include <queue>
#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>
// #include <time.h>
// #include <regex>
using namespace std;
using namespace emscripten;
// #include "slice.h"
typedef unsigned char u8;
// typedef signed char i8;
// typedef unsigned short u16;
// typedef signed short i16;
typedef unsigned int u32;
// typedef signed int i32;
#define PROP(name, type)                        \
    type name;                                  \
    val get##name() const                       \
    {                                           \
        return val(name);                       \
    }                                           \
    void set##name(val value)                   \
    {                                           \
        name = value.as<type>();                \
        emscripten_log(0, #name " = %d", name); \
    }

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavformat/avformat.h>
    void FFmpegLogFunc(void *ptr, int level, const char *fmt, va_list vl)
    {
        emscripten_log(level, fmt, vl);
    }
    void initAvLog()
    {
        //emscripten_log(0, "initAvLog");
        //av_log_set_callback(FFmpegLogFunc);
    }
}

//const int SamplingFrequencies[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0};
//const int AudioObjectTypes[] = {};
#include "mp4.h"
class FFmpeg
{
public:
    AVCodec *codec = nullptr;
    AVCodec *enc_codec = nullptr;
    AVCodecParserContext *parser = nullptr;
    AVCodecContext *dec_ctx = nullptr;
    AVFrame *frame;
    AVPacket *pkt;
    val jsObject;
    bool initialized = false;
    MP4 *mp4 = nullptr;
    AVStream *outStream;
    u32 timestamp = 0;
    u32 mp4Ts = 0;
    AVCodecParameters *par = nullptr;
    FFmpeg(val &&v) : jsObject(forward<val>(v))
    {
    }
    virtual ~FFmpeg()
    {
        clear();
    }
    void initCodec(enum AVCodecID id)
    {
        if (dec_ctx != nullptr)
        {
            clear();
        }
        pkt = av_packet_alloc();
        codec = avcodec_find_decoder(id);
        enc_codec = avcodec_find_encoder(id);
        parser = av_parser_init(codec->id);
        dec_ctx = avcodec_alloc_context3(codec);
        frame = av_frame_alloc();
    }
    void initCodec(enum AVCodecID id, string input)
    {
        initCodec(id);
        dec_ctx->extradata_size = input.length();
        dec_ctx->extradata = (u8 *)input.data();
        avcodec_open2(dec_ctx, codec, NULL);
        avcodec_parameters_from_context(par, dec_ctx);
        if (outStream != nullptr)
            avcodec_parameters_copy(outStream->codecpar, par);
        initialized = true;
    }
    virtual void setMp4(int p)
    {
        mp4 = (MP4 *)p;
        mp4Ts = timestamp;
        outStream = avformat_new_stream(mp4->output_fmtctx, enc_codec);
        if (par != nullptr)
            avcodec_parameters_copy(outStream->codecpar, par);
        outStream->id = mp4->output_fmtctx->nb_streams - 1;
    }
    virtual int decode(string input)
    {
        int ret = 0;
        pkt->data = (u8 *)input.data();
        pkt->size = input.length();
        //av_packet_from_data(pkt, (u8 *)input.data(), input.length());
        ret = avcodec_send_packet(dec_ctx, pkt);
        if (mp4 != nullptr)
        {
            pkt->stream_index = outStream->index;
            ret = mp4->write(pkt);
        }
        while (ret >= 0)
        {
            ret = avcodec_receive_frame(dec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                return 0;
            _decode();
        }
        return 0;
    }
    virtual void _decode(){};
    virtual void clear()
    {
        if (parser)
        {
            av_parser_close(parser);
            parser = nullptr;
        }
        if (dec_ctx)
        {
            avcodec_free_context(&dec_ctx);
        }
        if (frame)
        {
            av_frame_free(&frame);
        }
        if (pkt)
        {
            av_packet_free(&pkt);
        }
        codec = nullptr;
        initialized = false;
    }
};

class FFmpegAudioDecoder : public FFmpeg
{
    SwrContext *au_convert_ctx = nullptr;
    u8 *out_buffer[2];
    int output_nb_samples;
    int n_channel;

public:
    PROP(sample_rate, int)
    //    struct SwrContext *au_convert_ctx = nullptr;
    FFmpegAudioDecoder(val &&v) : FFmpeg(move(v))
    {
        //        emscripten_log(0, "FFMpegAudioDecoder init");
    }
    ~FFmpegAudioDecoder()
    {
        if (au_convert_ctx)
            swr_free(&au_convert_ctx);
        if (out_buffer[0])
            free(out_buffer[0]);
        //        emscripten_log(0, "FFMpegAudioDecoder destory");
    }
    void clear() override
    {
        FFmpeg::clear();
    }
    void setMp4(int p) override
    {
        FFmpeg::setMp4(p);
        outStream->time_base = (AVRational){1, dec_ctx->sample_rate};
    }
    int decode(u32 ts, string input)
    {
        timestamp = ts;
        u8 flag = (u8)input[0];
        u8 audioType = flag >> 4;
        if (initialized)
        {
            pkt->dts = pkt->pts = (int64_t)(ts - mp4Ts);
            return FFmpeg::decode(input.substr(audioType == 10 ? 2 : 1));
        }
        else
        {
            switch (audioType)
            {
            case 10:
                if (!input[1])
                {
                    initCodec(AV_CODEC_ID_AAC, input.substr(2));
                    n_channel = 2;
                }
                break;
            case 7:
                initCodec(AV_CODEC_ID_PCM_ALAW);
                dec_ctx->channel_layout = AV_CH_LAYOUT_MONO;
                dec_ctx->sample_rate = 8000;
                dec_ctx->channels = 1;
                avcodec_open2(dec_ctx, codec, NULL);
                n_channel = 1;
                initialized = true;
                break;
            case 8:
                initCodec(AV_CODEC_ID_PCM_MULAW);
                dec_ctx->channel_layout = AV_CH_LAYOUT_MONO;
                dec_ctx->sample_rate = 8000;
                dec_ctx->channels = 1;
                avcodec_open2(dec_ctx, codec, NULL);
                n_channel = 1;
                initialized = true;
                break;
            default:
                emscripten_log(0, "audio type not support:%d", audioType);
                break;
            }
            if (initialized)
            {
                if (outStream)
                    outStream->time_base = (AVRational){1, dec_ctx->sample_rate};

                jsObject.call<void>("initAudioPlanar", n_channel, sample_rate);
            }
        }
        return 0;
    }
    void _decode() override
    {
        int nb_samples = frame->nb_samples;
        int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_FLTP);
        if (dec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP && sample_rate == dec_ctx->sample_rate && dec_ctx->channel_layout == n_channel)
        {
            jsObject.call<void>("playAudioPlanar", int(frame->data), nb_samples *bytes_per_sample *n_channel);
            return;
        }
        if (!au_convert_ctx)
        {
            // out_buffer = (uint8_t *)av_malloc(av_get_bytes_per_sample(dec_ctx->sample_fmt)*dec_ctx->channels*dec_ctx->frame_size);
            emscripten_log(0, "au_convert_ctx channel_layout:%d,sample_fmt:%d", dec_ctx->channel_layout, dec_ctx->sample_fmt);
            au_convert_ctx = swr_alloc_set_opts(NULL, n_channel == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO, AV_SAMPLE_FMT_FLTP, sample_rate,
                                                dec_ctx->channel_layout, dec_ctx->sample_fmt, dec_ctx->sample_rate,
                                                0, NULL);
            int ret = swr_init(au_convert_ctx);
            int out_buffer_size = av_samples_get_buffer_size(NULL, n_channel, nb_samples, AV_SAMPLE_FMT_FLTP, 0);
            uint8_t *buffer = (uint8_t *)av_malloc(out_buffer_size);
            out_buffer[0] = buffer;
            out_buffer[1] = buffer + (out_buffer_size / 2);
            emscripten_log(0, "au_convert_ctx init sample_rate:%d->%d,ret:%d", dec_ctx->sample_rate, sample_rate, ret);
        }
        // // 转换
        int ret = swr_convert(au_convert_ctx, out_buffer, nb_samples, (const uint8_t **)frame->data, nb_samples);
        while (ret > 0)
        {
            jsObject.call<void>("playAudioPlanar", int(&out_buffer), ret);
            ret = swr_convert(au_convert_ctx, out_buffer, nb_samples, (const uint8_t **)frame->data, 0);
        }
    }
};
class FFmpegVideoDecoder : public FFmpeg
{
public:
    u32 videoWidth = 0;
    u32 videoHeight = 0;
    u32 y = 0;
    u32 u = 0;
    u32 v = 0;
    int NAL_unit_length = 0;
    u32 compositionTime = 0;
    FFmpegVideoDecoder(val &&v) : FFmpeg(move(v))
    {
        //        emscripten_log(0, "FFMpegVideoDecoder init");
    }
    ~FFmpegVideoDecoder()
    {
        //        emscripten_log(0, "FFMpegVideoDecoder destory");
    }
    void clear() override
    {
        videoWidth = 0;
        videoHeight = 0;
        FFmpeg::clear();
        if (y)
        {
            free((void *)y);
            y = 0;
        }
    }
    void setMp4(int p) override
    {
        FFmpeg::setMp4(p);
        outStream->time_base = (AVRational){1, 90000};
    }
    int decode(u32 ts, string data)
    {
        timestamp = ts;
        if (!initialized)
        {
            int codec_id = ((int)data[0]) & 0x0F;
            if (((int)(data[0]) >> 4) == 1 && data[1] == 0)
            {
                //                emscripten_log(0, "codec = %d", codec_id);
                switch (codec_id)
                {
                case 7:
                    initCodec(AV_CODEC_ID_H264, data.substr(5));
                    break;
                case 12:
                    initCodec(AV_CODEC_ID_H265, data.substr(5));
                    break;
                default:
                    emscripten_log(0, "codec not support: %d", codec_id);
                    return -1;
                }
            }
        }
        else
        {
            u8 *cts_ptr = (u8 *)(&compositionTime);
            *cts_ptr = data[4];
            *(cts_ptr + 1) = data[3];
            *(cts_ptr + 2) = data[2];
            pkt->dts = (int64_t)(ts - mp4Ts);
            pkt->pts = pkt->dts + (int64_t)compositionTime;
            emscripten_log(0, "set pts %d", pkt->pts);
            return FFmpeg::decode(data.substr(5));
        }
        return 0;
    }
    void _decode() override
    {
        if (videoWidth != frame->width || videoHeight != frame->height)
        {
            videoWidth = frame->width;
            videoHeight = frame->height;
            dec_ctx->width = videoWidth;
            dec_ctx->height = videoHeight;
            jsObject.call<void>("setVideoSize", videoWidth, videoHeight);
            int size = videoWidth * videoHeight;
            if (y)
                free((void *)y);
            y = (u32)malloc(size * 3 >> 1);
            u = y + size;
            v = u + (size >> 2);
        }
        u32 dst = y;
        for (int i = 0; i < videoHeight; i++)
        {
            memcpy((u8 *)dst, (const u8 *)(frame->data[0] + i * frame->linesize[0]), videoWidth);
            dst += videoWidth;
        }
        dst = u;
        int halfh = videoHeight >> 1;
        int halfw = videoWidth >> 1;
        for (int i = 0; i < halfh; i++)
        {
            memcpy((u8 *)dst, (const u8 *)(frame->data[1] + i * frame->linesize[1]), halfw);
            dst += halfw;
        }

        for (int i = 0; i < halfh; i++)
        {
            memcpy((u8 *)dst, (const u8 *)(frame->data[2] + i * frame->linesize[2]), halfw);
            dst += halfw;
        }
        jsObject.call<void>("draw", compositionTime, y, u, v);
    }
};

#define FUNC(name) function(#name, &FFmpegAudioDecoder::name)
#undef PROP
#define PROP(name) property(#name, &FFmpegAudioDecoder::get##name, &FFmpegAudioDecoder::set##name)
EMSCRIPTEN_BINDINGS(FFmpegAudioDecoder)
{
    class_<FFmpegAudioDecoder>("AudioDecoder")
        .constructor<val>()
        .PROP(sample_rate)
        .FUNC(clear)
        .FUNC(setMp4)
        .FUNC(decode);
}
#undef FUNC
#define FUNC(name) function(#name, &FFmpegVideoDecoder::name)
#undef PROP
#define PROP(name) property(#name, &FFmpegVideoDecoder::get##name, &FFmpegVideoDecoder::set##name)
EMSCRIPTEN_BINDINGS(FFmpegVideoDecoder)
{
    class_<FFmpegVideoDecoder>("VideoDecoder")
        .constructor<val>()
        .FUNC(clear)
        .FUNC(setMp4)
        .FUNC(decode);
}