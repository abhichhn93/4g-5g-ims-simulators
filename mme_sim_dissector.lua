-- ============================================================
-- MME Simulator Phase 2 — Wireshark TLV Dissector
-- Handles: port 38412 (S1AP eNB↔MME) + port 3868 (Diameter MME↔HSS)
--
-- INSTALL:
--   1. Wireshark → Help → About → Folders → Personal Lua Plugins
--   2. Copy this file there
--   3. Analyze → Reload Lua Plugins
--   4. Filter: (tcp.port == 38412 or tcp.port == 3868) and tcp.len > 0
-- ============================================================

local proto = Proto("mme_sim2", "MME-Sim Phase2 TLV")

-- Fields
local f_frame_len  = ProtoField.uint32("mme2.frame_len",  "Frame Payload Length", base.DEC)
local f_msg_type   = ProtoField.uint16("mme2.msg_type",   "Message Type",         base.HEX)
local f_flags      = ProtoField.uint16("mme2.flags",      "Flags",                base.HEX)
local f_seq        = ProtoField.uint32("mme2.seq",        "Sequence Number",      base.DEC)
local f_tlv_tag    = ProtoField.uint16("mme2.tlv.tag",    "Tag",                  base.HEX)
local f_tlv_len    = ProtoField.uint16("mme2.tlv.len",    "Length",               base.DEC)
local f_tlv_val_u8 = ProtoField.uint8 ("mme2.tlv.u8",    "Value",                base.HEX)
local f_tlv_val_u16= ProtoField.uint16("mme2.tlv.u16",   "Value",                base.DEC)
local f_tlv_val_u32= ProtoField.uint32("mme2.tlv.u32",   "Value",                base.DEC)
local f_tlv_val_hex= ProtoField.bytes ("mme2.tlv.bytes",  "Value")

proto.fields = { f_frame_len, f_msg_type, f_flags, f_seq,
                 f_tlv_tag, f_tlv_len, f_tlv_val_u8, f_tlv_val_u16, f_tlv_val_u32, f_tlv_val_hex }

-- Tag names (must match tlv.h Tag enum)
local TAG_NAME = {
    [0x0010] = "ENB_UE_S1AP_ID",
    [0x0011] = "MME_UE_S1AP_ID",
    [0x0012] = "RRC_CAUSE",
    [0x0013] = "TAI_MCC",
    [0x0014] = "TAI_MNC",
    [0x0015] = "TAI_TAC",
    [0x0016] = "EUTRAN_CGI",
    [0x0100] = "NAS_PROTO_DISC",
    [0x0101] = "NAS_SEC_HDR",
    [0x0102] = "NAS_MSG_TYPE",
    [0x0103] = "NAS_ATTACH_TYPE",
    [0x0104] = "NAS_KSI",
    [0x0105] = "NAS_ID_TYPE",
    [0x0106] = "NAS_IMSI",
    [0x0107] = "NAS_UE_CAP",
    [0x0108] = "NAS_RAND",
    [0x0109] = "NAS_AUTN",
    [0x010A] = "NAS_RES",
    [0x0200] = "DIA_IMSI",
    [0x0201] = "DIA_PLMN",
    [0x0202] = "DIA_RAND",
    [0x0203] = "DIA_AUTN",
    [0x0204] = "DIA_XRES",
    [0x0205] = "DIA_KASME",
}

local TAG_NOTE = {
    [0x0010] = "eNB's local UE handle",
    [0x0011] = "MME's local UE handle",
    [0x0012] = "0=emergency 1=highPri 2=mt-Access 3=mo-Sig 4=mo-Data",
    [0x0013] = "Mobile Country Code",
    [0x0014] = "Mobile Network Code",
    [0x0015] = "Tracking Area Code",
    [0x0100] = "0x07=EPS Mobility Management",
    [0x0101] = "0x00=plain NAS (IMSI in cleartext!)",
    [0x0102] = "0x41=AttachReq 0x52=AuthReq 0x53=AuthResp",
    [0x0104] = "7=no key → MME must run full AKA",
    [0x0105] = "1=IMSI 6=GUTI",
    [0x0106] = "IMSI — cleartext in 4G, encrypted in 5G (SUCI)",
    [0x0108] = "Random challenge to UE (16 bytes)",
    [0x0109] = "Auth Token — UE verifies network with this (16 bytes)",
    [0x010A] = "UE's auth response — MME compares with XRES (8 bytes)",
    [0x0200] = "Subscriber ID for HSS lookup",
    [0x0204] = "Expected Response — MME compares UE's RES with this",
    [0x0205] = "Base key — NAS/RRC keys derived from this (32 bytes)",
}

local MSG_TYPE_NAME = {
    [0x0001] = "S1AP InitialUEMessage         [TS 36.413 §9.1.7.1]",
    [0x0002] = "S1AP DL-NAS-Transport         [TS 36.413 §9.1.7.2]",
    [0x0003] = "S1AP UL-NAS-Transport         [TS 36.413 §9.1.7.3]",
    [0x0004] = "S1AP InitialContextSetupReq   [TS 36.413 §9.1.4.1]",
    [0x0005] = "S1AP InitialContextSetupRsp   [TS 36.413 §9.1.4.2]",
    [0x0101] = "Diameter AIR  [TS 29.272 §5.2.3.1]",
    [0x0102] = "Diameter AIA  [TS 29.272 §5.2.3.2]",
    [0x0201] = "GTP-C CreateSessionReq  [TS 29.274 §7.2.1]",
    [0x0202] = "GTP-C CreateSessionRsp  [TS 29.274 §7.2.2]",
    [0x0203] = "GTP-C ModifyBearerReq   [TS 29.274 §7.2.7]",
    [0x0204] = "GTP-C ModifyBearerRsp   [TS 29.274 §7.2.8]",
    [0x0401] = "Diameter Gx CCR-I  [TS 29.212 §4.5.1]",
    [0x0402] = "Diameter Gx CCA-I  [TS 29.212 §4.5.1]",
}

-- little-endian uint64 → decimal string (Lua number precision ok for 15-digit IMSI)
local function le_u64_str(tvbr)
    local lo = tvbr:range(0,4):le_uint()
    local hi = tvbr:range(4,4):le_uint()
    return string.format("%.0f", hi * 4294967296.0 + lo)
end

function proto.dissector(buffer, pinfo, tree)
    local buflen = buffer:len()

    -- Desegmentation: need 4-byte length prefix
    if buflen < 4 then
        pinfo.desegment_offset = 0; pinfo.desegment_len = DESEGMENT_ONE_MORE_SEGMENT; return
    end
    local plen = buffer(0,4):le_uint()
    if buflen < 4 + plen then
        pinfo.desegment_offset = 0; pinfo.desegment_len = 4 + plen - buflen; return
    end

    pinfo.cols.protocol = "MME-SIM2"

    local root = tree:add(proto, buffer(0, 4 + plen), "MME-Simulator TLV Frame")
    root:add_le(f_frame_len, buffer(0, 4)):append_text(
        "  (TCP framing — real SCTP doesn't need this)")

    -- FrameHeader: msg_type(2) + flags(2) + seq(4) = 8 bytes
    if plen < 8 then return end
    local mtype = buffer(4,2):le_uint()
    local mtype_str = MSG_TYPE_NAME[mtype] or string.format("UNKNOWN(0x%04X)", mtype)
    local hdr = root:add(proto, buffer(4, 8), "FrameHeader")
    hdr:add_le(f_msg_type, buffer(4,2)):append_text("  (" .. mtype_str .. ")")
    hdr:add_le(f_flags,    buffer(6,2))
    hdr:add_le(f_seq,      buffer(8,4))

    -- Update info column
    local seq_val = buffer(8,4):le_uint()
    pinfo.cols.info = string.format("%-35s seq=%d", mtype_str, seq_val)

    -- TLV loop
    local pos = 12  -- after 4-byte prefix + 8-byte header
    while pos + 4 <= 4 + plen do
        local tag = buffer(pos,2):le_uint()
        local vlen = buffer(pos+2,2):le_uint()
        if pos + 4 + vlen > 4 + plen then break end

        local tag_name = TAG_NAME[tag] or string.format("Unknown(0x%04X)", tag)
        local note     = TAG_NOTE[tag] or ""

        local tlv = root:add(proto, buffer(pos, 4+vlen), tag_name)
        tlv:add_le(f_tlv_tag, buffer(pos,   2)):append_text("  (0x" .. string.format("%04X", tag) .. ")")
        tlv:add_le(f_tlv_len, buffer(pos+2, 2))

        -- Value display based on length
        if vlen == 1 then
            local v = buffer(pos+4,1):uint()
            tlv:add(f_tlv_val_u8, buffer(pos+4,1)):append_text(
                string.format("  = 0x%02X (%d)  %s", v, v, note))
        elseif vlen == 2 then
            local v = buffer(pos+4,2):le_uint()
            tlv:add_le(f_tlv_val_u16, buffer(pos+4,2)):append_text(
                string.format("  = %d  %s", v, note))
        elseif vlen == 4 then
            local v = buffer(pos+4,4):le_uint()
            tlv:add_le(f_tlv_val_u32, buffer(pos+4,4)):append_text(
                string.format("  = %d  %s", v, note))
        elseif vlen == 8 then
            local s = le_u64_str(buffer(pos+4, 8))
            tlv:add(f_tlv_val_hex, buffer(pos+4,8)):append_text(
                "  = " .. s .. "  " .. note)
        else
            -- Bytes (RAND, AUTN, XRES, Kasme)
            local hex = ""
            for i = 0, math.min(vlen,4)-1 do
                hex = hex .. string.format("%02X", buffer(pos+4+i,1):uint())
            end
            if vlen > 4 then hex = hex .. "..." end
            tlv:add(f_tlv_val_hex, buffer(pos+4,vlen)):append_text(
                "  = " .. hex .. "  " .. note)
        end

        pos = pos + 4 + vlen
    end
end

-- Register on both ports
local tcp_table = DissectorTable.get("tcp.port")
tcp_table:add(38412, proto)  -- S1AP (eNB ↔ MME)
tcp_table:add(3868,  proto)  -- Diameter (MME ↔ HSS)
