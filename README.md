# Acoustic-Data-Transmission
This C program uses different frequencies of encoded sound data to transfer data wirelessly over short distances. 

# The How:
  This program acts as a Frequency-Shift Keying Receiver (FSK). It converts sound data (time domain voltage signals), into frequency-domain voltage specteral data to identify     patterns that corospond to different bytes. 

  My program takes in chunks of data (blocks), with each block having _FFT_SIZE_ total samples (defualt 2048). The script finds the most dominate frequency in the block. The
  higher the _FFT_FRAMES_ per block, the higher the accuracy, the more unique bytes packable into a frequency range, with a trade off of processing speed.

  The standard sample rate of most onboard microphones is 48,000hz. If we are getting 48,000 samples per second, we can fill up our _FFT_FRAMES_ in 48,000hz/_FFT_SIZE_ ms.
  In this case it is 48,000hz/2048 = 23.4hz. 23.3 is now our frequency range, or our _BIN_SPACING_. This means our program can differentaite between frequencies with at        least a 23.4hz gap (1000hz and 10234.hz). 

  Our sample size is a continious stream of data, our _FFT_FRAMES_ is the area we have loaded into memory, and the _STEP_SIZE_ is the amount we step _FFT_FRAMES_ forward.
  This means the lower the _STEP_SIZE_ the more times each data section will be analyzed. 
  
# Making the program work for your system:
   The speed, accuracy, and total unuiqe signals transferable are completely dependant on the speakers the decoder is inputting data from, and on the _THRESHOLD_ of the room.

   **THRESHOLD**: Any disturbances in the room like fans, tvs, voices or even the echo of the programs signals can interfere with the program. _THRESHOLD_ tells our program to       ignore all frequencies under the amplitude of its value. You can find the average ampltiude of your room using calibrate.c, at which         point you can set the _THRESHOLD_ slighty above this returned value. (**Defualt: 20f**)

   **DEBOUNCE**: The number of FFT frames that must agree on the value of a signal before it is accepted. The higher the debounce the slower the audio must be     played. For extremely low error systems with high end speakers to minamize smearing between frequency changes, a _DEBOUNCE_ of 0 can allow for extremely fast speeds. (**DEFAULT: 1**)

   **FFT FRAMES**: The number of frames in a block 
   
