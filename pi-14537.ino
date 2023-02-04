// © Kay Sievers <kay@versioduo.com>, 2021-2022
// SPDX-License-Identifier: Apache-2.0

#include <V2Buttons.h>
#include <V2Color.h>
#include <V2Device.h>
#include <V2LED.h>
#include <V2Link.h>
#include <V2MIDI.h>
#include <V2Music.h>

V2DEVICE_METADATA("de.vogelkuerstner.pi-14537", 47, "versioduo:samd:control");

static V2LED::WS2812 LED(2, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2LED::WS2812 LEDExt(88, PIN_LED_WS2812_EXT, &sercom1, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2Link::Port Socket(&SerialSocket);

static class Device : public V2Device {
public:
  Device() : V2Device() {
    metadata.vendor      = "Thomas Kürstner";
    metadata.product     = "pi-14537";
    metadata.description = "Hammerklavier";
    metadata.home        = "https://www.studiovogelkuerstner.de#pi-14537";

    system.download  = "https://versioduo.com/download";
    system.configure = "https://versioduo.com/configure";

    // https://github.com/versioduo/arduino-board-package/blob/main/boards.txt
    usb.pid          = 0xef30;
    usb.ports.access = 11;

    configuration = {.size{sizeof(config)}, .data{&config}};
  }

  enum class Program : uint8_t {
    Standard,
    Damper,
    TriggerDamper,
    Calibration,
    _count,
  };

  enum class CC {
    Volume     = V2MIDI::CC::ChannelVolume,
    Sustain    = V2MIDI::CC::Sustain,
    Color      = V2MIDI::CC::Controller14,
    Saturation = V2MIDI::CC::Controller15,
    Brightness = V2MIDI::CC::Controller89,
    Rainbow    = V2MIDI::CC::Controller90,
  };

  // 88 notes, A-1 - C7. The Middle C is C3.
  static constexpr struct {
    uint8_t start;
    uint8_t count;
    uint8_t damper_count;
  } notes{
    .start{V2MIDI::A(-1)},
    .count{88},
    .damper_count{72},
  };

  // Config, written to EEPROM
  struct {
    struct {
      uint8_t min;
      uint8_t max;
    } calibration[notes.count];

    // LED color.
    struct {
      uint8_t h{15};
      uint8_t s{40};
      uint8_t v{100};
    } color;
  } config{};

  void setProgram(Program number) {
    _program = number;
    LED.setHSV(_programs[(uint8_t)_program].color, 1, 0.25);
  }

  void setSustain(uint8_t value) {
    _sustain = value;

    if (_sustain < 64) {
      for (uint8_t i = 0; i < notes.count; i++) {
        if (_notes[i].playing)
          continue;

        if (!_notes[i].sustain)
          continue;

        _notes[i].playing = false;
        _notes[i].sustain = false;
        sendDamper(i, 0, 0);
      }
    }
  }

  void playDefault(uint8_t note, uint8_t velocity, uint8_t off_velocity) {
    const uint8_t index = note - notes.start;

    if (velocity == 0) {
      light(index, 0);

      if (_sustain < 64)
        sendDamper(index, 0, 0);
      else
        _notes[index].sustain = true;

      _notes[index].playing = false;
      return;
    }

    if (_volume > 0) {
      float fraction = getFractionCalibrated(index, velocity);
      fraction       = adjustVolume(fraction);

      float watts;
      float seconds;
      getPulse(fraction, watts, seconds);
      sendDamper(index, 5, 20);
      sendTrigger(index, watts, seconds);
    }

    _notes[index].playing = true;
    light(index, velocity);
  }

  void playDamper(uint8_t note, uint8_t velocity, uint8_t off_velocity) {
    const uint8_t index = note - notes.start;

    if (velocity == 0) {
      light(index, 0);
      return;
    }

    if (_volume > 0)
      sendDamper(index, 4, 0.01, false);

    light(index, velocity);
  }

  void playTriggerDamper(uint8_t note, uint8_t velocity, uint8_t off_velocity) {
    const uint8_t index = note - notes.start;

    if (velocity == 0) {
      light(index, 0);
      return;
    }

    if (_volume > 0) {
      float fraction = getFractionCalibrated(index, velocity);
      fraction       = adjustVolume(fraction);

      float watts;
      float seconds;
      getPulse(fraction, watts, seconds);
      sendTrigger(index, watts, seconds);
    }

    light(index, velocity);
  }

  void playCalibration(uint8_t note, uint8_t velocity, uint8_t off_velocity) {
    const uint8_t index = note - notes.start;

    float watts;
    float seconds;
    const float fraction = getFraction(velocity);
    getPulse(fraction, watts, seconds);
    sendDamper(index, 3.5, 0.5);
    sendTrigger(index, watts, seconds);
  }

  void play(uint8_t channel, uint8_t note, uint8_t velocity, uint8_t off_velocity = 0) {
    if (note < notes.start || note > (notes.start + notes.count - 1))
      return;

    _last_usec = micros();

    // Ignore the note when the same note with a higher priority is already playing.
    const uint8_t index = note - notes.start;
    if (!_notes_priority[index].set(velocity == 0 ? -1 : velocity, channel))
      return;

    led.flash(0.03, 0.3);

    switch (_program) {
      case Program::Standard:
        playDefault(note, velocity, off_velocity);
        break;

      case Program::Damper:
        playDamper(note, velocity, off_velocity);
        break;

      case Program::TriggerDamper:
        playTriggerDamper(note, velocity, off_velocity);
        break;

      case Program::Calibration:
        playCalibration(note, velocity, off_velocity);
        break;
    }
  }

  void allNotesOff() {
    _last_usec = 0;

    if (_force.trigger()) {
      reset();
      return;
    }

    _volume  = 100;
    _sustain = 0;
    _sustain_priority.reset();
    _rainbow = 0;

    _led.h = (float)config.color.h / 127.f * 360.f;
    _led.s = (float)config.color.s / 127.f;
    _led.v = (float)config.color.v / 127.f;

    LED.reset();
    LED.setHSV(_programs[(uint8_t)_program].color, 1, 0.25);
    LEDExt.reset();

    for (uint8_t i = 0; i < notes.count; i++) {
      _notes[i] = {};
      _notes_priority[i].reset();
    }

    const uint8_t n_children = 1 + (notes.count / 8);
    for (uint8_t i = 0; i < n_children; i++) {
      _midi.setPort(i);
      _midi.setControlChange(0, V2MIDI::CC::AllNotesOff);
      Socket.send(&_midi);
    }
  }

private:
  unsigned long _last_usec{};
  V2Music::ForcedStop _force;

  // LED color.
  struct {
    float h;
    float s;
    float v;
  } _led{};

  const struct {
    const char *name;
    float color;
  } _programs[(uint8_t)Program::_count]{
    [(uint8_t)Program::Standard]      = {.name{"Standard"}, .color{V2Color::Orange}},
    [(uint8_t)Program::Damper]        = {.name{"Damper"}, .color{V2Color::Cyan}},
    [(uint8_t)Program::TriggerDamper] = {.name{"Trigger + Damper"}, .color{V2Color::Green}},
    [(uint8_t)Program::Calibration]   = {.name{"Calibration"}, .color{V2Color::Magenta}},
  };
  Program _program{};
  uint16_t _bank{};

  uint8_t _volume{100};
  uint8_t _sustain{};
  V2Music::Priority<16> _sustain_priority{};
  float _rainbow{};

  struct {
    bool playing;
    bool sustain;
  } _notes[notes.count]{};

  V2Music::Priority<16> _notes_priority[notes.count]{};

  V2MIDI::Packet _midi{};
  V2Link::Packet _link;

  void handleInit() override {
    if (usb.ports.enableAccess) {
      usb.midi.setPortName(1, "control");
      usb.midi.setPortName(2, "pulse 01");
      usb.midi.setPortName(3, "pulse 02");
      usb.midi.setPortName(4, "pulse 03");
      usb.midi.setPortName(5, "pulse 04");
      usb.midi.setPortName(6, "pulse 05");
      usb.midi.setPortName(7, "pulse 06");
      usb.midi.setPortName(8, "pulse 07");
      usb.midi.setPortName(9, "pulse 08");
      usb.midi.setPortName(10, "pulse 09");
      usb.midi.setPortName(11, "pulse 10");
    }
  }

  void handleReset() override {
    _last_usec = 0;
    _force.reset();
    _program = Program::Standard;
    _bank    = 0;
    _volume  = 100;
    _sustain = 0;
    _sustain_priority.reset();
    _rainbow = 0;

    _led.h = (float)config.color.h / 127.f * 360.f;
    _led.s = (float)config.color.s / 127.f;
    _led.v = (float)config.color.v / 127.f;

    for (uint8_t i = 0; i < notes.count; i++) {
      _notes[i] = {};
      _notes_priority[i].reset();
    }

    LED.reset();
    LED.setHSV(_programs[(uint8_t)_program].color, 1, 0.25);
    LEDExt.reset();

    const uint8_t n_children = 1 + (notes.count / 8);
    for (uint8_t i = 0; i < n_children; i++) {
      _midi.setPort(i);
      _midi.set(0, V2MIDI::Packet::Status::SystemReset);
      Socket.send(&_midi);
    }
  }

  void handleLoop() override {
    // Reset all playing notes when idle.
    if (_last_usec > 0 && (unsigned long)(micros() - _last_usec) > 30 * 1000 * 1000) {
      _last_usec = 0;
      allNotesOff();
    }
  }

  void light(uint8_t index, uint8_t velocity) {
    if (velocity > 0) {
      // Brightness depending on the velocity
      const float fraction   = (float)velocity / 127.f;
      const float brightness = 0.5f + (0.5f * fraction * _led.v);
      LEDExt.setHSV(index, _led.h, _led.s, _led.v * brightness);

    } else
      LEDExt.setBrightness(index, 0);
  };

  float getFraction(uint8_t velocity) {
    const float fraction = (float)velocity / 127.f;
    return powf(fraction, 3);
  }

  float getFractionCalibrated(uint8_t index, uint8_t velocity) {
    const uint8_t min = config.calibration[index].min;
    const uint8_t max = config.calibration[index].max;

    // Default configuration, uncalibrated.
    if (min == 0 && max == 0)
      return getFraction(velocity);

    float floor = (float)min / 127.f;
    floor       = powf(floor, 3);

    float ceiling = (float)max / 127.f;
    ceiling       = powf(ceiling, 3);

    float fraction = (float)velocity / 127.f;
    fraction       = powf(fraction, 3);

    float range = (ceiling - floor) * fraction;
    return floor + range;
  }

  float adjustVolume(float fraction) {
    if (_volume < 100) {
      const float range = (float)_volume / 100.f;
      return fraction * range;
    }

    const float range = (float)(_volume - 100) / 27.f;
    return powf(fraction, 1 - (0.5f * range));
  }

  void getPulse(float fraction, float &watts, float &seconds) {
    static constexpr struct {
      struct {
        float watts{2.5};
        float seconds{0.02};
      } min;
      struct {
        float watts{10};
        float seconds{0.006};
      } max;
    } range;

    watts = range.min.watts;
    watts += (range.max.watts - range.min.watts) * fraction;

    seconds = range.min.seconds;
    seconds += (range.max.seconds - range.min.seconds) * fraction;
  }

  void sendPulse(uint8_t index, float watts, float seconds, bool fade_in, bool fade_out) {
    const uint8_t child = index / 16;
    const uint8_t port  = index % 16;

    V2Link::Packet packet{};
    V2Link::Packet::Pulse pulse{
      .port{port},
      .watts{watts},
      .seconds{seconds},
      .fade_in{fade_in},
      .fade_out{fade_out},
    };
    packet.setPulse(&pulse);
    Socket.send(child, &packet);
  }

  void sendTrigger(uint8_t index, float watts, float seconds) {
    sendPulse(index, watts, seconds, false, false);
  }

  void sendDamper(uint8_t index, float watts, float seconds, bool fade_out = true) {
    if (index > notes.damper_count - 1)
      return;

    sendPulse(notes.count + notes.damper_count - index - 1, watts, seconds, false, fade_out);
  }

  void handleNote(uint8_t channel, uint8_t note, uint8_t velocity) override {
    play(channel, note, velocity);
  }

  void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override {
    //  Work-around for some devices.
    if (velocity == 0)
      velocity = 64;

    play(channel, note, 0, velocity);
  }

  void handleProgramChange(uint8_t channel, uint8_t program) override {
    if (channel != 0)
      return;

    if (program != V2MIDI::GM::Program::AcousticGrandPiano)
      return;

    if (_bank >= (uint8_t)Program::_count)
      return;

    setProgram((Program)_bank);
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    switch (controller) {
      case V2MIDI::CC::AllSoundOff:
      case V2MIDI::CC::AllNotesOff:
        allNotesOff();
        return;

      case (uint8_t)CC::Sustain:
        if (!_sustain_priority.set(value == 0 ? -1 : value, channel))
          return;

        // Restore the value from the lower priority.
        if (value == 0) {
          const int8_t sustain = _sustain_priority.get();
          if (sustain >= 0)
            value = sustain;
        }

        setSustain(value);
        return;
    }

    if (channel != 0)
      return;

    // Controls for the main channel only.
    switch (controller) {
      case V2MIDI::CC::BankSelect:
        _bank = value << 7;
        break;

      case V2MIDI::CC::BankSelectLSB:
        _bank |= value;
        break;

      case (uint8_t)CC::Volume:
        _volume = value;
        break;

      case (uint8_t)CC::Color:
        _led.h = (float)value / 127.f * 360.f;
        break;

      case (uint8_t)CC::Saturation:
        _led.s = (float)value / 127.f;
        break;

      case (uint8_t)CC::Brightness:
        _led.v = (float)value / 127.f;
        if (_rainbow > 0.f)
          LEDExt.rainbow(1, 4.5f - (_rainbow * 4.f), _led.v);
        break;

      case (uint8_t)CC::Rainbow:
        _rainbow = (float)value / 127.f;
        if (_rainbow <= 0.f)
          LEDExt.reset();
        else
          LEDExt.rainbow(1, 4.5f - (_rainbow * 4.f), _led.v);
        break;
    }
  }

  void handleSystemReset() override {
    reset();
  }

  void exportSettings(JsonArray json) override {
    {
      JsonObject setting = json.createNestedObject();
      setting["type"]    = "calibration";
      setting["title"]   = "Calibration";

      // Notes are sent on a special program which plays the raw uncalibrated values.
      JsonObject json_program = setting.createNestedObject("program");
      json_program["number"]  = V2MIDI::GM::Program::AcousticGrandPiano;
      json_program["bank"]    = (uint8_t)Program::Calibration;

      JsonObject json_chromatic = setting.createNestedObject("chromatic");
      json_chromatic["start"]   = notes.start;
      json_chromatic["count"]   = notes.count;

      setting["path"] = "calibration";
    }

    {
      JsonObject setting = json.createNestedObject();
      setting["type"]    = "color";
      setting["title"]   = "Light";
      setting["path"]    = "color";
    }
  }

  void exportConfiguration(JsonObject json) override {
    json["#calibration"]       = "The “Raw” velocity values to play a note with velocity 1 and 127";
    JsonArray json_calibration = json.createNestedArray("calibration");
    for (uint8_t i = 0; i < notes.count; i++) {
      JsonObject note = json_calibration.createNestedObject();
      uint8_t min     = config.calibration[i].min;
      uint8_t max     = config.calibration[i].max;

      // The default values are all 0 when no configuration is stored.
      if (min == 0)
        min = 1;

      if (max == 0)
        max = 127;

      note["min"] = min;
      note["max"] = max;
    }

    {
      json["#color"]     = "The LED color. Hue, saturation, brightness, 0..127";
      JsonArray json_led = json.createNestedArray("color");
      json_led.add(config.color.h);
      json_led.add(config.color.s);
      json_led.add(config.color.v);
    }
  }

  void importConfiguration(JsonObject json) override {
    JsonArray json_calibration = json["calibration"];
    if (json_calibration) {
      for (uint8_t i = 0; i < notes.count; i++) {
        if (!json_calibration[i].isNull()) {
          uint8_t min = json_calibration[i]["min"];
          uint8_t max = json_calibration[i]["max"];

          // Limit
          if (min > 127)
            min = 127;

          if (max > 127)
            max = 127;

          if (min == 0)
            min = 1;

          if (max == 0)
            max = 127;

          // Invalid
          if (max < min)
            max = min;

          config.calibration[i].min = min;
          config.calibration[i].max = max;

        } else {
          config.calibration[i].min = 1;
          config.calibration[i].max = 127;
        }
      }
    }

    JsonArray json_led = json["color"];
    if (json_led) {
      uint8_t color = json_led[0];
      if (color > 127)
        color = 127;
      config.color.h = color;
      _led.h         = (float)color / 127.f * 360.f;

      uint8_t saturation = json_led[1];
      if (saturation > 127)
        saturation = 127;
      config.color.s = saturation;
      _led.s         = (float)saturation / 127.f;

      uint8_t brightness = json_led[2];
      if (brightness > 127)
        brightness = 127;
      config.color.v = brightness;
      _led.v         = (float)brightness / 127.f;
    }
  }

  void exportInput(JsonObject json) override {
    JsonArray json_programs = json.createNestedArray("programs");
    for (uint8_t i = 0; i < (uint8_t)Program::_count; i++) {
      JsonObject json_program = json_programs.createNestedObject();
      json_program["name"]    = _programs[i].name;
      json_program["number"]  = V2MIDI::GM::Program::AcousticGrandPiano;
      json_program["bank"]    = i;
      if (i == (uint8_t)_program)
        json_program["selected"] = true;
    }

    JsonArray json_controllers = json.createNestedArray("controllers");
    {
      JsonObject json_controller = json_controllers.createNestedObject();
      json_controller["name"]    = "Volume";
      json_controller["number"]  = (uint8_t)CC::Volume;
      json_controller["value"]   = _volume;
    }
    {
      JsonObject json_controller = json_controllers.createNestedObject();
      json_controller["name"]    = "Sustain";
      json_controller["number"]  = (uint8_t)CC::Sustain;
      json_controller["value"]   = _sustain;
    }
    {
      JsonObject json_controller = json_controllers.createNestedObject();
      json_controller["name"]    = "Hue";
      json_controller["number"]  = (uint8_t)CC::Color;
      json_controller["value"]   = (uint8_t)(_led.h / 360.f * 127.f);
    }
    {
      JsonObject json_controller = json_controllers.createNestedObject();
      json_controller["name"]    = "Saturation";
      json_controller["number"]  = (uint8_t)CC::Saturation;
      json_controller["value"]   = (uint8_t)(_led.s * 127.f);
    }
    {
      JsonObject json_controller = json_controllers.createNestedObject();
      json_controller["name"]    = "Brightness";
      json_controller["number"]  = (uint8_t)CC::Brightness;
      json_controller["value"]   = (uint8_t)(_led.v * 127.f);
    }
    {
      JsonObject json_controller = json_controllers.createNestedObject();
      json_controller["name"]    = "Rainbow";
      json_controller["number"]  = (uint8_t)CC::Rainbow;
      json_controller["value"]   = (uint8_t)(_rainbow * 127.f);
    }

    JsonObject json_chromatic = json.createNestedObject("chromatic");
    json_chromatic["start"]   = notes.start;
    json_chromatic["count"]   = notes.count;
  }
} Device;

// Dispatch MIDI packets
static class MIDI {
public:
  void loop() {
    if (!Device.usb.midi.receive(&_midi))
      return;

    if (_midi.getPort() == 0) {
      Device.dispatch(&Device.usb.midi, &_midi);

    } else {
      _midi.setPort(_midi.getPort() - 1);
      Socket.send(&_midi);
    }
  }

private:
  V2MIDI::Packet _midi{};
} MIDI;

// Dispatch Link packets
static class Link : public V2Link {
public:
  Link() : V2Link(NULL, &Socket) {}

private:
  V2MIDI::Packet _midi{};

  // Forward children device events to the host
  void receiveSocket(V2Link::Packet *packet) override {
    if (packet->getType() == V2Link::Packet::Type::MIDI) {
      uint8_t address = packet->getAddress();
      if (address == 0x0f)
        return;

      if (Device.usb.midi.connected()) {
        packet->receive(&_midi);
        _midi.setPort(address + 1);
        Device.usb.midi.send(&_midi);
      }
    }
  }
} Link;

static class Test {
public:
  static constexpr struct {
    uint8_t min;
    uint8_t step;
  } config{.min{1}, .step{15}};

  void stop() {
    Device.reset();
    _enabled = false;
  }

  void run(Device::Program program) {
    Device.reset();
    Device.setProgram(program);
    _reset_usec = micros();

    _enabled  = true;
    _velocity = config.min;
    _note     = 0;
    _usec     = 0;
  }

  void loop() {
    if (!_enabled)
      return;

    play();
  }

private:
  bool _enabled{};
  uint8_t _velocity{};
  uint8_t _note{};
  unsigned long _usec{};
  unsigned long _reset_usec{};

  void play() {
    if (_reset_usec > 0) {
      // Wait for the controllers to initialize after a reset.
      if ((unsigned long)(micros() - _reset_usec) < 500 * 1000)
        return;

      _reset_usec = 0;
    }

    if ((unsigned long)(micros() - _usec) < 180 * 1000)
      return;

    _usec = micros();

    if (_note == 0) {
      LEDExt.rainbow(2, 2, 1);

      _note = Device::notes.start;
      Device.play(0, _note, _velocity);

    } else if (_note < Device.notes.start + Device.notes.count - 1) {
      Device.play(0, _note, 0, 0);
      _note++;
      Device.play(0, _note, _velocity);

    } else {
      _note = 0;

      _velocity += config.step;
      if (_velocity > 127)
        _velocity = config.min;

      Device.allNotesOff();
    }
  }
} TestMode;

static class Button : public V2Buttons::Button {
public:
  Button() : V2Buttons::Button(&_config, PIN_BUTTON_REVISION_0) {}

private:
  const V2Buttons::Config _config{.click_usec{150 * 1000}, .hold_usec{300 * 1000}};

  void handleClick(uint8_t count) override {
    if (count >= (uint8_t)Device::Program::_count)
      return;

    Device.reset();
    Device.setProgram((Device::Program)count);
  }

  void handleHold(uint8_t count) override {
    if (count >= (uint8_t)Device::Program::_count)
      return;

    TestMode.run((Device::Program)count);
  }

  void handleRelease() override {
    TestMode.stop();
  }
} Button;

void setup() {
  Serial.begin(9600);
  LED.begin();
  LED.setMaxBrightness(0.5);
  LEDExt.begin();
  LEDExt.setMaxBrightness(0.75);
  Device.begin();
  Button.begin();

  Socket.begin();
  Device.link = &Link;

  // Set the SERCOM interrupt priority, it requires a stable ~300 kHz interrupt
  // frequency. This needs to be after begin().
  setSerialPriority(&SerialSocket, 2);

  Device.reset();
}

void loop() {
  LED.loop();
  LEDExt.loop();
  MIDI.loop();
  Link.loop();
  V2Buttons::loop();
  TestMode.loop();
  Device.loop();

  if (Link.idle() && Device.idle())
    Device.sleep();
}
