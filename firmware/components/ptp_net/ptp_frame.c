#include "ptp_frame.h"
#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xff;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;
    p[3] = v & 0xff;
}

bool ptp_frame_classify(const uint8_t *frame, size_t len, ptp_frame_info_t *info)
{
    if (len < PTP_FRAME_ETH_HDR) {
        return false;
    }
    size_t off = 12;
    uint16_t type = rd16(frame + off);
    if (type == 0x8100) { // single 802.1Q tag
        if (len < PTP_FRAME_ETH_HDR + 4) {
            return false;
        }
        off += 4;
        type = rd16(frame + off);
    }
    off += 2;
    memset(info, 0, sizeof(*info));
    memcpy(info->src_mac, frame + 6, 6);

    if (type == PTP_ETHERTYPE) {
        info->transport = PTP_TRANSPORT_L2;
        info->msg_offset = off;
        info->msg_len = len - off;
        return info->msg_len >= 34; // PTP common header
    }
    if (type != 0x0800 || len < off + 20) {
        return false;
    }
    const uint8_t *ip = frame + off;
    size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20 || ip[9] != 17 || len < off + ihl + 8) {
        return false;
    }
    if (rd16(ip + 6) & 0x3fff) { // fragmented (MF set or offset != 0)
        return false;
    }
    const uint8_t *udp = ip + ihl;
    uint16_t dport = rd16(udp + 2);
    if (dport != PTP_PORT_EVENT && dport != PTP_PORT_GENERAL) {
        return false;
    }
    size_t udp_len = rd16(udp + 4);
    if (udp_len < 8 + 34 || off + ihl + udp_len > len) {
        return false;
    }
    info->transport = PTP_TRANSPORT_UDP4;
    info->src_ip = rd32(ip + 12);
    info->dst_ip = rd32(ip + 16);
    info->dst_port = dport;
    info->msg_offset = off + ihl + 8;
    info->msg_len = udp_len - 8;
    return true;
}

void ptp_frame_ip4_mcast_mac(uint32_t group, uint8_t mac[6])
{
    mac[0] = 0x01;
    mac[1] = 0x00;
    mac[2] = 0x5e;
    mac[3] = (group >> 16) & 0x7f;
    mac[4] = (group >> 8) & 0xff;
    mac[5] = group & 0xff;
}

static uint16_t ip_checksum(const uint8_t *hdr, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i += 2) {
        sum += rd16(hdr + i);
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

void ptp_frame_build_udp4_hdr(uint8_t *out, const ptp_udp4_hdr_params_t *p, size_t payload_len)
{
    memcpy(out, p->dst_mac, 6);
    memcpy(out + 6, p->src_mac, 6);
    wr16(out + 12, 0x0800);

    uint8_t *ip = out + 14;
    ip[0] = 0x45;
    ip[1] = (uint8_t)(p->dscp << 2);
    wr16(ip + 2, (uint16_t)(20 + 8 + payload_len));
    wr16(ip + 4, p->ip_id);
    wr16(ip + 6, 0x4000); // DF
    ip[8] = p->ttl;
    ip[9] = 17;
    wr16(ip + 10, 0);
    wr32(ip + 12, p->src_ip);
    wr32(ip + 16, p->dst_ip);
    wr16(ip + 10, ip_checksum(ip, 20));

    uint8_t *udp = ip + 20;
    wr16(udp, p->dst_port);
    wr16(udp + 2, p->dst_port);
    wr16(udp + 4, (uint16_t)(8 + payload_len));
    wr16(udp + 6, 0);
}
