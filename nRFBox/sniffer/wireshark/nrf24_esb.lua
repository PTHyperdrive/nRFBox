-- Wireshark dissector for the nRF52840 ESB / nRF24 sniffer.
--
-- Registers on DLT_USER0 (wtap encap 45), which is what the extcap plugin
-- writes. Frame layout must stay in step with PacketHeader in the firmware:
--
--   0  channel      1 byte    2400 + n MHz
--   1  rate         1 byte    0 = 2Mbit, 1 = 1Mbit
--   2  rssi         1 byte    magnitude, displayed negated
--   3  flags        1 byte    b0 crc_ok, b1 promiscuous
--   4  addr_len     1 byte    3..5
--   5  pid          1 byte    0..3
--   6  no_ack       1 byte    0/1
--   7  payload_len  1 byte    0..32
--   8  address      addr_len bytes, MSB first
--      payload      payload_len bytes
--
-- Install: copy to the Wireshark "personal Lua plugins" directory
-- (Help > About Wireshark > Folders), then Analyze > Reload Lua Plugins.

local p_esb = Proto("nrf24esb", "nRF24 / Enhanced ShockBurst")

local RATES = { [0] = "2 Mbit", [1] = "1 Mbit" }

local f_channel = ProtoField.uint8("nrf24esb.channel", "Channel", base.DEC)
local f_freq    = ProtoField.uint16("nrf24esb.freq", "Frequency (MHz)", base.DEC)
local f_rate    = ProtoField.uint8("nrf24esb.rate", "Data rate", base.DEC, RATES)
local f_rssi    = ProtoField.int16("nrf24esb.rssi", "RSSI (dBm)", base.DEC)
local f_crcok   = ProtoField.bool("nrf24esb.crc_ok", "CRC valid")
local f_promisc = ProtoField.bool("nrf24esb.promiscuous", "Promiscuous capture")
local f_addr    = ProtoField.bytes("nrf24esb.address", "Pipe address")
local f_pid     = ProtoField.uint8("nrf24esb.pid", "PID", base.DEC)
local f_noack   = ProtoField.bool("nrf24esb.no_ack", "NO_ACK")
local f_plen    = ProtoField.uint8("nrf24esb.payload_len", "Payload length", base.DEC)
local f_payload = ProtoField.bytes("nrf24esb.payload", "Payload")

p_esb.fields = {
  f_channel, f_freq, f_rate, f_rssi, f_crcok, f_promisc,
  f_addr, f_pid, f_noack, f_plen, f_payload,
}

local ef_crc = ProtoExpert.new("nrf24esb.crc_bad", "CRC check failed",
                               expert.group.CHECKSUM, expert.severity.WARN)
local ef_promisc = ProtoExpert.new("nrf24esb.promisc_guess",
                                   "Promiscuous capture - framing is a guess",
                                   expert.group.UNDECODED, expert.severity.NOTE)
p_esb.experts = { ef_crc, ef_promisc }

function p_esb.dissector(buf, pinfo, tree)
  if buf:len() < 8 then return 0 end

  pinfo.cols.protocol = "nRF24/ESB"

  local channel  = buf(0, 1):uint()
  local rate     = buf(1, 1):uint()
  local rssi     = -buf(2, 1):uint()
  local flags    = buf(3, 1):uint()
  local addr_len = buf(4, 1):uint()
  local pid      = buf(5, 1):uint()
  local no_ack   = buf(6, 1):uint()
  local plen     = buf(7, 1):uint()

  local crc_ok  = bit.band(flags, 0x01) ~= 0
  local promisc = bit.band(flags, 0x02) ~= 0

  -- Guard against a truncated or corrupt frame rather than indexing past the end.
  if buf:len() < 8 + addr_len then return 0 end
  local avail = buf:len() - 8 - addr_len
  if plen > avail then plen = avail end

  local t = tree:add(p_esb, buf(), "nRF24 / Enhanced ShockBurst")

  t:add(f_channel, buf(0, 1))
  t:add(f_freq, buf(0, 1), 2400 + channel):set_generated()
  t:add(f_rate, buf(1, 1))
  t:add(f_rssi, buf(2, 1), rssi)
  t:add(f_crcok, buf(3, 1), crc_ok)
  t:add(f_promisc, buf(3, 1), promisc)

  local addr = buf(8, addr_len)
  t:add(f_addr, addr)
  t:add(f_pid, buf(5, 1))
  t:add(f_noack, buf(6, 1), no_ack ~= 0)
  t:add(f_plen, buf(7, 1))

  if plen > 0 then
    t:add(f_payload, buf(8 + addr_len, plen))
  end

  if not crc_ok and not promisc then t:add_proto_expert_info(ef_crc) end
  if promisc then t:add_proto_expert_info(ef_promisc) end

  -- An empty payload with NO_ACK clear is how ESB acknowledgements look.
  local kind = (plen == 0) and "ACK" or string.format("DATA len=%d", plen)

  -- CRC is switched off in promiscuous mode, so "CRC BAD" would be a lie there.
  local note = ""
  if promisc then      note = "  [promisc, unverified]"
  elseif not crc_ok then note = "  [CRC BAD]" end

  pinfo.cols.info = string.format("ch %d (%d MHz)  %s  addr %s  PID %d  %d dBm%s",
                                  channel, 2400 + channel, kind,
                                  tostring(addr:bytes()), pid, rssi, note)
  return buf:len()
end

-- DLT_USER0 -> wtap encap 45
local wtap_encap = DissectorTable.get("wtap_encap")
wtap_encap:add(wtap.USER0, p_esb)
