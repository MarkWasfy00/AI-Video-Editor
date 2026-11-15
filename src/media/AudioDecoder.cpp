#include "media/AudioDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ve {

bool AudioDecoder::open(const QString& path, int outSampleRate, QString* error)
{
    m_rate = outSampleRate;
    m_fmt = ff::openInput(path, error);
    if (!m_fmt) return false;
    m_streamIndex = ff::findAudioStream(m_fmt.get());
    if (m_streamIndex < 0) {
        if (error) *error = QStringLiteral("no audio stream");
        return false;
    }
    m_stream = m_fmt->streams[m_streamIndex];
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i)
        if (static_cast<int>(i) != m_streamIndex) m_fmt->streams[i]->discard = AVDISCARD_ALL;
    m_dec = ff::openDecoder(m_stream, 1, error);
    if (!m_dec) return false;
    m_pkt = ff::makePacket();
    m_frame = ff::makeFrame();
    return true;
}

void AudioDecoder::seekTo(TimeUs t)
{
    const std::int64_t ts = ff::usToPts(std::max<TimeUs>(0, t), m_stream);
    if (av_seek_frame(m_fmt.get(), m_streamIndex, ts, AVSEEK_FLAG_BACKWARD) < 0)
        avformat_seek_file(m_fmt.get(), m_streamIndex, INT64_MIN, ts, ts, 0);
    avcodec_flush_buffers(m_dec.get());
    m_swr.reset();
    m_eof = m_flushed = m_finished = false;
    m_buf.clear();
    m_bufStart = -1;
}

bool AudioDecoder::decodeMore()
{
    for (;;) {
        int rc = avcodec_receive_frame(m_dec.get(), m_frame.get());
        if (rc == AVERROR_EOF) {
            m_finished = true;
            return false;
        }
        if (rc == AVERROR(EAGAIN)) {
            if (m_eof) {
                if (m_flushed) { m_finished = true; return false; }
                avcodec_send_packet(m_dec.get(), nullptr);
                m_flushed = true;
                continue;
            }
            rc = av_read_frame(m_fmt.get(), m_pkt.get());
            if (rc < 0) { m_eof = true; continue; }
            if (m_pkt->stream_index == m_streamIndex)
                avcodec_send_packet(m_dec.get(), m_pkt.get());
            av_packet_unref(m_pkt.get());
            continue;
        }
        if (rc < 0) { m_finished = true; return false; }
        break;
    }

    AVFrame* f = m_frame.get();
    if (!m_swr) {
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, kChannels);
        AVChannelLayout inLayout;
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || f->ch_layout.nb_channels == 0)
            av_channel_layout_default(&inLayout, std::max(1, f->ch_layout.nb_channels));
        else
            av_channel_layout_copy(&inLayout, &f->ch_layout);
        SwrContext* swr = nullptr;
        const int rc = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, m_rate,
                                           &inLayout, static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);
        if (rc < 0 || swr_init(swr) < 0) {
            swr_free(&swr);
            av_frame_unref(f);
            m_finished = true;
            return false;
        }
        m_swr.reset(swr);
    }

    if (m_bufStart < 0) {
        const std::int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        const TimeUs us = ff::ptsToUs(pts, m_stream);
        m_bufStart = static_cast<std::int64_t>(std::llround(static_cast<double>(us) * m_rate / kUsPerSecond));
    }

    const int maxOut = swr_get_out_samples(m_swr.get(), f->nb_samples);
    m_convert.resize(static_cast<size_t>(std::max(0, maxOut)) * kChannels);
    auto* outPtr = reinterpret_cast<std::uint8_t*>(m_convert.data());
    const int got = swr_convert(m_swr.get(), &outPtr, maxOut,
                                const_cast<const std::uint8_t**>(f->extended_data), f->nb_samples);
    av_frame_unref(f);
    if (got > 0)
        m_buf.insert(m_buf.end(), m_convert.begin(), m_convert.begin() + static_cast<std::ptrdiff_t>(got) * kChannels);
    return true;
}

void AudioDecoder::read(TimeUs t, int frames, float* out)
{
    std::memset(out, 0, sizeof(float) * static_cast<size_t>(frames) * kChannels);
    if (!m_dec || frames <= 0) return;

    const std::int64_t s = static_cast<std::int64_t>(std::llround(static_cast<double>(t) * m_rate / kUsPerSecond));
    const std::int64_t bufFrames = static_cast<std::int64_t>(m_buf.size() / kChannels);
    const std::int64_t tolerance = m_rate / 4;

    if (m_bufStart < 0 || s < m_bufStart || s > m_bufStart + bufFrames + tolerance)
        seekTo(t);

    // Decode until the requested window is covered.
    while (!m_finished) {
        const std::int64_t have = m_bufStart < 0 ? -1 : m_bufStart + static_cast<std::int64_t>(m_buf.size() / kChannels);
        if (have >= s + frames) break;
        if (!decodeMore()) break;
        // Keep the buffer small: discard what lies before the window.
        if (m_bufStart >= 0 && s > m_bufStart) {
            const std::int64_t drop = std::min<std::int64_t>(s - m_bufStart, static_cast<std::int64_t>(m_buf.size() / kChannels));
            m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(drop * kChannels));
            m_bufStart += drop;
        }
    }

    if (m_bufStart < 0) return;
    const std::int64_t avail = static_cast<std::int64_t>(m_buf.size() / kChannels);
    for (int i = 0; i < frames; ++i) {
        const std::int64_t idx = s + i - m_bufStart;
        if (idx < 0) continue;
        if (idx >= avail) break;
        out[i * 2] = m_buf[static_cast<size_t>(idx) * 2];
        out[i * 2 + 1] = m_buf[static_cast<size_t>(idx) * 2 + 1];
    }

    // Drop consumed samples so the next sequential read starts at the front.
    const std::int64_t consumed = std::clamp<std::int64_t>(s + frames - m_bufStart, 0, avail);
    m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(consumed * kChannels));
    m_bufStart += consumed;
}

} // namespace ve
