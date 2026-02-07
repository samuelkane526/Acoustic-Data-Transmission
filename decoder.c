#define COBJMACROS //Must be at top to use COM interface macros.
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

#define PI 3.14159265358979323846


#define FFT_SIZE 2048 // Size of audio data being stored
#define STEP_SIZE 256 // Side of FFT audio data being analyzed each step
#define MAX_FILE_SIZE (1024 * 1024 * 5)
//^^^ Implemented to prevent buffer overflow (5MB).
// Buffer implemented to avoid overflowing ram, or overloading the CPU leading to spikes in latency.
#define RED_TEXT "\033[1;31m"
#define RESET_TEXT "\033[0m"

#include <signal.h>

//Allows for ctrl+c to end program.
volatile bool g_Running = true;
void SignalHandler(int sig) { g_Running = false; }

bool VERBOSE_MODE = true;    // Enable verbose logging of received bytes
bool AUTO_THRESHOLD = false; // Disbaled, often leads to a lower threshold than ideal.
bool AUTO_SPACING = true;    // DONT DISABLE UNLESS YOU KNOW WHAT YOU ARE DOING!

// --- Protocol Constants ---
float THRESHOLD =  10.0f;
int DEBOUNCE_LIMIT = 3;
#define REPEAT_IDX 256
/* Repeat IDX is a dummy value to repersent a repeating byte. This a work to avoid blending in
betweenrepeating frequeincies. Instead, this creates a clear gap, and simply tells the decoder
to repeat the last byte. */

// --- Bin-Dependent Variables (Auto-calculated via mic hz) ---
float BIN_WIDTH = 0.0f;   // The resolution of each FFT slot
float BIN_SPACING = 0.0f; // The gap between data bytes
float BASE_FREQ = 0.0f;   // The starting frequency for Byte 0
float FREQ_HELLO = 0.0f;  // Handshake signal
float FREQ_HEADER = 0.0f; // Sync signal
float FREQ_TERM = 0.0f;   // Termination signal

typedef enum
{
    STATE_IDLE,        // Waiting for hello.
    STATE_WAIT_HEADER, // Waiting for header after hello.
    STATE_READ_DATA    // Reading data until termination.
} ProtocolState;

#pragma pack(push, 1) // Saves current alignment settings, and disables padding. Prevents unfilled allocated space errors.
typedef struct
{
    char fileName[32];
    uint32_t fileSize;
    uint8_t checksum;
    uint8_t fileType;
} ChordHeader;
#pragma pack(pop) // Reverse alligment settings to previous state.

void PrintConfig(int sampleRate)
{
    printf("--- ChordCast Decoder Initialized ---\n");
    printf("Sample Rate:    %d Hz\n", sampleRate);
    printf("Bin Width:      %.4f Hz\n", BIN_WIDTH);
    printf("Threshold:      %.2f (%s)\n", THRESHOLD, AUTO_THRESHOLD ? "AUTO" : "MANUAL");
    printf("\nAUTO-CALIBRATED FREQUENCIES (Bin-Aligned):\n");
    printf("BASE_FREQ   = %.2f (Bin %d)\n", BASE_FREQ, (int)(BASE_FREQ / BIN_WIDTH));
    printf("BIN_SPACING = %.2f (2 Bins)\n", BIN_SPACING);
    printf("FREQ_HELLO  = %.3f (Bin 26)\n", FREQ_HELLO);
    printf("FREQ_HEADER = %.3f (Bin 34)\n", FREQ_HEADER);
    printf("FREQ_TERM   = %.1f (Bin 588)\n", FREQ_TERM);
    printf("--------------------------------------\n\n");
}

int main(void)
{
    // Declaring COM interfaces and variables to NULL for cleanup later
    CoInitialize(NULL);
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDevice *pDev = NULL;
    IAudioClient *pCl = NULL;
    IAudioCaptureClient *pCap = NULL;
    WAVEFORMATEX *pwfx = NULL;
    kiss_fft_cfg cfg = NULL;
    HRESULT hr;

    signal(SIGINT, SignalHandler); // Allow Ctrl+C to trigger cleanup

    // --- INITIALIZE AUDIO CAPTURE ---
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&pEnum);
    if (FAILED(hr)) { fprintf(stderr, "Failed to create Device Enumerator.\n"); goto cleanup; }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnum, eCapture, eConsole, &pDev);
    if (FAILED(hr)) { fprintf(stderr, "Default capture device not found.\n"); goto cleanup; }

    hr = IMMDevice_Activate(pDev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&pCl);
    if (FAILED(hr)) { fprintf(stderr, "Failed to activate Audio Client.\n"); goto cleanup; }

    hr = IAudioClient_GetMixFormat(pCl, &pwfx);
    if (FAILED(hr)) goto cleanup;

    hr = IAudioClient_Initialize(pCl, AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, pwfx, NULL);
    if (FAILED(hr)) { fprintf(stderr, "Failed to initialize Audio Client.\n"); goto cleanup; }

    hr = IAudioClient_GetService(pCl, &IID_IAudioCaptureClient, (void **)&pCap);
    if (FAILED(hr)) goto cleanup;

    hr = IAudioClient_Start(pCl);
    if (FAILED(hr)) goto cleanup;

    // --- AUTO SPACING LOGIC ---
    // Calculates bin alignment to eliminate spectral leakage
    // Specteral Leakage = data types "smearing" into adjacent bins without proper spacing.
    BIN_WIDTH = (float)pwfx->nSamplesPerSec / FFT_SIZE;
    if (AUTO_SPACING)
    {
        BIN_SPACING = BIN_WIDTH * 2.0f;  // Each byte is exactly 4 bins apart
        FREQ_HELLO = BIN_WIDTH * 26.0f;  // Handshake is bin 26
        FREQ_HEADER = BIN_WIDTH * 34.0f; // Header is bin 34
        BASE_FREQ = BIN_WIDTH * 51.0f;   // Start data around 1200hz (Bin 51 approx)
        FREQ_TERM = BIN_WIDTH * 588.0f;  // Termination at approx 13.8khz (Bin 588 is above all other data)
    }

    PrintConfig(pwfx->nSamplesPerSec); // Prints config

    // KissFFT Documentation: https://github.com/mborgerding/kissfft
    // Setting up FFT
    cfg = kiss_fft_alloc(FFT_SIZE, 0, NULL, NULL); // Allocating FFT config
    kiss_fft_cpx in[FFT_SIZE], out[FFT_SIZE];      // FFT input/output buffers
    float slidingBuffer[FFT_SIZE] = {0};           // Sliding window buffer for audio samples

    int stableCount = 0, lastByte = -1, processedByte = -1, lastValidByte = -1;
    ProtocolState state = STATE_IDLE;
    unsigned char *fileBuffer = malloc(MAX_FILE_SIZE);
    uint32_t bufPtr = 0;
    bool headerDone = false;
    ChordHeader header;

    while (g_Running) // Starting audio capture loop
    {
        UINT32 pSize = 0;
        IAudioCaptureClient_GetNextPacketSize(pCap, &pSize); // Get size of next packet
        if (pSize > 0)                                       // If there is data to read
        {
            BYTE *pData;
            UINT32 nRead;
            DWORD flags;
            IAudioCaptureClient_GetBuffer(pCap, &pData, &nRead, &flags, NULL, NULL);
            float *samples = (float *)pData; // Stores from pData in float format (32bit float stereo)

            for (UINT32 i = 0; i < nRead; i++) // nRead is number of frames available in audio buffer.
            // nRead is (8 bytes per frame for 32bit float stereo), yet we only keep one channel. (mono audio)
            {
                static int writeIdx = 0;
                slidingBuffer[writeIdx] = samples[i * pwfx->nChannels];
                // i * pwfx->nChannels is for mono to take left channel only
                writeIdx = (writeIdx + 1) % FFT_SIZE; // Circular wrap around buffer

                static int stepCounter = 0;
                if (++stepCounter >= STEP_SIZE)
                // Audio is analyzed every STEP_SIZE. Smaller step sizes = more CPU usage.
                {
                    stepCounter = 0;

                    for (int j = 0; j < FFT_SIZE; j++)
                    {
                        float multiplier = 0.5f * (1.0f - cosf(2.0f * PI * j / (FFT_SIZE - 1)));
                        // Hanning window condenses a signal , making it easier to analyze.
                        // https://en.wikipedia.org/wiki/Hann_function
                        in[j].r = slidingBuffer[(writeIdx + j) % FFT_SIZE] * multiplier; // Filling FFT input buffer and applying window
                        in[j].i = 0.0f; // Imaginary part is 0 as it is undeeded for single audio stream analysis.
                    }

                    kiss_fft(cfg, in, out);
                    // Converts mic positon over time in signal strength over frequency.
                    // FFT output is stored in out[] as complex numbers (real + imaginary)

                    float maxM = 0;
                    int maxI = 0;
                    for (int k = 1; k < FFT_SIZE / 2; k++)
                    // We only scan the first half of the FFT output
                    // because the second half is a mirrored version (Nyquist Theorem).
                    {
                        float currentFreq = k * BIN_WIDTH;
                        // Magnitude = sqrt(r^2 + i^2)
                        float compensation = 1.0f + (currentFreq / 20000.0f); 
                        float m = sqrtf(out[k].r * out[k].r + out[k].i * out[k].i) * compensation;
                        m *= 2.0f; // Compensating for decreased magnitude from Hanning Windowing

                        float localThreshold = THRESHOLD;

                        // High frequencies, like the 13.8kHz termination, lose energy faster.
                        if (currentFreq > 10000.0f) {
                            localThreshold *= 0.6f; 
                        }

                        if (m > localThreshold) // Finding the bin with the highest magnitude
                        {
                            maxM = m; // Highest magnitude found so far
                            maxI = k; // Index of the bin with the highest magnitude
                        }
                    }

                    // Mapping Logic
                    float freq = maxI * BIN_WIDTH; // Calculating frequency of the bin with the highest magnitude
                    int curByte = -1;
                    if (maxM > THRESHOLD)
                    {
                        float rawIdx = (freq - BASE_FREQ) / BIN_SPACING; // Calculating which byte this frequency maps to
                        curByte = (int)(rawIdx + 0.5f);
                        // ^^^ Round to nearest integer to prevent smearing if hz is directly between bins.

                        // Safety check: if the frequency is way outside the range, discard it
                        if (curByte < 0 || curByte > REPEAT_IDX)
                            curByte = -1;
                    }

                    // 1. TERMINATION (Must check first)
                    bool isTermFreq = fabs(freq - FREQ_TERM) < (BIN_WIDTH * 2.5f); // Term frequency more spread out due to high freq
                    float termThreshold = THRESHOLD * 0.7f; // Lower threshold specifically for high-freq term

                    if (state == STATE_READ_DATA && maxM > termThreshold && isTermFreq)
                    {
                        printf("\n >> TERMINATION DETECTED.");
                        if (headerDone)
                        /* If header was received successfully prepare to save file by calculating checksum
                        and using it to check for dropped/corrupted packets. */
                        {
                            uint8_t calcSum = 0;
                            // Calculate checksum starting AFTER the header
                            for (uint32_t j = 0; j < header.fileSize; j++)
                                calcSum += fileBuffer[sizeof(ChordHeader) + j];

                            if (calcSum == header.checksum)
                            {
                                FILE *f = fopen(header.fileName, "wb");
                                if (f)
                                {
                                    fwrite(fileBuffer + sizeof(ChordHeader), 1, header.fileSize, f);
                                    fclose(f);
                                    printf("\n [SUCCESS] Saved: %s\n", header.fileName);
                                }
                            }
                            else
                                printf("\n [ERROR] Checksum Mismatch (Recv: %d, Calc: %d)\n", header.checksum, calcSum);
                        }
                        state = STATE_IDLE;
                        processedByte = -1;
                        stableCount = 0;
                        continue;
                    }

                    // 2. STABILITY TRACKING
// 2. STABILITY TRACKING (With Drop Hysteresis)
                    static int dropCount = 0;
                    const int DROP_LIMIT = 6; // Number of frames to wait before clearing processedByte

                    if (maxM < THRESHOLD) 
                    {
                        dropCount++;
                        if (dropCount >= DROP_LIMIT) {
                            processedByte = -1; 
                            stableCount = 0;
                        }
                    }
                    else 
                    {
                        dropCount = 0; // Signal is back, reset drop timer

                        if (curByte != lastByte) {
                            stableCount = 0;
                            lastByte = curByte;
                        }
                        else {
                            stableCount++;
                        }
                    }

                    // 3. STATE MACHINE
                    if (maxM > THRESHOLD) 
                    {
                        if (state == STATE_IDLE) {
                            if (fabs(freq - FREQ_HELLO) < (BIN_WIDTH * 1.5f)) {
                                state = STATE_WAIT_HEADER;
                                printf("\n >> HANDSHAKE (Mag: %.2f)", maxM);
                            }
                        } 
                        else if (state == STATE_WAIT_HEADER) {
                            if (fabs(freq - FREQ_HEADER) < (BIN_WIDTH * 1.5f)) {
                                state = STATE_READ_DATA;
                                bufPtr = 0;
                                headerDone = false;
                                processedByte = -1;
                                memset(slidingBuffer, 0, sizeof(float) * FFT_SIZE);
                                printf("\n >> SYNC LOCKED. Receiving Data...\n");
                            }
                        } 
                        else if (state == STATE_READ_DATA && stableCount >= DEBOUNCE_LIMIT && curByte != processedByte) 
                        {
                            processedByte = curByte; // Lock this frequency
                            int byteToProcess = (curByte == REPEAT_IDX) ? lastValidByte : curByte;

                            if (curByte >= 0 && curByte <= 255)
                                lastValidByte = curByte;

                            if (bufPtr < MAX_FILE_SIZE && byteToProcess >= 0) {
                                fileBuffer[bufPtr++] = (unsigned char)byteToProcess;
                                if (VERBOSE_MODE) printf("[%02X]", (unsigned char)byteToProcess);

                                if (!headerDone && bufPtr == sizeof(ChordHeader)) {
                                    memcpy(&header, fileBuffer, sizeof(ChordHeader));
                                    if (header.fileSize > MAX_FILE_SIZE - sizeof(ChordHeader)) {
                                        printf(RED_TEXT "\n [ERROR] File too large.\n" RESET_TEXT);
                                        state = STATE_IDLE;
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
            IAudioCaptureClient_ReleaseBuffer(pCap, nRead); // Release buffer to OS
        }
        else
            Sleep(1); // No data to read, give the CPU much needed rest.
    }

    cleanup:
        if (pCl) IAudioClient_Stop(pCl);
        if (pCap) IAudioCaptureClient_Release(pCap);
        if (pCl) IAudioClient_Release(pCl);
        if (pwfx) CoTaskMemFree(pwfx);
        if (pDev) IMMDevice_Release(pDev);
        if (pEnum) IMMDevice_Release(pEnum);
        CoUninitialize();

        if (fileBuffer) free(fileBuffer);
        if (cfg) kiss_fft_free(cfg);

        printf("\n >> Resources released. Thanks for checking out my program! :D.\n");

    return 0;
}