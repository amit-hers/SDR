#pragma once
// Negotiated MCS (Modulation and Coding Scheme) changes between two peers.
//
// WHY THIS EXISTS, AND WHY IT IS SEPARATE FROM AdaptiveModem. AdaptiveModem
// already decides, from a measured SNR and hysteresis, which scheme WOULD be
// best. What has never existed is a safe way to get the PEER to actually
// switch to it: BridgeMode.hpp documents the failure that happens without
// one -- an earlier design let each node's local RX SNR drive that node's OWN
// TX scheme, so two independently-adapting nodes never converged, and the
// link's BPSK-only preamble correlator broke whenever acquisition's own
// modulation changed under it. This file is the negotiation layer that
// belongs between "I measured X" and "the peer now transmits Y": it is the
// thing that was missing, not a replacement for the measurement or the
// hysteresis.
//
// THE COUPLING BUG IS MADE STRUCTURALLY IMPOSSIBLE, NOT JUST AVOIDED BY
// CONVENTION. McsNegotiator::currentTxMode() has exactly one setter in this
// whole file, inside onRequest(), and onRequest() only runs for a message
// that ARRIVED from the peer. A node's own measured SNR can only ever
// produce an outgoing McsRequest (via requestScheme()) addressed to the
// peer -- there is no code path from "I measured low SNR" to "I lowered my
// own TX scheme". Only the peer, by accepting a request, can do that. Grep
// for `current_tx_mode_ =` to see this holds: one assignment site.
//
// RIDES THE SAME FL_CTRL CHANNEL HELLO AND THE HEALTH PROBE ALREADY USE, for
// the same reason both of those give: the bridge already fills idle airtime
// with control frames to hold the demodulator's timing loop, so this adds no
// new protocol and no extra airtime beyond occasionally substituting one
// control filler frame for another. The control channel itself always rides
// the BPSK-always preamble/header (see Frame.hpp's split-modulation layout),
// so negotiation messages stay reliable even while a payload-mode change is
// actually in flight -- there is no bootstrapping problem where a mode
// change could strand the very channel used to negotiate it.
//
// SCOPE. This is the protocol and the state machine only -- wiring it into
// sdr_bridge.cpp's control-filler priority queue (nextControlPayload) and
// into a live multi-mode FPGA datapath is deliberately NOT done here. Per
// the user's own ordering: (1) prove the FEC+QAM chain on hardware, (2) THIS
// -- define the protocol, (3) integrate fixed-mode 16-QAM into the live
// modem, (4) fixed-mode 64-QAM, (5) only then automatic fallback/upgrade.
// Items 3 and 4 are what give a node a real can_mod/can_demod beyond
// {BPSK, QPSK}; this file does not assume or require either yet.
#include "sdr/framing/Frame.hpp"  // ModCode
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace sdr {

// ── Capability sets ─────────────────────────────────────────────────────
// Bit (ModCode value - 1) set means that scheme is supported. ModCode::AUTO
// is never a bit -- it is not a real scheme, so it cannot be requested,
// advertised, or accepted; encode/decode and the setters below all reject it.
using ModeSet = uint8_t;

inline ModeSet modeBit(ModCode m) {
    return static_cast<ModeSet>(1u << (static_cast<uint8_t>(m) - 1));
}
inline bool modeSetHas(ModeSet s, ModCode m) {
    return m != ModCode::AUTO && (s & modeBit(m)) != 0;
}
inline ModeSet modeSetAdd(ModeSet s, ModCode m) {
    return m == ModCode::AUTO ? s : static_cast<ModeSet>(s | modeBit(m));
}

// ── Wire messages ───────────────────────────────────────────────────────
// Same convention as ProbeMessage/PeerIdentity (HealthProbe.hpp,
// PeerHandshake.hpp): magic-prefixed, versioned, fixed size, distinguished
// by magic (and size, redundantly, in case two magics ever collide after a
// future change to either -- see decodeProbe's own comment on this).
//
// Wire bytes little-endian, 4-character magics, same style as "SDRH"/"PREQ"/
// "PRPY".
static constexpr uint32_t MCS_CAPS_MAGIC    = 0x43534D4D; // "MMSC" little-endian -> bytes 4D 4D 53 43
static constexpr uint32_t MCS_REQUEST_MAGIC = 0x51534D4D; // "MMSQ"
static constexpr uint32_t MCS_ACK_MAGIC     = 0x41534D4D; // "MMSA"
static constexpr uint32_t MCS_NACK_MAGIC    = 0x4E534D4D; // "MMSN"
static constexpr uint8_t  MCS_VERSION       = 1;

struct McsCaps {
    ModeSet can_demod = 0;  // schemes this node can RECEIVE
    ModeSet can_mod   = 0;  // schemes this node can TRANSMIT
};
static constexpr std::size_t MCS_CAPS_SIZE = 4 + 1 + 1 + 1; // magic+version+can_demod+can_mod = 7

inline std::vector<uint8_t> encodeMcsCaps(const McsCaps& c) {
    std::vector<uint8_t> out(MCS_CAPS_SIZE, 0);
    uint8_t* p = out.data();
    std::memcpy(p, &MCS_CAPS_MAGIC, 4); p += 4;
    *p++ = MCS_VERSION;
    *p++ = c.can_demod;
    *p++ = c.can_mod;
    return out;
}
inline bool decodeMcsCaps(const uint8_t* b, std::size_t n, McsCaps& out) {
    if (n < MCS_CAPS_SIZE) return false;
    uint32_t magic; std::memcpy(&magic, b, 4);
    if (magic != MCS_CAPS_MAGIC) return false;
    const uint8_t* p = b + 4;
    if (*p++ != MCS_VERSION) return false; // a future version is not assumed compatible
    out.can_demod = *p++;
    out.can_mod   = *p++;
    return true;
}

// snr_db_x10 is advisory only -- logging/diagnostics on the receiving side,
// never required for correctness. It is NOT what the peer trusts to decide
// whether to accept: the peer re-validates the requested scheme against its
// own can_mod regardless of what SNR the requester claims motivated it.
struct McsRequest {
    uint32_t seq         = 0;
    ModCode  scheme      = ModCode::QPSK;
    int16_t  snr_db_x10  = 0; // signed: a request can be motivated by a link so bad it reads negative dB
};
static constexpr std::size_t MCS_REQUEST_SIZE = 4 + 1 + 4 + 1 + 2; // = 12

inline std::vector<uint8_t> encodeMcsRequest(const McsRequest& r) {
    std::vector<uint8_t> out(MCS_REQUEST_SIZE, 0);
    uint8_t* p = out.data();
    std::memcpy(p, &MCS_REQUEST_MAGIC, 4); p += 4;
    *p++ = MCS_VERSION;
    std::memcpy(p, &r.seq, 4); p += 4;
    *p++ = static_cast<uint8_t>(r.scheme);
    std::memcpy(p, &r.snr_db_x10, 2); p += 2;
    return out;
}
inline bool decodeMcsRequest(const uint8_t* b, std::size_t n, McsRequest& out) {
    if (n < MCS_REQUEST_SIZE) return false;
    uint32_t magic; std::memcpy(&magic, b, 4);
    if (magic != MCS_REQUEST_MAGIC) return false;
    const uint8_t* p = b + 4;
    if (*p++ != MCS_VERSION) return false;
    std::memcpy(&out.seq, p, 4); p += 4;
    out.scheme = static_cast<ModCode>(*p++);
    std::memcpy(&out.snr_db_x10, p, 2); p += 2;
    return out.scheme != ModCode::AUTO; // AUTO is never a valid request
}

struct McsAck {
    uint32_t seq    = 0;              // echoes the request's seq
    ModCode  scheme = ModCode::QPSK;  // confirms which scheme was accepted
};
static constexpr std::size_t MCS_ACK_SIZE = 4 + 1 + 4 + 1; // = 10

inline std::vector<uint8_t> encodeMcsAck(const McsAck& a) {
    std::vector<uint8_t> out(MCS_ACK_SIZE, 0);
    uint8_t* p = out.data();
    std::memcpy(p, &MCS_ACK_MAGIC, 4); p += 4;
    *p++ = MCS_VERSION;
    std::memcpy(p, &a.seq, 4); p += 4;
    *p++ = static_cast<uint8_t>(a.scheme);
    return out;
}
inline bool decodeMcsAck(const uint8_t* b, std::size_t n, McsAck& out) {
    if (n < MCS_ACK_SIZE) return false;
    uint32_t magic; std::memcpy(&magic, b, 4);
    if (magic != MCS_ACK_MAGIC) return false;
    const uint8_t* p = b + 4;
    if (*p++ != MCS_VERSION) return false;
    std::memcpy(&out.seq, p, 4); p += 4;
    out.scheme = static_cast<ModCode>(*p++);
    return true;
}

enum class McsNackReason : uint8_t {
    UNSUPPORTED = 1,  // the requested scheme is not in our can_mod
    BUSY        = 2,  // a different negotiation is already in flight
    STALE       = 3,  // seq is older than one we already acted on
};

struct McsNack {
    uint32_t      seq    = 0;
    McsNackReason reason = McsNackReason::UNSUPPORTED;
};
static constexpr std::size_t MCS_NACK_SIZE = 4 + 1 + 4 + 1; // = 10

inline std::vector<uint8_t> encodeMcsNack(const McsNack& n) {
    std::vector<uint8_t> out(MCS_NACK_SIZE, 0);
    uint8_t* p = out.data();
    std::memcpy(p, &MCS_NACK_MAGIC, 4); p += 4;
    *p++ = MCS_VERSION;
    std::memcpy(p, &n.seq, 4); p += 4;
    *p++ = static_cast<uint8_t>(n.reason);
    return out;
}
inline bool decodeMcsNack(const uint8_t* b, std::size_t n_len, McsNack& out) {
    if (n_len < MCS_NACK_SIZE) return false;
    uint32_t magic; std::memcpy(&magic, b, 4);
    if (magic != MCS_NACK_MAGIC) return false;
    const uint8_t* p = b + 4;
    if (*p++ != MCS_VERSION) return false;
    std::memcpy(&out.seq, p, 4); p += 4;
    out.reason = static_cast<McsNackReason>(*p++);
    return true;
}

// Dispatch helper matching decodeProbe()'s pattern: try each magic in turn.
// A caller that already rejected HELLO and probe sizes/magics calls this
// last, consistent with the layered-dispatch comment in sdr_bridge.cpp
// ("Not a HELLO ... try the [probe]").
enum class McsKind { NONE, CAPS, REQUEST, ACK, NACK };

struct McsMessage {
    McsCaps    caps;
    McsRequest request;
    McsAck     ack;
    McsNack    nack;
};

inline McsKind decodeMcs(const uint8_t* b, std::size_t n, McsMessage& out) {
    if (decodeMcsCaps(b, n, out.caps))       return McsKind::CAPS;
    if (decodeMcsRequest(b, n, out.request)) return McsKind::REQUEST;
    if (decodeMcsAck(b, n, out.ack))         return McsKind::ACK;
    if (decodeMcsNack(b, n, out.nack))       return McsKind::NACK;
    return McsKind::NONE;
}

// ── Negotiator state machine ────────────────────────────────────────────
//
// One instance per peer link. Drives nothing on its own -- it has no clock
// and no I/O. The caller (sdr_bridge's control loop, eventually) feeds it
// incoming messages via onRequest/onAck/onNack/onCaps, asks it to originate
// a request via requestScheme() when ITS OWN decision logic (AdaptiveModem)
// says a change would help, and polls timedOut() to know when a pending
// request should be retried or given up on.
class McsNegotiator {
public:
    // my_caps: what this node can demodulate/modulate TODAY. Starts as
    // {BPSK, QPSK} on every node until items 3/4 land; grows from there.
    // baseline: the scheme to run until a negotiation changes it. Must be
    // in my_caps.can_mod, and should be the same safe default on every node
    // (QPSK, matching the deployed bridge today) so a node that has never
    // heard from its peer still transmits something the peer can decode.
    explicit McsNegotiator(McsCaps my_caps, ModCode baseline = ModCode::QPSK)
        : my_caps_(my_caps), current_tx_mode_(baseline) {}

    // ── Peer state, learned passively ───────────────────────────────────
    void onCaps(const McsCaps& peer) { peer_caps_ = peer; have_peer_caps_ = true; }
    bool havePeerCaps() const { return have_peer_caps_; }
    const McsCaps& peerCaps() const { return peer_caps_; }

    // ── Originating a request ───────────────────────────────────────────
    // Returns the wire bytes to send, or nullopt if the request should not
    // be sent: scheme is AUTO, we don't believe the peer can transmit it
    // (peer_caps_ unknown or lacks it in can_mod), or a request is already
    // pending (never two in flight at once -- the caller's AdaptiveModem
    // hysteresis should not be proposing changes faster than one RTT anyway,
    // but this is the structural guard, not just a tuning assumption).
    std::optional<std::vector<uint8_t>> requestScheme(ModCode scheme, int16_t snr_db_x10) {
        if (scheme == ModCode::AUTO) return std::nullopt;
        if (!have_peer_caps_ || !modeSetHas(peer_caps_.can_mod, scheme)) return std::nullopt;
        if (pending_) return std::nullopt;
        McsRequest r;
        r.seq = next_seq_++;
        r.scheme = scheme;
        r.snr_db_x10 = snr_db_x10;
        pending_ = PendingRequest{r.seq, scheme};
        return encodeMcsRequest(r);
    }

    // ── Responding to an incoming request ───────────────────────────────
    // THE ONLY PLACE current_tx_mode_ IS ASSIGNED. Only runs for a message
    // that arrived from the peer, so this node's own SNR measurements can
    // never reach this assignment directly -- see the file header.
    std::vector<uint8_t> onRequest(const McsRequest& req) {
        if (!modeSetHas(my_caps_.can_mod, req.scheme)) {
            McsNack n{req.seq, McsNackReason::UNSUPPORTED};
            return encodeMcsNack(n);
        }
        current_tx_mode_ = req.scheme;
        McsAck a{req.seq, req.scheme};
        return encodeMcsAck(a);
    }

    // ── Resolving a request we originated ───────────────────────────────
    // Returns true if this ack/nack matched our pending request (and
    // cleared it); false for a stale/mismatched seq, which the caller
    // should simply ignore rather than treat as a protocol error -- a
    // duplicate or late reply for a request we already gave up on is
    // expected traffic, not a fault.
    bool onAck(const McsAck& ack) {
        if (!pending_ || pending_->seq != ack.seq) return false;
        last_known_peer_tx_mode_ = ack.scheme; // informational only
        pending_.reset();
        return true;
    }
    bool onNack(const McsNack&) {
        // Reason is available to the caller for logging via the last
        // decoded McsNack if it wants it; the state machine's own behavior
        // does not depend on WHICH reason, because every reason means the
        // same thing here: stay on the current scheme.
        if (!pending_) return false;
        pending_.reset();
        return true;
    }

    // ── Timeout ──────────────────────────────────────────────────────────
    // The caller supplies "has it been long enough" rather than this class
    // owning a clock, matching HealthProbe's PROBE_TIMEOUT_US pattern of
    // the bridge doing its own time bookkeeping. Clearing a timed-out
    // pending request does NOT touch current_tx_mode_: an un-ACKed outgoing
    // request only affects what we EXPECT to receive, never what we
    // transmit. Returns true if a pending request was cleared.
    bool expirePending() {
        if (!pending_) return false;
        pending_.reset();
        return true;
    }
    bool hasPending() const { return pending_.has_value(); }

    // ── State for the caller ─────────────────────────────────────────────
    ModCode currentTxMode() const { return current_tx_mode_; }
    std::optional<ModCode> lastKnownPeerTxMode() const { return last_known_peer_tx_mode_; }
    const McsCaps& myCaps() const { return my_caps_; }

private:
    struct PendingRequest {
        uint32_t seq;
        ModCode  scheme;
    };

    McsCaps  my_caps_;
    McsCaps  peer_caps_{};
    bool     have_peer_caps_ = false;
    ModCode  current_tx_mode_;
    uint32_t next_seq_ = 1;
    std::optional<PendingRequest> pending_;
    std::optional<ModCode> last_known_peer_tx_mode_;
};

} // namespace sdr
