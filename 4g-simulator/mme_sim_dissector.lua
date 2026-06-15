-- MME-IMS Simulator Lua Dissector
-- 3GPP Simulator Helper Dissector
-- Save this as mme_sim_dissector.lua and add to Wireshark Plugins folder

mme_sim_proto = Proto("3GPP-Helper", "3GPP Simulator Decoder")

local diameter_dissector = Dissector.get("diameter")
local sip_dissector      = Dissector.get("sip")

function mme_sim_proto.dissector(buffer, pinfo, tree)
    length = buffer:len()
    if length < 4 then return end

    -- Diameter (S6a port 3868 + Gx port 3869): our PCAP writer already emits
    -- a real RFC 6733 header (Version/Length/Flags/Command-Code/App-ID/H2H/E2E),
    -- so hand off to Wireshark's real "diameter" dissector instead of faking
    -- the column text.
    if pinfo.src_port == 3868 or pinfo.dst_port == 3868 or
       pinfo.src_port == 3869 or pinfo.dst_port == 3869 then
        diameter_dissector:call(buffer, pinfo, tree)
        return
    end

    -- SIP (P-CSCF/S-CSCF, ports 5060/5070): our PCAP writer emits real SIP
    -- text, so hand off to Wireshark's real "sip" dissector.
    if pinfo.src_port == 5060 or pinfo.dst_port == 5060 or
       pinfo.src_port == 5070 or pinfo.dst_port == 5070 then
        sip_dissector:call(buffer, pinfo, tree)
        return
    end

    -- S1AP (eNB <-> MME, SCTP PPI 18) is now real ASN.1 Aligned PER, dissected
    -- natively by Wireshark's built-in "s1ap" dissector (registered for PPI 18
    -- by default — we deliberately do NOT register on sctp.ppi 18 below so we
    -- don't shadow it).
end

-- Register on all simulator ports
local tcp_port = DissectorTable.get("tcp.port")

tcp_port:add(3868, mme_sim_proto) -- Diameter S6a (delegated to real "diameter" dissector above)
tcp_port:add(3869, mme_sim_proto) -- Diameter Gx  (delegated to real "diameter" dissector above)
tcp_port:add(5060, mme_sim_proto) -- SIP P-CSCF   (delegated to real "sip" dissector above)
tcp_port:add(5070, mme_sim_proto) -- SIP S-CSCF   (delegated to real "sip" dissector above)
