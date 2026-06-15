# pytest + tshark integration test for the 5G simulator's Registration flow.
#
# Runs the 4 real binaries (nrf_sim, udm_sim, amf_sim, gnb_sim) -- the same
# way you'd run them by hand -- registers UE 1 and UE 2, then inspects the
# 5g_capture.pcap that amf_sim writes using tshark, which is the only
# "ground truth" for what's actually on the wire.
#
# NRF registration/discovery (UDM/AMF <-> NRF, SBI port 29510) is NOT written
# to 5g_capture.pcap -- only amf_sim writes that pcap, and two processes can't
# safely share one pcap file, so UDM's NRF call would be missing anyway. NRF
# traffic is instead verified via the session log files (g5_*_session.log),
# which capture the same BEGINNER-level "X -> NRF: ..." lines you'd see on
# the terminal.
#
# Every field name / value asserted below was found by running tshark on a
# real generated pcap first (frame numbers, http.file_data, tcp.payload).
# Nothing here is copied from a spec sheet or another tool's example output.
#
# Requires: the project already built (cd ../build && cmake .. && make) and
# tshark on PATH.
import json
import subprocess
import time
from pathlib import Path

import pytest

BUILD_DIR = Path(__file__).resolve().parent.parent / "build"
PCAP = BUILD_DIR / "5g_capture.pcap"

pytestmark = pytest.mark.skipif(
    not (BUILD_DIR / "udm_sim").exists() or not (BUILD_DIR / "nrf_sim").exists(),
    reason="5g-simulator not built -- run: cd 5g-simulator/build && cmake .. && make",
)


def _tshark(*args):
    """Run tshark against the generated pcap, return stdout lines."""
    out = subprocess.run(
        ["tshark", "-r", str(PCAP), *args],
        capture_output=True, text=True, check=True,
    )
    return [line for line in out.stdout.strip().splitlines() if line]


def _http_json_body(frame_num):
    """JSON-decode the HTTP request/response body of a frame."""
    hexdata = subprocess.run(
        ["tshark", "-r", str(PCAP), "-Y", f"frame.number == {frame_num}",
         "-T", "fields", "-e", "http.file_data"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    return json.loads(bytes.fromhex(hexdata).decode())


def _n2_json(frame_num):
    """JSON-decode an N2 frame's payload (plain JSON text, one per TCP segment)."""
    hexdata = subprocess.run(
        ["tshark", "-r", str(PCAP), "-Y", f"frame.number == {frame_num}",
         "-T", "fields", "-e", "tcp.payload"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    return json.loads(bytes.fromhex(hexdata).decode())


@pytest.fixture(scope="module")
def registration_pcap():
    """Run nrf_sim + udm_sim + amf_sim + gnb_sim once: register UE 1 and UE 2."""
    for f in (PCAP, BUILD_DIR / "g5_nrf_session.log", BUILD_DIR / "g5_udm_session.log",
              BUILD_DIR / "g5_amf_session.log", BUILD_DIR / "g5_gnb_session.log"):
        f.unlink(missing_ok=True)

    nrf = subprocess.Popen(["./nrf_sim"], cwd=BUILD_DIR,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)  # NRF must be up before UDM/AMF register with it on startup
    udm = subprocess.Popen(["./udm_sim"], cwd=BUILD_DIR,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    amf = subprocess.Popen(["./amf_sim"], cwd=BUILD_DIR,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1)  # let both servers bind + register with NRF
        gnb = subprocess.run(
            ["./gnb_sim"], cwd=BUILD_DIR, input="REG 1\nREG 2\nQUIT\n",
            capture_output=True, text=True, timeout=10,
        )
        time.sleep(0.5)  # let amf_sim flush 5g_capture.pcap
        yield gnb
    finally:
        amf.terminate()
        udm.terminate()
        nrf.terminate()
        amf.wait(timeout=5)
        udm.wait(timeout=5)
        nrf.wait(timeout=5)


def test_gnb_completes_registration_for_both_ues(registration_pcap):
    assert "registration finished -- 5G-GUTI=5g-guti-404-10-amf01-00001" in registration_pcap.stdout
    assert "registration finished -- 5G-GUTI=5g-guti-404-10-amf01-00002" in registration_pcap.stdout


def test_pcap_has_no_malformed_packets():
    assert _tshark("-Y", "_ws.malformed") == []


def test_pcap_has_expected_packet_count():
    # 3-way handshake + 1 N2 + (2x SBI request/response over their own
    # 3-way handshake) + N2 reply, twice over -> 24 frames total. If a
    # third UE/extra SBI call is added later this number must be updated.
    assert len(_tshark("-T", "fields", "-e", "frame.number")) == 24


def test_sbi_auth_request_is_real_nudm_ueau_post(registration_pcap):
    """AMF -> UDM: Nudm_UEAuthentication_Get (TS 29.509), one POST per UE."""
    for ue in (1, 2):
        suci = f"suci-0-404-10-0000-0-0-000000000{ue}"
        frames = _tshark(
            "-Y", f'http.request.method == "POST" and http.request.uri contains "{suci}"',
            "-T", "fields", "-e", "frame.number",
        )
        assert len(frames) == 1, f"expected one auth-data POST for UE {ue}"
        assert _tshark(
            "-Y", f"frame.number == {frames[0]}", "-T", "fields", "-e", "http.request.uri",
        ) == [f"/nudm-ueau/v2/{suci}/security-information/generate-auth-data"]
        body = _http_json_body(frames[0])
        assert body == {"servingNetworkName": "5G:mnc010.mcc404.3gppnetwork.org"}


def test_sbi_auth_response_carries_5g_aka_vectors(registration_pcap):
    """UDM -> AMF: 200 OK with 5G-AKA auth vectors (rand/autn/xresStar/kausf)."""
    ok_frames = _tshark("-Y", 'http.response.code == 200', "-T", "fields", "-e", "frame.number")
    auth_bodies = {b["supi"]: b for b in (_http_json_body(f) for f in ok_frames) if "authType" in b}

    assert set(auth_bodies) == {"imsi-404100000000001", "imsi-404100000000002"}
    for supi, body in auth_bodies.items():
        assert body["authType"] == "5G_AKA"
        assert len(bytes.fromhex(body["rand"])) == 16
        assert len(bytes.fromhex(body["autn"])) == 16
        assert len(bytes.fromhex(body["xresStar"])) == 16
        assert len(bytes.fromhex(body["kausf"])) == 32

    # different RAND per UE -> different vectors
    assert auth_bodies["imsi-404100000000001"]["rand"] != auth_bodies["imsi-404100000000002"]["rand"]


def test_sbi_sdm_request_and_response(registration_pcap):
    """AMF -> UDM: Nudm_SDM_Get (TS 29.503) GET .../am-data, 200 OK with NSSAI+AMBR."""
    for ue in (1, 2):
        imsi = f"imsi-40410000000000{ue}"
        frames = _tshark(
            "-Y", f'http.request.method == "GET" and http.request.uri contains "{imsi}"',
            "-T", "fields", "-e", "http.request.uri",
        )
        assert frames == [f"/nudm-sdm/v2/{imsi}/am-data"]

    ok_frames = _tshark("-Y", 'http.response.code == 200', "-T", "fields", "-e", "frame.number")
    sdm_bodies = [b for b in (_http_json_body(f) for f in ok_frames) if "nssai" in b]
    assert len(sdm_bodies) == 2
    for body in sdm_bodies:
        assert body["nssai"] == {"defaultSingleNssais": [{"sst": 1, "sd": "000001"}]}
        assert body["subscribedUeAmbr"] == {"uplink": "100Mbps", "downlink": "200Mbps"}


def test_n2_message_sequence_per_ue(registration_pcap):
    """gNB<->AMF over real NGAP port 38412: length-prefixed JSON NAS messages."""
    frames = _tshark("-Y", "tcp.port == 38412 and tcp.len > 0", "-T", "fields", "-e", "frame.number")
    by_ue = {}
    for f in frames:
        msg = _n2_json(f)
        by_ue.setdefault(msg["ranUeNgapId"], []).append(msg["msgType"])

    expected = ["RegistrationRequest", "AuthenticationRequest", "AuthenticationResponse",
                "RegistrationAccept", "RegistrationComplete"]
    assert by_ue[1] == expected
    assert by_ue[2] == expected


def test_n2_registration_accept_grants_5g_guti_and_nssai(registration_pcap):
    frames = _tshark("-Y", "tcp.port == 38412 and tcp.len > 0", "-T", "fields", "-e", "frame.number")
    accepts = {m["ranUeNgapId"]: m for f in frames
               for m in [_n2_json(f)] if m["msgType"] == "RegistrationAccept"}

    assert accepts[1]["5gGuti"] == "5g-guti-404-10-amf01-00001"
    assert accepts[2]["5gGuti"] == "5g-guti-404-10-amf01-00002"
    for m in accepts.values():
        assert m["allowedNssai"] == [{"sst": 1, "sd": "000001"}]


def test_nrf_session_log_shows_udm_and_amf_registration(registration_pcap):
    """UDM and AMF each PUT their profile to the NRF on startup
    (Nnrf_NFManagement_NFRegister) -- visible in nrf_sim's own session log."""
    log = (BUILD_DIR / "g5_nrf_session.log").read_text()
    assert "NRF <- UDM: Nnrf_NFManagement_NFRegister (PUT)" in log
    assert "nfType       = UDM" in log
    assert "host:port    = 127.0.0.1:29503" in log
    assert "NRF -> UDM: 201 Created" in log

    assert "NRF <- AMF: Nnrf_NFManagement_NFRegister (PUT)" in log
    assert "nfType       = AMF" in log
    assert "NRF -> AMF: 201 Created" in log


def test_amf_discovers_udm_via_nrf(registration_pcap):
    """AMF does NOT use a hardcoded UDM address: it asks the NRF
    (Nnrf_NFDiscovery_Search, target-nf-type=UDM) and gets back UDM's
    host:port, which matches where udm_sim actually registered itself."""
    nrf_log = (BUILD_DIR / "g5_nrf_session.log").read_text()
    assert "NRF <- Nnrf_NFDiscovery_Search (GET ?target-nf-type=UDM)" in nrf_log
    assert "found UDM @ 127.0.0.1:29503" in nrf_log

    amf_log = (BUILD_DIR / "g5_amf_session.log").read_text()
    assert "-> NRF: Nnrf_NFDiscovery_Search (target-nf-type=UDM)" in amf_log
    assert "<- NRF: UDM is at 127.0.0.1:29503" in amf_log
    assert "AMF: SBI peer UDM at 127.0.0.1:29503" in amf_log


def test_n2_resstar_matches_sbi_xresstar(registration_pcap):
    """The actual 5G-AKA check: UE's RES* (sent to AMF over N2) must equal
    the XRES* UDM computed and sent to AMF over SBI -- AMF compares them."""
    n2_frames = _tshark("-Y", "tcp.port == 38412 and tcp.len > 0", "-T", "fields", "-e", "frame.number")
    res_star = {m["ranUeNgapId"]: m["resStar"] for f in n2_frames
                for m in [_n2_json(f)] if m["msgType"] == "AuthenticationResponse"}

    sbi_frames = _tshark("-Y", 'http.response.code == 200', "-T", "fields", "-e", "frame.number")
    xres_star = {}
    for f in sbi_frames:
        body = _http_json_body(f)
        if "xresStar" in body:
            ue_id = int(body["supi"][-1])
            xres_star[ue_id] = body["xresStar"]

    assert res_star == xres_star
    assert res_star[1] != res_star[2]
