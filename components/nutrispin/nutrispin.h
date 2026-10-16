#pragma once
#include "protocol.h"
#include "pairing_discovery.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/ble_device_base/ble_device.h"
#include "esphome/components/bluetooth_connection/bluetooth_connection_gatt_backend.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text/text.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include <deque>

namespace esphome::nutrispin {
class NutriSpin;
struct Binding {
  void bind(NutriSpin *owner, int op, int meal) { owner_=owner; op_=op; meal_=meal; }
  NutriSpin *owner_{}; int op_{}, meal_{-1};
};
class FeederButton : public button::Button, public Binding { void press_action() override; };
class FeederSwitch : public switch_::Switch, public Binding { void write_state(bool value) override; };
class FeederNumber : public number::Number, public Binding { void control(float value) override; };
class FeederText : public text::Text, public Binding { void control(const std::string &value) override; };
class FeederSelect : public select::Select {
 public: void bind(NutriSpin *owner) { owner_=owner; }
  void bind_pairing(NutriSpin *owner) { owner_=owner; pairing_=true; }
 protected: void control(const std::string &value) override;
 private: NutriSpin *owner_{}; bool pairing_{};
};
class PicoCrypto : public wire::Crypto {
 public:
  void md5(const uint8_t *p, size_t n, uint8_t out[16]) override;
  void cbc(bool encrypt, const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t n) override;
  bool random(uint8_t *out, size_t n) override;
};
struct Credentials {
  uint32_t magic{};
  uint8_t stage{};  // 1: durable pending key, 2: verified through a fresh login
  uint8_t uuid[16]{}, login[6]{}, virtual_id[22]{}, beacon[16]{};
};
struct SlotRecord {
  uint32_t magic{};
  uint64_t mac{};
  Credentials credentials;
};
class NutriSpin : public Component, public ble_device_base::ESPBTDeviceListener,
                  public ble_device_base::GattClientListener {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
  bool parse_device(const ble_device_base::ESPBTDevice &device) override;
  void on_connection_state(bool connected, uint16_t mtu, int error) override;
  void on_service_discovery_done(int error) override;
  void on_write_result(uint16_t handle, int error) override;
  void on_notify_state(uint16_t handle, bool enabled, int error) override;
  void on_notify_data(uint16_t handle, const uint8_t *data, uint16_t len) override;
  void set_backend(ble_device_base::BLEGattConnection *p) { backend_=p; }
  void set_mac(uint64_t m) { mac_=configured_mac_=m; }
  void set_slot_id(const std::string &id) { slot_key_=fnv1_hash("nutrispin:slot:"+id); }
  void set_label(const std::string &s) { label_=s; }
  void set_initial_credentials(const std::string &s) { initial_=s; }
  void set_poll_interval(uint32_t n) { poll_interval_=n; }
  void set_clock_sync_interval(uint32_t n) { clock_sync_interval_=n; }
  void set_clock(time::RealTimeClock *p) { clock_=p; }
  void set_sensor(int n, sensor::Sensor *p) { sensors_[n]=p; }
  void set_text_sensor(int n, text_sensor::TextSensor *p) { texts_[n]=p; }
  void set_binary_sensor(int n, binary_sensor::BinarySensor *p) { binaries_[n]=p; }
  void set_manual(FeederSwitch *p) { manual_=p; }
  void set_voice(FeederNumber *p) { voice_=p; }
  void set_language(FeederSelect *p) { language_=p; }
  void set_pairing_select(FeederSelect *p) { pairing_select_=p; }
  void set_pairing_devices(text_sensor::TextSensor *p) { pairing_devices_=p; }
  void set_schedule_text(FeederText *p) { schedule_text_=p; }
  void set_meal_switch(int n, FeederSwitch *p) { meals_enabled_[n]=p; }
  void set_meal_text(int n, FeederText *p) { meals_at_[n]=p; }
  void set_meal_number(int n, int field, FeederNumber *p) { meals_number_[n][field]=p; }
  void action(int action);
  void switch_control(int op, int meal, bool value);
  void number_control(int op, int meal, float value);
  void text_control(int op, int meal, const std::string &value);
  void language_control(const std::string &value);
  void pairing_control(const std::string &value);
 protected:
  enum State { IDLE, CONNECTING, DISCOVERING, SUBSCRIBING, INFO, PAIRING, AUTH, READY, CLOSING };
  struct Command { uint16_t op; wire::Bytes data; uint8_t mode{5}; uint32_t reply{}; };
  void status_(const char *s);
  void fail_(const char *s);
  void close_(uint32_t delay=3000);
  bool save_();
  bool import_(const std::string &s);
  bool generate_(const wire::Identity &identity);
  bool claimed_by_other_(uint64_t mac) const;
  void begin_pairing_();
  void finish_discovery_();
  void choose_candidate_(size_t index);
  void release_pairing_();
  void connect_(const wire::Identity &identity, uint8_t address_type);
  void queue_(uint16_t op, const wire::Bytes &data={}, uint8_t mode=5, uint32_t reply=0);
  void request_info_();
  void handle_(const wire::Frame &frame);
  void point_(const wire::Point &point);
  bool writable_();
  void dp_(uint8_t id, uint8_t type, const wire::Bytes &value);
  void poll_();
  void clock_reply_(uint16_t op, uint32_t reply);
  void publish_schedule_(const wire::Bytes &value);
  void edit_schedule_(int meal, int field, uint8_t value);
  void start_schedule_(const wire::Bytes &value);
  PicoCrypto crypto_;
  ble_device_base::BLEGattConnection *backend_{};
  time::RealTimeClock *clock_{};
  ESPPreferenceObject pref_;
  Credentials creds_{};
  uint64_t mac_{}, configured_mac_{};
  uint32_t slot_key_{}, discovery_check_{};
  bool discovering_pair_{}, choosing_pair_{};
  PairingDiscovery discovery_;
  std::vector<PairingCandidate> candidates_;
  static std::vector<NutriSpin *> instances_;
  static NutriSpin *pairing_owner_;
  std::string label_, initial_;
  State state_{IDLE};
  bool pair_requested_{}, pairing_connection_{}, await_reply_{}, write_busy_{}, cccd_busy_{};
  bool schedule_known_{}, schedule_pending_{}, clock_synced_{};
  bool clock_sync_requested_{};
  uint32_t last_clock_sync_{}, clock_sync_interval_{1800000};
  uint8_t address_type_{};
  uint16_t write_handle_{}, notify_handle_{}, cccd_handle_{}, mtu_{23};
  uint32_t sequence_{}, expected_sequence_{}, deadline_{}, pair_deadline_{}, next_connect_{}, last_poll_{}, poll_interval_{60000}, last_write_{}, state_deadline_{};
  uint16_t expected_op_{};
  uint32_t time_request_seq_{}; uint16_t time_request_op_{};
  std::deque<Command> commands_;
  std::vector<wire::Bytes> outgoing_;
  size_t outgoing_index_{};
  wire::Receiver receiver_;
  wire::Key login_key_{}, session_key_{};
  wire::Bytes service_, manufacturer_, schedule_, desired_schedule_;
  std::array<sensor::Sensor *,5> sensors_{};
  std::array<text_sensor::TextSensor *,9> texts_{};
  std::array<binary_sensor::BinarySensor *,2> binaries_{};
  FeederSwitch *manual_{}; FeederNumber *voice_{}; FeederSelect *language_{}; FeederText *schedule_text_{};
  FeederSelect *pairing_select_{};
  text_sensor::TextSensor *pairing_devices_{};
  std::array<FeederSwitch *,5> meals_enabled_{};
  std::array<FeederText *,5> meals_at_{};
  std::array<std::array<FeederNumber *,2>,5> meals_number_{};
};
}  // namespace esphome::nutrispin
