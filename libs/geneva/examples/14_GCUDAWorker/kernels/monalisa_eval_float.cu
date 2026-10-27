// CUDA Mona-Lisa fitness kernel for example 14 -- PIXEL-PARALLEL, FP32 version.
//
// Runtime-compiled by the GGPUConsumer's CUDA backend (NVRTC). The batch is evaluated in ONE launch
// of n_items * threads_per_item threads: each item is handled by `threads_per_item` cooperating
// threads, each rendering a STRIPE of the image's pixels and atomic-accumulating its partial deviation
// into the item's fitness. The backend zeroes the fitness buffer before launch. This gives strong GPU
// utilisation even for SMALL populations on a large canvas (where one-thread-per-individual would
// leave the GPU mostly idle); for large populations threads_per_item can be 1 and per-individual
// parallelism already saturates the device.
//
// This is the FP32 path: params/fitness/target are all float (the genome and the target image are
// float on the host too), so the whole device pipeline runs in single precision -- the actual FP32
// speedup. The math mirrors the host float score() in GMonaLisaProblem.hpp, so a CPU run and a GPU run
// agree closely (not bit-identical: the parallel-stripe atomic sum reassociates the per-pixel terms).
//
// problem-constant blob (floats): [W, H, target(W*H*3)].
// per-item parameters (dim = 10*NT + 3): per triangle [cx cy radius a1 a2 a3 r g b alpha], then bg[3].
//
// Triangle-corner caching: in the default launch geometry the backend maps ONE CUDA BLOCK to ONE item
// (blockDim.x == threads_per_item, see GCUDABackend.cpp). In that fast path the item's triangle corners
// are computed ONCE into SHARED memory cooperatively by the block's threads, so all pixel-threads of the
// item read them -- this is what lets each item carry ~1000 triangles (a per-thread corner array of that
// size would be infeasible). A general (any-geometry) fallback computes each triangle's corners inline
// per pixel with no cache and no cap.

// Maximum triangles whose corners are cached in shared memory (fast path). The cap is the example's
// maximum triangle count (the genome's n_triangles range is [1,1000]), kept identical to the DOUBLE
// kernel so both personalities behave the same. For FLOAT a cached corner pair costs 6 floats = 24 B,
// so 1000 triangles need 1000*6*4 = 24000 B -- well under the 48 KB static-shared limit (the DOUBLE
// kernel is the tighter one at 48000 B; see monalisa_eval_double.cu).
#define MONALISA_MAXTRI 1000

extern "C" __global__ void evaluate(
    const float *params, int n_items, int dim,
    const unsigned char *pconst, int pconst_size,
    float *fitness,
    int threads_per_item)
{
    if (threads_per_item < 1) {
        threads_per_item = 1;
    }

    const float *pc = reinterpret_cast<const float *>(pconst);
    const int W = (int)pc[0];
    const int H = (int)pc[1];
    const float *target = pc + 2;

    const float scale = (float)(W < H ? W : H);
    const float twoPi = 2.0f * 3.14159265358979323846f;
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
        const float *p = params + (long long)item * dim;
        // NT is clamped to MONALISA_MAXTRI so the shared array can never overflow. With the cap at 1000
        // this covers the example-14 1000-triangle workload; any triangles beyond the cap are ignored.
        int NT = NT_full;
        if (NT > MONALISA_MAXTRI) {
            NT = MONALISA_MAXTRI;
        }
        const float bgR = p[10 * NT_full + 0];
        const float bgG = p[10 * NT_full + 1];
        const float bgB = p[10 * NT_full + 2];

        __shared__ float s_corners[MONALISA_MAXTRI * 6];
        // The triangle is inscribed in a circle (its three vertices lie on it), so a pixel can be inside
        // the triangle only if it is inside that circumcircle. Cache the circle (centre + radius^2, in
        // pixels) alongside the corners and use it as the per-pixel reject: it is cheaper than the
        // axis-aligned bounding box (no fminf/fmaxf, just one squared-distance test) and measured ~1.29x
        // faster than the bbox cull at 1000 triangles. The DOUBLE kernel cannot afford this extra cache
        // (9 doubles/triangle would exceed the 48 KB shared-memory limit), so it keeps the bbox cull.
        __shared__ float s_circle[MONALISA_MAXTRI * 3]; // cx, cy, radius^2 (pixels)
        // Cooperatively compute the corners and the bounding circle of every triangle once. The FLOAT
        // kernel uses the fast __cosf/__sinf intrinsics (the double kernel has no such intrinsic): corners
        // are computed only once per item, not per pixel, so the reduced precision barely shifts edge
        // pixels while giving the best GPU time for this FP32 personality.
        for (int t = threadIdx.x; t < NT; t += blockDim.x) {
            const float *tri = p + t * 10;
            const float cx = tri[0] * W;
            const float cy = tri[1] * H;
            const float radius = tri[2];
            const float radius_px = radius * scale;
            s_circle[t * 3 + 0] = cx;
            s_circle[t * 3 + 1] = cy;
            s_circle[t * 3 + 2] = radius_px * radius_px;
            for (int k = 0; k < 3; ++k) {
                const float ang = tri[3 + k] * twoPi;
                s_corners[t * 6 + k * 2 + 0] = cx + radius * __cosf(ang) * scale;
                s_corners[t * 6 + k * 2 + 1] = cy + radius * __sinf(ang) * scale;
            }
        }
        __syncthreads();

        // This thread renders the pixels pix where (pix % threads_per_item == stripe).
        float partial = 0.0f;
        for (long long pix = stripe; pix < nPixels; pix += threads_per_item) {
            const int x = (int)(pix % W);
            const int y = (int)(pix / W);
            const float px = x + 0.5f;
            const float py = y + 0.5f;
            float r = bgR, g = bgG, b = bgB;
            for (int t = 0; t < NT; ++t) {
                // Circumcircle reject: skip this triangle unless the pixel lies inside its bounding circle.
                const float ccx = s_circle[t * 3 + 0];
                const float ccy = s_circle[t * 3 + 1];
                const float cr2 = s_circle[t * 3 + 2];
                const float ddx = px - ccx, ddy = py - ccy;
                if (ddx * ddx + ddy * ddy > cr2) {
                    continue;
                }
                const float *c = s_corners + t * 6;
                const float x1 = c[0], y1 = c[1], x2 = c[2], y2 = c[3], x3 = c[4], y3 = c[5];
                const float d1 = (px - x2) * (y1 - y2) - (py - y2) * (x1 - x2);
                const float d2 = (px - x3) * (y2 - y3) - (py - y3) * (x2 - x3);
                const float d3 = (px - x1) * (y3 - y1) - (py - y1) * (x3 - x1);
                const bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
                const bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
                if (!(hasNeg && hasPos)) {
                    const float *tri = p + t * 10;
                    const float a = tri[9];
                    r = (1.0f - a) * r + a * tri[6];
                    g = (1.0f - a) * g + a * tri[7];
                    b = (1.0f - a) * b + a * tri[8];
                }
            }
            const long long idx = pix * 3;
            const float dr = target[idx] - r;
            const float dg = target[idx + 1] - g;
            const float db = target[idx + 2] - b;
            const float f = 0.04f;
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
    const float *p = params + (long long)item * dim;
    const int NT = NT_full;
    const float bgR = p[10 * NT + 0];
    const float bgG = p[10 * NT + 1];
    const float bgB = p[10 * NT + 2];

    float partial = 0.0f;
    for (long long pix = stripe; pix < nPixels; pix += threads_per_item) {
        const int x = (int)(pix % W);
        const int y = (int)(pix / W);
        const float px = x + 0.5f;
        const float py = y + 0.5f;
        float r = bgR, g = bgG, b = bgB;
        for (int t = 0; t < NT; ++t) {
            const float *tri = p + t * 10;
            const float cx = tri[0] * W;
            const float cy = tri[1] * H;
            const float radius = tri[2];
            const float a0 = tri[3] * twoPi, a1 = tri[4] * twoPi, a2 = tri[5] * twoPi;
            const float x1 = cx + radius * __cosf(a0) * scale, y1 = cy + radius * __sinf(a0) * scale;
            const float x2 = cx + radius * __cosf(a1) * scale, y2 = cy + radius * __sinf(a1) * scale;
            const float x3 = cx + radius * __cosf(a2) * scale, y3 = cy + radius * __sinf(a2) * scale;
            const float minx = fminf(x1, fminf(x2, x3));
            const float maxx = fmaxf(x1, fmaxf(x2, x3));
            const float miny = fminf(y1, fminf(y2, y3));
            const float maxy = fmaxf(y1, fmaxf(y2, y3));
            if (px < minx || px > maxx || py < miny || py > maxy) {
                continue;
            }
            const float d1 = (px - x2) * (y1 - y2) - (py - y2) * (x1 - x2);
            const float d2 = (px - x3) * (y2 - y3) - (py - y3) * (x2 - x3);
            const float d3 = (px - x1) * (y3 - y1) - (py - y1) * (x3 - x1);
            const bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
            const bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
            if (!(hasNeg && hasPos)) {
                const float a = tri[9];
                r = (1.0f - a) * r + a * tri[6];
                g = (1.0f - a) * g + a * tri[7];
                b = (1.0f - a) * b + a * tri[8];
            }
        }
        const long long idx = pix * 3;
        const float dr = target[idx] - r;
        const float dg = target[idx + 1] - g;
        const float db = target[idx + 2] - b;
        const float f = 0.04f;
        partial += dr * dr / (dr * dr + f);
        partial += dg * dg / (dg * dg + f);
        partial += db * db / (db * db + f);
    }
    atomicAdd(&fitness[item], partial);
}
