#include "yaudio.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <driver/i2s.h>

namespace YAudio {

///////////////////////////////// Configuration Constants //////////////////////
i2s_port_t I2S_PORT = I2S_NUM_0;

// The number of bits per sample.
static const int BITS_PER_SAMPLE = 16;
static const int BYTES_PER_SAMPLE = BITS_PER_SAMPLE / 8;
static const int SAMPLE_RATE = 16000; // sample rate in Hz
static const int MAX_NOTES_IN_BUFFER = 4000;

// The number of frames of valid PCM audio data in the audio buffer. This will be incremented when
// we add a note to the audio buffer, and decremented when we write a frame to the I2S buffer.
static int audio_buf_num_populated_frames;

// The audio buffer. This is where we store the PCM audio data that we want to play.
// This is sized large enough to hold an almost full frame (1023 bytes), plus the next note.
// The largest note is a whole note at tempo = 40bps, sample rate = 16000,
// which is 4*(60/40)*16000 = 96000 samples.
// 96000 samples + 1023 = 97023, which is less than 96 frames (96 * 1024 = 98208)
static const int FRAME_SIZE = 1024;
static const int AUDIO_BUF_NUM_FRAMES = 100;
static int16_t audio_buf[FRAME_SIZE * AUDIO_BUF_NUM_FRAMES];

// This is the number of WAVE frames to buffer. Making this larger will reduce the chance of
// speaker crackle, but will also increase the delay between changing the volume and hearing the
// change.  Given a frame size of 1024 and a sample rate of 16000, this is 1024 * 5 / 16000 = 0.32s
static const int WAVE_FRAMES_TO_BUFFER = 5;

////////////////////////////// Global Variables ////////////////////////////////
// This is the index of the next frame to write to the I2S buffer.
static int audio_buf_frame_idx_to_send;

// This is the index into the audio buffer of the next sample to write to
static int audio_buf_empty_idx;

static bool i2s_running;
static bool notes_running;
static bool wave_running;

////// Notes //////
// This is the sequence of notes to play
static std::string notes;

// Notes state
static int beats_per_minute;
static int octave;
static int volume_notes;

// Next note to play
static bool next_note_parsed;
static float next_note_freq;
static float next_note_duration_s;

////// Wave file //////
static fs::File file;
static int volume_wave;

//////////////////////////// Private Function Prototypes ///////////////////////
// Local private functions
static void generate_sine_wave(double frequency, int num_samples, double amplitude);
static void write_next_note_to_audio_buf();
static void parse_next_note();
static void set_note_defaults();
static void start_i2s();

////////////////////////////// Public Functions ///////////////////////////////
bool add_notes(const std::string &new_notes) {
    // If WAVE is running, then stop it and reset the audio buffer.
    // Otherwise if notes are running, it's fine to let them keep running.
    if (wave_running) {
        stop();
    }

    if ((notes.length() + new_notes.length()) > MAX_NOTES_IN_BUFFER) {
        Serial.printf("Error adding notes: too many notes in buffer (%d + %d > %d).\n",
                      new_notes.length(), notes.length(), MAX_NOTES_IN_BUFFER);
        return false;
    }

    // Removing ending z, which is used as a small rest at the end to prevent speaker crackle
    if (notes.length() && (notes[notes.size() - 1] == 'z')) {
        notes.pop_back();
    }

    // Append the new notes to the existing notes, and add an ending rest
    notes += new_notes + "z";

    notes_running = true;
    if (!i2s_running) {
        start_i2s();
    }

    return true;
}

////////////////////////////// Private Functions ///////////////////////////////

void start_i2s() {
    if (i2s_running) {
        return;
    }
    i2s_start(I2S_PORT);
    i2s_running = true;
}

static void generate_silence(float duration_s) {
    int num_samples = duration_s * SAMPLE_RATE;

    for (int i = 0; i < num_samples; i++) {
        int idx = audio_buf_empty_idx ^ 1;

        audio_buf[idx] = 0;
        audio_buf_empty_idx = (audio_buf_empty_idx + 1) % (FRAME_SIZE * AUDIO_BUF_NUM_FRAMES);
        if (audio_buf_empty_idx % FRAME_SIZE == 0) {
            // Serial.println("Frame filled");
            audio_buf_num_populated_frames++;
            yield();
        }
    }
}

static void generate_sine_wave(float duration_s, double frequency, double amplitude) {
    const float FADE_IN_FRAC = 0.02;
    const float FADE_OUT_FRAC = 0.02;
    int num_samples = duration_s * SAMPLE_RATE;

    for (int i = 0; i < num_samples; i++) {
        int16_t val;

        double _amp;

        float frac = i / (double)num_samples;
        if (frac < FADE_IN_FRAC) {
            _amp = amplitude * (frac / FADE_IN_FRAC);
        } else if (frac > 1 - FADE_OUT_FRAC) {
            _amp = amplitude * (1 - ((frac - (1 - FADE_OUT_FRAC)) / FADE_OUT_FRAC));
        } else {
            _amp = amplitude;
        }
        val = _amp * sin(2 * 3.14 * frequency * i / SAMPLE_RATE);

        int idx = audio_buf_empty_idx ^ 1;
        audio_buf[idx] = val;

        // Serial.printf("Wrote %d to index %d\n", audio_buf[idx], idx);

        audio_buf_empty_idx = (audio_buf_empty_idx + 1) % (FRAME_SIZE * AUDIO_BUF_NUM_FRAMES);
        if (audio_buf_empty_idx % FRAME_SIZE == 0) {
            audio_buf_num_populated_frames++;
            yield();
            // Serial.printf("Writing frame. # populated: %d\n", audio_buf_num_populated_frames);
        }
    }
}

static QueueHandle_t i2s_event_queue;
int I2S_Q_LEN = 10;
bool TXdoneEvent = false;

void I2Sout(void *params) {
    int retv;
    i2s_event_t i2s_evt;

    while (1) {
        TXdoneEvent = false;
        do // wait on I2S event queue until a TX_DONE is found
        {
            retv = xQueueReceive(
                i2s_event_queue, &i2s_evt,
                1); // don't let this block for long, as we check for the queue stalling
            if ((retv == pdPASS) &&
                (i2s_evt.type == I2S_EVENT_TX_DONE)) // I2S DMA finish sent 1 buffer
            {
                TXdoneEvent = true;
                break;
            }
            vTaskDelay(1); // make sure there's time for some other processing if we need to wait!
        } while (retv == pdPASS);

        if (TXdoneEvent) {
            if (audio_buf_num_populated_frames >= 0) {
                size_t bytes_written;
                i2s_write(I2S_PORT, &audio_buf[audio_buf_frame_idx_to_send * FRAME_SIZE],
                          FRAME_SIZE * BYTES_PER_SAMPLE, &bytes_written, portMAX_DELAY);

                audio_buf_frame_idx_to_send =
                    (audio_buf_frame_idx_to_send + 1) % AUDIO_BUF_NUM_FRAMES;
                audio_buf_num_populated_frames--;
            }
            // Serial.printf("Frame sent. # populated: %d\n", audio_buf_num_populated_frames);
        }
    }
}

void set_note_defaults() {
    beats_per_minute = 120;
    octave = 5;
    volume_notes = 5;
}

void reset_audio_buf() {
    audio_buf_num_populated_frames = 0;
    audio_buf_frame_idx_to_send = 0;
    audio_buf_empty_idx = 0;
    next_note_parsed = false;
    notes = "";
}

void setup() {
    // Initialize global variables
    reset_audio_buf();
    set_note_defaults();
    i2s_running = false;
    notes_running = false;
    wave_running = false;
    volume_wave = 5;

    const i2s_config_t i2s_config = {
        .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX), // Receive, not transfer
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // could only get it to work with 32bits
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // use left channel
        .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, // Interrupt level 1

        .dma_buf_count = 2,
        .dma_buf_len = 1024,
        .use_apll = 0};

    const i2s_pin_config_t pin_config = {
        .bck_io_num = 21, .ws_io_num = 47, .data_out_num = 14, .data_in_num = I2S_PIN_NO_CHANGE};

    int err;

    err = i2s_driver_install(I2S_PORT, &i2s_config, I2S_Q_LEN, &i2s_event_queue);
    if (err != ESP_OK) {
        Serial.printf("Failed installing I2S driver: %d\n", err);
        return;
    }

    TaskHandle_t I2StaskHandle;
    xTaskCreate(I2Sout, "I2Sout", 20000, NULL, 1, &I2StaskHandle);

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("Failed setting I2S pin configuration: %d\n", err);
        return;
    }

    i2s_running = true;
    // Serial.println("I2S setup complete and running");
}

void write_next_note_to_audio_buf() {

    // Check if we have enough space in the buffer to add the note
    int avail_space = (AUDIO_BUF_NUM_FRAMES - audio_buf_num_populated_frames - 1) * FRAME_SIZE;
    int note_samples = next_note_duration_s * SAMPLE_RATE;

    if (avail_space > note_samples) {

        int amplitude = 16000 * (volume_notes / 10.0);
        if (next_note_freq < 800) {
            amplitude *= 2;
        } else if (next_note_freq < 1100) {
            // Perform linear scaling of volume for frequencies between 800 and 1100
            amplitude = amplitude * (next_note_freq - 800) / 300;
        }

        if (next_note_freq == 0) {
            generate_silence(next_note_duration_s);
        } else {
            generate_sine_wave(next_note_duration_s, next_note_freq, amplitude);
        }
        next_note_parsed = false;
    }
}

void parse_next_note() {
    while (notes.length()) {
        // If first character is white space, remove it and continue
        if (isspace(notes[0])) {
            notes.erase(0, 1);
            continue;
        }

        // Octave
        if (notes[0] == 'O' || notes[0] == 'o') {
            int new_octave = notes[1] - '0';
            if (new_octave >= 4 && new_octave <= 7) {
                octave = new_octave;
            }
            notes.erase(0, 2);
            continue;
        }

        // Tempo
        if (notes[0] == 'T' || notes[0] == 't') {
            notes.erase(0, 1);
            size_t pos;
            int new_tempo = std::stoi(notes, &pos);
            notes = notes.substr(pos);
            if (new_tempo >= 40 && new_tempo <= 240) {
                beats_per_minute = new_tempo;
            }
            continue;
        }

        // Reset
        if (notes[0] == '!') {
            set_note_defaults();
            notes.erase(0, 1);
            continue;
        }

        // Volume
        if (notes[0] == 'V' || notes[0] == 'v') {
            notes.erase(0, 1);
            size_t pos;
            int new_volume = std::stoi(notes, &pos);
            notes = notes.substr(pos);
            if (new_volume >= 1 && new_volume <= 10) {
                volume_notes = new_volume;
            }
            continue;
        }

        next_note_duration_s = (60.0 / beats_per_minute); // Quarter note duration in seconds

        // A-G regular notes
        // R for rest
        // z for end rest, which is added internally to stop speaker crackle at the end
        if ((notes[0] >= 'A' && notes[0] <= 'G') || (notes[0] >= 'a' && notes[0] <= 'g') ||
            notes[0] == 'R' || notes[0] == 'r' || notes[0] == 'z') {
            switch (notes[0]) {
            case 'A':
            case 'a':
                next_note_freq = 440.0;
                break;
            case 'B':
            case 'b':
                next_note_freq = 493.88;
                break;
            case 'C':
            case 'c':
                next_note_freq = 523.25;
                break;
            case 'D':
            case 'd':
                next_note_freq = 587.33;
                break;
            case 'E':
            case 'e':
                next_note_freq = 659.25;
                break;
            case 'F':
            case 'f':
                next_note_freq = 698.46;
                break;
            case 'G':
            case 'g':
                next_note_freq = 783.99;
                break;
            case 'z':
                next_note_duration_s = 0.2;
                // Fallthough
            case 'R':
            case 'r':
                next_note_freq = 0;
                break;
            }

            // Adjust frequency for octave
            next_note_freq *= pow(2, octave - 4);
            notes.erase(0, 1);

            float dot_duration = next_note_duration_s;

            // Note modifiers
            while (1) {

                // Duration
                if (isdigit(notes[0])) {
                    size_t pos;
                    int frac_duration = std::stoi(notes, &pos);
                    notes = notes.substr(pos);
                    if (frac_duration >= 1 && frac_duration <= 2000) {
                        next_note_duration_s = next_note_duration_s * (4.0 / frac_duration);
                    }
                    continue;
                }

                // Dot
                if (notes[0] == '.') {
                    dot_duration /= 2;
                    next_note_duration_s += dot_duration;
                    notes.erase(0, 1);
                    continue;
                }

                // Octave
                if (notes[0] == '>') {
                    next_note_freq *= 2;
                    notes.erase(0, 1);
                    continue;
                }
                if (notes[0] == '<') {
                    next_note_freq /= 2;
                    notes.erase(0, 1);
                    continue;
                }

                // Sharp/flat
                if (notes[0] == '#' || notes[0] == '+' || notes[0] == '-') {
                    if (notes[0] == '#' || notes[0] == '+') {
                        next_note_freq *= pow(2, 1.0 / 12);
                    } else {
                        next_note_freq /= pow(2, 1.0 / 12);
                    }
                    notes.erase(0, 1);
                    continue;
                }

                break;
            }
            break;
        }

        // X<>M<> format for specific frequency (X) and duration (Milliseconds)
        if (notes[0] == 'X' || notes[0] == 'x') {
            notes.erase(0, 1);
            size_t x_pos;
            next_note_freq = std::stof(notes, &x_pos);
            notes = notes.substr(x_pos);

            if (!(next_note_freq >= 20 && next_note_freq <= 20000)) {
                next_note_freq = 0;
            }

            if (notes[0] == 'M' || notes[0] == 'm') {
                notes.erase(0, 1);
                size_t m_pos;
                next_note_duration_s = std::stof(notes, &m_pos) / 1000.0;
                notes = notes.substr(m_pos);
            }
            break;
        }

        // If we reach here then we have a syntax error
        Serial.printf("Syntax error in notes: %s\n", notes.c_str());
        notes = "";
        break;
    }
    next_note_parsed = true;
}

void loop() {
    if (notes_running) {
        // Parse the next note
        if (notes.length() && !next_note_parsed) {
            parse_next_note();
        }

        // Play the next note
        if (next_note_parsed) {
            write_next_note_to_audio_buf();
        }

        // Check if we should stop playing
        if (notes.length() == 0 && !next_note_parsed && audio_buf_num_populated_frames == 0 &&
            i2s_running) {
            stop();
        }
    } else if (wave_running) {
        if (file.available() && audio_buf_num_populated_frames < WAVE_FRAMES_TO_BUFFER) {
            int bytes_to_read = FRAME_SIZE * BYTES_PER_SAMPLE;

            size_t bytes_read =
                file.read((uint8_t *)&audio_buf[audio_buf_empty_idx], bytes_to_read);
            yield();
            if (bytes_read == bytes_to_read) {
                // Swap even and odd samples, and apply volume
                for (int i = 0; i < FRAME_SIZE; i += 2) {
                    int16_t temp = audio_buf[audio_buf_empty_idx + i];
                    audio_buf[audio_buf_empty_idx + i] =
                        audio_buf[audio_buf_empty_idx + i + 1] * volume_wave / 10.0;
                    audio_buf[audio_buf_empty_idx + i + 1] = temp * volume_wave / 10.0;
                }

                audio_buf_empty_idx =
                    (audio_buf_empty_idx + FRAME_SIZE) % (FRAME_SIZE * AUDIO_BUF_NUM_FRAMES);
                audio_buf_num_populated_frames++;
                // Serial.printf("Frame filled. # populated: %d\n", audio_buf_num_populated_frames);
            }
        } else if (audio_buf_num_populated_frames <= 0) {
            stop();
        }
    }
}

void set_wave_volume(uint8_t new_volume) { volume_wave = new_volume > 10 ? 10 : new_volume; }

void stop() {
    if (i2s_running) {
        i2s_stop(I2S_PORT);
        i2s_running = false;
    }
    if (wave_running) {
        file.close();
        wave_running = false;
    }
    if (notes_running) {
        notes_running = false;
        notes = "";
    }
    i2s_zero_dma_buffer(I2S_PORT);
    reset_audio_buf();
}

bool is_playing() { return i2s_running; }

bool play_sound_file(const std::string &filename) {
    // Whether notes or wave is running, stop it
    stop();

    // Read the WAVE file header
    file = SD.open(filename.c_str());
    uint8_t header[44];
    int bytes_read = file.read(header, 44);
    if (bytes_read != 44) {
        Serial.println("Error reading WAVE file header");
        file.close();
        return false;
    }

    int num_channels = *(uint16_t *)&header[22];
    if (num_channels != 1) {
        Serial.printf("This file has %f channels. Only mono WAVE files are supported.",
                      num_channels);
        file.close();
        return false;
    }

    uint32_t sample_rate = *(uint32_t *)&header[24];
    if (sample_rate != SAMPLE_RATE) {
        Serial.printf("This file has a sample rate of %d. Only %d Hz sample rate is supported\n",
                      sample_rate, SAMPLE_RATE);
        file.close();
        return false;
    }

    start_i2s();
    wave_running = true;
    return true;
}

}; // namespace YAudio
