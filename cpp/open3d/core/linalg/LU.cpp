// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/core/linalg/LU.h"

#include "open3d/core/linalg/LUImpl.h"
#include "open3d/core/linalg/LinalgHeadersCPU.h"
#include "open3d/core/linalg/TriangularMat.h"

namespace open3d {
namespace core {

// Get column permutation tensor from ipiv (swaping index array).
core::Tensor GetColPermutation(Tensor& ipiv,
                               int number_of_indices,
                               int number_of_rows);

// Decompose output in P, L, U matrix form.
inline void OutputToPLU(const Tensor& output,
                        Tensor& permutation,
                        Tensor& lower,
                        Tensor& upper,
                        Tensor& ipiv,
                        bool permute_l);

void LU_with_ipiv(const Tensor& A, Tensor& ipiv, Tensor& output) {
    Device device = A.GetDevice();
    // Check dtypes.
    Dtype dtype = A.GetDtype();
    if (dtype != Dtype::Float32 && dtype != Dtype::Float64) {
        utility::LogError(
                "Only tensors with Float32 or Float64 are supported, but "
                "received {}.",
                dtype.ToString());
    }

    // Check dimensions.
    SizeVector A_shape = A.GetShape();
    if (A_shape.size() != 2) {
        utility::LogError("Tensor A must be 2D, but got {}D.", A_shape.size());
    }
    if (A_shape[0] != A_shape[1]) {
        utility::LogError("Tensor A must be square, but got {} x {}.",
                          A_shape[0], A_shape[1]);
    }
    int64_t n = A_shape[0];
    if (n == 0) {
        utility::LogError(
                "Tensor shapes should not contain dimensions with zero.");
    }

    // "output" tensor is modified in-place as ouput.
    // Operations are COL_MAJOR.
    output = A.T().Clone();
    void* A_data = output.GetDataPtr();

    // Returns LU decomposition in form of an output matrix,
    // with lower triangular elements as L, upper triangular and diagonal
    // elements as U, (diagonal elements of L are unity), and ipiv array,
    // which has the pivot indices (for 1 <= i <= min(M,N), row i of the
    // matrix was interchanged with row IPIV(i).
    if (device.GetType() == Device::DeviceType::CUDA) {
#ifdef BUILD_CUDA_MODULE
        ipiv = core::Tensor::Empty({n}, core::Dtype::Int32, device);
        void* ipiv_data = ipiv.GetDataPtr();
        LUCUDA(A_data, ipiv_data, n, dtype, device);
#else
        utility::LogInfo("Unimplemented device.");
#endif
    } else {
        Dtype ipiv_dtype;
        if (sizeof(OPEN3D_CPU_LINALG_INT) == 4) {
            ipiv_dtype = Dtype::Int32;
        } else if (sizeof(OPEN3D_CPU_LINALG_INT) == 8) {
            ipiv_dtype = Dtype::Int64;
        } else {
            utility::LogError("Unsupported OPEN3D_CPU_LINALG_INT type.");
        }
        ipiv = core::Tensor::Empty({n}, ipiv_dtype, device);
        void* ipiv_data = ipiv.GetDataPtr();
        LUCPU(A_data, ipiv_data, n, dtype, device);
    }
    // COL_MAJOR -> ROW_MAJOR.
    output = output.T().Contiguous();
}

void LU(const Tensor& A,
        Tensor& permutation,
        Tensor& lower,
        Tensor& upper,
        bool permute_l) {
    // Get output matrix and ipiv.
    core::Tensor ipiv, output;
    LU_with_ipiv(A, ipiv, output);
    OutputToPLU(output, permutation, lower, upper, ipiv, permute_l);
}

inline void OutputToPLU(const Tensor& output,
                        Tensor& permutation,
                        Tensor& lower,
                        Tensor& upper,
                        Tensor& ipiv,
                        bool permute_l) {
    int n = output.GetShape()[0];
    core::Device device = output.GetDevice();
    std::tie(upper, lower) = output.Thiul();
    Tensor colPermutation = GetColPermutation(ipiv, ipiv.GetShape()[0], n);
    // Creating "Permutation Matrix (P in P.A = L.U)".
    permutation = core::Tensor::Eye(n, output.GetDtype(), device)
                          .IndexGet({colPermutation});
    // Calculating P in A = P.L.U. [After Inverse it is no longer Contiguous].
    permutation = permutation.Inverse().Contiguous();
    // Permute_l option, to return L as L = P.L.
    if (permute_l) {
        lower = permutation.Matmul(lower);
    }
}

core::Tensor GetColPermutation(Tensor& ipiv,
                               int number_of_indices,
                               int number_of_rows) {
    Tensor full_ipiv = Tensor::Arange(0, number_of_rows, 1, core::Dtype::Int32,
                                      Device("CPU:0"));
    Tensor ipiv_cpu =
            ipiv.To(Device("CPU:0"), core::Dtype::Int32, /*copy*/ false);
    int* ipiv_ptr = static_cast<int*>(ipiv_cpu.GetDataPtr());
    int* full_ipiv_ptr = static_cast<int*>(full_ipiv.GetDataPtr());
    for (int i = 0; i < number_of_rows; i++) {
        int temp = full_ipiv_ptr[i];
        full_ipiv_ptr[i] = full_ipiv_ptr[ipiv_ptr[i] - 1];
        full_ipiv_ptr[ipiv_ptr[i] - 1] = temp;
    }
    // This is column permutation for P, where P.A = L.U.
    // Int64 is required by AdvancedIndexing.
    return full_ipiv.To(ipiv.GetDevice(), core::Dtype::Int64, /*copy=*/false);
}

}  // namespace core
}  // namespace open3d