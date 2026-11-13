// CUDA evaluation kernel for the loadable-GPU paraboloid demo (example 21).
//
// This file is NOT compiled at build time: the courtier GPU consumer's CUDA backend reads it at RUN time
// (its path comes from config/GGPUConsumer.json) and compiles it with NVRTC. It follows the framework's
// evaluation ABI: an extern "C" (un-mangled) entry taking the flat row-major parameter buffer, the item
// count and dimension, an opaque problem-constant blob (unused here), and writing one fitness per item.
//
// The math is the n-dimensional paraboloid f(x) = sum_i x_i^2 -- the SAME objective that
// GGPUParaboloid::evaluate() computes on the CPU, so a CPU run (--consumer stc) and a GPU run
// (--consumer gpu) produce the same fitness. It is deliberately standalone (no Geneva/std headers) so NVRTC
// can compile it with no include path.

extern "C" __global__ void evaluate(
    const double *params, int n_items, int dim,
    const unsigned char * /*pconst*/, int /*pconst_size*/,
    double *fitness,
    int /*threads_per_item*/)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if(i >= n_items) {
        return;
    }
    const double *x = params + (long long)i * dim;
    double r = 0.;
    for(int j = 0; j < dim; ++j) {
        r += x[j] * x[j];
    }
    fitness[i] = r;
}
