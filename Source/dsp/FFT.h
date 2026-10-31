#pragma once
// Iterative radix-2 complex FFT. Small, allocation-free after construction.
#include <cmath>
#include <complex>
#include <vector>

namespace se
{
class FFT
{
public:
    explicit FFT (int order = 11) { setOrder (order); }

    void setOrder (int order)
    {
        n = 1 << order;
        rev.assign ((size_t) n, 0);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < order; ++b)
                if (i & (1 << b))
                    r |= 1 << (order - 1 - b);
            rev[(size_t) i] = r;
        }
        twiddle.resize ((size_t) n / 2);
        for (int i = 0; i < n / 2; ++i)
            twiddle[(size_t) i] = std::polar (1.0f, (float) (-2.0 * 3.14159265358979323846 * i / n));
    }

    int size() const { return n; }

    /** In-place transform. inverse == true computes the unscaled inverse. */
    void perform (std::complex<float>* data, bool inverse) const
    {
        for (int i = 0; i < n; ++i)
            if (i < rev[(size_t) i])
                std::swap (data[i], data[rev[(size_t) i]]);

        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2;
            const int step = n / len;
            for (int start = 0; start < n; start += len)
            {
                for (int k = 0; k < half; ++k)
                {
                    auto w = twiddle[(size_t) (k * step)];
                    if (inverse)
                        w = std::conj (w);
                    const auto u = data[start + k];
                    const auto v = data[start + k + half] * w;
                    data[start + k] = u + v;
                    data[start + k + half] = u - v;
                }
            }
        }
    }

private:
    int n = 0;
    std::vector<int> rev;
    std::vector<std::complex<float>> twiddle;
};
} // namespace se
