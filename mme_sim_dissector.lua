-- ============================================================
-- MME Simulator — Wireshark Dissector
-- Decodes: S1AP (38412), Diameter S6a (3868), Gx (3869)
--
-- INSTALL:
--   Wireshark → Help → About Wireshark → Folders → Personal Lua Plugins
--   Copy this file there → Analyze → Reload Lua Plugins
--
-- FILTER (after install):
--   mme_sim2                          → all simulator messages
--   mme_sim2 and tcp.port == 38412    → S1AP only
--   mme_sim2 and tcp.port == 3868     → Diameter S6a only
--   mme_sim2 and tcp.port == 3869     → Diameter Gx only
-- ============================================================

local proto = Proto("mme_sim2", "MME-SIM2")

local f_frame_len  = ProtoField.uint32("mme2.frame_len",  "Frame Length",    base.DEC)
local f_msg_type   = ProtoField.uint16("mme2.msg_type",   "Message Type",    base.HEX)
local f_flags      = ProtoField.uint16("mme2.flags",      "Flags",           base.HEX)
local f_seq        = ProtoField.uint32("mme2.seq",        "Sequence Number", base.DEC)
local f_tlv_tag    = ProtoField.uint16("mme2.tlv.tag",    "Tag",             base.HEX)
local f_tlv_len    = ProtoField.uint16("mme2.tlv.len",    "Length",          base.DEC)
local f_tlv_val_u8 = ProtoField.uint8 ("mme2.tlv.u8",    "Value",           base.HEX)
local f_tlv_val_u16= ProtoField.uint16("mme2.tlv.u16",   "Value",           base.DEC)
local f_tlv_val_u32= ProtoField.uint32("mme2.tlv.u32",   "Value",           base.DEC)
local f_tlv_val_hex= ProtoField.bytes ("mme2.tlv.bytes",  "Value")

proto.fields = {
    f_frame_len, f_msg_type, f_flags, f_seq,
    f_tlv_tag, f_tlv_len, f_tlv_val_u8, f_tlv_val_u16,
    f_tlv_val_u32, f_tlv_val_hex
}

-- ── Message type names ────────────────────────────────────────
local MSG_TYPE_NAME = {
    -- S1AP (port 38412)
    [0x0001] = "S1AP InitialUEMessage      [TS 36.413 §9.1.7.1] — Attach Request",
    [0x0002] = "S1AP DL-NAS-Transport      [TS 36.413 §9.1.7.2] — Downlink NAS",
    [0x0003] = "S1AP UL-NAS-Transport      [TS 36.413 §9.1.7.3] — Uplink NAS",
    [0x0004] = "S1AP InitialContextSetupReq[TS 36.413 §9.1.4.1] — Attach Accept",
    [0x0005] = "S1AP InitialContextSetupRsp[TS 36.413 §9.1.4.2]",
    [0x0006] = "NAS SecurityModeCommand    [TS 24.301 §8.2.20]  — EEA2+EIA2",
    [0x0007] = "NAS SecurityModeComplete   [TS 24.301 §8.2.21]  — Security Activated",
    -- Diameter S6a (port 3868)
    [0x0101] = "Diameter S6a AIR  [TS 29.272 §7.2.5]  — Authentication-Info-Req",
    [0x0102] = "Diameter S6a AIA  [TS 29.272 §7.2.6]  — Authentication-Info-Ans",
    [0x0103] = "Diameter S6a ULR  [TS 29.272 §7.2.3]  — Update-Location-Req",
    [0x0104] = "Diameter S6a ULA  [TS 29.272 §7.2.4]  — Update-Location-Ans",
    -- GTPv2 (written as UDP, not decoded here)
    [0x0201] = "GTP-C CreateSessionReq     [TS 29.274 §7.2.1]",
    [0x0202] = "GTP-C CreateSessionRsp     [TS 29.274 §7.2.2]",
    [0x0203] = "GTP-C ModifyBearerReq      [TS 29.274 §7.2.7]",
    [0x0204] = "GTP-C ModifyBearerRsp      [TS 29.274 §7.2.8]",
    [0x0205] = "GTP-C DeleteSessionReq     [TS 29.274 §7.2.9]",
    [0x0206] = "GTP-C DeleteSessionRsp     [TS 29.274 §7.2.10]",
    -- Diameter Gx (port 3869)
    [0x0401] = "Diameter Gx CCR-I [TS 29.212 §4.5.1]  — Credit-Control-Request",
    [0x0402] = "Diameter Gx CCA-I [TS 29.212 §4.5.2]  — Credit-Control-Answer",
    -- Diameter Cx (IMS, port 3870)
    [0x0501] = "Diameter Cx SAR   [TS 29.229 §6.1.3]  — Server-Assignment-Req",
    [0x0502] = "Diameter Cx SAA   [TS 29.229 §6.1.4]  — Server-Assignment-Ans",
    [0x0503] = "Diameter Cx UAR   [TS 29.229 §6.1.1]  — User-Authorization-Req",
    [0x0504] = "Diameter Cx UAA   [TS 29.229 §6.1.2]  — User-Authorization-Ans",
}

-- ── Tag names (matches tlv.h Tag enum) ───────────────────────
local TAG_NAME = {
    -- S1AP IEs
    [0x0010] = "eNB-UE-S1AP-ID    (local UE handle at eNB)",
    [0x0011] = "MME-UE-S1AP-ID    (local UE handle at MME)",
    [0x0012] = "RRC-Cause         (0=emergency 1=highPri 3=mo-Sig 4=mo-Data)",
    [0x0013] = "TAI-MCC           (Mobile Country Code)",
    [0x0014] = "TAI-MNC           (Mobile Network Code)",
    [0x0015] = "TAI-TAC           (Tracking Area Code — paging scope)",
    [0x0016] = "E-CGI             (E-UTRAN Cell Global ID)",
    -- NAS IEs
    [0x0100] = "NAS-ProtocolDisc  (0x07=EMM)",
    [0x0101] = "NAS-SecurityHdr   (0x00=plain NAS, cleartext!)",
    [0x0102] = "NAS-MsgType       (0x41=AttachReq 0x52=AuthReq 0x5D=SecModCmd)",
    [0x0103] = "NAS-AttachType    (1=EPS-initial 2=combined)",
    [0x0104] = "NAS-KSI           (7=no key → MME must run full EPS-AKA)",
    [0x0105] = "NAS-IdType        (1=IMSI 6=GUTI)",
    [0x0106] = "NAS-IMSI          (CLEARTEXT in 4G, encrypted SUCI in 5G!)",
    [0x0107] = "NAS-UE-NetCap     (supported ciphers/integrity algorithms)",
    [0x0108] = "NAS-RAND          (128-bit random challenge from HSS)",
    [0x0109] = "NAS-AUTN          (128-bit network auth token — UE verifies)",
    [0x010A] = "NAS-RES           (64-bit auth response from UE)",
    [0x010B] = "NAS-EBI           (EPS Bearer ID)",
    [0x010C] = "NAS-UE-IP         (IP address allocated by P-GW)",
    -- Diameter IEs
    [0x0200] = "DIA-IMSI          (subscriber identity for HSS lookup)",
    [0x0201] = "DIA-PLMN          (MCC+MNC — visited network)",
    [0x0202] = "DIA-RAND          (same as NAS-RAND, from HSS Milenage)",
    [0x0203] = "DIA-AUTN          (same as NAS-AUTN, 16 bytes)",
    [0x0204] = "DIA-XRES          (Expected-RES — MME compares with UE's RES)",
    [0x0205] = "DIA-KASME         (32-byte root key — NAS+AS keys derived from this)",
    -- S1AP bearer/ICSR IEs
    [0x0300] = "ICSR-AMBR-UL      (UE Aggregate Max Bit Rate Uplink)",
    [0x0301] = "ICSR-AMBR-DL      (UE Aggregate Max Bit Rate Downlink)",
    [0x0302] = "ICSR-SGW-S1U-IP   (S-GW S1-U IP address for GTP-U tunnel)",
    [0x0303] = "ICSR-SGW-TEID     (S-GW Tunnel Endpoint ID for user-plane)",
    [0x0304] = "ICSR-ENB-TEID     (eNB Tunnel Endpoint ID)",
    -- Gx IEs
    [0x0400] = "GX-IMSI           (subscriber ID for policy lookup)",
    [0x0401] = "GX-APN            (Access Point Name)",
    [0x0402] = "GX-QCI            (QoS Class Identifier — 1=voice, 9=data)",
    [0x0403] = "GX-RULE-NAME      (charging rule name from PCRF)",
}

-- ── le_uint64 → decimal string ────────────────────────────────
local function le_u64_str(tvbr)
    local lo = tvbr:range(0,4):le_uint()
    local hi = tvbr:range(4,4):le_uint()
    return string.format("%.0f", hi * 4294967296.0 + lo)
end

-- ── Main dissector ────────────────────────────────────────────
function proto.dissector(buffer, pinfo, tree)
    local buflen = buffer:len()
    if buflen < 4 then
        pinfo.desegment_offset = 0
        pinfo.desegment_len = DESEGMENT_ONE_MORE_SEGMENT
        return
    end

    local plen = buffer(0,4):le_uint()
    if plen == 0 or plen > 65536 then return end  -- not our protocol
    if buflen < 4 + plen then
        pinfo.desegment_offset = 0
        pinfo.desegment_len = 4 + plen - buflen
        return
    end

    pinfo.cols.protocol = "MME-SIM2"

    local root = tree:add(proto, buffer(0, 4+plen), "MME-Simulator Frame")
    root:add_le(f_frame_len, buffer(0,4)):append_text("  bytes payload")

    if plen < 8 then return end

    -- FrameHeader
    local mtype = buffer(4,2):le_uint()
    local mtype_str = MSG_TYPE_NAME[mtype] or string.format("UNKNOWN(0x%04X)", mtype)
    local seq_val   = buffer(8,4):le_uint()

    local hdr = root:add(proto, buffer(4,8), "Header")
    hdr:add_le(f_msg_type, buffer(4,2)):append_text("  " .. mtype_str)
    hdr:add_le(f_flags,    buffer(6,2))
    hdr:add_le(f_seq,      buffer(8,4))

    -- Info column — show message name prominently
    pinfo.cols.info = string.format("%s  seq=%d", mtype_str, seq_val)

    -- TLV fields
    local pos = 12
    while pos + 4 <= 4 + plen do
        local tag  = buffer(pos,  2):le_uint()
        local vlen = buffer(pos+2, 2):le_uint()
        if pos + 4 + vlen > 4 + plen then break end

        local tag_name = TAG_NAME[tag] or string.format("Tag-0x%04X", tag)
        local tlv = root:add(proto, buffer(pos, 4+vlen), tag_name)
        tlv:add_le(f_tlv_tag, buffer(pos,   2)):append_text("  (0x"..string.format("%04X",tag)..")")
        tlv:add_le(f_tlv_len, buffer(pos+2, 2))

        if vlen == 1 then
            local v = buffer(pos+4,1):uint()
            tlv:add(f_tlv_val_u8,  buffer(pos+4,1)):append_text(
                string.format("  = 0x%02X (%d)", v, v))
        elseif vlen == 2 then
            local v = buffer(pos+4,2):le_uint()
            tlv:add_le(f_tlv_val_u16, buffer(pos+4,2)):append_text(
                string.format("  = %d", v))
        elseif vlen == 4 then
            local v = buffer(pos+4,4):le_uint()
            tlv:add_le(f_tlv_val_u32, buffer(pos+4,4)):append_text(
                string.format("  = %d", v))
        elseif vlen == 8 then
            tlv:add(f_tlv_val_hex, buffer(pos+4,8)):append_text(
                "  = " .. le_u64_str(buffer(pos+4,8)))
        else
            local hex = ""
            for i = 0, math.min(vlen,8)-1 do
                hex = hex .. string.format("%02X", buffer(pos+4+i,1):uint())
            end
            if vlen > 8 then hex = hex .. "..." end
            tlv:add(f_tlv_val_hex, buffer(pos+4,vlen)):append_text("  = "..hex)
        end

        pos = pos + 4 + vlen
    end
end

-- ── Register on simulator ports ───────────────────────────────
local tcp_table = DissectorTable.get("tcp.port")
tcp_table:add(38412, proto)   -- S1AP    (eNB ↔ MME)
tcp_table:add(3868,  proto)   -- Diameter S6a (MME ↔ HSS)
tcp_table:add(3869,  proto)   -- Diameter Gx  (P-GW ↔ PCRF)
tcp_table:add(3870,  proto)   -- Diameter Cx  (S-CSCF ↔ IMS-HSS)
