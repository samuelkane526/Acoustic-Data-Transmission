#define COBJMACROS // Must be at top to use COM interface macros.
#include <initguid.h>
#include <stdio.h>
#include <stdbool.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <propkey.h>
#include "kissfft-131.2.0/kiss_fft.h"
#include <math.h>
#include <stdint.h>
#include <signal.h>

#define PI 3.14159265358979323846
#define MAX_FILE_SIZE (1024 * 1024 * 5) // Implemented to prevent buffer overflow (5MB)
#define RED_TEXT "\033[1;31m"
#define RESET_TEXT "\033[0m"

// --- Configurable Variables ---
int FFT_SIZE = 2048;       // Size of audio data being stored
int STEP_SIZE = 256;       // Size of FFT audio data being analyzed each step
bool VERBOSE_MODE = true;  // Enable verbose logging of received bytes
bool AUTO_SPACING = true;  // DONT DISABLE UNLESS YOU KNOW WHAT YOU ARE DOING!
bool AUTO_THRESHOLD = false; // Toggles dynamic noise floor adjustment
float THRESHOLD = 5.0f;    // Base threshold (overwritten if AutoThreshold is on)
int DEBOUNCE_LIMIT = 6;

#define REPEAT_IDX 256
/* Repeat IDX is a dummy value to represent a repeating byte. This avoids blending 
   between repeating frequencies and tells the decoder to repeat the last byte. */
#define SYNC_MARKER 0xFE 

// --- Bin-Dependent Variables (Auto-calculated via mic hz) ---
float BIN_WIDTH = 0.0f;   // The resolution of each FFT slot
float BIN_SPACING = 0.0f; // The gap between data bytes
float BASE_FREQ = 0.0f;   // The starting frequency for Byte 0
float FREQ_HELLO = 0.0f;  // Handshake signal
float FREQ_HEADER = 0.0f; // Sync signal
float FREQ_TERM = 0.0f;   // Termination signal

// Allows for ctrl+c to end program
volatile bool g_Running = true;
void SignalHandler(int sig) { g_Running = false; }

typedef enum {
    STATE_IDLE,        // Waiting for hello
    STATE_WAIT_HEADER, // Waiting for header after hello
    STATE_READ_DATA    // Reading data until termination
} ProtocolState;

#pragma pack(push, 1) // Saves current alignment and disables padding to prevent unfilled space errors
typedef struct {
    uint8_t syncMarker; 
    char fileName[32];
    uint32_t fileSize;
    uint8_t checksum;
    uint8_t fileType;
} ChordHeader;
#pragma pack(pop) // Reverse alignment settings to previous state

// --- INI Loader ---
bool load_config(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) return false;

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        // Skip comments and section headers
        if (line[0] == ';' || line[0] == '#' || line[0] == '[' || line[0] == '\n' || line[0] == '\r') continue;

        char key[64];
        float value;
        if (sscanf(line, "%63[^=]=%f", key, &value) == 2) {
            if (strcmp(key, "FFT_SIZE") == 0) FFT_SIZE = (int)value;
            else if (strcmp(key, "STEP_SIZE") == 0) STEP_SIZE = (int)value;
            else if (strcmp(key, "AutoSpacing") == 0) AUTO_SPACING = (bool)value;
            else if (strcmp(key, "AutoThreshold") == 0) AUTO_THRESHOLD = (bool)value;
            else if (strcmp(key, "Verbose") == 0) VERBOSE_MODE = (bool)value;
            else if (strcmp(key, "Threshold") == 0) THRESHOLD = value;
            else if (strcmp(key, "DebounceLimit") == 0) DEBOUNCE_LIMIT = (int)value;
            else if (strcmp(key, "BaseFreq") == 0) BASE_FREQ = value;
            else if (strcmp(key, "BinSpacing") == 0) BIN_SPACING = value;
            else if (strcmp(key, "FreqHello") == 0) FREQ_HELLO = value;
            else if (strcmp(key, "FreqHeader") == 0) FREQ_HEADER = value;
            else if (strcmp(key, "FreqTerm") == 0) FREQ_TERM = value;
        }
    }
    fclose(file);
    return true;
}

void PrintConfig(int sampleRate) {
    // 1. Calculate the time it takes to fill the buffer once (Acoustic Fill)
    float windowTimeMs = ((float)FFT_SIZE / sampleRate) * 1000.0f;
    
    // 2. Calculate the time consumed by the Debounce process (Processing Time)
    float stepTimeMs = ((float)STEP_SIZE / sampleRate) * 1000.0f;
    float debounceTimeMs = stepTimeMs * DEBOUNCE_LIMIT;

    // 3. The "Ideal" duration is the time to fill the window + the time to stay stable
    // We add a 15% safety buffer to account for hardware jitter
    float idealDataDurS = (windowTimeMs + debounceTimeMs) / 1000.0f * 1.15f;

    printf("\n============================================\n");
    printf("     CHORDCAST DECODER CONFIGURATED \n");
    printf("============================================\n");
    printf("--- COPY/PASTE THIS INTO YOUR ENCODER .INI FILE ---\n\n");
    
    printf("[Audio]\n");
    printf("SampleRate=%d\n", sampleRate);
    printf("\n[Frequencies]\n");
    printf("BaseFreq=%.3f\n", BASE_FREQ);
    printf("BinSpacing=%.3f\n", BIN_SPACING);
    printf("FreqHello=%.3f\n", FREQ_HELLO);
    printf("FreqHeader=%.3f\n", FREQ_HEADER);
    printf("FreqTerm=%.3f\n", FREQ_TERM);
    
    printf("\n[Timing]\n");
    printf("; Optimized for %dms Window + %dms Debounce\n", (int)windowTimeMs, (int)debounceTimeMs);
    printf("DataDur=%.3f\n", idealDataDurS);
    printf("ByteGap=%.3f\n", idealDataDurS * 0.5f);
    
    printf("\n------------------------------------------\n");
    printf("DECODER STATUS: Monitoring at %.2fHz intervals\n", BIN_WIDTH);
    if(AUTO_THRESHOLD) printf("AUTO THRESHOLD: ENABLED (Adaptive Noise Floor)\n");
    else printf("THRESHOLD: FIXED at %.2f\n", THRESHOLD);
    printf("============================================\n\n");
}

int main(void) {
    // 1. Declare ALL variables at the top to prevent 'goto' bypass errors
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDevice *pDev = NULL;
    IAudioClient *pCl = NULL;
    IAudioCaptureClient *pCap = NULL;
    WAVEFORMATEX *pwfx = NULL;
    kiss_fft_cfg cfg = NULL;
    HRESULT hr;

    // Pointers for dynamic memory allocation
    kiss_fft_cpx *in = NULL;
    kiss_fft_cpx *out = NULL;
    float *slidingBuffer = NULL;
    unsigned char *fileBuffer = NULL;

    int stableCount = 0, lastByte = -1, processedByte = -1, lastValidByte = -1;
    ProtocolState state = STATE_IDLE;
    uint32_t bufPtr = 0;
    bool headerDone = false;
    ChordHeader header;
    
    // Adaptive Threshold Variables
    float smoothedNoise = 1.0f; // Start low, will adapt quickly

    if (!load_config("decoder_config.ini")) { //Ensuring config exist.
        printf(RED_TEXT "ERROR: decoder_config.ini not found. Using default values.\n" RESET_TEXT);
    }

    // Declaring COM interfaces and variables to NULL for cleanup later
    CoInitialize(NULL);

    signal(SIGINT, SignalHandler); // Allow Ctrl+C to trigger cleanup

    // --- INITIALIZE AUDIO CAPTURE ---
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&pEnum);
    if (FAILED(hr)) goto cleanup;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnum, eCapture, eConsole, &pDev);
    if (FAILED(hr)) goto cleanup;
    hr = IMMDevice_Activate(pDev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&pCl);
    if (FAILED(hr)) goto cleanup;
    hr = IAudioClient_GetMixFormat(pCl, &pwfx);
    if (FAILED(hr)) goto cleanup;
    hr = IAudioClient_Initialize(pCl, AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, pwfx, NULL);
    if (FAILED(hr)) goto cleanup;
    hr = IAudioClient_GetService(pCl, &IID_IAudioCaptureClient, (void **)&pCap);
    if (FAILED(hr)) goto cleanup;
    hr = IAudioClient_Start(pCl);
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        printf(RED_TEXT "ERROR: Audio device was unplugged or changed.\n" RESET_TEXT);
        goto cleanup;
    } else if (FAILED(hr)) {
        printf(RED_TEXT "ERROR: Failed to start audio stream (0x%08lX)\n" RESET_TEXT, (long)hr);
        goto cleanup;
    }

    // --- AUTO SPACING LOGIC ---
    // Calculates bin alignment to eliminate spectral leakage (smearing into adjacent bins)
    BIN_WIDTH = (float)pwfx->nSamplesPerSec / FFT_SIZE;
    if (AUTO_SPACING) {
        BIN_SPACING = BIN_WIDTH * 2.0f;  
        FREQ_HELLO = BIN_WIDTH * 26.0f;  
        FREQ_HEADER = BIN_WIDTH * 36.0f; 
        BASE_FREQ = BIN_WIDTH * 52.0f;   
        FREQ_TERM = BIN_WIDTH * 588.0f;  
    }

    PrintConfig(pwfx->nSamplesPerSec);

    // KissFFT Setup: https://github.com/mborgerding/kissfft
    cfg = kiss_fft_alloc(FFT_SIZE, 0, NULL, NULL);

    // Allocate memory dynamically to prevent "transfer of control bypasses initialization" errors
    in = malloc(sizeof(kiss_fft_cpx) * FFT_SIZE); // FFT input buffer
    out = malloc(sizeof(kiss_fft_cpx) * FFT_SIZE); // FFT output buffer
    slidingBuffer = calloc(FFT_SIZE, sizeof(float)); // Sliding window buffer for audio samples
    fileBuffer = calloc(1, MAX_FILE_SIZE);

    if (!in || !out || !slidingBuffer || !fileBuffer) {
        printf(RED_TEXT "ERROR: Memory allocation failed.\n" RESET_TEXT);
        goto cleanup;
    }


    memset(&header, 0, sizeof(ChordHeader)); //Makes sure no data is left from previous runs, prevents dirty memory issues in header struct.
    while (g_Running) {
        UINT32 pSize = 0;
        IAudioCaptureClient_GetNextPacketSize(pCap, &pSize); // Get size of next packet
        if (pSize > 0) {
            BYTE *pData; UINT32 nRead; DWORD flags;
            IAudioCaptureClient_GetBuffer(pCap, &pData, &nRead, &flags, NULL, NULL);
            float *samples = (float *)pData; // Stores from pData in float format (32bit float stereo)

            for (UINT32 i = 0; i < nRead; i++) {
                static int writeIdx = 0;
                slidingBuffer[writeIdx] = samples[i * pwfx->nChannels]; // Take left channel only (mono)
                writeIdx = (writeIdx + 1) % FFT_SIZE; // Circular wrap around buffer

                static int stepCounter = 0;
                if (++stepCounter >= STEP_SIZE) { // Audio analyzed every STEP_SIZE to manage CPU usage
                    stepCounter = 0;
                    for (int j = 0; j < FFT_SIZE; j++) {
                        // HANNING REMOVED: Direct assignment of raw signal
                        in[j].r = slidingBuffer[(writeIdx + j) % FFT_SIZE]; 
                        in[j].i = 0.0f; // Imaginary part 0 for single stream analysis
                    }
                    
                    // Converts mic position over time into signal strength over frequency
                    kiss_fft(cfg, in, out);

                    float maxM = 0; int maxI = 0;
                    for (int k = 1; k < FFT_SIZE / 2; k++) {
                        // We only scan the first half because the second is mirrored (Nyquist Theorem)
                        float m = sqrtf(out[k].r * out[k].r + out[k].i * out[k].i);
                        if (m > maxM) { maxM = m; maxI = k; }
                    }

                    // Scaled for Rectangular Window (Raw)
                    maxM *= 2.0f; 

                    float freq = maxI * BIN_WIDTH;
                    int curByte = -1;

                    // --- ADAPTIVE THRESHOLD LOGIC ---
                    if (state == STATE_IDLE) {
                        // Low pass filter to create a rolling average of the noise floor
                        smoothedNoise = (smoothedNoise * 0.95f) + (maxM * 0.05f);
                        
                        if (AUTO_THRESHOLD) {
                            // Require signal to be 3x the noise floor
                            THRESHOLD = smoothedNoise * 3.0f;
                            // Clamp to a safe minimum to prevent hardware hiss triggering
                            if (THRESHOLD < 2.0f) THRESHOLD = 2.0f;
                        }
                    }
                    
                    if (maxM > THRESHOLD) {
                        float rawIdx = (freq - BASE_FREQ) / BIN_SPACING;
                        curByte = (int)(rawIdx + 0.5f); // Round to nearest integer to prevent smearing
                    }

                    // --- UI THROTTLING LOGIC ---
                    static int uiThrottle = 0;
                    if (++uiThrottle >= 15) { // Only update UI approx every 150ms
                        if (state == STATE_IDLE) {
                            printf(" MONITORING: Noise: %5.2f | Threshold: %5.2f | Freq: %7.2f\r", maxM, THRESHOLD, freq);                        }
                        uiThrottle = 0;
                    }

                    // 1. TERMINATION
                    if (state == STATE_READ_DATA && maxM > (THRESHOLD * 0.7f) && fabs(freq - FREQ_TERM) < (BIN_WIDTH * 2.5f)) {
                        printf("\n >> TERMINATION DETECTED.");
                        if (headerDone) {
                            /* Calculate checksum to check for dropped/corrupted packets */
                            uint8_t calcSum = 0;
                            uint32_t dataStartOffset = sizeof(ChordHeader);
                            
                            // Only sum up to what we actually received to prevent reading garbage memory
                            for (uint32_t j = 0; j < header.fileSize && (dataStartOffset + j) < bufPtr; j++)
                                calcSum += fileBuffer[dataStartOffset + j];

                            uint32_t expectedTotalBytes = sizeof(ChordHeader) + header.fileSize;

                            if (bufPtr < expectedTotalBytes) {
                                printf(RED_TEXT "\n [ERROR] Transmission Failed: Incomplete Data\n");
                                printf("         Expected: %u bytes | Received: %u bytes\n", expectedTotalBytes, bufPtr);
                                printf("         >> ADVICE: Signal lost. Increase sound volume or refer to README to fix dropping bytes.\n" RESET_TEXT);
                            }
                            else if (calcSum != header.checksum) {
                                printf(RED_TEXT "\n [ERROR] Transmission Failed: Checksum Mismatch (Recv: %d, Calc: %d)\n", header.checksum, calcSum);
                                printf("         >> ADVICE: Data corrupted. Reduce background noise or volume (to prevent clipping).\n" RESET_TEXT);
                            } 
                            else {
                                FILE *f = fopen(header.fileName, "wb");
                                if (f) { 
                                    fwrite(fileBuffer + sizeof(ChordHeader), 1, header.fileSize, f); 
                                    fclose(f); 
                                    printf("\n [SUCCESS] Saved: %s\n", header.fileName);
                                } else {
                                    printf(RED_TEXT "\n [ERROR] Write permission denied. Cannot save file.\n" RESET_TEXT);
                                }
                            }
                        }
                        state = STATE_IDLE; processedByte = -1; stableCount = 0; continue;
                    }

                    // 2. STABILITY
                    static int dropCount = 0;
                    if (maxM < THRESHOLD) {
                        if (++dropCount >= 6) { processedByte = -1; stableCount = 0; }
                    } else {
                        dropCount = 0; // Signal back, reset drop timer
                        if (curByte != lastByte) { stableCount = 0; lastByte = curByte; }
                        else { stableCount++; }
                    }

                    // 3. STATE MACHINE, ensures proper sequencing of hello, header, data, and termination signals. Also handles byte processing and debouncing.
                    if (maxM > THRESHOLD) {
                        if (state == STATE_IDLE) {
                            if (fabs(freq - FREQ_HELLO) < (BIN_WIDTH * 1.5f)) {
                                state = STATE_WAIT_HEADER;
                                printf("\n >> HANDSHAKE (Mag: %.2f)", maxM);
                            }
                        } else if (state == STATE_WAIT_HEADER) {
                            if (fabs(freq - FREQ_HEADER) < (BIN_WIDTH * 1.5f)) {
                                state = STATE_READ_DATA;
                                bufPtr = 0; headerDone = false; processedByte = -1;
                                printf("\n >> SYNC LOCKED. Receiving Data...\n");
                            }
                        } else if (state == STATE_READ_DATA) {
                            if (stableCount >= DEBOUNCE_LIMIT && curByte != processedByte) {
                                processedByte = curByte; // Lock this frequency
                                int byteToProcess = (curByte == REPEAT_IDX) ? lastValidByte : curByte;
                                if (curByte >= 0 && curByte <= 255) lastValidByte = curByte;

                                if (bufPtr == 0 && (uint8_t)byteToProcess != SYNC_MARKER) continue;

                                if (bufPtr < MAX_FILE_SIZE && byteToProcess >= -1) { //Simply logic to check for valid byte range and prevent overflow
                                    fileBuffer[bufPtr++] = (unsigned char)byteToProcess;
                                    if (VERBOSE_MODE) printf("[%02X]", (unsigned char)byteToProcess);

                                    if (!headerDone && bufPtr == sizeof(ChordHeader)) {
                                        memcpy(&header, fileBuffer, sizeof(ChordHeader));
                                        if (header.syncMarker != SYNC_MARKER) {
                                            printf(RED_TEXT "\n [ERROR] Sync Marker Fail (0x%02X). Resetting...\n" RESET_TEXT, header.syncMarker);
                                            bufPtr = 0;
                                        } else {
                                            headerDone = true;
                                            printf("\n >> FILENAME: %s | SIZE: %u bytes\n", header.fileName, header.fileSize);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            IAudioCaptureClient_ReleaseBuffer(pCap, nRead); // Release buffer to OS
        } else Sleep(1); // No data, rest CPU
    }

cleanup:
    if (pCl) IAudioClient_Stop(pCl);
    if (pCap) IAudioCaptureClient_Release(pCap);
    if (pCl) IAudioClient_Release(pCl);
    if (pDev) IMMDevice_Release(pDev);
    if (pEnum) IMMDevice_Release(pEnum);
    if (pwfx) CoTaskMemFree(pwfx);
    
    // Free the dynamic memory we allocated
    if (in) free(in);
    if (out) free(out);
    if (slidingBuffer) free(slidingBuffer);
    if (fileBuffer) free(fileBuffer);
    if (cfg) kiss_fft_free(cfg);
    
    CoUninitialize();

    printf("\nDecoder terminated gracefully. Thanks for checking out ChordCast! :D\n");
    return 0;
}