/* ************************************************************************
 * Copyright 2018-2020 Advanced Micro Devices, Inc.
 * ************************************************************************ */

#include "cblas_interface.hpp"
#include "near.hpp"
#include "norm.hpp"
#include "rocblas.hpp"
#include "rocblas_init.hpp"
#include "rocblas_math.hpp"
#include "rocblas_random.hpp"
#include "rocblas_test.hpp"
#include "rocblas_vector.hpp"
#include "unit.hpp"
#include "utility.hpp"

template <typename T>
void testing_nrm2_batched_bad_arg(const Arguments& arg)
{
    rocblas_int         N           = 100;
    rocblas_int         incx        = 1;
    rocblas_int         batch_count = 1;
    static const size_t safe_size   = 100;

    rocblas_local_handle handle;

    device_vector<T*, 0, T>  dx(safe_size);
    device_vector<real_t<T>> d_rocblas_result(1);
    if(!dx || !d_rocblas_result)
    {
        CHECK_HIP_ERROR(hipErrorOutOfMemory);
        return;
    }

    CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_device));

    EXPECT_ROCBLAS_STATUS(
        rocblas_nrm2_batched<T>(handle, N, nullptr, incx, batch_count, d_rocblas_result),
        rocblas_status_invalid_pointer);
    EXPECT_ROCBLAS_STATUS(rocblas_nrm2_batched<T>(handle, N, dx, incx, batch_count, nullptr),
                          rocblas_status_invalid_pointer);
    EXPECT_ROCBLAS_STATUS(
        rocblas_nrm2_batched<T>(nullptr, N, dx, incx, batch_count, d_rocblas_result),
        rocblas_status_invalid_handle);
}

template <typename T>
void testing_nrm2_batched(const Arguments& arg)
{
    rocblas_int N           = arg.N;
    rocblas_int incx        = arg.incx;
    rocblas_int batch_count = arg.batch_count;

    double rocblas_error_1;
    double rocblas_error_2;

    rocblas_local_handle handle;

    // check to prevent undefined memory allocation error
    if(N <= 0 || incx <= 0 || batch_count <= 0)
    {
        device_batch_vector<T> dx(3, 1, 2);
        CHECK_HIP_ERROR(dx.memcheck());
        device_vector<real_t<T>> dr(std::max(2, std::abs(batch_count)));
        CHECK_HIP_ERROR(dr.memcheck());
        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_device));
        EXPECT_ROCBLAS_STATUS(
            rocblas_nrm2_batched<T>(handle, N, dx.ptr_on_device(), incx, batch_count, dr),
            (N > 0 && incx > 0 && batch_count < 0) ? rocblas_status_invalid_size
                                                   : rocblas_status_success);
        return;
    }

    real_t<T> rocblas_result_1[batch_count];
    real_t<T> rocblas_result_2[batch_count];
    real_t<T> cpu_result[batch_count];

    size_t size_x = N * size_t(incx);

    // allocate memory on device
    device_vector<real_t<T>> d_rocblas_result_2(batch_count);
    if(!d_rocblas_result_2)
    {
        CHECK_HIP_ERROR(hipErrorOutOfMemory);
        return;
    }

    // Naming: dx is in GPU (device) memory. hx is in CPU (host) memory, plz follow this practice
    host_vector<T> hx[batch_count];

    // Initial Data on CPU
    rocblas_seedrand();
    for(int i = 0; i < batch_count; i++)
    {
        hx[i] = host_vector<T>(size_x);
        rocblas_init<T>(hx[i], 1, N, incx);
    }

    device_batch_vector<T> hdx(batch_count, size_x);

    // copy data from host to device
    for(int i = 0; i < batch_count; i++)
    {
        CHECK_HIP_ERROR(hipMemcpy(hdx[i], hx[i], size_x * sizeof(T), hipMemcpyHostToDevice));
    }

    // vector pointers on gpu
    device_vector<T*, 0, T> dx_pvec(batch_count);
    if(!dx_pvec)
    {
        CHECK_HIP_ERROR(hipErrorOutOfMemory);
        return;
    }
    // copy gpu vector pointers from host to device pointer array
    CHECK_HIP_ERROR(hipMemcpy(dx_pvec, hdx, sizeof(T*) * batch_count, hipMemcpyHostToDevice));

    double gpu_time_used, cpu_time_used;

    if(arg.unit_check || arg.norm_check)
    {
        // GPU BLAS, rocblas_pointer_mode_host
        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_host));
        CHECK_ROCBLAS_ERROR(
            rocblas_nrm2_batched<T>(handle, N, dx_pvec, incx, batch_count, rocblas_result_1));

        // GPU BLAS, rocblas_pointer_mode_device
        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_device));
        CHECK_ROCBLAS_ERROR(
            rocblas_nrm2_batched<T>(handle, N, dx_pvec, incx, batch_count, d_rocblas_result_2));
        CHECK_HIP_ERROR(hipMemcpy(rocblas_result_2,
                                  d_rocblas_result_2,
                                  batch_count * sizeof(real_t<T>),
                                  hipMemcpyDeviceToHost));

        // CPU BLAS
        cpu_time_used = get_time_us();
        for(int i = 0; i < batch_count; i++)
        {
            cblas_nrm2<T>(N, hx[i], incx, cpu_result + i);
        }
        cpu_time_used = get_time_us() - cpu_time_used;

        //      allowable error is sqrt of precision. This is based on nrm2 calculating the
        //      square root of a sum. It is assumed that the sum will have accuracy =approx=
        //      precision, so nrm2 will have accuracy =approx= sqrt(precision)
        real_t<T> abs_error
            = pow(10.0, -(std::numeric_limits<real_t<T>>::digits10 / 2.0)) * cpu_result[0];
        real_t<T> tolerance = 2.0; //  accounts for rounding in reduction sum. depends on n.
            //  If test fails, try decreasing n or increasing tolerance.
        abs_error *= tolerance;
        if(arg.unit_check)
        {
            near_check_general<real_t<T>, real_t<T>>(
                batch_count, 1, 1, cpu_result, rocblas_result_1, abs_error);
            near_check_general<real_t<T>, real_t<T>>(
                batch_count, 1, 1, cpu_result, rocblas_result_2, abs_error);
        }

        if(arg.norm_check)
        {
            rocblas_cout << "cpu=" << cpu_result[0] << ", gpu_host_ptr=" << rocblas_result_1[0]
                         << ", gpu_dev_ptr=" << rocblas_result_2[0] << "\n";
            rocblas_error_1 = std::abs((cpu_result[0] - rocblas_result_1[0]) / cpu_result[0]);
            rocblas_error_2 = std::abs((cpu_result[0] - rocblas_result_2[0]) / cpu_result[0]);
        }
    }

    if(arg.timing)
    {
        int number_cold_calls = 2;
        int number_hot_calls  = 100;
        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_host));

        for(int iter = 0; iter < number_cold_calls; iter++)
        {
            rocblas_nrm2_batched<T>(handle, N, dx_pvec, incx, batch_count, rocblas_result_2);
        }

        gpu_time_used = get_time_us(); // in microseconds

        for(int iter = 0; iter < number_hot_calls; iter++)
        {
            rocblas_nrm2_batched<T>(handle, N, dx_pvec, incx, batch_count, rocblas_result_2);
        }

        gpu_time_used = (get_time_us() - gpu_time_used) / number_hot_calls;

        rocblas_cout << "N,incx,batch_count,rocblas(us)";

        if(arg.norm_check)
            rocblas_cout << ",CPU(us),error_host_ptr,error_dev_ptr";

        rocblas_cout << std::endl;
        rocblas_cout << N << "," << incx << "," << batch_count << "," << gpu_time_used;

        if(arg.norm_check)
            rocblas_cout << "," << cpu_time_used << "," << rocblas_error_1 << ","
                         << rocblas_error_2;

        rocblas_cout << std::endl;
    }
}
