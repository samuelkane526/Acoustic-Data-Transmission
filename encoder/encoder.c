#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>

#define PI 3.14159265358979323846 // Defined here for use in tone generation calculations
#define REPEAT_IDX 256

typedef struct {
    int SAMPLE_RATE;
    float BASE_FREQ;
    float BIN_SPACING;
    float FREQ_HELLO;
    float FREQ_HEADER;
    float FREQ_TERM;
    float DATA_DUR;
    float BYTE_GAP;
    float HELLO_DUR;
    float HEADER_DUR;
    char INPUT_FILE[256]; // Added for configurable input
} EncoderConfig;

#pragma pack(push, 1) // Saves current alignment and disables padding to prevent unfilled space errors in header struct.
typedef struct {
    uint8_t syncMarker; 
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

// Lightweight INI Parser
bool load_config(const char* filename, EncoderConfig* config) {
    FILE* file = fopen(filename, "r");
    if (!file) return false;

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == ';' || line[0] == '#' || line[0] == '[' || line[0] == '\n' || line[0] == '\r') continue;
        
        char key[64];
        char str_val[256];
        if (sscanf(line, "%63[^=]=%255s", key, str_val) == 2) {
            if (strcmp(key, "SampleRate") == 0) config->SAMPLE_RATE = atoi(str_val);
            else if (strcmp(key, "BaseFreq") == 0) config->BASE_FREQ = atof(str_val);
            else if (strcmp(key, "BinSpacing") == 0) config->BIN_SPACING = atof(str_val);
            else if (strcmp(key, "FreqHello") == 0) config->FREQ_HELLO = atof(str_val);
            else if (strcmp(key, "FreqHeader") == 0) config->FREQ_HEADER = atof(str_val);
            else if (strcmp(key, "FreqTerm") == 0) config->FREQ_TERM = atof(str_val);
            else if (strcmp(key, "DataDur") == 0) config->DATA_DUR = atof(str_val);
            else if (strcmp(key, "ByteGap") == 0) config->BYTE_GAP = atof(str_val);
            else if (strcmp(key, "InputFile") == 0) strncpy(config->INPUT_FILE, str_val, 255);
        }
    }
    fclose(file);
    
    // Scale durations based on DataDur for protocol consistency
    config->HELLO_DUR = config->DATA_DUR * 5.0f;
    config->HEADER_DUR = config->DATA_DUR * 3.0f;
    return true;
}

void write_tone(FILE *f, float freq, float duration, int sampleRate) {
    int total_samples = (int)(sampleRate * duration);
    if (total_samples <= 0) return;

    int16_t *buffer = malloc(total_samples * sizeof(int16_t));
    if (!buffer) return;

    int fade_len = (int)(sampleRate * 0.005f); // 5ms fade to prevent clicks
    
    for (int i = 0; i < total_samples; i++) {
        float amplitude = 0.9f;
        if (freq > 0) {
            if (i < fade_len) amplitude *= (float)i / fade_len;
            else if (i >= total_samples - fade_len) amplitude *= (float)(total_samples - 1 - i) / fade_len;
        } else amplitude = 0.0f;

        float sample_val = 0.0f;
        if (freq > 0) sample_val = sinf(freq * 2.0f * PI * (float)i / sampleRate);
        buffer[i] = (int16_t)(sample_val * amplitude * 32767.0f);
    }
    fwrite(buffer, sizeof(int16_t), total_samples, f);
    free(buffer);
}

void print_progress(size_t current, size_t total, float total_time_s) {
    float percent = (float)current / total * 100.0f;
    int bar_width = 40;
    int pos = (int)(bar_width * current / total);

    printf("\rEncoding: [");
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) printf("=");
        else if (i == pos) printf(">");
        else printf(" ");
    }
    printf("] %.1f%% | Audio: %.2f min", percent, total_time_s / 60.0f); // Shows percentage and estimated total time for the audio file
    fflush(stdout);
}

int main(void) {
    EncoderConfig cfg = { .INPUT_FILE = "test.txt" }; // Default value
    if (!load_config("encoder_config.ini", &cfg)) {
        printf("Error: Could not load encoder_config.ini\n");
        return 1;
    }

    const char *out_filename = "transmit.wav";

    // 1. Read Payload using name from INI
    FILE *fin = fopen(cfg.INPUT_FILE, "rb");
    if (!fin) { printf("Error: %s not found\n", cfg.INPUT_FILE); return 1; }

    fseek(fin, 0, SEEK_END);
    long fsize = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    uint8_t *file_data = malloc(fsize);
    fread(file_data, 1, fsize, fin);
    fclose(fin);

    // 2. Build ChordCast Packet
    ChordHeader header = { .syncMarker = 0xFE, .fileSize = (uint32_t)fsize };
    uint32_t sum = 0;
    for (long i = 0; i < fsize; i++) sum += file_data[i];
    header.checksum = sum % 256;
    strncpy(header.fileName, cfg.INPUT_FILE, 31);
    
    size_t total_len = sizeof(ChordHeader) + fsize;
    uint8_t *all_data = malloc(total_len);
    memcpy(all_data, &header, sizeof(ChordHeader));
    memcpy(all_data + sizeof(ChordHeader), file_data, fsize);

    // 3. Estimates
    float est_play_time = cfg.HELLO_DUR + cfg.HEADER_DUR + (cfg.BYTE_GAP * 4) + (total_len * (cfg.DATA_DUR + cfg.BYTE_GAP)) + 1.0f;
    double expected_wav_size = (double)cfg.SAMPLE_RATE * 2 * est_play_time;

    // --- SESSION REPORT (MATCHED TO DECODER STYLE) ---
    printf("\n============================================\n");
    printf("        CHORDCAST ENCODER SESSION           \n");
    printf("============================================\n");
    printf("[Audio]\n");
    printf("SampleRate=%d\n", cfg.SAMPLE_RATE);
    printf("\n[Frequencies]\n");
    printf("BaseFreq=%.3f\n", cfg.BASE_FREQ);
    printf("BinSpacing=%.3f\n", cfg.BIN_SPACING);
    printf("FreqHello=%.3f\n", cfg.FREQ_HELLO);
    printf("FreqHeader=%.3f\n", cfg.FREQ_HEADER);
    printf("FreqTerm=%.3f\n", cfg.FREQ_TERM);
    printf("\n[Payload]\n");
    printf("InputFile=%s\n", cfg.INPUT_FILE);
    printf("TotalBytes=%zu\n", total_len);
    printf("\n[Estimates]\n");
    printf("TransmissionTime=%.2f min\n", est_play_time / 60.0f);
    printf("WavFileSize=%.2f MB\n", (float)(expected_wav_size / (1024.0 * 1024.0)));
    printf("------------------------------------------\n");
    printf("ENCODER STATUS: Ready to generate %s\n", out_filename);
    printf("============================================\n\n");

    if (est_play_time > 120.0f) {
        printf("WARNING: Transmission exceeds 2 minutes. Continue? (y/n): ");
        char confirm;
        if (scanf(" %c", &confirm) != 1 || (confirm != 'y' && confirm != 'Y')) {
            free(file_data); free(all_data); return 0;
        }
    }

    // 4. Initialize WAV and all of its data, then write the protocol tones and payload tones.
    FILE *fout = fopen(out_filename, "wb");
    WavHeader wav = {
        .riff = {'R','I','F','F'}, .wave = {'W','A','V','E'}, .fmt_chunk_marker = {'f','m','t',' '},
        .length_fmt = 16, .format_type = 1, .channels = 1, .sample_rate = cfg.SAMPLE_RATE,
        .bits_per_sample = 16, .block_align = 2, .byterate = cfg.SAMPLE_RATE * 2,
        .data_chunk_header = {'d','a','t','a'}
    };
    fwrite(&wav, sizeof(WavHeader), 1, fout);

    //Start of protocol transmission
    write_tone(fout, cfg.FREQ_HELLO, cfg.HELLO_DUR, cfg.SAMPLE_RATE); 
    write_tone(fout, 0, cfg.BYTE_GAP, cfg.SAMPLE_RATE);
    write_tone(fout, cfg.FREQ_HEADER, cfg.HEADER_DUR, cfg.SAMPLE_RATE);
    write_tone(fout, 0, cfg.BYTE_GAP, cfg.SAMPLE_RATE);

    int prev_byte = -1;
    bool last_was_repeat = false;
    for (size_t i = 0; i < total_len; i++) { //
        int val = all_data[i];
        if (val == prev_byte && !last_was_repeat) {
            val = REPEAT_IDX; /* When multiple bytes with the same sound run in a row, the decoder can get confused
              for when one byte ends and the next starts. By using a repeater value it splits up long chains of the same byte
              and makes it easier for the decoder to stay in sync. 
              The decoder treats this value as a signal to repeat the last valid byte. */
            last_was_repeat = true;
        } else last_was_repeat = false;

        write_tone(fout, cfg.BASE_FREQ + (val * cfg.BIN_SPACING), cfg.DATA_DUR, cfg.SAMPLE_RATE); //Calculating byte value with base frequency + spacing * byte value
        write_tone(fout, 0, cfg.BYTE_GAP, cfg.SAMPLE_RATE); //Writing the byte gap of silence after each byte
        prev_byte = all_data[i]; // Store the actual byte value for repeat detection in the next iteration
        if (i % 50 == 0 || i == total_len - 1) print_progress(i + 1, total_len, est_play_time); // Update progress every 50 bytes or on the last byte, showing percentage and estimated time remaining
    }

    write_tone(fout, 0, cfg.BYTE_GAP, cfg.SAMPLE_RATE); //Simple byte gap of silence before termination tones
    for (int k = 0; k < 3; k++) { 
        write_tone(fout, cfg.FREQ_TERM, 0.1f, cfg.SAMPLE_RATE); //We play the termination tone 3 times to ensure the decoder detects it, 
        // especially in noisy environments. Each tone is short to save time.
        write_tone(fout, 0, 0.02f, cfg.SAMPLE_RATE);
    }

    long f_len = ftell(fout);
    uint32_t r_len = (uint32_t)f_len - 8, d_len = (uint32_t)f_len - sizeof(WavHeader);
    fseek(fout, 4, SEEK_SET); fwrite(&r_len, 4, 1, fout); //Write the RIFF chunk size (overall file size - 8 bytes for RIFF header)
    fseek(fout, 40, SEEK_SET); fwrite(&d_len, 4, 1, fout); //Write the data chunk size (total file size - header size).

    fclose(fout);
    free(file_data); free(all_data);
    printf("\n\nEncoding Complete: %s\n", out_filename);

    printf("\nPress Enter to exit...");
    getchar();
    return 0;
}