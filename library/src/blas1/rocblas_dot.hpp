/* ************************************************************************
 * Copyright 2016-2021 Advanced Micro Devices, Inc.
 * ************************************************************************ */

#pragma once

#include "check_numerics_vector.hpp"
#include "handle.hpp"
#include "logging.hpp"
#include "reduction_strided_batched.hpp"
#include "utility.hpp"
#include <hip/hip_runtime.h>

static constexpr int rocblas_log2ui(int x)
{
    unsigned int ax = (unsigned int)x;
    int          v  = 0;
    while(ax >>= 1)
    {
        v++;
    }
    return v;
}

template <int N, typename T>
__inline__ __device__ T wavefront_reduce(T val)
{
    constexpr int WFBITS = rocblas_log2ui(N);
    int           offset = 1 << (WFBITS - 1);
    for(int i = 0; i < WFBITS; i++)
    {
        val += __shfl_down(val, offset);
        offset >>= 1;
    }
    return val;
}

template <int N>
__inline__ __device__ rocblas_float_complex wavefront_reduce(rocblas_float_complex val)
{
    constexpr int WFBITS = rocblas_log2ui(N);
    int           offset = 1 << (WFBITS - 1);
    for(int i = 0; i < WFBITS; i++)
    {
        val.real(val.real() + __shfl_down(val.real(), offset));
        val.imag(val.imag() + __shfl_down(val.imag(), offset));
        offset >>= 1;
    }
    return val;
}

template <int N>
__inline__ __device__ rocblas_double_complex wavefront_reduce(rocblas_double_complex val)
{
    constexpr int WFBITS = rocblas_log2ui(N);
    int           offset = 1 << (WFBITS - 1);
    for(int i = 0; i < WFBITS; i++)
    {
        val.real(val.real() + __shfl_down(val.real(), offset));
        val.imag(val.imag() + __shfl_down(val.imag(), offset));
        offset >>= 1;
    }
    return val;
}

template <int N>
__inline__ __device__ rocblas_bfloat16 wavefront_reduce(rocblas_bfloat16 val)
{
    union
    {
        int              i;
        rocblas_bfloat16 h;
    } tmp;
    constexpr int WFBITS = rocblas_log2ui(N);
    int           offset = 1 << (WFBITS - 1);
    for(int i = 0; i < WFBITS; i++)
    {
        tmp.h = val;
        tmp.i = __shfl_down(tmp.i, offset);
        val += tmp.h;
        offset >>= 1;
    }
    return val;
}

template <int N>
__inline__ __device__ rocblas_half wavefront_reduce(rocblas_half val)
{
    union
    {
        int          i;
        rocblas_half h;
    } tmp;
    constexpr int WFBITS = rocblas_log2ui(N);
    int           offset = 1 << (WFBITS - 1);
    for(int i = 0; i < WFBITS; i++)
    {
        tmp.h = val;
        tmp.i = __shfl_down(tmp.i, offset);
        val += tmp.h;
        offset >>= 1;
    }
    return val;
}

template <rocblas_int NB, typename T>
__inline__ __device__ T rocblas_dot_block_reduce(T val)
{
    __shared__ T psums[warpSize];

    rocblas_int wavefront = hipThreadIdx_x / warpSize;
    rocblas_int wavelet   = hipThreadIdx_x % warpSize;

    if(wavefront == 0)
        psums[wavelet] = 0;
    __syncthreads();

    val = wavefront_reduce<warpSize>(val); // sum over wavefront
    if(wavelet == 0)
        psums[wavefront] = val; // store sum for wavefront

    __syncthreads(); // Wait for all wavefront reductions

    // ensure wavefront was run
    static constexpr rocblas_int num_wavefronts = NB / warpSize;
    val = (hipThreadIdx_x < num_wavefronts) ? psums[wavelet] : 0;
    if(wavefront == 0)
        val = wavefront_reduce<num_wavefronts>(val); // sum wavefront sums

    return val;
}

template <rocblas_int NB, rocblas_int WIN, bool CONJ, typename T, typename U, typename V = T>
__global__ __launch_bounds__(NB) void rocblas_dot_kernel_inc1(rocblas_int n,
                                                              const U __restrict__ xa,
                                                              ptrdiff_t      shiftx,
                                                              rocblas_stride stridex,
                                                              const U __restrict__ ya,
                                                              ptrdiff_t   shifty,
                                                              rocblas_int stridey,
                                                              V* __restrict__ workspace,
                                                              T* __restrict__ out)
{
    const T* x = load_ptr_batch(xa, hipBlockIdx_y, shiftx, stridex);
    const T* y = load_ptr_batch(ya, hipBlockIdx_y, shifty, stridey);

    V sum = 0;

    int inc = hipBlockDim_x * hipGridDim_x * WIN;

    int i = (hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x) * WIN;

    int remainder = n % WIN;
    int end       = n - remainder;
    for(; i < end; i += inc)
    {
        const T* xvec = x + i;
        const T* yvec = y + i;
        for(int j = 0; j < WIN; j++)
        {
            sum += V(yvec[j]) * V(CONJ ? conj(xvec[j]) : xvec[j]);
        }
    }

    if(hipBlockIdx_x == 0 && hipThreadIdx_x < remainder)
    {
        int idx = n - 1 - hipThreadIdx_x;
        sum += V(y[idx]) * V(CONJ ? conj(x[idx]) : x[idx]);
    }

    sum = rocblas_dot_block_reduce<NB>(sum);

    if(hipThreadIdx_x == 0)
    {
        workspace[hipBlockIdx_x + hipBlockIdx_y * hipGridDim_x] = sum;
        if(hipGridDim_x == 1) // small N avoid second kernel
            out[hipBlockIdx_y] = T(sum);
    }
}

template <rocblas_int NB, rocblas_int WIN, bool CONJ, typename T, typename U, typename V = T>
__global__ __launch_bounds__(NB, 1) void rocblas_dot_kernel(rocblas_int n,
                                                            const U __restrict__ xa,
                                                            ptrdiff_t      shiftx,
                                                            rocblas_int    incx,
                                                            rocblas_stride stridex,
                                                            const U __restrict__ ya,
                                                            ptrdiff_t   shifty,
                                                            rocblas_int incy,
                                                            rocblas_int stridey,
                                                            V* __restrict__ workspace,
                                                            T* __restrict__ out)
{
    const T* x = load_ptr_batch(xa, hipBlockIdx_y, shiftx, stridex);
    const T* y = load_ptr_batch(ya, hipBlockIdx_y, shifty, stridey);

    int i = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;

    V sum = 0;

    // sum WIN elements per thread
    int inc = hipBlockDim_x * hipGridDim_x;
    for(int j = 0; j < WIN && i < n; j++, i += inc)
    {
        sum += V(y[i * incy]) * V(CONJ ? conj(x[i * incx]) : x[i * incx]);
    }
    sum = rocblas_dot_block_reduce<NB>(sum);

    if(hipThreadIdx_x == 0)
    {
        workspace[hipBlockIdx_x + hipBlockIdx_y * hipGridDim_x] = sum;
        if(hipGridDim_x == 1) // small N avoid second kernel
            out[hipBlockIdx_y] = T(sum);
    }
}

template <rocblas_int NB, rocblas_int WIN, bool CONJ, typename T, typename U, typename V = T>
__global__ void __launch_bounds__(NB) rocblas_dot_kernel_magsq(rocblas_int n,
                                                               const U __restrict__ xa,
                                                               ptrdiff_t      shiftx,
                                                               rocblas_int    incx,
                                                               rocblas_stride stridex,
                                                               V* __restrict__ workspace,
                                                               T* __restrict__ out)
{
    const T* x = load_ptr_batch(xa, hipBlockIdx_y, shiftx, stridex);

    int i = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;

    V sum = 0;

    // sum WIN elements per thread
    int inc = hipBlockDim_x * hipGridDim_x;
    for(int j = 0; j < WIN && i < n; j++, i += inc)
    {
        sum += V(x[i * incx]) * V(CONJ ? conj(x[i * incx]) : x[i * incx]);
    }
    sum = rocblas_dot_block_reduce<NB>(sum);

    if(hipThreadIdx_x == 0)
    {
        workspace[hipBlockIdx_x + hipBlockIdx_y * hipGridDim_x] = sum;
        if(hipGridDim_x == 1) // small N avoid second kernel
            out[hipBlockIdx_y] = T(sum);
    }
}

template <rocblas_int NB, rocblas_int WIN, typename V, typename T = V>
__global__ __launch_bounds__(NB) void rocblas_dot_kernel_reduce(rocblas_int n_sums,
                                                                V* __restrict__ in,
                                                                T* __restrict__ out)
{
    V sum = 0;

    int offset = hipBlockIdx_y * n_sums;
    in += offset;

    int inc = hipBlockDim_x * hipGridDim_x * WIN;

    int i         = hipThreadIdx_x * WIN;
    int remainder = n_sums % WIN;
    int end       = n_sums - remainder;
    for(; i < end; i += inc) // cover all sums as 1 block
    {
        for(int j = 0; j < WIN; j++)
            sum += in[i + j];
    }
    if(hipThreadIdx_x < remainder)
    {
        sum += in[n_sums - 1 - hipThreadIdx_x];
    }

    sum = rocblas_dot_block_reduce<NB>(sum);
    if(hipThreadIdx_x == 0)
        out[hipBlockIdx_y] = T(sum);
}

/*
inline size_t rocblas_dot_kernel_block_count(rocblas_int n, rocblas_int NB)
{
    if(n <= 0)
        n = 1; // avoid sign loss issues
    return size_t(n - 1) / NB + 1;
}

template <rocblas_int NB, typename To>
size_t rocblas_dot_kernel_workspace_size(rocblas_int n, rocblas_int batch_count = 1)
{
    if(n <= 0)
        n = 1; // allow for return value of empty set
    if(batch_count <= 0)
        batch_count = 1;
    auto blocks = rocblas_dot_kernel_block_count(n, NB);
    return sizeof(To) * (blocks + 1) * batch_count;
}
*/

// work item number (WIN) of elements to process
template <typename T>
constexpr int rocblas_dot_WIN()
{
    size_t nb = sizeof(T);

    int n = 8;
    if(nb >= 8)
        n = 2;
    else if(nb >= 4)
        n = 4;

    return n;
}

constexpr int rocblas_dot_WIN(size_t nb)
{
    int n = 8;
    if(nb >= 8)
        n = 2;
    else if(nb >= 4)
        n = 4;

    return n;
}

// assume workspace has already been allocated, recommened for repeated calling of dot_strided_batched product
// routine
template <rocblas_int NB, bool CONJ, typename T, typename U, typename V = T>
ROCBLAS_INTERNAL_EXPORT_NOINLINE rocblas_status
    rocblas_internal_dot_template(rocblas_handle __restrict__ handle,
                                  rocblas_int n,
                                  const U __restrict__ x,
                                  rocblas_int    offsetx,
                                  rocblas_int    incx,
                                  rocblas_stride stridex,
                                  const U __restrict__ y,
                                  rocblas_int    offsety,
                                  rocblas_int    incy,
                                  rocblas_stride stridey,
                                  rocblas_int    batch_count,
                                  T* __restrict__ results,
                                  V* __restrict__ workspace)
{

    // One or two kernels are used to finish the reduction
    // kernel 1 write partial results per thread block in workspace, number of partial results is blocks
    // kernel 2 if blocks > 1 the partial results in workspace are reduced to output

    static constexpr int WIN = rocblas_dot_WIN<T>();

    // Quick return if possible.
    if(n <= 0 || batch_count == 0)
    {
        if(handle->is_device_memory_size_query())
            return rocblas_status_size_unchanged;
        else if(rocblas_pointer_mode_device == handle->pointer_mode && batch_count > 0)
        {
            RETURN_IF_HIP_ERROR(hipMemset(results, 0, batch_count * sizeof(T)));
        }
        else
        {
            for(int i = 0; i < batch_count; i++)
            {
                results[i] = T(0);
            }
        }

        return rocblas_status_success;
    }

    // in case of negative inc shift pointer to end of data for negative indexing tid*inc
    auto shiftx = incx < 0 ? offsetx - ptrdiff_t(incx) * (n - 1) : offsetx;
    auto shifty = incy < 0 ? offsety - ptrdiff_t(incy) * (n - 1) : offsety;

    rocblas_int blocks = rocblas_reduction_kernel_block_count(n, NB * WIN);
    dim3        grid(blocks, batch_count);
    dim3        threads(NB);
    size_t      offset = size_t(batch_count) * blocks;
    T*          output = results;
    if(handle->pointer_mode != rocblas_pointer_mode_device)
    {
        output = (T*)(workspace + offset);
    }

    if(x != y || incx != incy || offsetx != offsety || stridex != stridey)
    {
        if(incx == 1 && incy == 1 && sizeof(T) >= 8)
        {
            hipLaunchKernelGGL((rocblas_dot_kernel_inc1<NB, WIN, CONJ, T>),
                               grid,
                               threads,
                               0,
                               handle->get_stream(),
                               n,
                               x,
                               shiftx,
                               stridex,
                               y,
                               shifty,
                               stridey,
                               workspace,
                               output);
        }
        else
        {
            hipLaunchKernelGGL((rocblas_dot_kernel<NB, WIN, CONJ, T>),
                               grid,
                               threads,
                               0,
                               handle->get_stream(),
                               n,
                               x,
                               shiftx,
                               incx,
                               stridex,
                               y,
                               shifty,
                               incy,
                               stridey,
                               workspace,
                               output);
        }
    }
    else // x dot x
    {
        hipLaunchKernelGGL((rocblas_dot_kernel_magsq<NB, WIN, CONJ, T>),
                           grid,
                           threads,
                           0,
                           handle->get_stream(),
                           n,
                           x,
                           shiftx,
                           incx,
                           stridex,
                           workspace,
                           output);
    }

    if(handle->pointer_mode == rocblas_pointer_mode_device)
    {
        if(blocks > 1) // if single block first kernel did all work
            hipLaunchKernelGGL((rocblas_dot_kernel_reduce<NB, WIN>),
                               dim3(1, batch_count),
                               threads,
                               0,
                               handle->get_stream(),
                               blocks,
                               workspace,
                               results);
    }
    else
    {
        if(blocks > 1) // if single block first kernel did all work
            hipLaunchKernelGGL((rocblas_dot_kernel_reduce<NB, WIN>),
                               dim3(1, batch_count),
                               threads,
                               0,
                               handle->get_stream(),
                               blocks,
                               workspace,
                               output);

        RETURN_IF_HIP_ERROR(hipMemcpyAsync(&results[0],
                                           output,
                                           sizeof(T) * batch_count,
                                           hipMemcpyDeviceToHost,
                                           handle->get_stream()));
    }

    return rocblas_status_success;
}

template <typename T>
rocblas_status rocblas_dot_check_numerics(const char*    function_name,
                                          rocblas_handle handle,
                                          rocblas_int    n,
                                          T              x,
                                          rocblas_int    offset_x,
                                          rocblas_int    inc_x,
                                          rocblas_stride stride_x,
                                          T              y,
                                          rocblas_int    offset_y,
                                          rocblas_int    inc_y,
                                          rocblas_stride stride_y,
                                          rocblas_int    batch_count,
                                          const int      check_numerics,
                                          bool           is_input)
{
    rocblas_status check_numerics_status
        = rocblas_internal_check_numerics_vector_template(function_name,
                                                          handle,
                                                          n,
                                                          x,
                                                          offset_x,
                                                          inc_x,
                                                          stride_x,
                                                          batch_count,
                                                          check_numerics,
                                                          is_input);
    if(check_numerics_status != rocblas_status_success)
        return check_numerics_status;

    check_numerics_status = rocblas_internal_check_numerics_vector_template(function_name,
                                                                            handle,
                                                                            n,
                                                                            y,
                                                                            offset_y,
                                                                            inc_y,
                                                                            stride_y,
                                                                            batch_count,
                                                                            check_numerics,
                                                                            is_input);

    return check_numerics_status;
}
