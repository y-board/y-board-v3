#include "yboard.h"

YBoardV3 Yboard;

YBoardV3::YBoardV3() : strip(led_count, led_pin, NEO_GRB + NEO_KHZ800) {}

YBoardV3::~YBoardV3() {}

void YBoardV3::setup() {
    setup_leds();
    setup_switches();
    setup_buttons();
    setup_speaker();
    setup_accelerometer();
    setup_temperature();
}

////////////////////////////// LEDs ///////////////////////////////
void YBoardV3::setup_leds() {
    strip.begin();
    strip.clear();
    set_led_brightness(50);
}

void YBoardV3::set_led_color(uint16_t index, uint8_t red, uint8_t green, uint8_t blue) {
    strip.setPixelColor(index - 1, red, green, blue);
    strip.show();
}

void YBoardV3::set_led_brightness(uint8_t brightness) { strip.setBrightness(brightness); }

void YBoardV3::set_all_leds_color(uint8_t red, uint8_t green, uint8_t blue) {
    for (int i = 0; i < this->led_count; i++) {
        strip.setPixelColor(i, red, green, blue, false);
    }
    strip.show();
}

////////////////////////////// Switches ///////////////////////////////
void YBoardV3::setup_switches() {
    pinMode(this->switch1_pin, INPUT);
    pinMode(this->switch2_pin, INPUT);
}

bool YBoardV3::get_switch(uint8_t switch_idx) {
    switch (switch_idx) {
    case 1:
        return digitalRead(this->switch1_pin);
    case 2:
        return digitalRead(this->switch2_pin);
    default:
        return false;
    }
}

////////////////////////////// Buttons ///////////////////////////////
void YBoardV3::setup_buttons() {
    pinMode(this->button1_pin, INPUT);
    pinMode(this->button2_pin, INPUT);
}

bool YBoardV3::get_button(uint8_t button_idx) {
    switch (button_idx) {
    case 1:
        return !digitalRead(this->button1_pin);
    case 2:
        return !digitalRead(this->button2_pin);
    default:
        return false;
    }
}

////////////////////////////// Knob ///////////////////////////////
int YBoardV3::get_knob() {
    int value = map(analogRead(this->knob_pin), 2888, 8, 0, 100);
    value = max(0, value);
    value = min(100, value);
    return value;
}

////////////////////////////// Speaker/Tones //////////////////////////////////
void YBoardV3::setup_speaker() {
    // Set microSD Card CS as OUTPUT and set HIGH
    pinMode(sd_cs_pin, OUTPUT);
    digitalWrite(sd_cs_pin, HIGH);

    // Initialize SPI bus for microSD Card
    SPI.begin(spi_sck_pin, spi_miso_pin, spi_mosi_pin);

    // Start microSD Card
    if (!SD.begin(sd_cs_pin)) {
        Serial.println("Error accessing microSD card!");
    }

    // Setup I2S
    audio.setPinout(i2s_bclk_pin, i2s_lrc_pin, i2s_dout_pin);

    // Make the volume out of 100
    audio.setVolumeSteps(100);

    // Set Volume
    audio.setVolume(25);
}

void YBoardV3::loop_speaker() { audio.loop(); }

void YBoardV3::play_song_from_sd(const char *filename) { audio.connecttoFS(SD, filename); }

void YBoardV3::set_speaker_volume(uint8_t volume) { audio.setVolume(volume); }

////////////////////////////// Accelerometer /////////////////////////////////////
void YBoardV3::setup_accelerometer() {
    if (!wire_begin) {
        Wire.begin(sda_pin, scl_pin);
        wire_begin = true;
    }

    if (!accel.begin(accel_addr, Wire)) {
        Serial.println("WARNING: Accelerometer not detected.");
    }
}

void YBoardV3::get_accelerometer(float *accelX, float *accelY, float *accelZ) {
    if (accel.available()) {
        *accelX = accel.getX();
        *accelY = accel.getY();
        *accelZ = accel.getZ();
    } else {
        Serial.println("WARNING: Accelerometer data not available.");
    }
}

// ////////////////////////////// Temperature /////////////////////////////////////
void YBoardV3::setup_temperature() {
    if (!wire_begin) {
        Wire.begin(sda_pin, scl_pin);
        wire_begin = true;
    }

    if (!aht.begin(&Wire)) {
        Serial.println("WARNING: Could not find temperature sensor.");
    }
}

void YBoardV3::get_temperature(float *temperature, float *humidity) {
    sensors_event_t h, t;
    aht.getEvent(&h, &t);

    *temperature = t.temperature;
    *humidity = h.relative_humidity;
}
