#pragma once
// Internal: small H.264 Annex-B helpers.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace glint {

inline bool hex_to_bytes(const std::string& hex, std::vector<uint8_t>& out)
{
    out.clear();
    if (hex.empty() || hex.size() % 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            out.clear();
            return false;
        }
        out.push_back((uint8_t)(hi << 4 | lo));
    }
    return true;
}

// Calls fn(type, nal, len) for each NAL unit (start codes removed).
template <typename Fn>
void for_each_nal(const uint8_t* p, size_t n, Fn fn)
{
    size_t i = 0, start = (size_t)-1;
    auto emit = [&](size_t end) {
        while (end > start && p[end - 1] == 0) end--;   // trailing zeros / next 4-byte start code
        if (end > start) fn(p[start] & 0x1f, p + start, end - start);
    };
    while (i + 3 <= n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if (start != (size_t)-1) emit(i);
            start = i + 3;
            i += 3;
        } else {
            i++;
        }
    }
    if (start != (size_t)-1 && start < n) emit(n);
}

inline bool has_idr(const uint8_t* p, size_t n)
{
    bool idr = false;
    for_each_nal(p, n, [&](int type, const uint8_t*, size_t) {
        if (type == 5) idr = true;
    });
    return idr;
}

// SPS and PPS valid enough to describe the stream.
inline bool valid_param_sets(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps)
{
    return sps.size() >= 4 && (sps[0] & 0x1f) == 7 && !pps.empty() && (pps[0] & 0x1f) == 8;
}

// Reads the quantiser (QP) of an access unit's first slice: how hard the
// encoder compressed that frame (lower is better, 0-51). For stats only.
class SliceQp {
public:
    bool set_params(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps)
    {
        ok_ = false;
        if (!valid_param_sets(sps, pps)) return false;
        Bits s(sps.data() + 1, sps.size() - 1);
        int profile = (int)s.u(8);
        s.u(16);
        s.ue();
        if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
            profile == 83 || profile == 86 || profile == 118 || profile == 128) {
            if (s.ue() == 3) s.u(1);
            s.ue();
            s.ue();
            s.u(1);
            if (s.u(1)) return false;   // scaling matrices: not worth parsing here
        }
        log2_frame_num_ = (int)s.ue() + 4;
        poc_type_ = (int)s.ue();
        if (poc_type_ == 0) {
            log2_poc_lsb_ = (int)s.ue() + 4;
        } else if (poc_type_ == 1) {
            delta_poc_zero_ = s.u(1) != 0;
            s.se();
            s.se();
            unsigned cycle = s.ue();
            for (unsigned i = 0; i < cycle && i < 256; i++) s.se();
        }
        s.ue();
        s.u(1);
        s.ue();
        s.ue();
        frame_mbs_only_ = s.u(1) != 0;

        Bits p(pps.data() + 1, pps.size() - 1);
        p.ue();
        p.ue();
        cabac_ = p.u(1) != 0;
        bottom_poc_ = p.u(1) != 0;
        if (p.ue() != 0) return false;   // slice groups
        ref_l0_ = (int)p.ue() + 1;
        ref_l1_ = (int)p.ue() + 1;
        weighted_ = p.u(1) != 0;
        bipred_ = (int)p.u(2);
        init_qp_ = 26 + p.se();
        p.se();
        p.se();
        p.u(1);
        p.u(1);
        redundant_ = p.u(1) != 0;
        ok_ = !s.bad && !p.bad;
        return ok_;
    }

    // -1 when there is no slice or it can't be read.
    int qp(const uint8_t* au, size_t n) const
    {
        if (!ok_) return -1;
        int result = -1;
        for_each_nal(au, n, [&](int type, const uint8_t* nal, size_t len) {
            if (result < 0 && (type == 1 || type == 5) && len > 1) result = slice_qp(nal, len, type == 5);
        });
        return result;
    }

private:
    // Bit reader over the RBSP (emulation-prevention bytes dropped).
    struct Bits {
        std::vector<uint8_t> d;
        size_t pos = 0;
        bool bad = false;
        Bits(const uint8_t* p, size_t n)
        {
            n = n < 96 ? n : 96;   // headers fit easily
            for (size_t i = 0; i < n; i++) {
                if (i >= 2 && p[i] == 3 && p[i - 1] == 0 && p[i - 2] == 0) continue;
                d.push_back(p[i]);
            }
        }
        uint32_t u(int bits)
        {
            uint32_t v = 0;
            for (int i = 0; i < bits; i++) {
                if (pos >= d.size() * 8) {
                    bad = true;
                    return 0;
                }
                v = v << 1 | ((d[pos / 8] >> (7 - pos % 8)) & 1);
                pos++;
            }
            return v;
        }
        uint32_t ue()
        {
            int zeros = 0;
            while (!bad && u(1) == 0 && zeros < 32) zeros++;
            return zeros ? ((1u << zeros) - 1 + u(zeros)) : 0;
        }
        int se()
        {
            uint32_t k = ue();
            return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);
        }
    };

    int slice_qp(const uint8_t* nal, size_t len, bool idr) const
    {
        int ref_idc = (nal[0] >> 5) & 3;
        Bits b(nal + 1, len - 1);
        b.ue();
        int st = (int)(b.ue() % 5);   // 0 P, 1 B, 2 I, 3 SP, 4 SI
        bool is_b = st == 1, is_i = st == 2 || st == 4, is_p = st == 0 || st == 3;
        b.ue();
        b.u(log2_frame_num_);
        bool field = false;
        if (!frame_mbs_only_ && (field = b.u(1) != 0)) b.u(1);
        if (idr) b.ue();
        if (poc_type_ == 0) {
            b.u(log2_poc_lsb_);
            if (bottom_poc_ && !field) b.se();
        } else if (poc_type_ == 1 && !delta_poc_zero_) {
            b.se();
            if (bottom_poc_ && !field) b.se();
        }
        if (redundant_) b.ue();
        if (is_b) b.u(1);
        if (is_p || is_b) {
            if (b.u(1)) {
                b.ue();
                if (is_b) b.ue();
            }
        }
        for (int list = 0; list < (is_b ? 2 : is_i ? 0 : 1); list++) {
            if (b.u(1)) {
                for (int guard = 0; guard < 64 && !b.bad; guard++) {
                    uint32_t idc = b.ue();
                    if (idc == 3) break;
                    b.ue();
                }
            }
        }
        if ((weighted_ && is_p) || (bipred_ == 1 && is_b)) return -1;   // weight tables: skip
        if (ref_idc) {
            if (idr) {
                b.u(1);
                b.u(1);
            } else if (b.u(1)) {
                for (int guard = 0; guard < 64 && !b.bad; guard++) {
                    uint32_t op = b.ue();
                    if (op == 0) break;
                    if (op == 1 || op == 3) b.ue();
                    if (op == 2) b.ue();
                    if (op == 3 || op == 6) b.ue();
                    if (op == 4) b.ue();
                }
            }
        }
        if (cabac_ && !is_i) b.ue();
        int qp = init_qp_ + b.se();
        return b.bad || qp < 0 || qp > 51 ? -1 : qp;
    }

    bool ok_ = false;
    int log2_frame_num_ = 4, poc_type_ = 0, log2_poc_lsb_ = 4;
    bool delta_poc_zero_ = false, frame_mbs_only_ = true;
    bool cabac_ = false, bottom_poc_ = false, weighted_ = false, redundant_ = false;
    int ref_l0_ = 1, ref_l1_ = 1, bipred_ = 0, init_qp_ = 26;
};

}
