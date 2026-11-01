#include <musyx/pc_audio_math.h>
#include <stdio.h>
#include <stdlib.h>

static void check(int condition) { if (!condition) abort(); }

int main(void) {
  check(musyxPcScaleQ15(100000, 32767) == 99996);
  check(musyxPcScaleQ15(-100000, 32767) == -99997);
  check(musyxPcScaleQ15(INT32_MAX, 65535) == INT32_MAX);
  check(musyxPcClamp32((int64_t)INT32_MAX + 1) == INT32_MAX);
  check(musyxPcClamp32((int64_t)INT32_MIN - 1) == INT32_MIN);
  check(musyxPcPcm8(0) == 0);
  check(musyxPcPcm8(127) == 32512);
  check(musyxPcPcm8(128) == -32768);
  check(musyxPcPcm8(255) == -256);

  int16_t coefficients[8][2] = {{2048, 0}}; // previous sample plus signed nibble
  const uint8_t block[8] = {0, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
  int16_t h1 = 0, h2 = 0;
  const unsigned end = musyxPcLoopEnd(14, 5, 5);
  check(end == 10); // do not play the tail after the loop end
  unsigned position = 0;
  for (unsigned output = 0; output < 30; ++output) {
    if (position == end) {
      position = 5;
      h1 = 5;
      h2 = 4;
    }
    const int sample = musyxPcAdpcmSample(block, position++, 0, coefficients, &h1, &h2);
    check(sample == (output < 10 ? (int)output + 1 : 6 + (int)(output - 10) % 5));
  }
  // Ring-buffer ADPCM is continuous: retaining history produces 15, not 1,
  // when the next buffer block follows the first fourteen samples.
  h1 = h2 = 0;
  for (unsigned i = 0; i < 14; ++i) musyxPcAdpcmSample(block, i, 0, coefficients, &h1, &h2);
  check(musyxPcStreamedAdpcm(4) && musyxPcStreamedAdpcm(5) && !musyxPcStreamedAdpcm(0));
  check(musyxPcAdpcmSample(block, 0, 0, coefficients, &h1, &h2) == 15);
  check(musyxPcLoopEnd(100, 14, 28) == 42);
  check(musyxPcLoopEnd(100, 0, 0) == 100);
  check(musyxPcLoopEnd(100, 60, 41) == 100); // loop end one past the last sample: still a loop
  check(musyxPcLoopEnd(UINT32_MAX, UINT32_MAX - 2, 10) == UINT32_MAX);
  puts("ADPCM loop, signed PCM8 and wide-mix regressions passed");
  return 0;
}
