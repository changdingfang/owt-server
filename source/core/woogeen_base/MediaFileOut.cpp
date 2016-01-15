/*
 * Copyright 2015 Intel Corporation All Rights Reserved. 
 * 
 * The source code contained or described herein and all documents related to the 
 * source code ("Material") are owned by Intel Corporation or its suppliers or 
 * licensors. Title to the Material remains with Intel Corporation or its suppliers 
 * and licensors. The Material contains trade secrets and proprietary and 
 * confidential information of Intel or its suppliers and licensors. The Material 
 * is protected by worldwide copyright and trade secret laws and treaty provisions. 
 * No part of the Material may be used, copied, reproduced, modified, published, 
 * uploaded, posted, transmitted, distributed, or disclosed in any way without 
 * Intel's prior express written permission.
 * 
 * No license under any patent, copyright, trade secret or other intellectual 
 * property right is granted to or conferred upon you by disclosure or delivery of 
 * the Materials, either expressly, by implication, inducement, estoppel or 
 * otherwise. Any license under such intellectual property rights must be express 
 * and approved by Intel in writing.
 */

#include <boost/algorithm/string.hpp>
#include "MediaFileOut.h"
#include <rtputils.h>

extern "C" {
#include <libavutil/channel_layout.h>
}

namespace woogeen_base {

DEFINE_LOGGER(MediaFileOut, "woogeen.media.MediaFileOut");

inline AVCodecID frameFormat2VideoCodecID(int frameFormat)
{
    switch (frameFormat) {
        case FRAME_FORMAT_VP8: return AV_CODEC_ID_VP8;
        case FRAME_FORMAT_H264: return AV_CODEC_ID_H264;
        default: return AV_CODEC_ID_VP8;
    }
}

inline AVCodecID frameFormat2AudioCodecID(int frameFormat)
{
    switch (frameFormat) {
        case FRAME_FORMAT_PCMU: return AV_CODEC_ID_PCM_MULAW;
        case FRAME_FORMAT_OPUS: return AV_CODEC_ID_OPUS;
        default: return AV_CODEC_ID_PCM_MULAW;
    }
}

MediaFileOut::MediaFileOut(const std::string& recordUrl, int snapshotInterval)
    : m_videoStream(nullptr)
    , m_audioStream(nullptr)
    , m_context(nullptr)
    , m_avTrailerNeeded(false)
    , m_videoId(-1)
    , m_audioId(-1)
    , m_videoFrameFormat(woogeen_base::FRAME_FORMAT_UNKNOWN)
    , m_audioFrameFormat(woogeen_base::FRAME_FORMAT_UNKNOWN)
    , m_recordPath(recordUrl)
    , m_snapshotInterval(snapshotInterval)
{
    m_videoQueue.reset(new MediaFrameQueue());
    m_audioQueue.reset(new MediaFrameQueue());

    // FIXME: These should really only be called once per application run
    av_register_all();
    avcodec_register_all();
    av_log_set_level(AV_LOG_WARNING);

    m_context = avformat_alloc_context();
    assert(m_context);

    m_recordPath.copy(m_context->filename, sizeof(m_context->filename), 0);
    m_context->oformat = av_guess_format(NULL, m_context->filename, NULL);
    assert(m_context->oformat);

    m_jobTimer.reset(new woogeen_base::JobTimer(100, this));

    ELOG_DEBUG("created");
}

MediaFileOut::~MediaFileOut()
{
    close();
}

void MediaFileOut::close()
{
    m_jobTimer->stop();

    if (m_context != NULL && m_avTrailerNeeded)
        av_write_trailer(m_context);

    if (m_videoStream && m_videoStream->codec != NULL)
        avcodec_close(m_videoStream->codec);

    if (m_audioStream && m_audioStream->codec != NULL)
        avcodec_close(m_audioStream->codec);

    if (m_context != NULL) {
        if (!(m_context->oformat->flags & AVFMT_NOFILE))
            avio_close(m_context->pb);
        avformat_free_context(m_context);
        m_context = NULL;
    }

    ELOG_DEBUG("closed");
}

void MediaFileOut::onFrame(const Frame& frame)
{
    if (m_status == AVStreamOut::Context_ERROR)
        return;

    switch (frame.format) {
    case FRAME_FORMAT_VP8:
    case FRAME_FORMAT_H264:
        if (!m_videoStream) {
            if (frame.additionalInfo.video.width > 0 && frame.additionalInfo.video.height > 0) {
                if (addVideoStream(frameFormat2VideoCodecID(frame.format), frame.additionalInfo.video.width, frame.additionalInfo.video.height)) {
                    ELOG_DEBUG("video stream added: %dx%d, %s", frame.additionalInfo.video.width, frame.additionalInfo.video.height,
                        (frame.format == woogeen_base::FRAME_FORMAT_VP8) ? "VP8" : "H264");
                    m_videoFrameFormat = frame.format;
                }
            } else
                break;
        } else if (m_videoStream->codec->codec_id != frameFormat2VideoCodecID(frame.format)) {
            ELOG_ERROR("different video frame formats cannot be recorded together.");
            m_status = AVStreamOut::Context_ERROR;
            break;
        }

        m_videoQueue->pushFrame(frame.payload, frame.length);
        break;

    case FRAME_FORMAT_PCMU:
    case FRAME_FORMAT_OPUS: {
        if (m_videoStream && !m_audioStream) { // make sure video stream is added first.
            if (addAudioStream(frameFormat2AudioCodecID(frame.format), frame.additionalInfo.audio.channels, frame.additionalInfo.audio.sampleRate)) {
                ELOG_DEBUG("audio stream added: %d channel(s), %d Hz, %s", frame.additionalInfo.audio.channels, frame.additionalInfo.audio.sampleRate,
                    (frame.format == woogeen_base::FRAME_FORMAT_PCMU) ? "PCMU" : "OPUS");
                m_audioFrameFormat = frame.format;
            }
        } else if (m_audioStream && m_audioStream->codec->codec_id != frameFormat2AudioCodecID(frame.format)) {
            ELOG_ERROR("different audio frame formats cannot be recorded together.");
            m_status = AVStreamOut::Context_ERROR;
            break;
        }

        uint8_t* payload = frame.payload;
        uint32_t length = frame.length;
        if (frame.additionalInfo.audio.isRtpPacket) {
            RTPHeader* rtp = reinterpret_cast<RTPHeader*>(payload);
            uint32_t headerLength = rtp->getHeaderLength();
            assert(length >= headerLength);
            payload += headerLength;
            length -= headerLength;
        }

        m_audioQueue->pushFrame(payload, length);
        break;
    }
    default:
        ELOG_ERROR("improper frame format. only VP8/H264 and PCMU/OPUS can be recorded currently");
        m_status = AVStreamOut::Context_ERROR;
        break;
    }
}

bool MediaFileOut::addAudioStream(enum AVCodecID codec_id, int nbChannels, int sampleRate)
{
    boost::lock_guard<boost::mutex> lock(m_contextMutex);
    AVStream* stream = avformat_new_stream(m_context, nullptr);
    if (!stream) {
        ELOG_ERROR("cannot add audio stream");
        m_status = AVStreamOut::Context_ERROR;
        return false;
    }
    AVCodecContext* c = stream->codec;
    c->codec_id       = codec_id;
    c->codec_type     = AVMEDIA_TYPE_AUDIO;
    c->channels       = nbChannels;
    c->channel_layout = av_get_default_channel_layout(nbChannels);
    c->sample_rate    = sampleRate;
    if (codec_id == AV_CODEC_ID_OPUS)
        c->sample_fmt = AV_SAMPLE_FMT_FLT;
    else
        c->sample_fmt = AV_SAMPLE_FMT_S16;

    if (m_context->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    m_audioStream = stream;
    return true;
}


bool MediaFileOut::addVideoStream(enum AVCodecID codec_id, unsigned int width, unsigned int height)
{
    boost::lock_guard<boost::mutex> lock(m_contextMutex);
    m_context->oformat->video_codec = codec_id;
    AVStream* stream = avformat_new_stream(m_context, nullptr);
    if (!stream) {
        ELOG_ERROR("cannot add video stream");
        m_status = AVStreamOut::Context_ERROR;
        return false;
    }
    AVCodecContext* c = stream->codec;
    c->codec_id   = codec_id;
    c->codec_type = AVMEDIA_TYPE_VIDEO;
    c->width      = width;
    c->height     = height;
    c->pix_fmt    = AV_PIX_FMT_YUV420P;
    /* Some formats want stream headers to be separate. */
    if (m_context->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    m_context->oformat->flags |= AVFMT_VARIABLE_FPS;
    m_videoStream = stream;
    return true;
}

void MediaFileOut::onTimeout()
{
    switch (m_status) {
    case AVStreamOut::Context_EMPTY:
        if (m_audioStream && m_videoStream) {
            if (!(m_context->oformat->flags & AVFMT_NOFILE)) {
                if (avio_open(&m_context->pb, m_context->filename, AVIO_FLAG_WRITE) < 0) {
                    m_status = AVStreamOut::Context_ERROR;
                    notifyAsyncEvent("RecordingStream", "output file does not exist or cannot be opened for write");
                    ELOG_ERROR("open output file failed");
                    return;
                }
            }
            av_dump_format(m_context, 0, m_context->filename, 1);
            if (avformat_write_header(m_context, nullptr) < 0) {
                m_status = AVStreamOut::Context_ERROR;
                notifyAsyncEvent("RecordingStream", "write file header error");
                ELOG_ERROR("avformat_write_header failed");
                return;
            }
            m_avTrailerNeeded = true;
            m_status = AVStreamOut::Context_READY;
            ELOG_DEBUG("context ready");
        } else
            return;
        break;
    case AVStreamOut::Context_READY:
        break;
    case AVStreamOut::Context_ERROR:
    default:
        notifyAsyncEvent("RecordingStream", "context initialization failed");
        ELOG_ERROR("context error");
        return;
    }

    boost::shared_ptr<EncodedFrame> mediaFrame;
    while (mediaFrame = m_audioQueue->popFrame())
        this->writeAudioFrame(*mediaFrame);

    while (mediaFrame = m_videoQueue->popFrame())
        this->writeVideoFrame(*mediaFrame);
}

void MediaFileOut::writeVideoFrame(EncodedFrame& encodedVideoFrame)
{
    AVPacket avpkt;
    av_init_packet(&avpkt);
    avpkt.data = encodedVideoFrame.m_payloadData;
    avpkt.size = encodedVideoFrame.m_payloadSize;
    avpkt.pts = (int64_t)(encodedVideoFrame.m_timeStamp / (av_q2d(m_videoStream->time_base) * 1000));
    avpkt.stream_index = 0;
    av_write_frame(m_context, &avpkt);
    av_free_packet(&avpkt);
}

void MediaFileOut::writeAudioFrame(EncodedFrame& encodedAudioFrame)
{
    AVPacket avpkt;
    av_init_packet(&avpkt);
    avpkt.data = encodedAudioFrame.m_payloadData;
    avpkt.size = encodedAudioFrame.m_payloadSize;
    avpkt.pts = (int64_t)(encodedAudioFrame.m_timeStamp / (av_q2d(m_audioStream->time_base) * 1000));
    avpkt.stream_index = 1;
    av_write_frame(m_context, &avpkt);
    av_free_packet(&avpkt);
}

} /* namespace mcu */