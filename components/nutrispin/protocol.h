#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace esphome::nutrispin::wire {
using Bytes = std::vector<uint8_t>;
using Key = std::array<uint8_t, 16>;
class Crypto {
 public:
  virtual void md5(const uint8_t *p, size_t n, uint8_t out[16]) = 0;
  virtual void cbc(bool encrypt, const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t n) = 0;
  virtual bool random(uint8_t *out, size_t n) = 0;
  virtual ~Crypto() = default;
};
inline uint16_t crc(const uint8_t *p, size_t n) {
  uint16_t c = 0xffff;
  while (n--) { c ^= *p++; for (int b=0;b<8;b++) c = (c>>1) ^ ((c&1) ? 0xa001 : 0); }
  return c;
}
inline void u16(Bytes &b, uint16_t v) { b.push_back(v>>8); b.push_back(v); }
inline void u32(Bytes &b, uint32_t v) { for (int i=3;i>=0;i--) b.push_back(v>>(8*i)); }
inline uint32_t read32(const uint8_t *p) { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; }
inline void var(Bytes &b, uint32_t v) { while (v>=128) { b.push_back((v&127)|128); v>>=7; } b.push_back(v); }
inline bool getvar(const uint8_t *p, size_t n, size_t &pos, uint32_t &out) {
  out=0; for (int shift=0;shift<28;shift+=7) { if (pos>=n) return false; uint8_t c=p[pos++]; out|=uint32_t(c&127)<<shift; if (!(c&128)) return true; } return false;
}
inline std::string hex(const uint8_t *p, size_t n) {
  static const char H[]="0123456789abcdef"; std::string s; s.reserve(n*2);
  while(n--) { uint8_t c=*p++; s+=H[c>>4]; s+=H[c&15]; } return s;
}
inline bool unhex(const std::string &s, uint8_t *out, size_t n) {
  if (s.size()!=n*2) return false;
  auto digit=[](char c)->int { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; };
  for(size_t i=0;i<n;i++) { int a=digit(s[i*2]), b=digit(s[i*2+1]); if(a<0||b<0) return false; out[i]=(a<<4)|b; } return true;
}
struct Frame { uint32_t seq{}, reply{}; uint16_t opcode{}; uint8_t mode{}; Bytes data; };
inline std::vector<Bytes> encode(Crypto &crypto, const Frame &f, const Key &key, uint8_t major) {
  Bytes raw; u32(raw,f.seq); u32(raw,f.reply); u16(raw,f.opcode); u16(raw,f.data.size());
  raw.insert(raw.end(),f.data.begin(),f.data.end()); u16(raw,crc(raw.data(),raw.size()));
  Bytes envelope{f.mode};
  if(f.mode) {
    raw.resize((raw.size()+15)/16*16,0); Key iv;
    if(!crypto.random(iv.data(),16)) return {};
    envelope.insert(envelope.end(),iv.begin(),iv.end()); crypto.cbc(true,key.data(),iv.data(),raw.data(),raw.size());
  }
  envelope.insert(envelope.end(),raw.begin(),raw.end());
  std::vector<Bytes> out; size_t pos=0;
  while(pos<envelope.size()) {
    Bytes packet; var(packet,out.size()); if(out.empty()) { var(packet,envelope.size()); packet.push_back(major<<4); }
    size_t len=std::min(size_t(20)-packet.size(),envelope.size()-pos);
    packet.insert(packet.end(),envelope.begin()+pos,envelope.begin()+pos+len); pos+=len; out.push_back(std::move(packet));
  } return out;
}
inline bool decode(Crypto &crypto, const Bytes &envelope, const Key &login, const Key &session, Frame &f) {
  if(envelope.empty()) return false; f.mode=envelope[0]; Bytes raw;
  if(f.mode==0) raw.assign(envelope.begin()+1,envelope.end());
  else {
    if((f.mode!=4&&f.mode!=5)||envelope.size()<33||(envelope.size()-17)%16) return false;
    raw.assign(envelope.begin()+17,envelope.end());
    crypto.cbc(false,(f.mode==4?login:session).data(),envelope.data()+1,raw.data(),raw.size());
  }
  if(raw.size()<14) return false;
  size_t len=(size_t(raw[10])<<8)|raw[11], end=12+len;
  if(end+2>raw.size()||crc(raw.data(),end)!=((uint16_t(raw[end])<<8)|raw[end+1])) return false;
  f.seq=read32(raw.data()); f.reply=read32(raw.data()+4); f.opcode=(uint16_t(raw[8])<<8)|raw[9];
  f.data.assign(raw.begin()+12,raw.begin()+end); return true;
}
class Receiver {
 public:
  enum Result { INCOMPLETE, COMPLETE, INVALID };
  Result feed(const uint8_t *p, size_t n) {
    uint32_t number; size_t pos=0;
    if(!getvar(p,n,pos,number)) return reset_invalid();
    if(number==0) { if(!getvar(p,n,pos,length_)||pos>=n||!length_||length_>4096) return reset_invalid(); pos++; expected_=0; data.clear(); }
    if(number!=expected_||!length_||data.size()+n-pos>length_) return reset_invalid();
    expected_++; data.insert(data.end(),p+pos,p+n);
    if(data.size()==length_) { length_=0; return COMPLETE; } return INCOMPLETE;
  }
  void reset() { data.clear(); length_=expected_=0; }
  Bytes data;
 private:
  Result reset_invalid() { reset(); return INVALID; }
  uint32_t length_{}, expected_{};
};
struct Identity { std::array<uint8_t,16> uuid{}; bool bound{}; std::string product; };
inline bool identity(Crypto &crypto, const Bytes &service, const Bytes &manufacturer, Identity &out) {
  if(service.size()<5||(service[0]&0xc1)!=0x41||service[2]!=0||service[3]>16||!service[3]||service.size()!=4+service[3]) return false;
  if(manufacturer.size()!=20||manufacturer[0]!=0||manufacturer[3]!=0) return false;
  Key key; crypto.md5(service.data(),service.size(),key.data());
  std::copy(manufacturer.begin()+4,manufacturer.end(),out.uuid.begin());
  crypto.cbc(false,key.data(),key.data(),out.uuid.data(),16);
  for(auto c:out.uuid) if(c<33||c>126) return false;
  out.bound=service[0]&8; out.product=out.bound?"":std::string(service.begin()+4,service.end()); return true;
}
struct Point { uint8_t id{}, type{}; Bytes value; };
inline bool points(const Bytes &b, size_t pos, uint8_t width, std::vector<Point> &out) {
  out.clear(); while(pos<b.size()) {
    if(pos+2+width>b.size()) return false;
    Point p; p.id=b[pos++]; p.type=b[pos++]; if(p.type>5) return false;
    size_t n=0; for(uint8_t i=0;i<width;i++) n=(n<<8)|b[pos++];
    if(pos+n>b.size()) return false;
    p.value.assign(b.begin()+pos,b.begin()+pos+n); pos+=n; out.push_back(std::move(p));
  } return true;
}
inline bool valid_schedule(const Bytes &b) {
  if(b.size()!=25) return false;
  for(size_t i=0;i<25;i+=5) if(b[i]>127||b[i+1]>23||b[i+2]>59||b[i+4]>1) return false;
  return true;
}
struct HistoryRecord { uint8_t code{}; uint32_t timestamp{}; };
inline std::vector<HistoryRecord> history(const Bytes &b) {
  std::vector<HistoryRecord> out; if(b.size()%5) return out;
  for(size_t i=0;i<b.size();i+=5) {
    uint32_t ts=uint32_t(b[i+1])|(uint32_t(b[i+2])<<8)|(uint32_t(b[i+3])<<16)|(uint32_t(b[i+4])<<24);
    if(ts) out.push_back({b[i],ts});
  } return out;
}
}  // namespace esphome::nutrispin::wire
