#ifndef YBOARDV3_H
#define YBOARDV3_H

#include <Adafruit_AHTX0.h>
#include <Adafruit_NeoPixel.h>
#include <FS.h>
#include <SD.h>
#include <SparkFun_LIS2DH12.h>
#include <stdint.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "yaudio.h"

struct accelerometer_data {
    float x;
    float y;
    float z;
};

struct temperature_data {
    float temperature;
    float humidity;
};

class YBoardV3 {
  public:
    YBoardV3();
    virtual ~YBoardV3();

    /*
     *  This function initializes the YBoard. This function must be called before using any of the
     * YBoard features.
     */
    void setup();

    ////////////////////////////// LEDs ///////////////////////////////////////////

    /*
     *  This function sets the color of an individual LED.
     *  The index is an integer between 1 and 20, representing the number of the
     * target LED (for example, 1 corresponds to the first LED on the board).
     *  The red, green, and blue values are integers between 0 and 255, representing
     * the intensity of the corresponding color. For example, if you want to set the
     * LED to red, you would set red to 255 and green and blue to 0.
     */
    void set_led_color(uint16_t index, uint8_t red, uint8_t green, uint8_t blue);

    /*
     *  This function sets the brightness of all the LEDs on the board.
     *  The brightness is an integer between 0 and 255, representing the intensity
     * of the LEDs. A brightness of 0 is off, and a brightness of 255 is full
     * brightness.
     */
    void set_led_brightness(uint8_t brightness);

    /*
     *  This function sets the color of all the LEDs on the board.
     *  The red, green, and blue values are integers between 0 and 255, representing
     * the intensity of the corresponding color. For example, if you want to set all
     * the LEDs to red, you would set red to 255 and green and blue to 0.
     */
    void set_all_leds_color(uint8_t red, uint8_t green, uint8_t blue);

    ////////////////////////////// Switches/Buttons ///////////////////////////////
    /*
     *  This function returns the state of a switch.
     *  The switch_idx is an integer between 1 and 2, representing the number of the
     * target switch (for example, 1 corresponds to switch 1 on the board). The bool
     * return type means that this function returns a boolean value (true or false).
     *  True corresponds to the switch being on, and false corresponds to the switch
     * being off.
     */
    bool get_switch(uint8_t switch_idx);

    /*
     *  This function returns the state of a button.
     *  The button_idx is an integer between 1 and 2, representing the number of the
     * target button (for example, 1 corresponds to button 1 on the board). The bool
     * return type means that this function returns a boolean value (true or false).
     *  True corresponds to the button being pressed, and false corresponds to the
     * button being released.
     */
    bool get_button(uint8_t button_idx);

    /*
     *  This function returns the value of the knob.
     *  The return type is an integer between 0 and 100, representing the position
     * of the knob. A value of 0 corresponds to the knob being turned all the way to
     * the left, and a value of 100 corresponds to the knob being turned all the way
     * to the right.
     */
    int get_knob();

    ////////////////////////////// Speaker/Tones //////////////////////////////////
    /*
     *  This function continues to play a sound on the speaker after the
     * play_sound_file function is called. This function must be called often to
     * playback the sound on the speaker.
     */
    void loop_speaker();

    /*
     *  This function plays a sound on the speaker. The filename is a string
     * representing the name of the sound file to play. The return type is a boolean
     * value (true or false). True corresponds to the sound being played
     * successfully, and false corresponds to an error playing the sound. The sound
     * file must be stored on the microSD card.
     */
    bool play_sound_file(const std::string &filename);

    /* This is similar to the function above, except that it will start the song playing
     * in the background and return immediately. The song will continue to play in the
     * background until it is stopped with the stop_audio function, another song is
     * played, play_notes is called, or the song finishes. After this function is called, the
     * loop_speaker function must be called often to playback the sound on the speaker.
     */
    bool play_sound_file_background(const std::string &filename);

    /*
     * This function sets the speaker volume when playing a sound file. The volume
     * is an integer between 0 and 10. A volume of 0 is off, and a volume of 10 is full volume.
     * This does not affect the volume of notes played with the play_notes function, as
     * their volume is set with the V command in the notes themselves.
     */
    void set_sound_file_volume(uint8_t volume);

    /* Plays the specified sequence of notes. The function will return once the notes
     * have finished playing.
     *
     * A–G	                Specifies a note that will be played.
     * R                    Specifies a rest (no sound for the duration of the note).
     * + or # after a note  Raises the preceding note one half-step (sharp).
     * - after a note	      Lowers the preceding note one half-step.
     * > after a note	      Plays the note one octave higher (multiple >’s can be used, eg: C>>)
     * < after a note	      Plays the note one octave lower (multiple <’s can be used, eg: C<<)
     * 1–2000 after a note	Determines the duration of the preceding note. For example,
     *                      C16 specifies C played as a sixteenth note, B1 is B played as a whole
     *                      note. If no duration is specified, the note is played as a quarter note.
     * O followed by a #    Changes the octave. Valid range is 4-7. Default is 5.
     * T followed by a #    Changes the tempo. Valid range is 40-240. Default is 120.
     * V followed by a #    Changes the volume.  Valid range is 1-10. Default is 5.
     * !                    Resets octave, tempo, and volume to default values.
     * spaces               Spaces can be placed between notes or commands for readability,
     *                      but not within a note or command (eg: "C4# D4" is valid, "C 4 # D 4" is
     *                      not. "T120 A B C" is valid, "T 120 A B C" is not).
     */
    bool play_notes(const std::string &new_notes);

    /* This is similar to the function above, except that it will start playing the notes
     * in the background and return immediately. The notes will continue to play in the
     * background until they are stopped with the stop_audio function, a WAVE file is played,
     * or the notes finish. If you call this function again before the notes finish, the
     * the new notes will be appended to the end of the current notes.  This allows you to
     * call this function multiple times to build up multiple sequences of notes to play.
     * After this function is called, the loop_speaker function must be called often to
     * playback the sound on the speaker.
     */
    bool play_notes_background(const std::string &new_notes);

    /*
     * This function stops the audio from playing (either a song or a sequence of notes)
     */
    void stop_audio();

    /*
     *  This function returns whether audio is playing.
     */
    bool is_audio_playing();

    ////////////////////////////// Microphone ////////////////////////////////////////
    /*
     *  This function starts recording audio from the microphone. The filename is a
     * string representing the name of the file to save the recording to. The return
     * type is a boolean value (true or false). True corresponds to the recording
     * starting successfully, and false corresponds to an error starting the recording.
     * The recording will continue until stop_recording is called.
     */
    bool start_recording(const std::string &filename);

    /*
     *  This function stops recording audio from the microphone.
     */
    void stop_recording();

    /*
     *  This function returns whether the microphone is currently recording.
     */
    bool is_recording();

    /*
     *  This function sets the volume of the microphone when recording. The volume is
     * an integer between 0 and 12. A volume of 0 is off, and a volume of 12 is full volume.
     */
    void set_recording_volume(uint8_t volume);

    ///////////////////////////// Accelerometer ////////////////////////////////////
    /*
     *  This function returns whether accelerometer data is available.
     *  The bool return type means that this function returns a boolean value (true
     * or false). True corresponds to accelerometer data being available, and false
     * corresponds to accelerometer data not being available.
     */
    bool accelerometer_available();

    /*
     *  This function returns the accelerometer data.
     *  The return type is a struct containing the x, y, and z values of the
     * accelerometer data. These values are floats, representing the acceleration
     * in the x, y, and z directions, respectively.
     */
    accelerometer_data get_accelerometer();

    /////////////////////////////// Temperature /////////////////////////////////////

    /*
     *  This function returns the temperature and humidity data.
     *  The return type is a struct with temperature and humidity fields.
     * These values are floats.
     */
    temperature_data get_temperature();


    /*
     *  This fucntion applies a high pass filter to audio data in a .wav file.
     *  The first field is a string representing the path to the input file.
     *  The second is a string representing the path to the output file.
     *  The third is an integer representing the cutoff frequency of the high pass filter.
     *  Returns true for successful processing and false for an error in reading or writing to the SD card
     */
    bool apply_high_pass(const std::string &inputFilePath, const std::string &outputFilePath, int cuttoff_freq);
    /*
     *  This fucntion applies a low pass filter to audio data in a .wav file.
     *  The fields and parameters are the same as the apply_high_pass function.
     */
    bool apply_low_pass(const std::string &inputFilePath, const std::string &outputFilePath, int cuttoff_freq);
    /*
     *  This fucntion applies a band reject filter to audio data in a .wav file.
     *  The fields and parameters are the same as the other filter functions but with low and high cutoff frequencies.
     */
    bool apply_band_reject(const std::string &inputFilePath, const std::string &outputFilePath,int low_cuttoff, int high_cutoff);

    /*
      *  This function displays text on the OLED screen. Takes in a parameter text size
     */
    void display_text(const std::string &text, const int text_size);
    //   Clears the display
    void clear_display();

    // LEDs
    static constexpr int led_pin = 5;
    static constexpr int led_count = 20;

    // Controls
    static constexpr int knob_pin = 9;
    static constexpr int switch1_pin = 16;
    static constexpr int switch2_pin = 18;
    static constexpr int button1_pin = 17;
    static constexpr int button2_pin = 7;

    // I2C Connections
    static constexpr int sda_pin = 2;
    static constexpr int scl_pin = 1;

    // I2C Devices
    static constexpr int accel_addr = 0x19;

    // microSD Card Reader connections
    static constexpr int sd_cs_pin = 10;
    static constexpr int spi_mosi_pin = 11;
    static constexpr int spi_miso_pin = 13;
    static constexpr int spi_sck_pin = 12;

    // I2S Connections
    static constexpr int i2s_dout_pin = 14;
    static constexpr int i2s_bclk_pin = 21;
    static constexpr int i2s_lrc_pin = 47;

  private:
    Adafruit_NeoPixel strip;
    SPARKFUN_LIS2DH12 accel;
    Adafruit_AHTX0 aht;
    Adafruit_SSD1306 display;

    // Screen Constants
    float brightness_damper = 0.8; // 0 is brightest
    int refresh_rate  = 50;     // Measured in ms
    bool wire_begin = false;
    bool sd_card_present = false;

    void setup_leds();
    void setup_switches();
    void setup_buttons();
    bool setup_speaker();
    bool setup_mic();
    bool setup_accelerometer();
    bool setup_temperature();
    bool setup_sd_card();
    bool setup_display();

    
};

extern YBoardV3 Yboard;

#endif /* YBOARDV3_H */
