// CUDA evaluation kernel for the GPU optimisation benchmark.
//
// This file is NOT compiled at build time: the courtier GPU consumer's CUDA backend reads it at run
// time (its path comes from config/GGPUConsumer.json) and compiles it with NVRTC. It must follow the
// framework's evaluation ABI: an extern "C" (un-mangled) entry taking the flat row-major parameter
// buffer, the item count and dimension, an opaque problem-constant blob, and writing one fitness per
// item.
//
// Problem constant: a single int "function id" matching Gem::Geneva::Benchmarks::FUNC_* (0..14). The
// math below MIRRORS geneva/individuals/GBenchmarkFunctions.hpp (the shared CPU/host reference), so a
// CPU run and a GPU run produce the same fitness. Keep the two in sync if functions change.
//
// Standalone (no Geneva/std headers) so NVRTC can compile it: constants are spelled out and only CUDA
// device math intrinsics are used.

#define BM_PI 3.14159265358979323846
#define BM_E  2.71828182845904523536

// ── Function id constants (must match GBenchmarkFunctions.hpp) ──────────────────────────────────
#define FUNC_PARABOLA          0
#define FUNC_NOISYPARABOLA     1
#define FUNC_ROSENBROCK        2
#define FUNC_ACKLEY            3
#define FUNC_RASTRIGIN         4
#define FUNC_SCHWEFEL          5
#define FUNC_SALOMON           6
#define FUNC_NEGPARABOLA       7
#define FUNC_ACKLEY_CANONICAL  8
#define FUNC_GRIEWANK          9
#define FUNC_LEVY             10
#define FUNC_STYBLINSKI_TANG  11
#define FUNC_ELLIPSOID        12
#define FUNC_MICHALEWICZ      13
#define FUNC_ZAKHAROV         14

__device__ inline double bm_parabola(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n; ++i) { r += x[i] * x[i]; }
    return r;
}

__device__ inline double bm_noisyParabola(const double *x, int n) {
    double sq = 0.;
    for(int i = 0; i < n; ++i) { sq += x[i] * x[i]; }
    return (cos(sq) + 2.) * sq;
}

__device__ inline double bm_rosenbrock(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n - 1; ++i) {
        double t = x[i + 1] - x[i] * x[i];
        double u = 1. - x[i];
        r += 100. * t * t + u * u;
    }
    return r;
}

__device__ inline double bm_ackley(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n - 1; ++i) {
        double s = x[i] * x[i] + x[i + 1] * x[i + 1];
        r += exp(-0.2) * sqrt(s) + 3. * (cos(2. * x[i]) + sin(2. * x[i + 1]));
    }
    return r;
}

__device__ inline double bm_rastrigin(const double *x, int n) {
    double r = 10. * n;
    for(int i = 0; i < n; ++i) { r += x[i] * x[i] - 10. * cos(2. * BM_PI * x[i]); }
    return r;
}

__device__ inline double bm_schwefel(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n; ++i) { r += -x[i] * sin(sqrt(fabs(x[i]))); }
    return r / n;
}

__device__ inline double bm_salomon(const double *x, int n) {
    double sq = 0.;
    for(int i = 0; i < n; ++i) { sq += x[i] * x[i]; }
    const double r = sqrt(sq);
    return -cos(2. * BM_PI * r) + 0.1 * r + 1.;
}

__device__ inline double bm_ackleyCanonical(const double *x, int n) {
    double sq = 0., cs = 0.;
    for(int i = 0; i < n; ++i) {
        sq += x[i] * x[i];
        cs += cos(2. * BM_PI * x[i]);
    }
    const double inv_n = 1. / n;
    return -20. * exp(-0.2 * sqrt(sq * inv_n)) - exp(cs * inv_n) + 20. + BM_E;
}

__device__ inline double bm_griewank(const double *x, int n) {
    double sq = 0., prod = 1.;
    for(int i = 0; i < n; ++i) {
        sq += x[i] * x[i];
        prod *= cos(x[i] / sqrt((double)(i + 1)));
    }
    return sq / 4000. - prod + 1.;
}

__device__ inline double bm_levy(const double *x, int n) {
    const double w0 = 1. + (x[0] - 1.) / 4.;
    double r = sin(BM_PI * w0) * sin(BM_PI * w0);
    for(int i = 0; i < n - 1; ++i) {
        const double wi = 1. + (x[i] - 1.) / 4.;
        const double wi1 = 1. + (x[i + 1] - 1.) / 4.;
        const double sm = sin(BM_PI * wi1);
        r += (wi - 1.) * (wi - 1.) * (1. + 10. * sm * sm);
    }
    const double wn = 1. + (x[n - 1] - 1.) / 4.;
    const double sn = sin(2. * BM_PI * wn);
    r += (wn - 1.) * (wn - 1.) * (1. + sn * sn);
    return r;
}

__device__ inline double bm_styblinskiTang(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n; ++i) {
        const double xi = x[i];
        const double x2 = xi * xi;
        r += x2 * x2 - 16. * x2 + 5. * xi;
    }
    return 0.5 * r;
}

__device__ inline double bm_ellipsoid(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n; ++i) {
        const double e = (n > 1) ? 6. * i / (n - 1.) : 0.;
        r += pow(10., e) * x[i] * x[i];
    }
    return r;
}

__device__ inline double bm_michalewicz(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n; ++i) {
        const double s = sin((double)(i + 1) * x[i] * x[i] / BM_PI);
        const double s2 = s * s;
        const double s20 = s2 * s2 * s2 * s2 * s2 * s2 * s2 * s2 * s2 * s2; // s^20
        r -= sin(x[i]) * s20;
    }
    return r;
}

__device__ inline double bm_zakharov(const double *x, int n) {
    double sq = 0., lin = 0.;
    for(int i = 0; i < n; ++i) {
        sq += x[i] * x[i];
        lin += 0.5 * (i + 1) * x[i];
    }
    return sq + lin * lin + lin * lin * lin * lin;
}

__device__ inline double bm_eval(int fid, const double *x, int n) {
    switch(fid) {
    case FUNC_PARABOLA:         return bm_parabola(x, n);
    case FUNC_NOISYPARABOLA:    return bm_noisyParabola(x, n);
    case FUNC_ROSENBROCK:       return bm_rosenbrock(x, n);
    case FUNC_ACKLEY:           return bm_ackley(x, n);
    case FUNC_RASTRIGIN:        return bm_rastrigin(x, n);
    case FUNC_SCHWEFEL:         return bm_schwefel(x, n);
    case FUNC_SALOMON:          return bm_salomon(x, n);
    case FUNC_NEGPARABOLA:      return -bm_parabola(x, n);
    case FUNC_ACKLEY_CANONICAL: return bm_ackleyCanonical(x, n);
    case FUNC_GRIEWANK:         return bm_griewank(x, n);
    case FUNC_LEVY:             return bm_levy(x, n);
    case FUNC_STYBLINSKI_TANG:  return bm_styblinskiTang(x, n);
    case FUNC_ELLIPSOID:        return bm_ellipsoid(x, n);
    case FUNC_MICHALEWICZ:      return bm_michalewicz(x, n);
    case FUNC_ZAKHAROV:         return bm_zakharov(x, n);
    default:                    return 0.;
    }
}

extern "C" __global__ void evaluate(
    const double *params, int n_items, int dim,
    const unsigned char *pconst, int pconst_size,
    double *fitness,
    int threads_per_item)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if(i >= n_items) {
        return;
    }
    int fid = 0;
    if(pconst_size >= (int)sizeof(int)) {
        fid = *reinterpret_cast<const int *>(pconst);
    }
    const double *x = params + (long long)i * dim;
    fitness[i] = bm_eval(fid, x, dim);
}
