#!/usr/bin/env python3
"""Build a stock-TYT-compatible AES-256-ECB encrypted DMR SMS (IPv4/UDP/TMS) burst frame.

Scheme (RE'd + bench-validated 2026-06-27): the DMR data/SMS payload is AES-256-ECB — each
16-byte block of the reassembled rate-1/2 payload is independently AES-encrypted with the key
(no IV / no chaining; the header MI is unused for data). A stock TYT with the same key decrypts
it. Framing CRCs (header CCITT16d^0xCCCC, CSBK ^0xA5A5, data-PDU CRC32) reproduced byte-exact
from off-air captures.
"""
import struct

# ---------------- AES-256 (ECB encrypt) ----------------
_SBOX=bytes.fromhex(
"637c777bf26b6fc53001672bfed7ab76ca82c97dfa5947f0add4a2af9ca472c0"
"b7fd9326363ff7cc34a5e5f171d8311504c723c31896059a071280e2eb27b275"
"09832c1a1b6e5aa0523bd6b329e32f8453d100ed20fcb15b6acbbe394a4c58cf"
"d0efaafb434d338545f9027f503c9fa851a3408f929d38f5bcb6da2110fff3d2"
"cd0c13ec5f974417c4a77e3d645d197360814fdc222a908846eeb814de5e0bdb"
"e0323a0a4906245cc2d3ac629195e479e7c8376d8dd54ea96c56f4ea657aae08"
"ba78252e1ca6b4c6e8dd741f4bbd8b8a703eb5664803f60e613557b986c11d9e"
"e1f8981169d98e949b1e87e9ce5528df8ca1890dbfe6426841992d0fb054bb16")
_INV=bytes(_SBOX.index(i) for i in range(256))
def _mul(a,b):
    r=0
    for _ in range(8):
        if b&1:r^=a
        hi=a&0x80;a=(a<<1)&0xff
        if hi:a^=0x1b
        b>>=1
    return r
_RCON=[1,2,4,8,16,32,64,128,0x1b,0x36,0x6c,0xd8,0xab,0x4d]
def _expand(key):
    w=[list(key[4*i:4*i+4]) for i in range(8)]
    for i in range(8,60):
        t=list(w[i-1])
        if i%8==0:t=t[1:]+t[:1];t=[_SBOX[x] for x in t];t[0]^=_RCON[i//8-1]
        elif i%8==4:t=[_SBOX[x] for x in t]
        w.append([w[i-8][j]^t[j] for j in range(4)])
    return w
def aes256_ecb_block(pt,key,decrypt=False):
    w=_expand(key);st=[[pt[r+4*c] for c in range(4)] for r in range(4)]
    def ark(rd):
        for c in range(4):
            for r in range(4):st[r][c]^=w[rd*4+c][r]
    if not decrypt:
        ark(0)
        for rd in range(1,14):
            for r in range(4):
                for c in range(4):st[r][c]=_SBOX[st[r][c]]
            st=[st[r][r:]+st[r][:r] for r in range(4)]
            ns=[[0]*4 for _ in range(4)]
            for c in range(4):
                col=[st[r][c] for r in range(4)]
                ns[0][c]=_mul(col[0],2)^_mul(col[1],3)^col[2]^col[3]
                ns[1][c]=col[0]^_mul(col[1],2)^_mul(col[2],3)^col[3]
                ns[2][c]=col[0]^col[1]^_mul(col[2],2)^_mul(col[3],3)
                ns[3][c]=_mul(col[0],3)^col[1]^col[2]^_mul(col[3],2)
            st=ns;ark(rd)
        for r in range(4):
            for c in range(4):st[r][c]=_SBOX[st[r][c]]
        st=[st[r][r:]+st[r][:r] for r in range(4)];ark(14)
    else:
        ark(14)
        for rd in range(13,0,-1):
            st=[st[r][-r:]+st[r][:-r] for r in range(4)]
            for r in range(4):
                for c in range(4):st[r][c]=_INV[st[r][c]]
            ark(rd)
            ns=[[0]*4 for _ in range(4)]
            for c in range(4):
                col=[st[r][c] for r in range(4)]
                ns[0][c]=_mul(col[0],14)^_mul(col[1],11)^_mul(col[2],13)^_mul(col[3],9)
                ns[1][c]=_mul(col[0],9)^_mul(col[1],14)^_mul(col[2],11)^_mul(col[3],13)
                ns[2][c]=_mul(col[0],13)^_mul(col[1],9)^_mul(col[2],14)^_mul(col[3],11)
                ns[3][c]=_mul(col[0],11)^_mul(col[1],13)^_mul(col[2],9)^_mul(col[3],14)
            st=ns
        st=[st[r][-r:]+st[r][:-r] for r in range(4)]
        for r in range(4):
            for c in range(4):st[r][c]=_INV[st[r][c]]
        ark(0)
    return bytes(st[r][c] for c in range(4) for r in range(4))
def aes256_ecb(data,key,decrypt=False):
    return b"".join(aes256_ecb_block(data[i:i+16],key,decrypt) for i in range(0,len(data),16))

# ---------------- checksums / CRCs ----------------
def _ipcksum(b):
    s=0
    for i in range(0,len(b),2): s+=(b[i]<<8)|(b[i+1] if i+1<len(b) else 0)
    while s>>16: s=(s&0xFFFF)+(s>>16)
    return (~s)&0xFFFF
def _udpcksum(src_ip,dst_ip,udp):
    b=src_ip+dst_ip+bytes([0,0x11,(len(udp)>>8)&0xFF,len(udp)&0xFF])+udp
    if len(b)%2: b+=b"\x00"
    c=_ipcksum(b); return c or 0xFFFF
def _crc16d(data):
    crc=0
    for byte in data:
        for k in range(7,-1,-1):
            bit=(byte>>k)&1
            crc=(((crc<<1)^0x1021)&0xFFFF) if (((crc>>15)&1)^bit) else ((crc<<1)&0xFFFF)
    return crc^0xFFFF
def _hdr_crc(h,mask):
    v=_crc16d(h)^mask; return bytes([(v>>8)&0xFF,v&0xFF])
def _crc32_dmr(pdu):  # pdu incl 4-byte placeholder; byte-pair swap, poly 0x04C11DB7, over len*8-32
    bits=[]
    for i in range(0,len(pdu)-1,2):
        for k in range(7,-1,-1): bits.append((pdu[i+1]>>k)&1)
        for k in range(7,-1,-1): bits.append((pdu[i]>>k)&1)
    n=len(pdu)*8-32; CRC=0;poly=0x04C11DB7
    for i in range(n):
        if ((CRC>>31)&1)^(bits[i]&1): CRC=((CRC<<1)^poly)&0xFFFFFFFF
        else: CRC=(CRC<<1)&0xFFFFFFFF
    a=(CRC&0xFF)<<24;b=((CRC&0xFF00)>>8)<<16;c=((CRC&0xFF0000)>>16)<<8;d=(CRC&0xFF000000)>>24
    return ((a+b+c+d)&0xFFFFFFFF).to_bytes(4,'big')

# ---------------- IPv4/UDP/TMS plaintext ----------------
def build_tms_plaintext(text, src, dst, seq=0, ipid=0):
    tb=text.encode('utf-16-le'); L=len(tb)
    tms=bytes([0x00,8+L, 0xA0,0x00,seq&0xFF,0x04, (L+3)&0xFF,0x00, L&0xFF,0x00])+tb
    udp=bytes([0x0F,0xA7,0x0F,0xA7,((8+len(tms))>>8)&0xFF,(8+len(tms))&0xFF,0,0])+tms
    src_ip=bytes([0x0C,0x00,(src>>8)&0xFF,src&0xFF]); dst_ip=bytes([0xE1,0x00,(dst>>8)&0xFF,dst&0xFF])
    uc=_udpcksum(src_ip,dst_ip,udp); udp=udp[:6]+bytes([(uc>>8)&0xFF,uc&0xFF])+udp[8:]
    tot=20+len(udp)
    ip=bytes([0x45,0,(tot>>8)&0xFF,tot&0xFF,(ipid>>8)&0xFF,ipid&0xFF,0,0,0x40,0x11,0,0])+src_ip+dst_ip
    ic=_ipcksum(ip); ip=ip[:10]+bytes([(ic>>8)&0xFF,ic&0xFF])+ip[12:]
    return ip+udp

# ---------------- full burst frame ----------------
DT_CSBK,DT_DATA_HEADER,DT_RATE12_DATA=0x30,0x60,0x70
def _csbk_preamble(dst,src,group,count,tail=0):
    # countdown byte = frames remaining AFTER this CSBK (= remaining CSBKs + tail),
    # matching stock radios (e.g. 6 CSBK + 2 hdr + 5 blocks -> first countdown 0x0C).
    out=[]
    for i in range(count):
        body=bytes([0xBD,0x00,0xC0 if group else 0x80,((count-1-i)+tail)&0xFF,
                    (dst>>16)&0xFF,(dst>>8)&0xFF,dst&0xFF,(src>>16)&0xFF,(src>>8)&0xFF,src&0xFF])
        out.append(bytes([DT_CSBK])+body+_hdr_crc(body,0xA5A5))
    return out
def build_encrypted_sms(text, dst, src, key, group=True, mi=0, seq=0, ipid=0, preamble=6):
    """Return list of (type+12B) bursts for a stock-compatible AES-256-ECB SMS."""
    pt=build_tms_plaintext(text,src,dst,seq,ipid)
    if len(pt)%16: pt=pt+bytes(16-len(pt)%16)            # pad plaintext to AES block
    ct=aes256_ecb(pt,key)                                 # ECB encrypt
    # data blocks after ENC ext header = ct split into 12B + final (pad8 + CRC32) block
    nfull=len(ct)//12; rem=len(ct)-nfull*12
    # pad ct so (ct + 4-byte CRC) fills whole 12B blocks
    data_no_crc=ct
    total_data=(((len(data_no_crc)+4)+11)//12)*12         # round up to 12
    poc=total_data-len(data_no_crc)-4                      # pad octets
    pdu=data_no_crc+bytes(poc)+bytes(4)                    # placeholder CRC
    crc=_crc32_dmr(pdu)
    pdu=data_no_crc+bytes(poc)+crc
    data_blocks=[pdu[i:i+12] for i in range(0,len(pdu),12)]
    nblocks=1+len(data_blocks)                             # +1 ENC ext header
    # Unconfirmed Delivery data header (SAP 09 EXTD HDR)
    g=0x80 if group else 0x00
    h=bytes([g|0x02, (9<<4)|(poc&0x0F),
             (dst>>16)&0xFF,(dst>>8)&0xFF,dst&0xFF,(src>>16)&0xFF,(src>>8)&0xFF,src&0xFF,
             0x80|(nblocks&0x7F), 0x00])
    hdr1=bytes([DT_DATA_HEADER])+h+_hdr_crc(h,0xCCCC)
    # Extended DMRA ENC header (SAP04 IP, MFID Moto, ALG05 AES256)
    e=bytes([0x4F,0x10,0x51,0x01,0x00,0x00,(mi>>24)&0xFF,(mi>>16)&0xFF,(mi>>8)&0xFF,mi&0xFF])
    hdr2=bytes([DT_DATA_HEADER])+e+_hdr_crc(e,0xCCCC)
    tail=2+len(data_blocks)                               # 2 headers + data blocks follow CSBKs
    bursts=_csbk_preamble(dst,src,group,preamble,tail)+[hdr1,hdr2]
    bursts+=[bytes([DT_RATE12_DATA])+b for b in data_blocks]
    return bursts

if __name__=="__main__":
    KEY=bytes.fromhex('93A5CF3BDAB558BCF61ECA5732A8657832396F678150E17811EAA7491F94B3EE')
    b=build_encrypted_sms("Hello",9661,12341,KEY,group=True,mi=0xAB5C8269,seq=0x90,ipid=2,preamble=6)
    print("built %d bursts:"%len(b))
    for x in b: print("  type 0x%02X  %s"%(x[0],x[1:].hex()))
    # validate the encrypted blocks decrypt back to Hello
    ct=b"".join(x[1:] for x in b if x[0]==DT_RATE12_DATA)[:48]
    pt=aes256_ecb(ct,KEY,decrypt=True)
    print("decrypt of own ct -> ends:",pt[-12:].hex(),"(0a00 48 00 65 00... = Hello)")
