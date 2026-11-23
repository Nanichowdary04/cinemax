// Small in-place complex FFT (radix-2, iterative) for the real-time path.
// prepare() allocates the tables; forward()/inverse() are allocation-free.
// Used by the immersive up-mixer, which packs two real signals into one complex
// transform (L + jR in, two output feeds per inverse transform).
#pragma once

#include <cmath>
#include <complex>
#include <utility>
#include <vector>

namespace cinemax::dsp
{
class Fft
{
public:
    using Complex = std::complex<float>;

    /** size must be a power of two (>= 4). */
    void prepare (int size)
    {
        n = size;
        int bits = 0;
        while ((1 << bits) < n) ++bits;

        rev.assign ((size_t) n, 0);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                r |= ((i >> b) & 1) << (bits - 1 - b);
            rev[(size_t) i] = r;
        }

        twiddle.resize ((size_t) (n / 2));
        for (int k = 0; k < n / 2; ++k)
        {
            const double a = -2.0 * 3.14159265358979323846 * k / n;
            twiddle[(size_t) k] = Complex ((float) std::cos (a), (float) std::sin (a));
        }
    }

    int size() const noexcept { return n; }

    /** Unscaled forward transform: X[k] = sum x[t] e^(-j 2 pi k t / n). */
    void forward (Complex* x) const noexcept { transform (x, false); }

    /** Inverse transform, scaled by 1/n (forward then inverse = identity). */
    void inverse (Complex* x) const noexcept
    {
        transform (x, true);
        const float s = 1.0f / (float) n;
        for (int i = 0; i < n; ++i)
            x[i] *= s;
    }

private:
    void transform (Complex* x, bool inv) const noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int r = rev[(size_t) i];
            if (r > i)
                std::swap (x[i], x[r]);
        }

        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len >> 1;
            const int step = n / len;
            for (int start = 0; start < n; start += len)
            {
                for (int k = 0; k < half; ++k)
                {
                    Complex w = twiddle[(size_t) (k * step)];
                    if (inv)
                        w = std::conj (w);
                    Complex& a = x[start + k];
                    Complex& b = x[start + k + half];
                    const Complex t (w.real() * b.real() - w.imag() * b.imag(),
                                     w.real() * b.imag() + w.imag() * b.real());
                    b = a - t;
                    a += t;
                }
            }
        }
    }

    int n = 0;
    std::vector<int> rev;
    std::vector<Complex> twiddle;
};

} // namespace cinemax::dsp
