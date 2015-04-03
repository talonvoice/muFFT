
#include <complex>
#include <cmath>
#include <immintrin.h>
#include <assert.h>

#include <fftw3.h>

#define N 4096
#define ITERATIONS 1
#define RADIX2 1
#define RADIX4 1
#define FFTW 1
#define DEBUG 1

using namespace std;

static inline complex<float> twiddle(int direction, int k, int p)
{
   double phase = (M_PI * direction * k) / p;
   return complex<float>(cos(phase), sin(phase));
}

static inline __m256 _mm256_cmul_ps(__m256 a, __m256 b)
{
   auto r3 = _mm256_permute_ps(a, _MM_SHUFFLE(2, 3, 0, 1));
   auto r1 = _mm256_moveldup_ps(b);
   auto R0 = _mm256_mul_ps(a, r1);
   auto r2 = _mm256_movehdup_ps(b);
   auto R1 = _mm256_mul_ps(r2, r3);
   return _mm256_addsub_ps(R0, R1);
}

static void fft_forward_radix4_p1(complex<float> *output, const complex<float> *input,
      unsigned samples)
{
   const auto flip_signs = _mm256_set_ps(-0.0f, 0.0f, -0.0f, 0.0f, -0.0f, 0.0f, -0.0f, 0.0f);

   unsigned quarter_samples = samples >> 2;
   for (unsigned i = 0; i < quarter_samples; i += 4)
   {
      auto a = _mm256_load_ps((const float*)&input[i]);
      auto b = _mm256_load_ps((const float*)&input[i + quarter_samples]);
      auto c = _mm256_load_ps((const float*)&input[i + 2 * quarter_samples]);
      auto d = _mm256_load_ps((const float*)&input[i + 3 * quarter_samples]);

      auto r0 = _mm256_add_ps(a, c);
      auto r1 = _mm256_sub_ps(a, c);
      auto r2 = _mm256_add_ps(b, d);
      auto r3 = _mm256_sub_ps(b, d);
      r3 = _mm256_xor_ps(_mm256_permute_ps(r3, _MM_SHUFFLE(2, 3, 0, 1)), flip_signs);

      auto o0 = _mm256_add_ps(r0, r2);
      auto o1 = _mm256_add_ps(r1, r3);
      auto o2 = _mm256_sub_ps(r0, r2);
      auto o3 = _mm256_sub_ps(r1, r3);

      // Transpose
      auto o0o1_lo = (__m256)_mm256_unpacklo_pd((__m256d)o0, (__m256d)o1);
      auto o0o1_hi = (__m256)_mm256_unpackhi_pd((__m256d)o0, (__m256d)o1);
      auto o2o3_lo = (__m256)_mm256_unpacklo_pd((__m256d)o2, (__m256d)o3);
      auto o2o3_hi = (__m256)_mm256_unpackhi_pd((__m256d)o2, (__m256d)o3);
      o0 = _mm256_permute2f128_ps(o0o1_lo, o2o3_lo, (2 << 4) | (0 << 0));
      o1 = _mm256_permute2f128_ps(o0o1_lo, o2o3_lo, (3 << 4) | (1 << 0));
      o2 = _mm256_permute2f128_ps(o0o1_hi, o2o3_hi, (2 << 4) | (0 << 0));
      o3 = _mm256_permute2f128_ps(o0o1_hi, o2o3_hi, (3 << 4) | (1 << 0));

      unsigned j = i << 2;
      _mm256_store_ps((float*)&output[j +  0], o0);
      _mm256_store_ps((float*)&output[j +  4], o1);
      _mm256_store_ps((float*)&output[j +  8], o2);
      _mm256_store_ps((float*)&output[j + 12], o3);
   }

#if 0
   unsigned quarter_samples = samples >> 2;
   for (unsigned i = 0; i < quarter_samples; i++)
   {
      auto a = input[i];
      auto b = input[i + quarter_samples];
      auto c = input[i + 2 * quarter_samples];
      auto d = input[i + 3 * quarter_samples];

      auto r0 = a + c;
      auto r1 = a - c;
      auto r2 = b + d;
      auto r3 = b - d;
      r3 = complex<float>(r3.imag(), -r3.real());

      unsigned j = i << 2;
      output[j + 0] = r0 + r2;
      output[j + 1] = r1 + r3;
      output[j + 2] = r0 - r2;
      output[j + 3] = r1 - r3;
   }
#endif
}

static void fft_forward_radix4_generic(complex<float> *output, const complex<float> *input,
      const complex<float> *twiddles, unsigned p, unsigned samples)
{
   unsigned quarter_samples = samples >> 2;

   for (unsigned i = 0; i < quarter_samples; i += 4)
   {
      unsigned k = i & (p - 1);

      auto w = _mm256_load_ps((const float*)&twiddles[k]);
      auto w0 = _mm256_load_ps((const float*)&twiddles[p + k]);
      auto w1 = _mm256_load_ps((const float*)&twiddles[2 * p + k]);

      auto a = _mm256_load_ps((const float*)&input[i]);
      auto b = _mm256_load_ps((const float*)&input[i + quarter_samples]);
      auto c = _mm256_load_ps((const float*)&input[i + 2 * quarter_samples]);
      auto d = _mm256_load_ps((const float*)&input[i + 3 * quarter_samples]);

      c = _mm256_cmul_ps(c, w);
      d = _mm256_cmul_ps(d, w);

      auto r0 = _mm256_add_ps(a, c);
      auto r1 = _mm256_sub_ps(a, c);
      auto r2 = _mm256_add_ps(b, d);
      auto r3 = _mm256_sub_ps(b, d);

      r2 = _mm256_cmul_ps(r2, w0);
      r3 = _mm256_cmul_ps(r3, w1);

      auto o0 = _mm256_add_ps(r0, r2);
      auto o1 = _mm256_sub_ps(r0, r2);
      auto o2 = _mm256_add_ps(r1, r3);
      auto o3 = _mm256_sub_ps(r1, r3);

      unsigned j = ((i - k) << 2) + k;
      _mm256_store_ps((float*)&output[j + 0], o0);
      _mm256_store_ps((float*)&output[j + 1 * p], o2);
      _mm256_store_ps((float*)&output[j + 2 * p], o1);
      _mm256_store_ps((float*)&output[j + 3 * p], o3);
   }

#if 0
   for (unsigned i = 0; i < quarter_samples; i++)
   {
      unsigned k = i & (p - 1);

      auto a = input[i];
      auto b = input[i + quarter_samples];
      auto c = twiddles[k] * input[i + 2 * quarter_samples];
      auto d = twiddles[k] * input[i + 3 * quarter_samples];

      // DFT-2
      auto r0 = a + c;
      auto r1 = a - c;
      auto r2 = b + d;
      auto r3 = b - d;

      r2 *= twiddles[p + k];
      r3 *= twiddles[p + k + p];

      // DFT-2
      auto o0 = r0 + r2;
      auto o1 = r0 - r2;
      auto o2 = r1 + r3;
      auto o3 = r1 - r3;

      unsigned j = ((i - k) << 2) + k;
      output[j +     0] = o0;
      output[j + 1 * p] = o2;
      output[j + 2 * p] = o1;
      output[j + 3 * p] = o3;
   }
#endif
}

static void fft_forward_radix2_p1(complex<float> *output, const complex<float> *input,
      unsigned samples)
{
   unsigned half_samples = samples >> 1;
   for (unsigned i = 0; i < half_samples; i += 4)
   {
      auto a = _mm256_load_ps((const float*)&input[i]);
      auto b = _mm256_load_ps((const float*)&input[i + half_samples]);

      auto r0 = _mm256_add_ps(a, b);
      auto r1 = _mm256_sub_ps(a, b);
      a = (__m256)_mm256_unpacklo_pd((__m256d)r0, (__m256d)r1);
      b = (__m256)_mm256_unpackhi_pd((__m256d)r0, (__m256d)r1);

      unsigned j = i << 1;
      _mm256_store_ps((float*)&output[j + 0], a);
      _mm256_store_ps((float*)&output[j + 4], b);
   }

#if 0
   unsigned half_samples = samples >> 1;
   for (unsigned i = 0; i < half_samples; i++)
   {
      auto a = input[i];
      auto b = input[i + half_samples]; 

      unsigned j = i << 1;
      output[j + 0] = a + b;
      output[j + 1] = a - b;
   }
#endif
}

static void fft_forward_radix2_p2(complex<float> *output, const complex<float> *input,
      const complex<float> *twiddles, unsigned samples)
{
   unsigned half_samples = samples >> 1;
   const auto flip_signs = _mm256_set_ps(-0.0f, 0.0f, 0.0f, 0.0f, -0.0f, 0.0f, 0.0f, 0.0f);

   for (unsigned i = 0; i < half_samples; i += 4)
   {
      auto a = _mm256_load_ps((const float*)&input[i]);
      auto b = _mm256_load_ps((const float*)&input[i + half_samples]);
      b = _mm256_xor_ps(_mm256_permute_ps(b, _MM_SHUFFLE(2, 3, 1, 0)), flip_signs);

      auto r0 = _mm256_add_ps(a, b); // { c0, c1, c4, c5 }
      auto r1 = _mm256_sub_ps(a, b); // { c2, c3, c6, c7 }
      a = _mm256_permute2f128_ps(r0, r1, (2 << 4) | (0 << 0));
      b = _mm256_permute2f128_ps(r0, r1, (3 << 4) | (1 << 0));

      unsigned j = i << 1;
      _mm256_store_ps((float*)&output[j + 0], a);
      _mm256_store_ps((float*)&output[j + 4], b);
   }

#if 0
   unsigned half_samples = samples >> 1;
   for (unsigned i = 0; i < half_samples; i++)
   {
      unsigned k = i & (2 - 1);
      auto a = input[i];
      auto b = twiddles[k] * input[i + half_samples];

      unsigned j = (i << 1) - k;
      output[j + 0] = a + b;
      output[j + 2] = a - b;
   }
#endif
}

static void fft_forward_radix2_generic(complex<float> *output, const complex<float> *input,
      const complex<float> *twiddles, unsigned p, unsigned samples)
{
   unsigned half_samples = samples >> 1;

   for (unsigned i = 0; i < half_samples; i += 4)
   {
      unsigned k = i & (p - 1);

      auto w = _mm256_load_ps((const float*)&twiddles[k]);
      auto a = _mm256_load_ps((const float*)&input[i]);
      auto b = _mm256_load_ps((const float*)&input[i + half_samples]);
      b = _mm256_cmul_ps(b, w);

      auto r0 = _mm256_add_ps(a, b);
      auto r1 = _mm256_sub_ps(a, b);

      unsigned j = (i << 1) - k;
      _mm256_store_ps((float*)&output[j + 0], r0);
      _mm256_store_ps((float*)&output[j + p], r1);
   }

#if 0
   unsigned half_samples = samples >> 1;
   for (unsigned i = 0; i < half_samples; i++)
   {
      unsigned k = i & (p - 1);
      auto a = input[i];
      auto b = twiddles[k] * input[i + half_samples];

      unsigned j = (i << 1) - k;
      output[j + 0] = a + b;
      output[j + p] = a - b;
   }
#endif
}

int main()
{
   complex<float> twiddles[N] __attribute__((aligned(64)));
   complex<float> input[N] __attribute__((aligned(64)));
   complex<float> tmp0[N] __attribute__((aligned(64)));
   complex<float> tmp1[N] __attribute__((aligned(64)));

   auto *pt = twiddles;
   for (unsigned p = 1; p < N; p <<= 1)
   {
      for (unsigned k = 0; k < p; k++)
         pt[k] = twiddle(-1, k, p);
      pt += p;
      if (p == 2)
         pt++;
   }

   for (unsigned i = 0; i < N; i++)
      input[i] = complex<float>(5.0f - i, i);

#if RADIX2
   // Radix-2

   for (unsigned i = 0; i < ITERATIONS; i++)
   {
      pt = twiddles;

      fft_forward_radix2_p1(tmp0, input, N);
      pt += 1;
      fft_forward_radix2_p2(tmp1, tmp0, pt, N);
      pt += 3;

      auto *out = tmp0;
      auto *in = tmp1;
      for (unsigned p = 4; p < N; p <<= 1)
      {
         fft_forward_radix2_generic(out, in, pt, p, N);
         pt += p;
         swap(out, in);
      }

#if DEBUG
      for (unsigned i = 0; i < N; i++)
         printf("Radix-2 FFT[%03u] = (%+8.3f, %+8.3f)\n", i, in[i].real(), in[i].imag());
#endif
   }
#endif

#if RADIX4
   // Radix-4

   for (unsigned i = 0; i < ITERATIONS; i++)
   {
      pt = twiddles;

      fft_forward_radix4_p1(tmp0, input, N);
      pt += 4;
      auto *out = tmp1;
      auto *in = tmp0;

      for (unsigned p = 4; p < N; p <<= 2)
      {
         fft_forward_radix4_generic(out, in, pt, p, N);
         swap(out, in);
         pt += p * 3;
      }

#if DEBUG
      for (unsigned i = 0; i < 256; i++)
         printf("Radix-4 FFT[%03u] = (%+8.3f, %+8.3f)\n", i, in[i].real(), in[i].imag());
#endif
   }
#endif

#if FFTW
   complex<float> *in, *out;
   fftwf_plan p;
   in = (complex<float>*)fftwf_malloc(sizeof(fftw_complex) * N);
   out = (complex<float>*)fftwf_malloc(sizeof(fftw_complex) * N);

   p = fftwf_plan_dft_1d(N, (fftwf_complex*)in, (fftwf_complex*)out, FFTW_FORWARD, FFTW_ESTIMATE);
   if (!p)
      return 1;

   for (unsigned i = 0; i < N; i++)
      in[i] = complex<float>(5.0f - i, i);
   for (unsigned i = 0; i < ITERATIONS; i++)
   {
      fftwf_execute(p);
#if DEBUG
      for (unsigned i = 0; i < N; i++)
         printf("FFTW FFT[%03u] = (%+8.3f, %+8.3f)\n", i, out[i].real(), out[i].imag());
#endif
   }
   fftwf_destroy_plan(p);
   fftwf_free(in);
   fftwf_free(out);
#endif
}
