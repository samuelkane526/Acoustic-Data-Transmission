# Acoustic-Data-Transmission
This C program uses different frequencies of encoded sound data to transfer data wirelessly over short distances. 

# The How:
  This program acts as a Frequency-Shift Keying Receiver (FSK). It converts sound data (time domain voltage signals), into frequency-domain voltage specteral data to           identify patterns that corospond to different bytes. 

  My program takes in chunks of data (blocks), with each block having _FFT_SIZE_ total samples (defualt 2048). The script finds the most dominate frequency in the block. The
  higher the _FFT_FRAMES_ per block, the higher the accuracy, the more unique bytes packable into a frequency range, with a trade off of processing speed.

  The standard sample rate of most onboard microphones is 48,000hz. If we are getting 48,000 samples per second, we can fill up our _FFT_FRAMES_ in 48,000hz/_FFT_SIZE_ ms.
  In this case it is 48,000hz/2048 = 23.4hz. 23.3 is now our frequency range, or our _BIN_SPACING_. This means our program can differentaite between frequencies with at        least a 23.4hz gap (1000hz and 10234.hz). 

  Our sample size is a continious stream of data, our _FFT_FRAMES_ is the area we have loaded into memory, and the _STEP_SIZE_ is the amount we step _FFT_FRAMES_ forward.
  This means the lower the _STEP_SIZE_ the more times each data section will be analyzed. 

  Using the _BIN_SPACING_ value we already got, we populate the FILL IN TEXT~~~~~~~~~~~~~~~~~~

  Any disturbances in the room like fans, tvs, voices or even the echo of the programs signals can interfere with the program. _THRESHOLD_ tells our program to ignore all      frequencies under the amplitude of its value. You can fine tune this with calibrate.c. The larger and less padded the room is, the higher the required threshold.

  The final part to our program in the _DEBOUNCE_. The _DEBOUNCE_ is the number of framtes in FFT that is checked before it decides on a signal. The higher the _DEBOUNCE_  
  the higher the accuracy. However, this means you will need to play the audio at a slower speed. (0 = one check)

  That is all of the technical details behind the audio transmission!

  **Making This Work For Your System**:
  

   
