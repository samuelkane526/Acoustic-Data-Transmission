#define COBJMACROS 
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
#define FFT_SIZE 2048 
#define STEP_SIZE 256 
#define MAX_FILE_SIZE (1024 * 1024 * 5)
#define RED_TEXT "\033[1;31m"
#define RESET_TEXT "\033[0m"

volatile bool g_Running = true;
void SignalHandler(int sig) { g_Running = false; }

bool VERBOSE_MODE = true;    
bool AUTO_THRESHOLD = false; 
bool AUTO_SPACING = true;    

// --- Protocol Constants ---
float THRESHOLD = 5.0f;     // Lowered to account for non-windowed magnitude
int DEBOUNCE_LIMIT = 6;     // Slightly lowered for responsiveness
#define REPEAT_IDX 256
#define SYNC_MARKER 0xFE 

float BIN_WIDTH = 0.0f;   
float BIN_SPACING = 0.0f; 
float BASE_FREQ = 0.0f;   
float FREQ_HELLO = 0.0f;  
float FREQ_HEADER = 0.0f; 
float FREQ_TERM = 0.0f;   

typedef enum {
    STATE_IDLE,
    STATE_WAIT_HEADER,
    STATE_READ_DATA
} ProtocolState;

#pragma pack(push, 1)
typedef struct {
    uint8_t syncMarker; 
    char fileName[32];
    uint32_t fileSize;
    uint8_t checksum;
    uint8_t fileType;
} ChordHeader;
#pragma pack(pop)

void PrintConfig(int sampleRate) {
    printf("--- ChordCast Decoder Initialized ---\n");
    printf("Sample Rate:    %d Hz\n", sampleRate);
    printf("Bin Width:      %.4f Hz\n", BIN_WIDTH);
    printf("Threshold:      %.2f\n", THRESHOLD);
    printf("\nAUTO-CALIBRATED FREQUENCIES:\n");
    printf("BASE_FREQ   = %.2f (Bin 52)\n", BASE_FREQ);
    printf("FREQ_HELLO  = %.3f (Bin 26)\n", FREQ_HELLO);
    printf("FREQ_HEADER = %.3f (Bin 36)\n", FREQ_HEADER);
    printf("--------------------------------------\n\n");
}

int main(void) {
    CoInitialize(NULL);
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDevice *pDev = NULL;
    IAudioClient *pCl = NULL;
    IAudioCaptureClient *pCap = NULL;
    WAVEFORMATEX *pwfx = NULL;
    kiss_fft_cfg cfg = NULL;
    HRESULT hr;

    signal(SIGINT, SignalHandler);

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&pEnum);
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnum, eCapture, eConsole, &pDev);
    hr = IMMDevice_Activate(pDev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&pCl);
    hr = IAudioClient_GetMixFormat(pCl, &pwfx);
    hr = IAudioClient_Initialize(pCl, AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, pwfx, NULL);
    hr = IAudioClient_GetService(pCl, &IID_IAudioCaptureClient, (void **)&pCap);
    hr = IAudioClient_Start(pCl);

    BIN_WIDTH = (float)pwfx->nSamplesPerSec / FFT_SIZE;
    if (AUTO_SPACING) {
        BIN_SPACING = BIN_WIDTH * 2.0f;  
        FREQ_HELLO = BIN_WIDTH * 26.0f;  
        FREQ_HEADER = BIN_WIDTH * 36.0f; 
        BASE_FREQ = BIN_WIDTH * 52.0f;   
        FREQ_TERM = BIN_WIDTH * 588.0f;  
    }

    PrintConfig(pwfx->nSamplesPerSec);

    cfg = kiss_fft_alloc(FFT_SIZE, 0, NULL, NULL);
    kiss_fft_cpx in[FFT_SIZE], out[FFT_SIZE];
    float slidingBuffer[FFT_SIZE] = {0};

    int stableCount = 0, lastByte = -1, processedByte = -1, lastValidByte = -1;
    ProtocolState state = STATE_IDLE;
    unsigned char *fileBuffer = malloc(MAX_FILE_SIZE);
    uint32_t bufPtr = 0;
    bool headerDone = false;
    ChordHeader header;

    while (g_Running) {
        UINT32 pSize = 0;
        IAudioCaptureClient_GetNextPacketSize(pCap, &pSize);
        if (pSize > 0) {
            BYTE *pData; UINT32 nRead; DWORD flags;
            IAudioCaptureClient_GetBuffer(pCap, &pData, &nRead, &flags, NULL, NULL);
            float *samples = (float *)pData;

            for (UINT32 i = 0; i < nRead; i++) {
                static int writeIdx = 0;
                slidingBuffer[writeIdx] = samples[i * pwfx->nChannels];
                writeIdx = (writeIdx + 1) % FFT_SIZE;

                static int stepCounter = 0;
                if (++stepCounter >= STEP_SIZE) {
                    stepCounter = 0;
                    for (int j = 0; j < FFT_SIZE; j++) {
                        in[j].r = slidingBuffer[(writeIdx + j) % FFT_SIZE]; 
                        in[j].i = 0.0f;
                    }
                    kiss_fft(cfg, in, out);

                    float maxM = 0; int maxI = 0;
                    for (int k = 1; k < FFT_SIZE / 2; k++) {
                        float m = sqrtf(out[k].r * out[k].r + out[k].i * out[k].i);
                        if (m > maxM) { maxM = m; maxI = k; }
                    }

                    // Rectangular window compensation (Peaks are sharper but narrower)
                    maxM *= 2.0f; 

                    float freq = maxI * BIN_WIDTH;
                    int curByte = -1;
                    if (maxM > THRESHOLD) {
                        float rawIdx = (freq - BASE_FREQ) / BIN_SPACING;
                        curByte = (int)(rawIdx + 0.5f);
                    }

                    // 1. TERMINATION
                    if (state == STATE_READ_DATA && maxM > (THRESHOLD * 0.7f) && fabs(freq - FREQ_TERM) < (BIN_WIDTH * 2.5f)) {
                        printf("\n >> TERMINATION DETECTED.");
                        if (headerDone) {
                            uint8_t calcSum = 0;
                            for (uint32_t j = 0; j < header.fileSize; j++)
                                calcSum += fileBuffer[sizeof(ChordHeader) + j];
                            if (calcSum == header.checksum) {
                                FILE *f = fopen(header.fileName, "wb");
                                if (f) { fwrite(fileBuffer + sizeof(ChordHeader), 1, header.fileSize, f); fclose(f); }
                                printf("\n [SUCCESS] Saved: %s\n", header.fileName);
                            } else printf("\n [ERROR] Checksum Mismatch (Recv: %d, Calc: %d)\n", header.checksum, calcSum);
                        }
                        state = STATE_IDLE; processedByte = -1; stableCount = 0; continue;
                    }

                    // 2. STABILITY
                    static int dropCount = 0;
                    if (maxM < THRESHOLD) {
                        if (++dropCount >= 6) { processedByte = -1; stableCount = 0; }
                    } else {
                        dropCount = 0;
                        if (curByte != lastByte) { stableCount = 0; lastByte = curByte; }
                        else { stableCount++; }
                    }

                    // 3. STATE MACHINE
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
                            // DIAGNOSTIC: Uncomment the line below to see raw detections
                            printf(" Freq: %7.2f | Mag: %5.2f | Byte: %3d | Stable: %d\r", freq, maxM, curByte, stableCount);

                            if (stableCount >= DEBOUNCE_LIMIT && curByte != processedByte) {
                                processedByte = curByte;
                                int byteToProcess = (curByte == REPEAT_IDX) ? lastValidByte : curByte;
                                if (curByte >= 0 && curByte <= 255) lastValidByte = curByte;

                                // Ignore leading noise until Sync Marker 0xFE appears
                                if (bufPtr == 0 && (uint8_t)byteToProcess != SYNC_MARKER) continue;

                                if (bufPtr < MAX_FILE_SIZE && byteToProcess >= -1) {
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
            IAudioCaptureClient_ReleaseBuffer(pCap, nRead);
        } else Sleep(1);
    }

cleanup:
    if (pCl) IAudioClient_Stop(pCl);
    if (fileBuffer) free(fileBuffer);
    if (cfg) kiss_fft_free(cfg);
    CoUninitialize();
    return 0;
}
