// The FM1 engine sources compiled as one unit: the SDK's build.py compiles only the .cpp files directly in src/.
// kissfft (BSD-3-Clause, src/kissfft/COPYING) is C that also compiles as C++; its headers declare it extern "C".
#include "kissfft/kiss_fft.c"
#include "kissfft/kiss_fftr.c"

#include "fm1/Paul.cpp"
#include "fm1/Sampler.cpp"
#include "fm1/Stretch.cpp"
#include "fm1/Wav.cpp"
