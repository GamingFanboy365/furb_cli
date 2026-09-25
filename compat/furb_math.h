// Force-included into every Furbtendulator source (build.py).  The palette
// generator (GFX.cpp: NTSC/PAL/VT0x colour decoding, gamma), the sound
// filters and the expansion-sound tables (emu2413, FDS, Butterworth) call
// sin/cos/tan/exp/log/log2/log10/pow.  Their last bit differs between libm
// builds (i386 glibc uses x87 code), and a colour sitting on a rounding edge
// then comes out 1 off, so frame hashes would differ between the 32-bit,
// 64-bit and ARM builds.  These calls go to a vendored copy of musl's
// portable C implementations instead (compat/libm, built with FMA
// contraction off), so every architecture computes the same bits.  float
// arguments pick the float versions as <cmath>'s overloads would.
#pragma once
#include <math.h>
#include <cmath>

extern "C" {
double furb_sin(double); double furb_cos(double); double furb_tan(double);
double furb_exp(double); double furb_log(double); double furb_log2(double);
double furb_log10(double); double furb_pow(double, double);
float furb_sinf(float); float furb_cosf(float); float furb_powf(float, float);
}

namespace furb_m {
inline float sin(float x) { return furb_sinf(x); }
inline float cos(float x) { return furb_cosf(x); }
inline float pow(float x, float y) { return furb_powf(x, y); }
template <class T> inline double sin(T x) { return furb_sin((double)x); }
template <class T> inline double cos(T x) { return furb_cos((double)x); }
template <class A, class B> inline double pow(A x, B y) { return furb_pow((double)x, (double)y); }
// no float kernels bundled for these; the few float callers get the double result
template <class T> inline double tan(T x) { return furb_tan((double)x); }
template <class T> inline double exp(T x) { return furb_exp((double)x); }
template <class T> inline double log(T x) { return furb_log((double)x); }
template <class T> inline double log2(T x) { return furb_log2((double)x); }
template <class T> inline double log10(T x) { return furb_log10((double)x); }
}

#define sin(...) furb_m::sin(__VA_ARGS__)
#define cos(...) furb_m::cos(__VA_ARGS__)
#define tan(...) furb_m::tan(__VA_ARGS__)
#define exp(...) furb_m::exp(__VA_ARGS__)
#define log(...) furb_m::log(__VA_ARGS__)
#define log2(...) furb_m::log2(__VA_ARGS__)
#define log10(...) furb_m::log10(__VA_ARGS__)
#define pow(...) furb_m::pow(__VA_ARGS__)
