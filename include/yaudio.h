#ifndef YAUDIO_H
#define YAUDIO_H

#include <stdint.h>
#include <string>

namespace YAudio {

bool setup_speaker();
bool setup_mic();
void loop();
void set_wave_volume(uint8_t volume);
bool add_notes(const std::string &new_notes);
void stop_speaker();
bool is_playing();
bool play_sound_file(const std::string &filename);
bool start_recording(const std::string &filename);
void stop_recording();
bool is_recording();
void set_recording_gain(uint8_t new_gain);
bool processWAVFile(const std::string &inputFilePath,const std::string &outputFilePath, int cuttoff_freq,bool highPass);
bool bandRejectFilter(const std::string &inputFilePath, const std::string &outputFilePath,int low_cuttoff, int high_cutoff);
}; // namespace YAudio

#endif /* YAUDIO_H */
