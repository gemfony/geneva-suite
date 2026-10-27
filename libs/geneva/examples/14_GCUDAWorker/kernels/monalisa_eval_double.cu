// CUDA Mona-Lisa fitness kernel for example 14 -- PIXEL-PARALLEL version.
//
// Runtime-compiled by the GGPUConsumer's CUDA backend (NVRTC). The batch is evaluated in ONE launch
// of n_items * threads_per_item threads: each item is handled by `threads_per_item` cooperating
// threads, each rendering a STRIPE of the image's pixels and atomic-accumulating its partial deviation
// into the item's fitness. The backend zeroes the fitness buffer before launch. This gives strong GPU
// utilisation even for SMALL populations on a large canvas (where one-thread-per-individual would
// leave the GPU mostly idle); for large populations threads_per_item can be 1 and per-individual
// parallelism already saturates the device.
//
// The math mirrors the host score() in GMonaLisaProblem.hpp exactly (double precision), so the GPU
// result equals the CPU evaluate().
//
// problem-constant blob (doubles): [W, H, target(W*H*3)].
// per-item parameters (dim = 10*NT + 3): per triangle [cx cy radius a1 a2 a3 r g b alpha], then bg[3].
//
// Triangle-corner caching: in the default launch geometry the backend maps ONE CUDA BLOCK to ONE item
// (blockDim.x == threads_per_item, see GCUDABackend.cpp). In that fast path the item's triangle corners
// are computed ONCE into SHARED memory cooperatively by the block's threads, so all pixel-threads of the
// item read them -- this is what lets each item carry ~1000 triangles (a per-thread corner array of that
// size, 24-48 KB/thread, would be infeasible). A general (any-geometry) fallback computes each triangle's
// corners inline per pixel with no cache and no cap.

// Maximum triangles whose corners are cached in shared memory (fast path). The cap is the example's
// maximum triangle count (the genome's n_triangles range is [1,1000]), kept identical to the FLOAT
// kernel so both personalities behave the same. For DOUBLE a cached corner pair costs 6 doubles = 48 B,
// so 1000 triangles need 1000*6*8 = 48000 B -- just under the 48 KB (49152 B) static-shared limit.
#define MONALISA_MAXTRI 1000

extern "C" __global__ void evaluate(
    const double *params, int n_items, int dim,
    const unsigned char *pconst, int pconst_size,
    double *fitness,
    int threads_per_item)
{
    if (threads_per_item < 1) {
        threads_per_item = 1;
    }

    const double *pc = reinterpret_cast<const double *>(pconst);
    const int W = (int)pc[0];
    const int H = (int)pc[1];
    const double *target = pc + 2;

    const double scale = (double)(W < H ? W : H);
    const double twoPi = 2.0 * 3.14159265358979323846;
    const long long nPixels = (long long)W * H;
    const int NT_full = (dim - 3) / 10;

    // Fast path: one block == one item (blockDim.x == threads_per_item). Cache this item's triangle
    // corners once into shared memory, then every pixel-thread of the block reads them.
    if (blockDim.x == (unsigned)threads_per_item) {
        const int item = blockIdx.x;
        if (item >= n_items) {
            return;
        }
        const int stripe = threadIdx.x;
        const double *p = params + (long long)item * dim;
        // NT is clamped to MONALISA_MAXTRI so the shared array can never overflow. With the cap at 1000
        // this covers the example-14 1000-triangle workload; any triangles beyond the cap are ignored.
        int NT = NT_full;
        if (NT > MONALISA_MAXTRI) {
            NT = MONALISA_MAXTRI;
        }
        const double bgR = p[10 * NT_full + 0];
        const double bgG = p[10 * NT_full + 1];
        const double bgB = p[10 * NT_full + 2];

        __shared__ double s_corners[MONALISA_MAXTRI * 6];
        // Cooperatively compute the three corners of every triangle once.
        for (int t = threadIdx.x; t < NT; t += blockDim.x) {
            const double *tri = p + t * 10;
            const double cx = tri[0] * W;
            const double cy = tri[1] * H;
            const double radius = tri[2];
            for (int k = 0; k < 3; ++k) {
                const double ang = tri[3 + k] * twoPi;
                s_corners[t * 6 + k * 2 + 0] = cx + radius * cos(ang) * scale;
                s_corners[t * 6 + k * 2 + 1] = cy + radius * sin(ang) * scale;
            }
        }
        __syncthreads();

        // This thread renders the pixels pix where (pix % threads_per_item == stripe).
        double partial = 0.0;
        for (long long pix = stripe; pix < nPixels; pix += threads_per_item) {
            const int x = (int)(pix % W);
            const int y = (int)(pix / W);
            const double px = x + 0.5;
            const double py = y + 0.5;
            double r = bgR, g = bgG, b = bgB;
            for (int t = 0; t < NT; ++t) {
                const double *c = s_corners + t * 6;
                const double x1 = c[0], y1 = c[1], x2 = c[2], y2 = c[3], x3 = c[4], y3 = c[5];
                const double minx = fmin(x1, fmin(x2, x3));
                const double maxx = fmax(x1, fmax(x2, x3));
                const double miny = fmin(y1, fmin(y2, y3));
                const double maxy = fmax(y1, fmax(y2, y3));
                if (px < minx || px > maxx || py < miny || py > maxy) {
                    continue;
                }
                const double d1 = (px - x2) * (y1 - y2) - (py - y2) * (x1 - x2);
                const double d2 = (px - x3) * (y2 - y3) - (py - y3) * (x2 - x3);
                const double d3 = (px - x1) * (y3 - y1) - (py - y1) * (x3 - x1);
                const bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
                const bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
                if (!(hasNeg && hasPos)) {
                    const double *tri = p + t * 10;
                    const double a = tri[9];
                    r = (1.0 - a) * r + a * tri[6];
                    g = (1.0 - a) * g + a * tri[7];
                    b = (1.0 - a) * b + a * tri[8];
                }
            }
            const long long idx = pix * 3;
            const double dr = target[idx] - r;
            const double dg = target[idx + 1] - g;
            const double db = target[idx + 2] - b;
            const double f = 0.04;
            partial += dr * dr / (dr * dr + f);
            partial += dg * dg / (dg * dg + f);
            partial += db * db / (db * db + f);
        }
        atomicAdd(&fitness[item], partial);
        return;
    }

    // Fallback path: arbitrary launch geometry (a block may span multiple items). No corner cache and no
    // triangle cap -- corners are recomputed inline per pixel. Slower, but correct for any geometry. Not
    // used by the default config.
    const long long gid = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    const int item = (int)(gid / threads_per_item);
    const int stripe = (int)(gid % threads_per_item);
    if (item >= n_items) {
        return;
    }
    const double *p = params + (long long)item * dim;
    const int NT = NT_full;
    const double bgR = p[10 * NT + 0];
    const double bgG = p[10 * NT + 1];
    const double bgB = p[10 * NT + 2];

    double partial = 0.0;
    for (long long pix = stripe; pix < nPixels; pix += threads_per_item) {
        const int x = (int)(pix % W);
        const int y = (int)(pix / W);
        const double px = x + 0.5;
        const double py = y + 0.5;
        double r = bgR, g = bgG, b = bgB;
        for (int t = 0; t < NT; ++t) {
            const double *tri = p + t * 10;
            const double cx = tri[0] * W;
            const double cy = tri[1] * H;
            const double radius = tri[2];
            const double a0 = tri[3] * twoPi, a1 = tri[4] * twoPi, a2 = tri[5] * twoPi;
            const double x1 = cx + radius * cos(a0) * scale, y1 = cy + radius * sin(a0) * scale;
            const double x2 = cx + radius * cos(a1) * scale, y2 = cy + radius * sin(a1) * scale;
            const double x3 = cx + radius * cos(a2) * scale, y3 = cy + radius * sin(a2) * scale;
            const double minx = fmin(x1, fmin(x2, x3));
            const double maxx = fmax(x1, fmax(x2, x3));
            const double miny = fmin(y1, fmin(y2, y3));
            const double maxy = fmax(y1, fmax(y2, y3));
            if (px < minx || px > maxx || py < miny || py > maxy) {
                continue;
            }
            const double d1 = (px - x2) * (y1 - y2) - (py - y2) * (x1 - x2);
            const double d2 = (px - x3) * (y2 - y3) - (py - y3) * (x2 - x3);
            const double d3 = (px - x1) * (y3 - y1) - (py - y1) * (x3 - x1);
            const bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
            const bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
            if (!(hasNeg && hasPos)) {
                const double a = tri[9];
                r = (1.0 - a) * r + a * tri[6];
                g = (1.0 - a) * g + a * tri[7];
                b = (1.0 - a) * b + a * tri[8];
            }
        }
        const long long idx = pix * 3;
        const double dr = target[idx] - r;
        const double dg = target[idx + 1] - g;
        const double db = target[idx + 2] - b;
        const double f = 0.04;
        partial += dr * dr / (dr * dr + f);
        partial += dg * dg / (dg * dg + f);
        partial += db * db / (db * db + f);
    }
    atomicAdd(&fitness[item], partial);
}
