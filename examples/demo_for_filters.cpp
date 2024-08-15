#include "Arduino.h"
#include "yboard.h"
// File names for SD card
static const std::string FILE_NAME = "/recording.wav";
static const std::string OUT_FILE_NAME = "/filtered.wav";

// For LED breathing effect
static const float smoothness_pts = 500;    // larger=slower change in brightness
static const float smoothness_gamma = 0.14; // affects the width of peak (more or less darkness)
static const float smoothness_beta = 0.5;   // shifts the gaussian to be symmetric
unsigned long previousMillis = 0; // Stores last time the action was performed
unsigned int smoothness_index = 0;

// For recording and playing audio
static const int sound_volume = 6;
static const int recording_volume = 3;
static const int cutoff_frequency = 1500; // cufoff frequency for filters

// This is all logic to have the LEDs continously change brightness. You DON'T need to modify this.
void led_breathing(unsigned long currentMillis) {
    
    if (currentMillis - previousMillis >= 5) {
        // Save the last time you performed the action
        previousMillis = currentMillis;

        float pwm_val =
            255.0 *
            (exp(-(pow(((smoothness_index / smoothness_pts) - smoothness_beta) / smoothness_gamma,
                       2.0)) /
                 2.0));
        Yboard.set_all_leds_color(int(pwm_val), int(pwm_val), int(pwm_val));

        // Increment the index and reset if it exceeds the number of points
        smoothness_index++;
        if (smoothness_index >= smoothness_pts) {
            smoothness_index = 0;
        }
    }
}

void setup() {
    Serial.begin(9600);
    Yboard.setup();
    Yboard.set_sound_file_volume(sound_volume);
    Yboard.set_recording_volume(recording_volume);
    Yboard.set_all_leds_color(255, 255, 255);
}


// You will edit code in this section
void loop() {
    unsigned long currentMillis = millis();

    //If button one is pressed, start recording
    if (Yboard.get_button(1)) {
        bool started_recording = Yboard.start_recording(FILE_NAME);
        while (Yboard.get_button(1)) {
            if (started_recording) {
                Yboard.set_all_leds_color(255, 0, 0); // Set the LEDs to Red while recording
                Yboard.display_text("Recording",1);
                delay(100);
            } else {
                Yboard.set_all_leds_color(100, 100, 100);
                delay(100);
                Yboard.set_all_leds_color(0, 0, 0);
                delay(100);
            }
        }

        if (started_recording) {
            delay(200); // This is to make sure you finish the recording before stopping
            Yboard.stop_recording();
        }
        
        Yboard.set_all_leds_color(0, 0, 0);
        Yboard.clear_display();

    }
    
    if (Yboard.get_button(2) && !Yboard.get_switch(2)) {
        //Just play the original sound file
        Yboard.set_all_leds_color(0, 0, 255); // Set the LEDs to green while playing the audio
        Yboard.display_text("Playing: " + FILE_NAME,1);
        Yboard.play_sound_file(FILE_NAME);
        Yboard.set_all_leds_color(0, 0, 0);
        Yboard.clear_display();
    }

    //If button two is pressed, play the sound file
    //If switch two is on, apply a filter to the sound file
    
    if (Yboard.get_button(2) && Yboard.get_switch(2)) {
        //Apply the filter and save the audio to a new file
        Yboard.set_all_leds_color(0, 0, 255); // Set the LEDs to blue while applying the filter
        Yboard.display_text("Applying filter",1);
        Yboard.bandRejectFilter(FILE_NAME, OUT_FILE_NAME, 410, 470);
        
        
        //Play the new sound file
        Yboard.set_sound_file_volume(10); // Increase the volume as the filter reduces it
        Yboard.set_all_leds_color(0, 255, 0); // Set the LEDs to green while playing the audio
        Yboard.display_text("Playing: " + OUT_FILE_NAME,1);
        Yboard.play_sound_file(OUT_FILE_NAME);
        Yboard.set_all_leds_color(0, 0, 0);
        Yboard.clear_display();
        Yboard.set_sound_file_volume(sound_volume); // Reset the volume
    }
    
    

led_breathing(currentMillis);
    
}

