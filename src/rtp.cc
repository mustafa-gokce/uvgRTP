#ifdef __linux__
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include <chrono>

#include "clock.hh"
#include "debug.hh"
#include "random.hh"
#include "rtp.hh"

#define INVALID_TS UINT64_MAX

uvg_rtp::rtp::rtp(rtp_format_t fmt):
    wc_start_(0),
    sent_pkts_(0),
    timestamp_(INVALID_TS),
    payload_size_(MAX_PAYLOAD)
{
    seq_  = uvg_rtp::random::generate_32() & 0xffff;
    ts_   = uvg_rtp::random::generate_32();
    ssrc_ = uvg_rtp::random::generate_32();

    set_payload(fmt);
}

uvg_rtp::rtp::~rtp()
{
}

uint32_t uvg_rtp::rtp::get_ssrc()
{
    return ssrc_;
}

uint16_t uvg_rtp::rtp::get_sequence()
{
    return seq_;
}

void uvg_rtp::rtp::set_payload(rtp_format_t fmt)
{
    payload_ = fmt_ = fmt;

    switch (fmt_) {
        case RTP_FORMAT_H265:
            clock_rate_ = 90000;
            break;

        case RTP_FORMAT_OPUS:
            clock_rate_ = 48000;
            break;

        default:
            LOG_WARN("Unknown RTP format, setting clock rate to 10000");
            clock_rate_ = 10000;
            break;
    }
}

void uvg_rtp::rtp::set_dynamic_payload(uint8_t payload)
{
    payload_ = payload;
}

void uvg_rtp::rtp::inc_sequence()
{
    seq_++;
}

void uvg_rtp::rtp::inc_sent_pkts()
{
    sent_pkts_++;
}

void uvg_rtp::rtp::update_sequence(uint8_t *buffer)
{
    if (!buffer)
        return;

    *(uint16_t *)&buffer[2] = htons(seq_);
}

void uvg_rtp::rtp::fill_header(uint8_t *buffer)
{
    if (!buffer)
        return;

    /* This is the first RTP message, get wall clock reading (t = 0)
     * and generate random RTP timestamp for this reading */
    if (wc_start_ == 0) {
        ts_        = uvg_rtp::random::generate_32();
        wc_start_2 = uvg_rtp::clock::hrc::now();
        wc_start_  = 1;
    }

    buffer[0] = 2 << 6; // RTP version
    buffer[1] = (payload_ & 0x7f) | (0 << 7);

    *(uint16_t *)&buffer[2] = htons(seq_);
    *(uint32_t *)&buffer[8] = htonl(ssrc_);

    if (timestamp_ == INVALID_TS) {
        *(uint32_t *)&buffer[4] = htonl(
            ts_
            + uvg_rtp::clock::hrc::diff_now(wc_start_2)
            * clock_rate_
            / 1000
        );
    } else {
        *(uint32_t *)&buffer[4] = htonl(timestamp_);
    }
}

void uvg_rtp::rtp::set_timestamp(uint64_t timestamp)
{
    timestamp_= timestamp;
}

uint32_t uvg_rtp::rtp::get_clock_rate(void)
{
    return clock_rate_;
}

void uvg_rtp::rtp::set_payload_size(size_t payload_size)
{
    payload_size_ = payload_size;
}

size_t uvg_rtp::rtp::get_payload_size()
{
    return payload_size_;
}

rtp_format_t uvg_rtp::rtp::get_payload()
{
    return (rtp_format_t)fmt_;
}

rtp_error_t uvg_rtp::rtp::packet_handler(ssize_t size, void *packet, int flags, uvg_rtp::frame::rtp_frame **out)
{
    (void)flags;

    /* not an RTP frame */
    if (size < 12)
        return RTP_PKT_NOT_HANDLED;

    uint8_t *ptr = (uint8_t *)packet;

    /* invalid version */
    if (((ptr[0] >> 6) & 0x03) != 0x2)
        return RTP_PKT_NOT_HANDLED;

    if (!(*out = uvg_rtp::frame::alloc_rtp_frame()))
        return RTP_GENERIC_ERROR;

    (*out)->header.version   = (ptr[0] >> 6) & 0x03;
    (*out)->header.padding   = (ptr[0] >> 5) & 0x01;
    (*out)->header.ext       = (ptr[0] >> 4) & 0x01;
    (*out)->header.cc        = (ptr[0] >> 0) & 0x0f;
    (*out)->header.marker    = (ptr[1] & 0x80) ? 1 : 0;
    (*out)->header.payload   = (ptr[1] & 0x7f);
    (*out)->header.seq       = ntohs(*(uint16_t *)&ptr[2]);
    (*out)->header.timestamp = ntohl(*(uint32_t *)&ptr[4]);
    (*out)->header.ssrc      = ntohl(*(uint32_t *)&ptr[8]);

    (*out)->payload_len = (size_t)size - sizeof(uvg_rtp::frame::rtp_header);

    /* Skip the generic RTP header
     * There may be 0..N CSRC entries after the header, so check those */
    ptr += sizeof(uvg_rtp::frame::rtp_header);

    if ((*out)->header.cc > 0) {
        LOG_DEBUG("frame contains csrc entries");

        if ((ssize_t)((*out)->payload_len - (*out)->header.cc * sizeof(uint32_t)) < 0) {
            LOG_DEBUG("Invalid frame length, %d CSRC entries, total length %zu", (*out)->header.cc, (*out)->payload_len);
            (void)uvg_rtp::frame::dealloc_frame(*out);
            return RTP_GENERIC_ERROR;
        }
        LOG_DEBUG("Allocating %u CSRC entries", (*out)->header.cc);

        (*out)->csrc         = new uint32_t[(*out)->header.cc];
        (*out)->payload_len -= (*out)->header.cc * sizeof(uint32_t);

        for (size_t i = 0; i < (*out)->header.cc; ++i) {
            (*out)->csrc[i]  = *(uint32_t *)ptr;
            ptr             += sizeof(uint32_t);
        }
    }

    if ((*out)->header.ext) {
        LOG_DEBUG("Frame contains extension information");
        (*out)->ext = new uvg_rtp::frame::ext_header;

        (*out)->ext->type  = ntohs(*(uint16_t *)&ptr[0]);
        (*out)->ext->len   = ntohs(*(uint32_t *)&ptr[1]);
        (*out)->ext->data  = (uint8_t *)memdup(ptr + 2 * sizeof(uint16_t), (*out)->ext->len);
        ptr               += 2 * sizeof(uint16_t) + (*out)->ext->len;
    }

    /* If padding is set to 1, the last byte of the payload indicates
     * how many padding bytes was used. Make sure the padding length is
     * valid and subtract the amount of padding bytes from payload length */
    if ((*out)->header.padding) {
        LOG_DEBUG("Frame contains padding");
        uint8_t padding_len = (*out)->payload[(*out)->payload_len - 1];

        if (!padding_len || (*out)->payload_len <= padding_len) {
            uvg_rtp::frame::dealloc_frame(*out);
            return RTP_GENERIC_ERROR;
        }

        (*out)->payload_len -= padding_len;
        (*out)->padding_len  = padding_len;
    }

    (*out)->payload    = (uint8_t *)memdup(ptr, (*out)->payload_len);
    (*out)->dgram      = (uint8_t *)packet;
    (*out)->dgram_size = size;

    return RTP_PKT_MODIFIED;
}
