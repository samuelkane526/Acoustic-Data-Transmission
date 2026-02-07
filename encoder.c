#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>

#define SAMPLE_RATE 48000
#define PI 3.14159265358979323846
#define REPEAT_IDX 256

#define BASE_FREQ     1218.75f  // Bin 52 (Even bin)
#define BIN_SPACING   46.875f   // Exactly 2 bins
#define FREQ_HELLO    609.375f  // Bin 26
#define FREQ_HEADER   843.75f   // Bin 36
#define FREQ_TERM     13781.25f // Bin 588

// --- HIGH-SPEED TIMING UPDATES ---
#define DATA_DUR      0.1f 
#define BYTE_GAP      0.06f
#define HELLO_DUR     0.6f
#define HEADER_DUR    0.3f
#define GAP_DUR       0.1f

#pragma pack(push, 1)
typedef struct {
    char fileName[32];
    uint32_t fileSize;
    uint8_t checksum;
    uint8_t fileType;
} ChordHeader;

typedef struct {
    char riff[4];
    uint32_t overall_size;
    char wave[4];
    char fmt_chunk_marker[4];
    uint32_t length_fmt;
    uint16_t format_type;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byterate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_chunk_header[4];
    uint32_t data_size;
} WavHeader;
#pragma pack(pop)

void write_tone(FILE *f, float freq, float duration) {
    int total_samples = (int)(SAMPLE_RATE * duration);
    int fade_len = (int)(SAMPLE_RATE * 0.005f); // 5ms fade
    
    for (int i = 0; i < total_samples; i++) {
        float amplitude = 0.9f;
        
        if (freq > 0) {
            if (i < fade_len) {
                amplitude *= (float)i / fade_len;
            } else if (i >= total_samples - fade_len) {
                amplitude *= (float)(total_samples - 1 - i) / fade_len;
            }
        } else {
            amplitude = 0.0f;
        }

        float sample_val = 0.0f;
        if (freq > 0) {
            sample_val = sinf(freq * 2.0f * PI * (float)i / SAMPLE_RATE);
        }
        
        int16_t pcm = (int16_t)(sample_val * amplitude * 32767.0f);
        fwrite(&pcm, sizeof(int16_t), 1, f);
    }
}

int main(void) {
    const char *in_filename = "test.txt";
    const char *out_filename = "transmit.wav";

    // 1. Read Input File
    FILE *fin = fopen(in_filename, "rb");
    if (!fin) {
        printf("Error: Could not open %s\n", in_filename);
        return 1;
    }

    fseek(fin, 0, SEEK_END);
    long fsize = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    uint8_t *file_data = malloc(fsize);
    fread(file_data, 1, fsize, fin);
    fclose(fin);

    // 2. Prepare ChordHeader
    ChordHeader header;
    memset(&header, 0, sizeof(ChordHeader));
    
    uint32_t sum = 0;
    for (long i = 0; i < fsize; i++) sum += file_data[i];
    header.checksum = sum % 256;

    memset(header.fileName, ' ', 32);
    size_t name_len = strlen(in_filename);
    if (name_len > 31) name_len = 31;
    memcpy(header.fileName, in_filename, name_len);
    
    header.fileSize = (uint32_t)fsize;
    header.fileType = 0;

    size_t total_len = sizeof(ChordHeader) + fsize;
    uint8_t *all_data = malloc(total_len);
    memcpy(all_data, &header, sizeof(ChordHeader));
    memcpy(all_data + sizeof(ChordHeader), file_data, fsize);

    // 3. Open WAV File
    FILE *fout = fopen(out_filename, "wb");
    WavHeader wav = {0};
    memcpy(wav.riff, "RIFF", 4);
    memcpy(wav.wave, "WAVE", 4);
    memcpy(wav.fmt_chunk_marker, "fmt ", 4);
    wav.length_fmt = 16;
    wav.format_type = 1; 
    wav.channels = 1;
    wav.sample_rate = SAMPLE_RATE;
    wav.bits_per_sample = 16;
    wav.block_align = wav.channels * wav.bits_per_sample / 8;
    wav.byterate = wav.sample_rate * wav.block_align;
    memcpy(wav.data_chunk_header, "data", 4);
    
    fwrite(&wav, sizeof(WavHeader), 1, fout);

    // 4. Generate Sequence
    printf("Encoding %zu bytes at %.2f Bps...\n", total_len, 1.0f/(DATA_DUR + BYTE_GAP));

    // Handshake
    write_tone(fout, FREQ_HELLO, HELLO_DUR);
    write_tone(fout, 0, GAP_DUR);
    write_tone(fout, FREQ_HEADER, HEADER_DUR);
    write_tone(fout, 0, GAP_DUR);

    // Data Payload Loop
    int prev_byte = -1;
    bool last_was_repeat = false;

    for (size_t i = 0; i < total_len; i++) {
        int val = all_data[i];
        
        // Handle Repeat characters
        if (val == prev_byte && !last_was_repeat) {
            val = REPEAT_IDX;
            last_was_repeat = true;
        } else {
            last_was_repeat = false;
        }

        float freq = BASE_FREQ + (val * BIN_SPACING);
        
        // Tone + Silence Gap
        write_tone(fout, freq, DATA_DUR);
        write_tone(fout, 0, BYTE_GAP); 
        
        prev_byte = all_data[i];
    }

    // Termination Sequence
    write_tone(fout, 0, GAP_DUR);
    for (int k = 0; k < 3; k++) {
        write_tone(fout, FREQ_TERM, 0.1f);
        write_tone(fout, 0, 0.02f);
    }

    // 5. Finalize WAV Header
    long file_length = ftell(fout);
    uint32_t data_len = file_length - sizeof(WavHeader);
    
    fseek(fout, 4, SEEK_SET);
    uint32_t riff_len = file_length - 8;
    fwrite(&riff_len, 4, 1, fout);

    fseek(fout, 40, SEEK_SET);
    fwrite(&data_len, 4, 1, fout);

    fclose(fout);
    free(file_data);
    free(all_data);

    printf("Done. Checksum: %d\n", header.checksum);
    return 0;
}