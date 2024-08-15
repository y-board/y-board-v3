#include "yaudio.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <driver/i2s.h>

namespace YAudio {

static const int MIC_SAMPLE_RATE = 16000;
static const int MIC_ORIGINAL_SAMPLE_BITS = 32;
static const int MIC_CONVERTED_SAMPLE_BITS = 16;
static const int MIC_READ_BUF_SIZE = 2048;
static const int MIC_NUM_CHANNELS = 1;
static const i2s_port_t MIC_I2S_PORT = I2S_NUM_1;

// Wave header as struct
typedef struct {
    char riff_tag[4];
    uint32_t riff_length;
    char wave_tag[4];
    char fmt_tag[4];
    uint32_t fmt_length;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_tag[4];
    uint32_t data_length;
} wave_header_t;

//filter coefficients as a struct
typedef struct {
    double a0, a1, a2, b0, b1, b2, x1, x2, y1, y2;
} filter_coefficients_t;

uint32_t i2s_read_buff[MIC_READ_BUF_SIZE];
uint16_t file_write_buff[MIC_READ_BUF_SIZE];

static bool recording_audio = false;
static bool done_recording_audio = true;
static File speaker_recording_file;

static int recording_gain = 5;

///////////////////////////////// Configuration Constants //////////////////////
i2s_port_t SPEAKER_I2S_PORT = I2S_NUM_0;

// The number of bits per sample.
static const int SPEAKER_BITS_PER_SAMPLE = 16;
static const int SPEAKER_BYTES_PER_SAMPLE = SPEAKER_BITS_PER_SAMPLE / 8;
static const int SPEAKER_SAMPLE_RATE = 16000; // sample rate in Hz
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

static QueueHandle_t i2s_event_queue;
int I2S_Q_LEN = 10;
bool TXdoneEvent = false;

//////////////////////////// Private Function Prototypes ///////////////////////
// Local private functions
static void generate_sine_wave(double frequency, int num_samples, double amplitude);
static void write_next_note_to_audio_buf();
static void parse_next_note();
static void set_note_defaults();
static void reset_audio_buf();
static void start_i2s();
static void I2Sout(void *params);
static void create_wave_header(wave_header_t *header, int data_length);
static void convert_samples(uint16_t *dest, const uint32_t *src, int num_samples);

int16_t filter(int16_t& input, filter_coefficients_t& coefficients) {
    
    double output = coefficients.b0 * input + coefficients.b1 * coefficients.x1 + coefficients.b2 * coefficients.x2 - coefficients.a1 * coefficients.y1 - coefficients.a2 * coefficients.y2;
    coefficients.x2 = coefficients.x1;
    coefficients.x1 = input;
    coefficients.y2 = coefficients.y1;
    coefficients.y1 = output;

    if (output > INT16_MAX) {
        output = INT16_MAX;
    }
    return int16_t(output);
}
filter_coefficients_t calculate_filter_coefficients_butterworth(int cuttoff_freq, int sample_rate, bool high_pass) {
    filter_coefficients_t coefficients;
    coefficients.x1 = 0;
    coefficients.x2 = 0;
    coefficients.y1 = 0;
    coefficients.y2 = 0;
    double omega = 2.0 * PI * cuttoff_freq / sample_rate;
    double sn = sin(omega);
    double cs = cos(omega);
    double alpha = sn / (2.0 * sqrt(2.0)); // Q = sqrt(2)/2 for Butterworth
    double norm = 1.0 / (1.0 + alpha);
    coefficients.b0 = high_pass ? (1.0 + cs) / 2.0 * norm : (1.0 - cs) / 2.0 * norm;
    coefficients.b1 = high_pass ? -(1.0 + cs) * norm : (1.0 - cs) * norm;
    coefficients.b2 = coefficients.b0;
    coefficients.a1 = -2.0 * cs * norm;
    coefficients.a2 = (1.0 - alpha) * norm;
    return coefficients;
}

filter_coefficients_t calculate_filter_coefficients_biquad(int low_cutoff, int high_cutoff, int sample_rate) {
    filter_coefficients_t coefficients;
    coefficients.x1 = 0;
    coefficients.x2 = 0;
    coefficients.y1 = 0;
    coefficients.y2 = 0;

    int center_freq = (low_cutoff + high_cutoff) / 2;
    int bandwidth = high_cutoff - low_cutoff;

    double omega = 2.0 * PI * center_freq / sample_rate;
    double alpha = std::sin(omega) * std::sinh(std::log(2.0) / 2.0 * bandwidth * omega / std::sin(omega));
    alpha = 1;
    
    

    coefficients.b0 = 1.0;
    coefficients.b1 = -2.0 * std::cos(omega);
    coefficients.b2 = 1.0;
    coefficients.a0 = 1.0 + alpha;
    coefficients.a1 = -2.0 * std::cos(omega);
    coefficients.a2 = 1.0 - alpha;

    // Normalize the coefficients
    coefficients.b0 /= coefficients.a0;
    coefficients.b1 /= coefficients.a0;
    coefficients.b2 /= coefficients.a0;
    coefficients.a1 /= coefficients.a0;
    coefficients.a2 /= coefficients.a0;


    

    return coefficients;
}

////////////////////////////// Public Functions ///////////////////////////////
bool setup_speaker() {
    esp_err_t err;

    // Initialize global variables
    reset_audio_buf();
    set_note_defaults();
    i2s_running = false;
    notes_running = false;
    wave_running = false;
    volume_wave = 5;

    const i2s_config_t i2s_config_speaker = {
        .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX), // Receive, not transfer
        .sample_rate = SPEAKER_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // could only get it to work with 32bits
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // use left channel
        .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, // Interrupt level 1

        .dma_buf_count = 2,
        .dma_buf_len = 1024,
        .use_apll = 0};

    const i2s_pin_config_t pin_config_speaker = {
        .bck_io_num = 21, .ws_io_num = 47, .data_out_num = 14, .data_in_num = I2S_PIN_NO_CHANGE};

    err = i2s_driver_install(SPEAKER_I2S_PORT, &i2s_config_speaker, I2S_Q_LEN, &i2s_event_queue);
    if (err != ESP_OK) {
        Serial.printf("Failed installing I2S driver: %d\n", err);
        return false;
    }

    xTaskCreate(I2Sout, "I2Sout", 20000, NULL, 1, NULL);

    err = i2s_set_pin(SPEAKER_I2S_PORT, &pin_config_speaker);
    if (err != ESP_OK) {
        Serial.printf("Failed setting I2S pin configuration: %d\n", err);
        return false;
    }

    i2s_running = true;

    Serial.println("I2S setup complete and running for speaker");
    return true;
}

bool setup_mic() {
    esp_err_t err;

    const i2s_config_t i2s_config_mic = {

        .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX), // Receive, not transfer
        .sample_rate = MIC_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // could only get it to work with 32bits
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // use left channel
        .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, // Interrupt level 1

        .dma_buf_count = 2,
        .dma_buf_len = 1024,
        .use_apll = 1};

    const i2s_pin_config_t pin_config_mic = {
        .bck_io_num = 42, .ws_io_num = 41, .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = 40};

    err = i2s_driver_install(MIC_I2S_PORT, &i2s_config_mic, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("Failed installing driver: %d\n", err);
        return false;
    }

    REG_SET_BIT(I2S_RX_TIMING_REG(MIC_I2S_PORT), BIT(0));
    REG_SET_BIT(I2S_RX_CONF1_REG(MIC_I2S_PORT), I2S_RX_MSB_SHIFT);

    err = i2s_set_pin(MIC_I2S_PORT, &pin_config_mic);
    if (err != ESP_OK) {
        Serial.printf("Failed setting pin: %d\n", err);
        return false;
    }

    Serial.println("I2S setup complete for mic");
    return true;
}

void set_wave_volume(uint8_t new_volume) { volume_wave = new_volume > 10 ? 10 : new_volume; }

void stop_speaker() {
    if (i2s_running) {
        i2s_stop(SPEAKER_I2S_PORT);
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
    i2s_zero_dma_buffer(SPEAKER_I2S_PORT);
    reset_audio_buf();
}

bool is_playing() { return i2s_running; }

bool play_sound_file(const std::string &filename) {
    // Whether notes or wave is running, stop it
    stop_speaker();

    // Read the WAVE file header
    file = SD.open(filename.c_str());
    wave_header_t header;
    int bytes_read = file.read((uint8_t *)&header, sizeof(header));
    if (bytes_read != 44) {
        Serial.println("Error reading WAVE file header");
        file.close();
        return false;
    }

    if (header.num_channels != 1) {
        Serial.printf("This file has %f channels. Only mono WAVE files are supported.",
                      header.num_channels);
        file.close();
        return false;
    }

    if (header.sample_rate != SPEAKER_SAMPLE_RATE) {
        Serial.printf("This file has a sample rate of %d. Only %d Hz sample rate is supported\n",
                      header.sample_rate, SPEAKER_SAMPLE_RATE);
        file.close();
        return false;
    }

    if (header.bits_per_sample != SPEAKER_BITS_PER_SAMPLE) {
        Serial.printf("This file has %d bits per sample. Only %d bits per sample is supported\n",
                      header.bits_per_sample, SPEAKER_BITS_PER_SAMPLE);
        file.close();
        return false;
    }

    start_i2s();
    wave_running = true;
    return true;
}

bool add_notes(const std::string &new_notes) {
    // If WAVE is running, then stop it and reset the audio buffer.
    // Otherwise if notes are running, it's fine to let them keep running.
    if (wave_running) {
        stop_speaker();
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

bool start_recording(const std::string &filename) {
    if (recording_audio) {
        Serial.println("Already recording audio");
        return false;
    }

    speaker_recording_file = SD.open(filename.c_str(), FILE_WRITE);
    if (!speaker_recording_file) {
        Serial.println("Error opening/creating file for recording.");
        return false;
    }

    // Clear out the buffer before we start recording
    size_t bytes_read;
    i2s_read(MIC_I2S_PORT, i2s_read_buff, MIC_READ_BUF_SIZE, &bytes_read, portMAX_DELAY);
    i2s_read(MIC_I2S_PORT, i2s_read_buff, MIC_READ_BUF_SIZE, &bytes_read, portMAX_DELAY);

    // Set up initial state
    recording_audio = true;
    done_recording_audio = false;

    // Create the task to actually do the recording
    xTaskCreate(
        [](void *arg) {
            int total_bytes_read = 0;
            size_t bytes_read = 0;
            wave_header_t header;

            // Skip over the header for now
            speaker_recording_file.seek(sizeof(header));

            Serial.println(" *** Recording Start *** ");
            while (recording_audio) {
                // Read in an audio sample
                if (i2s_read(MIC_I2S_PORT, i2s_read_buff, MIC_READ_BUF_SIZE, &bytes_read,
                             portMAX_DELAY) != ESP_OK) {
                    Serial.println("Failed to read data from I2S");
                    break;
                }

                // Convert the 32-bit samples to 16-bit samples
                convert_samples(file_write_buff, i2s_read_buff, bytes_read / 4);
                int bytes_to_write = bytes_read / 2;

                // Write it to the file
                if (speaker_recording_file.write((uint8_t *)file_write_buff, bytes_to_write) !=
                    bytes_to_write) {
                    Serial.println("Failed to write data to file");
                    break;
                }

                total_bytes_read += bytes_read;
                Serial.println("Recording audio...");
            }

            // Fill in the header
            create_wave_header(&header, total_bytes_read);

            // Go back to the beginning of the file so we can write the header
            speaker_recording_file.seek(0);

            // Write the header to the disk
            speaker_recording_file.write((uint8_t *)&header, sizeof(header));
            speaker_recording_file.close();

            // Indicate to the main task that we are done
            done_recording_audio = true;

            // This task is done so delete itself
            vTaskDelete(NULL);
        },
        "recording_audio", 4096, NULL, 1, NULL);

    return true;
}

void stop_recording() {
    // Notify the other task it should be done
    recording_audio = false;

    // Wait for other task to finish
    while (!done_recording_audio) {
        delay(10);
    }
}

bool is_recording() { return recording_audio; }

void set_recording_gain(uint8_t new_gain) { recording_gain = new_gain; }

////////////////////////////// Private Functions ///////////////////////////////

void start_i2s() {
    if (i2s_running) {
        return;
    }
    i2s_start(SPEAKER_I2S_PORT);
    i2s_running = true;
}

static void generate_silence(float duration_s) {
    int num_samples = duration_s * SPEAKER_SAMPLE_RATE;

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
    int num_samples = duration_s * SPEAKER_SAMPLE_RATE;

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
        val = _amp * sin(2 * 3.14 * frequency * i / SPEAKER_SAMPLE_RATE);

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
                i2s_write(SPEAKER_I2S_PORT, &audio_buf[audio_buf_frame_idx_to_send * FRAME_SIZE],
                          FRAME_SIZE * SPEAKER_BYTES_PER_SAMPLE, &bytes_written, portMAX_DELAY);

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

void create_wave_header(wave_header_t *header, int data_length) {
    memcpy(header->riff_tag, "RIFF", 4);
    header->riff_length = data_length + sizeof(header) - 8; // TODO: why is there 8?
    memcpy(header->wave_tag, "WAVE", 4);
    memcpy(header->fmt_tag, "fmt ", 4);
    header->fmt_length = 16;
    header->audio_format = 1;
    header->num_channels = MIC_NUM_CHANNELS;
    header->sample_rate = MIC_SAMPLE_RATE;
    header->byte_rate = MIC_SAMPLE_RATE * (MIC_CONVERTED_SAMPLE_BITS / 8) * MIC_NUM_CHANNELS;
    header->block_align = 4;
    header->bits_per_sample = MIC_CONVERTED_SAMPLE_BITS;
    memcpy(header->data_tag, "data", 4);
    header->data_length = data_length;
}

void convert_samples(uint16_t *dest, const uint32_t *src, int num_samples) {
    for (int i = 0; i < num_samples; i++) {
        dest[i] = (src[i] >> 16) * recording_gain;
    }
}

void write_next_note_to_audio_buf() {

    // Check if we have enough space in the buffer to add the note
    int avail_space = (AUDIO_BUF_NUM_FRAMES - audio_buf_num_populated_frames - 1) * FRAME_SIZE;
    int note_samples = next_note_duration_s * SPEAKER_SAMPLE_RATE;

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
            stop_speaker();
        }
    } else if (wave_running) {
        if (file.available() && audio_buf_num_populated_frames < WAVE_FRAMES_TO_BUFFER) {
            int bytes_to_read = FRAME_SIZE * SPEAKER_BYTES_PER_SAMPLE;

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
            stop_speaker();
        }
    }
}

bool processWAVFile(const std::string &inputFilePath, const std::string &outputFilePath,int cuttoff_freq,bool high_pass) {
    File inputFile = SD.open(inputFilePath.c_str(), FILE_READ);
    if (!inputFile) {
        Serial.println("Error opening input file");
        return false;
    }

    File outputFile = SD.open(outputFilePath.c_str(), FILE_WRITE);
    if (!outputFile) {
        Serial.println("Error opening output file");
        inputFile.close();
        return false;
    }

    //read and clone the header to the ouput file
    wave_header_t header;
    inputFile.read((uint8_t*)&header, sizeof(wave_header_t));
    outputFile.write((uint8_t*)&header, sizeof(wave_header_t));

    // Buffer for audio data
    const int bufferSize = 512;
    int16_t buffer[bufferSize];
    

    //calculate coefficients for 2nd order butterworth filter
    filter_coefficients_t filter_coefficients_1 = calculate_filter_coefficients_butterworth(cuttoff_freq, MIC_SAMPLE_RATE, high_pass);
    filter_coefficients_t filter_coefficients_2 = calculate_filter_coefficients_butterworth(cuttoff_freq, MIC_SAMPLE_RATE, high_pass);
    filter_coefficients_t filter_coefficients_3 = calculate_filter_coefficients_butterworth(cuttoff_freq, MIC_SAMPLE_RATE, high_pass);
    

    while (inputFile.available()) {
        int bytesRead = inputFile.read((uint8_t*)buffer, sizeof(buffer));
        int samplesRead = bytesRead / sizeof(int16_t);

        

        // Cascade filter 3 times for extreme effect
        for (int i = 0; i < samplesRead; i++) {
            buffer[i] = filter(buffer[i], filter_coefficients_1);
            buffer[i] = filter(buffer[i], filter_coefficients_2);
            buffer[i] = filter(buffer[i], filter_coefficients_3);
    
        }

        // Write filtered data to the output file
        outputFile.write((uint8_t*)buffer, bytesRead);
    }

    inputFile.close();
    outputFile.close();
    
    return true;
}

bool bandRejectFilter(const std::string &inputFilePath, const std::string &outputFilePath,int low_cuttoff, int high_cutoff) {
    File inputFile = SD.open(inputFilePath.c_str(), FILE_READ);
    if (!inputFile) {
        Serial.println("Error opening input file");
        return false;
    }

    File outputFile = SD.open(outputFilePath.c_str(), FILE_WRITE);
    if (!outputFile) {
        Serial.println("Error opening output file");
        inputFile.close();
        return false;
    }

    //read and clone the header to the ouput file
    wave_header_t header;
    inputFile.read((uint8_t*)&header, sizeof(wave_header_t));
    outputFile.write((uint8_t*)&header, sizeof(wave_header_t));

    // Buffer for audio data
    const int bufferSize = 512;
    int16_t buffer[bufferSize];
    

    //calculate coefficients for 2nd order butterworth filter
    filter_coefficients_t filter_coefficients_1 = calculate_filter_coefficients_biquad(low_cuttoff,high_cutoff, MIC_SAMPLE_RATE);
    filter_coefficients_t filter_coefficients_2 = calculate_filter_coefficients_biquad(low_cuttoff,high_cutoff, MIC_SAMPLE_RATE);

    


    
    int chuncks_read = 0;
    while (inputFile.available()) {
        
        int bytesRead = inputFile.read((uint8_t*)buffer, sizeof(buffer));
        int samplesRead = bytesRead / sizeof(int16_t);

        for (int i = 0; i < samplesRead; i++) {
            buffer[i] = filter(buffer[i], filter_coefficients_1);
            buffer[i] = filter(buffer[i], filter_coefficients_2);
            if (chuncks_read > 10) {
                buffer[i] = buffer[i] * 3;
            }
    
        }


        // Write filtered data to the output file
        outputFile.write((uint8_t*)buffer, bytesRead);
        chuncks_read++;
    }
    

    inputFile.close();
    outputFile.close();
    
    return true;
}
    


}; // namespace YAudio
