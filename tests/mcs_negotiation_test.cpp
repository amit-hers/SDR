// Acceptance tests for the negotiated-MCS wire format and state machine.
//
// The requirement that matters most here is not the wire round-trip (though
// that's checked too): it's that the TX/RX coupling bug documented in
// BridgeMode.hpp -- a node's own measured SNR silently driving its OWN TX
// scheme, so two independently-adapting nodes never converge -- is
// structurally impossible with this design, not just avoided by convention.
// The two-node simulation below is the test that actually proves it.
#include "sdr/bridge/McsNegotiation.hpp"
#include <cstdio>
#include <string>

using namespace sdr;
static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  %-70s %s\n", what.c_str(), ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

int main() {
    std::printf("wire format\n");
    {
        McsCaps c{modeSetAdd(modeSetAdd(0, ModCode::BPSK), ModCode::QPSK), modeSetAdd(0, ModCode::QPSK)};
        auto w = encodeMcsCaps(c);
        check(w.size() == MCS_CAPS_SIZE, "caps encodes to MCS_CAPS_SIZE");
        McsCaps r;
        check(decodeMcsCaps(w.data(), w.size(), r), "caps decodes back");
        check(r.can_demod == c.can_demod && r.can_mod == c.can_mod, "caps fields survive the round trip");
    }
    {
        McsRequest m; m.seq = 42; m.scheme = ModCode::QAM16; m.snr_db_x10 = -35;
        auto w = encodeMcsRequest(m);
        check(w.size() == MCS_REQUEST_SIZE, "request encodes to MCS_REQUEST_SIZE");
        McsRequest r;
        check(decodeMcsRequest(w.data(), w.size(), r), "request decodes back");
        check(r.seq == 42 && r.scheme == ModCode::QAM16 && r.snr_db_x10 == -35,
              "request fields survive the round trip, including negative SNR");
    }
    {
        McsRequest m; m.scheme = ModCode::AUTO;
        auto w = encodeMcsRequest(m);
        McsRequest r;
        check(!decodeMcsRequest(w.data(), w.size(), r), "a request for AUTO is rejected on decode");
    }
    {
        McsAck m; m.seq = 7; m.scheme = ModCode::QAM64;
        auto w = encodeMcsAck(m);
        McsAck r;
        check(decodeMcsAck(w.data(), w.size(), r), "ack decodes back");
        check(r.seq == 7 && r.scheme == ModCode::QAM64, "ack fields survive the round trip");
    }
    {
        McsNack m; m.seq = 9; m.reason = McsNackReason::BUSY;
        auto w = encodeMcsNack(m);
        McsNack r;
        check(decodeMcsNack(w.data(), w.size(), r), "nack decodes back");
        check(r.seq == 9 && r.reason == McsNackReason::BUSY, "nack fields survive the round trip");
    }
    {
        // ACK and NACK are the same wire size (10 bytes), so magic -- not
        // size -- must be what tells them apart. decodeMcs() is the shared
        // dispatcher; prove it resolves each to the right kind.
        McsAck a{1, ModCode::QPSK};
        auto wa = encodeMcsAck(a);
        McsMessage out;
        check(decodeMcs(wa.data(), wa.size(), out) == McsKind::ACK,
              "an ACK-sized, ACK-shaped payload dispatches as ACK, not NACK");
        McsNack n{1, McsNackReason::STALE};
        auto wn = encodeMcsNack(n);
        check(decodeMcs(wn.data(), wn.size(), out) == McsKind::NACK,
              "a NACK-sized, NACK-shaped payload dispatches as NACK, not ACK");
    }
    {
        McsCaps c{}; auto w = encodeMcsCaps(c);
        w[4] = 99; // version byte
        McsCaps r;
        check(!decodeMcsCaps(w.data(), w.size(), r), "an unknown version is NOT assumed compatible");
    }
    {
        uint8_t shortbuf[4] = {0};
        McsMessage out;
        check(decodeMcs(shortbuf, sizeof shortbuf, out) == McsKind::NONE, "a short payload is rejected, not read past");
    }

    std::printf("\nstate machine: capability gating\n");
    {
        McsNegotiator me(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(0, ModCode::QPSK)});
        check(!me.requestScheme(ModCode::QAM16, 0).has_value(),
              "cannot request a scheme before peer caps are known at all");
        me.onCaps(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(0, ModCode::QPSK)}); // peer lacks QAM16
        check(!me.requestScheme(ModCode::QAM16, 0).has_value(),
              "cannot request a scheme the peer has not advertised in its can_mod");
        me.onCaps(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(modeSetAdd(0, ModCode::QPSK), ModCode::QAM16)});
        check(me.requestScheme(ModCode::QAM16, 0).has_value(),
              "can request a scheme once the peer advertises it in can_mod");
    }
    {
        McsNegotiator me(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(0, ModCode::QPSK)});
        check(!me.requestScheme(ModCode::AUTO, 0).has_value(), "AUTO can never be requested");
    }
    {
        McsNegotiator me(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(0, ModCode::QPSK)});
        me.onCaps(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(modeSetAdd(0, ModCode::QPSK), ModCode::QAM16)});
        check(me.requestScheme(ModCode::QAM16, -10).has_value(), "first request is sent");
        check(!me.requestScheme(ModCode::QAM16, -10).has_value(),
              "a second request cannot be sent while one is already pending");
    }

    std::printf("\nstate machine: current_tx_mode_ has exactly one path to change\n");
    {
        McsNegotiator me(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(0, ModCode::QPSK)}, ModCode::QPSK);
        check(me.currentTxMode() == ModCode::QPSK, "starts at the configured baseline");
        me.onCaps(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(modeSetAdd(0, ModCode::QPSK), ModCode::QAM16)});
        me.requestScheme(ModCode::QAM16, -10);
        check(me.currentTxMode() == ModCode::QPSK,
              "ORIGINATING a request does not change our own TX mode -- this is the coupling-bug guard");
    }
    {
        // An unsupported incoming request must NACK, not silently switch.
        McsNegotiator me(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(0, ModCode::QPSK)}, ModCode::QPSK);
        McsRequest req; req.seq = 1; req.scheme = ModCode::QAM64;
        auto reply = me.onRequest(req);
        McsMessage out;
        check(decodeMcs(reply.data(), reply.size(), out) == McsKind::NACK,
              "a request for a scheme outside our can_mod is NACKed");
        check(out.nack.reason == McsNackReason::UNSUPPORTED, "NACK reason is UNSUPPORTED");
        check(me.currentTxMode() == ModCode::QPSK, "a NACKed request does not change our TX mode");
    }
    {
        // An accepted incoming request DOES switch -- the one legitimate path.
        McsNegotiator me(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(modeSetAdd(0, ModCode::QPSK), ModCode::QAM16)}, ModCode::QPSK);
        McsRequest req; req.seq = 5; req.scheme = ModCode::QAM16;
        auto reply = me.onRequest(req);
        McsMessage out;
        check(decodeMcs(reply.data(), reply.size(), out) == McsKind::ACK, "a supported request is ACKed");
        check(out.ack.seq == 5 && out.ack.scheme == ModCode::QAM16, "the ACK echoes the request's seq and scheme");
        check(me.currentTxMode() == ModCode::QAM16,
              "an ACCEPTED INCOMING request is the one path that changes our own TX mode");
    }

    std::printf("\nstate machine: timeout and NACK fall back safely\n");
    {
        McsNegotiator me(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(0, ModCode::QPSK)}, ModCode::QPSK);
        me.onCaps(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(modeSetAdd(0, ModCode::QPSK), ModCode::QAM16)});
        me.requestScheme(ModCode::QAM16, -10);
        check(me.hasPending(), "a sent request is tracked as pending");
        check(me.expirePending(), "the pending request can be expired");
        check(!me.hasPending(), "no longer pending after expiry");
        check(me.currentTxMode() == ModCode::QPSK, "an expired (timed-out) request never changed our TX mode");
        check(me.requestScheme(ModCode::QAM16, -10).has_value(), "a fresh request can be sent after expiry");
    }
    {
        McsNegotiator me(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(0, ModCode::QPSK)}, ModCode::QPSK);
        me.onCaps(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(modeSetAdd(0, ModCode::QPSK), ModCode::QAM16)});
        auto w = me.requestScheme(ModCode::QAM16, -10);
        McsRequest sent; decodeMcsRequest(w->data(), w->size(), sent);
        check(me.onNack(McsNack{sent.seq, McsNackReason::UNSUPPORTED}),
              "a NACK matching our pending seq clears it");
        check(me.currentTxMode() == ModCode::QPSK, "a NACK never changes our TX mode");
        check(!me.onNack(McsNack{sent.seq, McsNackReason::UNSUPPORTED}),
              "a duplicate/late NACK with no pending request is a no-op, not an error");
    }
    {
        McsNegotiator me(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(0, ModCode::QPSK)}, ModCode::QPSK);
        me.onCaps(McsCaps{modeSetAdd(0, ModCode::QPSK), modeSetAdd(modeSetAdd(0, ModCode::QPSK), ModCode::QAM16)});
        auto w = me.requestScheme(ModCode::QAM16, -10);
        McsRequest sent; decodeMcsRequest(w->data(), w->size(), sent);
        check(!me.onAck(McsAck{sent.seq + 1, ModCode::QAM16}),
              "an ACK with the wrong seq does not clear our pending request");
        check(me.hasPending(), "pending request is still outstanding after a mismatched ACK");
    }

    std::printf("\ntwo-node simulation: the actual coupling-bug proof\n");
    {
        // A measures its own RX quality of B's transmissions and decides (via
        // its own AdaptiveModem, simulated here as "we just know") that QAM16
        // would serve better. This must result in B's TX mode changing and
        // A's TX mode staying exactly where it started -- A requests a
        // change in what it RECEIVES, never in what it SENDS.
        McsCaps capsA{modeSetAdd(0, ModCode::QPSK), modeSetAdd(0, ModCode::QPSK)};
        McsCaps capsB{modeSetAdd(0, ModCode::QPSK), modeSetAdd(modeSetAdd(0, ModCode::QPSK), ModCode::QAM16)};
        McsNegotiator A(capsA, ModCode::QPSK);
        McsNegotiator B(capsB, ModCode::QPSK);

        A.onCaps(capsB);
        B.onCaps(capsA);

        const ModCode a_tx_before = A.currentTxMode();
        const ModCode b_tx_before = B.currentTxMode();
        check(a_tx_before == ModCode::QPSK && b_tx_before == ModCode::QPSK, "both start at QPSK");

        // A decided (from ITS OWN measured SNR of B's signal) that it wants
        // B to transmit QAM16 to it.
        auto req_wire = A.requestScheme(ModCode::QAM16, -5);
        check(req_wire.has_value(), "A originates a request to B");
        check(A.currentTxMode() == a_tx_before,
              "A's own TX mode is UNCHANGED immediately after originating the request");

        McsRequest req; decodeMcsRequest(req_wire->data(), req_wire->size(), req);
        auto ack_wire = B.onRequest(req);
        check(B.currentTxMode() == ModCode::QAM16,
              "B's TX mode changed to what A asked for -- B is the one transmitting to A");
        check(A.currentTxMode() == a_tx_before,
              "A's own TX mode is STILL unchanged after B accepted -- A never switches its own TX "
              "from receiving a request; only an ACK of A's own request could inform A of anything, "
              "and even then nothing in this design lets that touch A's TX mode either");

        McsMessage out; decodeMcs(ack_wire.data(), ack_wire.size(), out);
        check(A.onAck(out.ack), "A's pending request resolves against B's ACK");
        check(A.currentTxMode() == a_tx_before,
              "A's TX mode is STILL QPSK after processing the ACK -- the coupling bug is structurally absent: "
              "A asked B to change what B sends, and that is the only thing that changed");
        check(A.lastKnownPeerTxMode() == ModCode::QAM16,
              "A now knows B is transmitting QAM16 -- informational, does not drive A's own TX");
    }

    std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
