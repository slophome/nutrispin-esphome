#include "nutrispin.h"
#include "esphome/core/log.h"
#include "esphome/components/md5/md5.h"
#include "esphome/components/json/json_util.h"
#include <bearssl/bearssl.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace esphome::nutrispin {
using namespace wire;
static const char *const TAG="nutrispin";
static constexpr uint32_t MAGIC=0x4e535031;
static constexpr uint32_t SLOT_MAGIC=0x4e535032;
std::vector<NutriSpin *> NutriSpin::instances_;
NutriSpin *NutriSpin::pairing_owner_=nullptr;
static const char *const LANGUAGES[]={"Simplified Chinese","Traditional Chinese","English","Japanese","Korean"};
void FeederButton::press_action() { owner_->action(op_); }
void FeederSwitch::write_state(bool v) { owner_->switch_control(op_,meal_,v); }
void FeederNumber::control(float v) { owner_->number_control(op_,meal_,v); }
void FeederText::control(const std::string &v) { owner_->text_control(op_,meal_,v); }
void FeederSelect::control(const std::string &v) { if(pairing_) owner_->pairing_control(v); else owner_->language_control(v); }
void PicoCrypto::md5(const uint8_t *p,size_t n,uint8_t out[16]) { md5::MD5Digest d; d.init(); d.add(p,n); d.calculate(); d.get_bytes(out); }
void PicoCrypto::cbc(bool enc,const uint8_t key[16],const uint8_t iv[16],uint8_t *data,size_t n) {
  uint8_t mutable_iv[16]; memcpy(mutable_iv,iv,16);
  if(enc) { br_aes_ct_cbcenc_keys ctx; br_aes_ct_cbcenc_init(&ctx,key,16); br_aes_ct_cbcenc_run(&ctx,mutable_iv,data,n); }
  else { br_aes_ct_cbcdec_keys ctx; br_aes_ct_cbcdec_init(&ctx,key,16); br_aes_ct_cbcdec_run(&ctx,mutable_iv,data,n); }
}
bool PicoCrypto::random(uint8_t *p,size_t n) { return random_bytes(p,n); }
static std::string mac_string(uint64_t mac) { uint8_t b[6]; ble_device_base::uint64_to_mac_msb_first(mac,b); char out[18]; snprintf(out,sizeof(out),"%02X:%02X:%02X:%02X:%02X:%02X",b[0],b[1],b[2],b[3],b[4],b[5]); return out; }
static std::string timestamp(uint32_t ts) { time_t t=ts; struct tm parts{}; gmtime_r(&t,&parts); char out[24]; strftime(out,sizeof(out),"%Y-%m-%dT%H:%M:%SZ",&parts); return out; }
void NutriSpin::status_(const char *s) { texts_[0]->publish_state(s); }
bool NutriSpin::save_() { SlotRecord saved{SLOT_MAGIC,mac_,creds_}; return pref_.save(&saved) && global_preferences->sync(); }
bool NutriSpin::import_(const std::string &s) {
  if(s.empty()) return false;
  auto doc=json::parse_json(s);
  if(doc.isNull()||std::string(doc["model"]|"")!="PF006"||std::string(doc["product_id"]|"")!="66d1tpmo") return false;
  std::string configured=doc["mac"]|"";
  std::transform(configured.begin(),configured.end(),configured.begin(),[](unsigned char c){ return std::toupper(c); });
  std::string compact; for(auto c:configured) if(c!=':') compact+=c;
  uint8_t address[6]; if(!unhex(compact,address,6)) return false;
  uint64_t imported_mac=0; for(auto c:address) imported_mac=(imported_mac<<8)|c;
  if(!imported_mac||(configured_mac_&&imported_mac!=configured_mac_)||claimed_by_other_(imported_mac)) return false;
  std::string uuid=doc["uuid"]|"", vid=doc["device_id"]|"", key=doc["local_key_hex"]|"";
  if(uuid.size()!=16||vid.empty()||vid.size()>22) return false;
  Credentials c{}; c.magic=MAGIC; c.stage=2; memcpy(c.uuid,uuid.data(),16); memcpy(c.virtual_id,vid.data(),vid.size());
  if(!key.empty()) { if(!unhex(key,c.login,6)) return false; }
  else { key=doc["local_key"]|""; if(key.size()<6) return false; memcpy(c.login,key.data(),6); }
  std::string beacon=doc["beacon_key_hex"]|""; if(!beacon.empty()&&!unhex(beacon,c.beacon,16)) return false;
  creds_=c; mac_=imported_mac; return save_();
}
void NutriSpin::setup() {
  instances_.push_back(this);
  pref_=global_preferences->make_preference<SlotRecord>(slot_key_);
  SlotRecord saved{};
  bool loaded=pref_.load(&saved)&&saved.magic==SLOT_MAGIC&&saved.mac&&saved.credentials.magic==MAGIC
              &&(saved.credentials.stage==1||saved.credentials.stage==2);
  if(loaded) {
    if(configured_mac_&&configured_mac_!=saved.mac) { status_("Saved feeder differs from configured address"); mark_failed(); return; }
    mac_=saved.mac; creds_=saved.credentials;
  } else if(configured_mac_) {
    // Import preferences from the former address-based configuration once.
    uint32_t hash=2166136261u; for(int i=0;i<8;i++) hash=(hash^uint8_t(mac_>>(i*8)))*16777619u;
    auto legacy=global_preferences->make_preference<Credentials>(hash^MAGIC);
    if(legacy.load(&creds_)&&creds_.magic==MAGIC&&(creds_.stage==1||creds_.stage==2)) loaded=save_();
    else creds_={};
  }
  if(!loaded&&!initial_.empty()&&!import_(initial_)) { status_("Invalid credentials or key storage failure"); mark_failed(); return; }
  // Do not keep a second heap copy of credentials or print them.
  std::fill(initial_.begin(),initial_.end(),'\0'); initial_.clear(); initial_.shrink_to_fit();
  if(mac_&&claimed_by_other_(mac_)) { status_("Feeder already assigned to another slot"); mark_failed(); return; }
  texts_[1]->publish_state(mac_?mac_string(mac_):"Not assigned"); binaries_[0]->publish_state(false); binaries_[1]->publish_state(creds_.stage==2);
  pairing_select_->publish_state("Automatic"); pairing_devices_->publish_state("Press Pair to scan");
  status_(creds_.stage ? "Waiting for feeder" : "Factory-reset the feeder and press Pair");
}
void NutriSpin::dump_config() { ESP_LOGCONFIG(TAG,"%s: PF006 %s; automatic discovery",label_.c_str(),mac_?mac_string(mac_).c_str():"not assigned"); }
bool NutriSpin::claimed_by_other_(uint64_t mac) const {
  for(auto *slot:instances_) if(slot!=this&&!slot->is_failed()&&slot->mac_==mac) return true;
  return false;
}
void NutriSpin::release_pairing_() {
  pair_requested_=discovering_pair_=choosing_pair_=false;
  if(pairing_owner_==this) pairing_owner_=nullptr;
}
void NutriSpin::begin_pairing_() {
  if(pairing_owner_&&pairing_owner_!=this) { status_("Another slot is pairing - wait for it to finish"); return; }
  if(pair_requested_&&!discovering_pair_) { status_("Pairing already in progress"); return; }
  pairing_owner_=this; pair_requested_=discovering_pair_=true; choosing_pair_=false;
  discovery_.clear(); candidates_.clear(); discovery_check_=millis()+5000; pair_deadline_=millis()+60000;
  pairing_select_->publish_state("Automatic"); pairing_devices_->publish_state("Scanning for factory-reset PF006 feeders");
  status_("Searching - factory-reset the feeder to add");
  if(state_!=IDLE) close_(500);
}
void NutriSpin::finish_discovery_() {
  if(state_!=IDLE) { discovery_check_=millis()+500; return; }
  auto found=discovery_.available(millis()); candidates_.clear();
  for(const auto &candidate:found)
    if(!claimed_by_other_(candidate.mac)&&(!configured_mac_||configured_mac_==candidate.mac)) candidates_.push_back(candidate);
  if(candidates_.empty()) { discovery_check_=millis()+2000; return; }
  if(candidates_.size()>6) candidates_.resize(6);
  std::string listing;
  for(size_t i=0;i<candidates_.size();i++) {
    if(i) listing+="; ";
    listing+=std::to_string(i+1)+": "+mac_string(candidates_[i].mac)+" "+std::to_string(candidates_[i].rssi)+"dBm";
  }
  pairing_devices_->publish_state(listing);
  choosing_pair_=true;
  if(candidates_.size()==1) choose_candidate_(0);
  else status_("Several feeders found - choose a Pairing Device");
}
void NutriSpin::choose_candidate_(size_t index) {
  if(!pair_requested_||!discovering_pair_||!choosing_pair_||index>=candidates_.size()) { status_("No matching discovery result - press Pair to scan"); return; }
  auto current=discovery_.available(millis());
  auto it=std::find_if(current.begin(),current.end(),[this,index](const PairingCandidate &c){ return c.mac==candidates_[index].mac; });
  if(it==current.end()) { status_("Selected feeder is no longer available - scan again"); return; }
  if(claimed_by_other_(it->mac)) { status_("Selected feeder is assigned to another slot"); return; }
  auto previous=creds_; uint64_t previous_mac=mac_;
  mac_=it->mac;
  if(!generate_(it->identity)) { mac_=previous_mac; creds_=previous; release_pairing_(); return; }
  // Persist the complete slot assignment before any pairing bytes can leave.
  if(!save_()) { mac_=previous_mac; creds_=previous; release_pairing_(); status_("Unable to save feeder assignment"); return; }
  discovering_pair_=choosing_pair_=false;
  service_.clear(); manufacturer_.clear(); texts_[1]->publish_state(mac_string(mac_));
  binaries_[1]->publish_state(false); pairing_select_->publish_state("Device "+std::to_string(index+1));
  connect_(it->identity,it->address_type);
}
void NutriSpin::pairing_control(const std::string &value) {
  if(value=="Automatic") { pairing_select_->publish_state(value); return; }
  if(value.size()==8&&value.compare(0,7,"Device ")==0&&value[7]>='1'&&value[7]<='6') choose_candidate_(value[7]-'1');
}
bool NutriSpin::generate_(const Identity &id) {
  if(creds_.stage==1&&memcmp(creds_.uuid,id.uuid.data(),16)==0) return true;
  Credentials c{}; c.magic=MAGIC; c.stage=1; memcpy(c.uuid,id.uuid.data(),16);
  uint8_t vid[11]; if(!crypto_.random(c.login,6)||!crypto_.random(c.beacon,16)||!crypto_.random(vid,11)) { status_("Secure random generation failed"); return false; }
  auto encoded=hex(vid,11); memcpy(c.virtual_id,encoded.data(),22); creds_=c;
  return true;
}
bool NutriSpin::parse_device(const ble_device_base::ESPBTDevice &device) {
  if(is_failed()) return false;
  uint64_t address=device.address_uint64();
  if(address!=mac_&&!discovering_pair_) return false;
  Bytes service,manufacturer;
  for(const auto &s:device.get_service_datas()) if(s.uuid==ble_device_base::ESPBTUUID::from_uint16(0xfd50)) service.assign(s.data.begin(),s.data.end());
  for(const auto &s:device.get_manufacturer_datas()) if(s.uuid==ble_device_base::ESPBTUUID::from_uint16(0x07d0)) manufacturer.assign(s.data.begin(),s.data.end());
  if(discovering_pair_) {
    discovery_.observe(crypto_,address,device.get_address_type(),device.get_rssi(),service,manufacturer,millis());
    return true;
  }
  if(address!=mac_) return false;
  sensors_[1]->publish_state(device.get_rssi()); address_type_=device.get_address_type();
  if(!service.empty()) service_=service;
  if(!manufacturer.empty()) manufacturer_=manufacturer;
  Identity id; if(!identity(crypto_,service_,manufacturer_,id)) return true;
  if(state_!=IDLE||int32_t(millis()-next_connect_)<0) return true;
  connect_(id,address_type_);
  return true;
}
void NutriSpin::connect_(const Identity &id,uint8_t address_type) {
  address_type_=address_type;
  if(!id.bound) {
    binaries_[1]->publish_state(false);
    if(id.product!="66d1tpmo") { status_("Unsupported model"); return; }
    if(!pair_requested_&&creds_.stage!=1) { status_("Factory reset detected - press Pair"); return; }
    if(!generate_(id)||!save_()) { release_pairing_(); status_("Unable to save pending pairing credentials"); return; }
    pairing_connection_=true; status_("Pairing locally");
  } else {
    if(!creds_.stage) { status_("Feeder already paired - factory reset required"); return; }
    if(memcmp(creds_.uuid,id.uuid.data(),16)!=0) { status_("Factory UUID mismatch - connection stopped"); return; }
    pairing_connection_=false; status_("Connecting");
  }
  state_=CONNECTING; state_deadline_=millis()+45000;
  if(backend_->connect(mac_,address_type_)!=0) fail_("Unable to start connection");
}
void NutriSpin::close_(uint32_t delay) {
  next_connect_=millis()+delay; state_=CLOSING; state_deadline_=millis()+15000;
  commands_.clear(); outgoing_.clear(); outgoing_index_=0; await_reply_=write_busy_=cccd_busy_=false;
  // A deferred time response belongs only to the session that requested it.
  time_request_op_=0; time_request_seq_=0; clock_synced_=false;
  receiver_.reset(); if(backend_->gatt_disconnect()!=0) state_=IDLE;
  binaries_[0]->publish_state(false);
}
void NutriSpin::fail_(const char *s) { release_pairing_(); status_(s); ESP_LOGW(TAG,"%s: %s",label_.c_str(),s); close_(15000); }
void NutriSpin::on_connection_state(bool connected,uint16_t mtu,int error) {
  if(!connected) {
    if(state_!=CLOSING) status_("Disconnected - reconnecting");
    state_=IDLE; commands_.clear(); outgoing_.clear(); receiver_.reset(); await_reply_=write_busy_=cccd_busy_=false;
    outgoing_index_=0; time_request_op_=0; time_request_seq_=0; clock_synced_=false;
    schedule_pending_=false; schedule_known_=false; binaries_[0]->publish_state(false); next_connect_=millis()+3000; return;
  }
  mtu_=mtu; sequence_=0; receiver_.reset(); write_handle_=notify_handle_=cccd_handle_=0; clock_synced_=false;
  time_request_op_=0; time_request_seq_=0;
  state_=DISCOVERING; state_deadline_=millis()+20000;
  if(backend_->discover_services()!=0) fail_("Unable to discover BLE services");
}
void NutriSpin::on_service_discovery_done(int error) {
  if(error) { fail_("BLE service discovery failed"); return; }
  auto table=backend_->get_service_table();
  auto write_uuid=ble_device_base::ESPBTUUID::from_raw("00000001-0000-1001-8001-00805f9b07d0");
  auto notify_uuid=ble_device_base::ESPBTUUID::from_raw("00000002-0000-1001-8001-00805f9b07d0");
  for(size_t i=0;i<table.characteristic_count;i++) {
    const auto &c=table.characteristics[i];
    if(c.uuid==write_uuid) write_handle_=c.value_handle;
    if(c.uuid==notify_uuid) {
      notify_handle_=c.value_handle;
      for(size_t d=c.first_descriptor;d<size_t(c.first_descriptor)+c.descriptor_count&&d<table.descriptor_count;d++)
        if(table.descriptors[d].uuid==ble_device_base::ESPBTUUID::from_uint16(0x2902)) cccd_handle_=table.descriptors[d].handle;
    }
  }
  backend_->release_services();
  if(!write_handle_||!notify_handle_||!cccd_handle_) { fail_("FD50 characteristics missing"); return; }
  state_=SUBSCRIBING; state_deadline_=millis()+15000;
  if(backend_->notify_characteristic(notify_handle_,true)!=0) fail_("BLE subscription failed");
}
void NutriSpin::on_notify_state(uint16_t handle,bool enabled,int error) {
  if(handle!=notify_handle_) return;
  if(error||!enabled) { fail_("BLE notifications failed"); return; }
  uint8_t enable[2]={1,0}; cccd_busy_=true;
  if(backend_->write_descriptor(cccd_handle_,enable,2)!=0) { cccd_busy_=false; fail_("CCCD write failed"); }
}
void NutriSpin::on_write_result(uint16_t handle,int error) {
  if(state_==IDLE||state_==CLOSING) return;
  if(error) { fail_("BLE write failed - command will not be retried"); return; }
  if(cccd_busy_&&handle==cccd_handle_) { cccd_busy_=false; request_info_(); return; }
  if(handle==write_handle_) { write_busy_=false; outgoing_index_++; last_write_=millis(); }
}
void NutriSpin::request_info_() {
  crypto_.md5(creds_.login,6,login_key_.data()); state_=INFO; state_deadline_=millis()+15000;
  // Negotiate Tuya's incoming packet size within the actual ATT payload.
  // Larger packets reduce fragmentation/bursts into the bounded GATT queue.
  uint16_t payload=std::min<uint16_t>(243,mtu_>23?mtu_-3:20);
  queue_(0,Bytes{uint8_t(payload>>8),uint8_t(payload)},pairing_connection_?0:4);
}
void NutriSpin::queue_(uint16_t op,const Bytes &data,uint8_t mode,uint32_t reply) {
  if(commands_.size()>=24) { status_("Queue full - command rejected"); return; }
  Command c{op,data,mode,reply}; if(reply) commands_.push_front(std::move(c)); else commands_.push_back(std::move(c));
}
void NutriSpin::loop() {
  uint32_t now=millis();
  if(discovering_pair_&&int32_t(now-pair_deadline_)>=0) { release_pairing_(); status_("No feeder selected - pairing window expired"); }
  if(pair_requested_&&!discovering_pair_&&int32_t(now-pair_deadline_)>=0) { release_pairing_(); status_("Pairing verification pending - saved keys retained"); }
  if(discovering_pair_&&!choosing_pair_&&int32_t(now-discovery_check_)>=0) finish_discovery_();
  if(state_!=IDLE&&state_!=READY&&int32_t(now-state_deadline_)>=0) { fail_("Connection timed out"); return; }
  if(await_reply_&&int32_t(now-deadline_)>=0) {
    // The pair may have committed despite a lost ACK. Reconnect with the
    // same durable pending key, never generate replacements on a retry.
    if(expected_op_==1&&pairing_connection_) { status_("Verifying saved credentials"); close_(); }
    else fail_("No response - command will not be retried");
    return;
  }
  if(state_==READY) {
    if(time_request_op_&&clock_->now().is_valid()) { clock_reply_(time_request_op_,time_request_seq_); time_request_op_=0; }
    // Time can only be sent in response to a feeder request. Reconnect when
    // idle to solicit it, after queued commands and schedule readback finish.
    if((clock_sync_requested_||now-last_clock_sync_>=clock_sync_interval_)
        &&clock_->now().is_valid()&&!discovering_pair_&&!pair_requested_
        &&!schedule_pending_&&!await_reply_&&!write_busy_&&!cccd_busy_
        &&commands_.empty()&&outgoing_index_>=outgoing_.size()) {
      last_clock_sync_=now; clock_sync_requested_=false;
      status_("Reconnecting to synchronize clock"); close_(500); return;
    }
    if(now-last_poll_>=poll_interval_&&commands_.size()<3) { last_poll_=now; poll_(); }
  }
  if(write_busy_||cccd_busy_||state_==IDLE||state_==CLOSING||!write_handle_) return;
  if(outgoing_index_>=outgoing_.size()) {
    outgoing_.clear(); outgoing_index_=0;
    if(commands_.empty()) return;
    if(await_reply_&&!commands_.front().reply) return;
    auto c=std::move(commands_.front()); commands_.pop_front(); Frame frame{++sequence_,c.reply,c.op,c.mode,std::move(c.data)};
    outgoing_=encode(crypto_,frame,c.mode==4?login_key_:session_key_,c.op==0?2:4);
    if(outgoing_.empty()) { fail_("Secure random generation failed"); return; }
    if(!c.reply) { await_reply_=true; expected_sequence_=frame.seq; expected_op_=c.op; deadline_=now+12000; }
  }
  if(now-last_write_<35) return;
  auto &packet=outgoing_[outgoing_index_]; write_busy_=true;
  if(backend_->write_characteristic(write_handle_,packet.data(),packet.size(),false)!=0) { write_busy_=false; fail_("BLE busy - command rejected"); }
}
void NutriSpin::on_notify_data(uint16_t handle,const uint8_t *data,uint16_t len) {
  if(handle!=notify_handle_||state_==CLOSING||state_==IDLE) return;
  auto result=receiver_.feed(data,len);
  if(result==Receiver::INVALID) { fail_("Invalid packet sequence"); return; }
  if(result!=Receiver::COMPLETE) return;
  Frame frame;
  if(!decode(crypto_,receiver_.data,login_key_,session_key_,frame)) { fail_("Invalid key or CRC"); return; }
  handle_(frame);
}
void NutriSpin::handle_(const Frame &f) {
  bool response=await_reply_&&f.reply==expected_sequence_&&f.opcode==expected_op_;
  if(response) await_reply_=false;
  const auto &d=f.data;
  if(f.opcode==0&&response&&state_==INFO) {
    if(d.size()!=96||d[2]!=4||d[3]!=2||d[4]!=0x10||d[5]!=(pairing_connection_?0:1)||f.mode!=(pairing_connection_?0:4)) { fail_("Unsupported protocol or pairing state"); return; }
    uint64_t physical=0; for(int i=5;i>=0;i--) physical=(physical<<8)|d[89+i];
    if(physical!=mac_) { fail_("MAC does not match selected feeder"); return; }
    if(!pairing_connection_&&memcmp(d.data()+55,creds_.virtual_id,22)!=0) { fail_("Pairing ID mismatch - factory reset required"); return; }
    uint8_t material[12]; memcpy(material,creds_.login,6); memcpy(material+6,d.data()+6,6); crypto_.md5(material,12,session_key_.data());
    texts_[2]->publish_state(std::to_string(d[0])+"."+std::to_string(d[1]));
    texts_[3]->publish_state(std::to_string(d[12])+"."+std::to_string(d[13])); texts_[4]->publish_state("4.2");
    Bytes payload(creds_.uuid,creds_.uuid+16); payload.insert(payload.end(),creds_.login,creds_.login+6); payload.insert(payload.end(),creds_.virtual_id,creds_.virtual_id+22);
    if(pairing_connection_) { if(!save_()) { fail_("Keys not saved - pairing stopped"); return; } payload.push_back(16); payload.insert(payload.end(),creds_.beacon,creds_.beacon+16); state_=PAIRING; }
    else state_=AUTH;
    state_deadline_=millis()+20000; queue_(1,payload,pairing_connection_?0:5); return;
  }
  if(f.opcode==1&&response&&(state_==PAIRING||state_==AUTH)) {
    if(d.size()!=1||f.mode!=5||(pairing_connection_?d[0]!=0:(d[0]!=0&&d[0]!=2))) { fail_("Authentication rejected"); return; }
    if(pairing_connection_) { status_("Keys accepted - verifying fresh connection"); close_(); return; }
    if(creds_.stage!=2) { creds_.stage=2; if(!save_()) { fail_("Unable to save pairing confirmation"); return; } }
    release_pairing_(); state_=READY; binaries_[0]->publish_state(true); binaries_[1]->publish_state(true); status_("Connected"); last_poll_=millis(); poll_(); return;
  }
  if(f.mode!=5||(state_!=READY&&state_!=AUTH)) return;
  if(f.opcode==0x8011||f.opcode==0x8012) {
    if(clock_->now().is_valid()) { time_request_op_=0; time_request_seq_=0; clock_reply_(f.opcode,f.seq); }
    else { time_request_seq_=f.seq; time_request_op_=f.opcode; status_("Connected - waiting for Home Assistant time"); }
    return;
  }
  if(f.opcode==3&&response) { if(d.size()!=1||d[0]!=0) fail_("Status query rejected"); return; }
  if(f.opcode==0x27&&response) {
    if(d.size()!=6||d[5]!=0) fail_("Feeder rejected data");
    return;
  }
  if(f.opcode==0x8006||f.opcode==0x8007||f.opcode==0x8001) {
    size_t pos=0; uint8_t width=1; Bytes ack;
    if(f.opcode!=0x8001) {
      if(d.size()<7||d[0]!=0) { fail_("Invalid data report"); return; }
      width=2; pos=7; ack.assign(d.begin(),d.begin()+7); ack.push_back(0);
      if(f.opcode==0x8007) { if(pos>=d.size()||d[pos]>1) { fail_("Unsupported timestamp format"); return; } pos+=d[pos]==0?14:5; }
      if(!(d[5]&0x80)) queue_(f.opcode,ack,5,f.seq);
    } else queue_(f.opcode,{},5,f.seq);
    std::vector<Point> report;
    if(pos>d.size()||!points(d,pos,width,report)) { fail_("Invalid datapoint data"); return; }
    for(const auto &p:report) point_(p);
  }
}
void NutriSpin::poll_() { queue_(3); dp_(103,0,Bytes{0}); dp_(102,0,Bytes{0}); }
void NutriSpin::dp_(uint8_t id,uint8_t type,const Bytes &value) {
  Bytes data{0}; u32(data,++sequence_); data.push_back(id); data.push_back(type); u16(data,value.size()); data.insert(data.end(),value.begin(),value.end()); queue_(0x27,data);
}
void NutriSpin::clock_reply_(uint16_t op,uint32_t reply) {
  auto t=clock_->now(); if(!t.is_valid()) { status_("Time unavailable - clock not synchronized"); return; }
  int16_t zone=ESPTime::timezone_offset()/36; Bytes data;
  if(op==0x8011) { auto text=std::to_string(uint64_t(t.timestamp)*1000); data.assign(text.begin(),text.end()); }
  else data={uint8_t(t.year%100),t.month,t.day_of_month,t.hour,t.minute,t.second,uint8_t((t.day_of_week+5)%7)};
  u16(data,uint16_t(zone)); queue_(op,data,5,reply); clock_synced_=true;
  last_clock_sync_=millis();
  // Clear the temporary wait message once HA time is available, without
  // overwriting a newer status about another operation.
  if(texts_[0]->state=="Connected - waiting for Home Assistant time") status_("Connected");
}
bool NutriSpin::writable_() { if(state_==READY) return true; status_("Not connected - command rejected"); return false; }
void NutriSpin::action(int action) {
  if(action==0) {
    begin_pairing_();
    return;
  }
  if(!writable_()) return;
  switch(action) {
    case 1: dp_(2,1,Bytes{1}); dp_(102,0,Bytes{0}); break;
    case 2: dp_(2,1,Bytes{0}); dp_(102,0,Bytes{0}); break;
    case 3: poll_(); break;
    case 4:
      // Tuya time is a response to the device's request. Reconnect makes
      // the feeder request time after authentication; never spoof a reply.
      if(!clock_->now().is_valid()) status_("Home Assistant time unavailable");
      else { clock_sync_requested_=true; status_("Clock synchronization queued"); } break;
    case 5: dp_(9,1,Bytes{1}); status_("Factory reset requested"); break;
  }
}
void NutriSpin::switch_control(int op,int meal,bool value) {
  if(!writable_()) return;
  if(op==0) { dp_(104,1,Bytes{uint8_t(value)}); queue_(3); }
  else edit_schedule_(meal,4,value);
}
void NutriSpin::number_control(int op,int meal,float value) {
  if(!std::isfinite(value)||std::floor(value)!=value||!writable_()) return;
  if(op==0) { if(value<0||value>2) return; Bytes raw; u32(raw,uint32_t(value)); dp_(18,2,raw); queue_(3); }
  else if(value>=0&&value<=(op==1?127:255)) edit_schedule_(meal,op==1?0:3,uint8_t(value));
}
void NutriSpin::language_control(const std::string &value) {
  if(!writable_()) return;
  for(uint8_t i=0;i<5;i++) if(value==LANGUAGES[i]) { dp_(101,4,Bytes{i}); queue_(3); break; }
}
void NutriSpin::text_control(int op,int meal,const std::string &value) {
  if(!writable_()) return;
  if(op==0) { Bytes raw(25); if(!unhex(value,raw.data(),25)||!valid_schedule(raw)) { status_("Invalid raw schedule"); return; } start_schedule_(raw); }
  else {
    if(meal<0||meal>=5||value.size()!=5||value[2]!=':'||value[0]<'0'||value[0]>'2'||value[1]<'0'||value[1]>'9'||value[3]<'0'||value[3]>'5'||value[4]<'0'||value[4]>'9') return;
    uint8_t hour=(value[0]-'0')*10+value[1]-'0', minute=(value[3]-'0')*10+value[4]-'0';
    if(hour>23||!schedule_known_||schedule_pending_) { status_("Wait for schedule readback"); return; }
    auto raw=schedule_; raw[meal*5+1]=hour; raw[meal*5+2]=minute; start_schedule_(raw);
  }
}
void NutriSpin::edit_schedule_(int meal,int field,uint8_t value) {
  if(meal<0||meal>=5||!schedule_known_||schedule_pending_) { status_("Wait for schedule readback"); return; }
  auto raw=schedule_; raw[meal*5+field]=value; start_schedule_(raw);
}
void NutriSpin::start_schedule_(const Bytes &raw) {
  if(schedule_pending_||!valid_schedule(raw)) { status_("Schedule awaiting confirmation"); return; }
  desired_schedule_=raw; schedule_pending_=true; dp_(1,0,raw); dp_(103,0,Bytes{0});
  set_timeout("schedule_confirmation",10000,[this]() { if(schedule_pending_) { schedule_pending_=false; status_("Schedule change not confirmed"); } });
}
void NutriSpin::publish_schedule_(const Bytes &value) {
  if(!valid_schedule(value)) { status_("Invalid schedule report"); return; }
  schedule_=value; schedule_known_=true;
  if(schedule_pending_&&value==desired_schedule_) { schedule_pending_=false; cancel_timeout("schedule_confirmation"); status_("Schedule confirmed"); }
  auto raw=hex(value.data(),value.size()); texts_[8]->publish_state(raw); schedule_text_->publish_state(raw);
  for(size_t i=0;i<5;i++) { size_t p=i*5; meals_enabled_[i]->publish_state(value[p+4]); char at[6]; snprintf(at,sizeof(at),"%02u:%02u",value[p+1],value[p+2]); meals_at_[i]->publish_state(at); meals_number_[i][0]->publish_state(value[p]); meals_number_[i][1]->publish_state(value[p+3]); }
}
void NutriSpin::point_(const Point &p) {
  uint32_t value=0; for(auto c:p.value) value=(value<<8)|c;
  if(p.id==1&&p.type==0) publish_schedule_(p.value);
  else if(p.id==11&&p.type==2&&p.value.size()==4) sensors_[0]->publish_state(value<=100?float(value):NAN);
  else if(p.id==15&&p.type==2&&p.value.size()==4) sensors_[2]->publish_state(value);
  else if(p.id==18&&p.type==2&&p.value.size()==4&&value<=2) voice_->publish_state(value);
  else if(p.id==101&&p.type==4&&p.value.size()==1&&value<5) language_->publish_state(LANGUAGES[value]);
  else if(p.id==104&&p.type==1&&p.value.size()==1) manual_->publish_state(value!=0);
  else if(p.id==102&&p.type==0) {
    if(p.value.size()%5||p.value.size()>200) { status_("Unsupported history size"); return; }
    auto records=history(p.value); sensors_[3]->publish_state(records.size());
    std::string raw=hex(p.value.data(),p.value.size()); texts_[6]->publish_state(raw.substr(0,200)); texts_[7]->publish_state(raw.size()>200?raw.substr(200):"");
    if(!records.empty()) { auto last=std::max_element(records.begin(),records.end(),[](const auto &a,const auto &b){return a.timestamp<b.timestamp;}); texts_[5]->publish_state(timestamp(last->timestamp)); sensors_[4]->publish_state(last->code); }
    else { texts_[5]->publish_state(""); sensors_[4]->publish_state(NAN); }
  }
}
}  // namespace esphome::nutrispin
