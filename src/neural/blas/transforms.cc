/*
 This file is part of Leela Chess Zero.
 Copyright (C) 2018 The LCZero Authors

 Leela Chess is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Leela Chess is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "neural/blas/transforms.h"
#include "neural/blas/blas.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace lczero {

std::vector<float> Transforms::ZeropadU(const std::vector<float>& U,
                                        const int outputs, const int channels,
                                        const int outputs_pad,
                                        const int channels_pad) {
  // Fill with zeroes
  auto Upad = std::vector<float>(kWinogradTile * outputs_pad * channels_pad);

  for (auto o = 0; o < outputs; o++) {
    for (auto c = 0; c < channels; c++) {
      for (auto xi = 0; xi < kWinogradAlpha; xi++) {
        for (auto nu = 0; nu < kWinogradAlpha; nu++) {
          Upad[xi * (kWinogradAlpha * outputs_pad * channels_pad) +
               nu * (outputs_pad * channels_pad) + c * outputs_pad + o] =
              U[xi * (kWinogradAlpha * outputs * channels) +
                nu * (outputs * channels) + c * outputs + o];
        }
      }
    }
  }
  return Upad;
}

std::vector<float> Transforms::WinogradTransformF(const std::vector<float>& f,
                                                  const int outputs,
                                                  const int channels) {
  // F(2x2, 3x3) Winograd filter transformation
  // transpose(G.dot(f).dot(G.transpose()))
  // U matrix is transposed for better memory layout in SGEMM
  auto U = std::vector<float>(kWinogradTile * outputs * channels);
  auto G = std::array<float, kWinogradTile>{1.0, 0.0,  0.0, 0.5, 0.5, 0.5,
                                            0.5, -0.5, 0.5, 0.0, 0.0, 1.0};
  auto temp = std::array<float, 12>{};

  for (auto o = 0; o < outputs; o++) {
    for (auto c = 0; c < channels; c++) {
      for (auto i = 0; i < 4; i++) {
        for (auto j = 0; j < 3; j++) {
          auto acc = 0.0f;
          for (auto k = 0; k < 3; k++) {
            acc += G[i * 3 + k] * f[o * channels * 9 + c * 9 + k * 3 + j];
          }
          temp[i * 3 + j] = acc;
        }
      }

      for (auto xi = 0; xi < 4; xi++) {
        for (auto nu = 0; nu < 4; nu++) {
          auto acc = 0.0f;
          for (int k = 0; k < 3; k++) {
            acc += temp[xi * 3 + k] * G[nu * 3 + k];
          }
          U[xi * (4 * outputs * channels) + nu * (outputs * channels) +
            c * outputs + o] = acc;
        }
      }
    }
  }
  return U;
}

void Transforms::WinogradTransformIn(const int batch_size, const float* input,
                                     float* V, const int channels) {

  float x[kWinogradAlpha][kWinogradAlpha];
  float T1[kWinogradAlpha][kWinogradAlpha];

  for (auto batch_index = 0; batch_index < batch_size; batch_index++) {
    const float* input_batch = input + batch_index * kWidth * kHeight * channels;
    float* V_batch = V + channels * kTiles * batch_index;

    for (auto channel = 0; channel < channels; channel++) {
      float* V_channel = V_batch + channel;
      const float* input_channel = input_batch + channel * (kWidth * kHeight);

      for (auto block_y = 0; block_y < kWtiles; block_y++) {
        for (auto block_x = 0; block_x < kWtiles; block_x++) {
          // Tiles overlap by 2
          const auto yin = 2 * block_y - 1;
          const auto xin = 2 * block_x - 1;

          for (auto i = 0; i < kWinogradAlpha; i++) {
            for (auto j = 0; j < kWinogradAlpha; j++) {
              if ((yin + i) >= 0 && (xin + j) >= 0 && (yin + i) < kHeight &&
                  (xin + j) < kWidth) {
                x[i][j] = input_channel[(yin + i) * kWidth + (xin + j)];
              } else {
                x[i][j] = 0.0f;
              }
            }
          }

          // Calculates transpose(B).x.B
          // B = [[ 1.0,  0.0,  0.0,  0.0],
          //      [ 0.0,  1.0, -1.0,  1.0],
          //      [-1.0,  1.0,  1.0,  0.0],
          //      [ 0.0,  0.0,  0.0, -1.0]]

          //     WinogradTile T1, T2;

          T1[0][0] = x[0][0] - x[2][0];
          T1[0][1] = x[0][1] - x[2][1];
          T1[0][2] = x[0][2] - x[2][2];
          T1[0][3] = x[0][3] - x[2][3];
          T1[1][0] = x[1][0] + x[2][0];
          T1[1][1] = x[1][1] + x[2][1];
          T1[1][2] = x[1][2] + x[2][2];
          T1[1][3] = x[1][3] + x[2][3];
          T1[2][0] = x[2][0] - x[1][0];
          T1[2][1] = x[2][1] - x[1][1];
          T1[2][2] = x[2][2] - x[1][2];
          T1[2][3] = x[2][3] - x[1][3];
          T1[3][0] = x[1][0] - x[3][0];
          T1[3][1] = x[1][1] - x[3][1];
          T1[3][2] = x[1][2] - x[3][2];
          T1[3][3] = x[1][3] - x[3][3];

          const int V_incr = channels * kTiles * batch_size;
          float* wTile_V = V_channel + channels * (block_y * kWtiles + block_x);


          *wTile_V  = T1[0][0] - T1[0][2];
          wTile_V += V_incr;
          *wTile_V  = T1[0][1] + T1[0][2];
          wTile_V += V_incr;
          *wTile_V  = T1[0][2] - T1[0][1];
          wTile_V += V_incr;
          *wTile_V  = T1[0][1] - T1[0][3];
          wTile_V += V_incr;
          *wTile_V  = T1[1][0] - T1[1][2];
          wTile_V += V_incr;
          *wTile_V  = T1[1][1] + T1[1][2];
          wTile_V += V_incr;
          *wTile_V  = T1[1][2] - T1[1][1];
          wTile_V += V_incr;
          *wTile_V  = T1[1][1] - T1[1][3];
          wTile_V += V_incr;
          *wTile_V  = T1[2][0] - T1[2][2];
          wTile_V += V_incr;
          *wTile_V  = T1[2][1] + T1[2][2];
          wTile_V += V_incr;
          *wTile_V  = T1[2][2] - T1[2][1];
          wTile_V += V_incr;
          *wTile_V  = T1[2][1] - T1[2][3];
          wTile_V += V_incr;
          *wTile_V  = T1[3][0] - T1[3][2];
          wTile_V += V_incr;
          *wTile_V  = T1[3][1] + T1[3][2];
          wTile_V += V_incr;
          *wTile_V  = T1[3][2] - T1[3][1];
          wTile_V += V_incr;
          *wTile_V  = T1[3][1] - T1[3][3];

        }
      }
    }
  }
}

void Transforms::WinogradSgemm(int batch_size, const float* weights, float* V,
                               float* M, const int input_channels,
                               const int output_channels) {
#ifdef USE_MKL

  /*
   void cblas_sgemm_batch (const CBLAS_LAYOUT Layout, const CBLAS_TRANSPOSE*
   transa_array, const CBLAS_TRANSPOSE* transb_array, const MKL_INT* m_array,
   const MKL_INT* n_array, const MKL_INT* k_array, const float* alpha_array,
   const float **a_array, const MKL_INT* lda_array, const float **b_array, const
   MKL_INT* ldb_array, const float* beta_array, float **c_array, const MKL_INT*
   ldc_array, const MKL_INT group_count, const MKL_INT* group_size);
   */

  CBLAS_TRANSPOSE transA = CblasNoTrans;
  CBLAS_TRANSPOSE transB = CblasNoTrans;
  int m_array = output_channels;
  int n_array = batch_size * tiles;
  int k_array = input_channels;
  float alpha_array = 1.0;
  const float* a_array[kWinogradTile];
  int lda_array = output_channels;
  const float* b_array[kWinogradTile];
  int ldb_array = input_channels;
  float* c_array[kWinogradTile];
  int ldc_array = output_channels;
  float beta_array = 0.0;
  int groupSize = kWinogradTile;

  for (auto b = 0; b < kWinogradTile; b++) {
    auto offset_u = b * output_channels * input_channels;
    auto offset_v = b * batch_size * input_channels * tiles;
    auto offset_m = b * batch_size * output_channels * tiles;

    a_array[b] = &weights[offset_u];
    b_array[b] = &V[offset_v];
    c_array[b] = &M[offset_m];
  }

  cblas_sgemm_batch(CblasColMajor, &transA, &transB, &m_array, &n_array,
                    &k_array, &alpha_array, a_array, &lda_array, b_array,
                    &ldb_array, &beta_array, c_array, &ldc_array, 1,
                    &groupSize);

#else

  for (auto b = 0; b < kWinogradTile; b++) {
    auto offset_u = b * output_channels * input_channels;

    // In col major
    //
    //            M               =         weights(T)        x          V
    //
    // cols      tiles                  input_channels              tiles
    // rows   output_channels          output_channels            input_channels

    auto offset_v = b * batch_size * input_channels * kTiles;
    auto offset_m = b * batch_size * output_channels * kTiles;

    cblas_sgemm(  // Format       trans W       transV
        CblasColMajor, CblasNoTrans, CblasNoTrans,
        // rows W, M     cols V, M     cols W, rows V       alpha
        output_channels, batch_size * kTiles, input_channels, 1.0f,
        // W         ldW
        &weights[offset_u], output_channels,
        // V         ldV   beta
        &V[offset_v], input_channels, 0.0f,
        // M         ldM
        &M[offset_m], output_channels);
  }

#endif
}

void Transforms::WinogradTransformOut(const int batch_size, const float* M,
                                      float* output, const int channels) {

  float m[kWinogradTile];

  for (auto batch_index = 0; batch_index < batch_size; batch_index++) {
    const float* M_batch = M + channels * kTiles * batch_index;
    float* output_batch = output + batch_index * kWidth * kHeight * channels;

    for (auto channel = 0; channel < channels; channel++) {
      const float* M_channel = M_batch + channel;
      float* output_channel = output_batch + channel * (kHeight * kWidth);

      for (auto block_x = 0; block_x < kWtiles; block_x++) {
        for (auto block_y = 0; block_y < kWtiles; block_y++) {
          const auto x = 2 * block_x;
          const auto y = 2 * block_y;

          const auto b = block_y * kWtiles + block_x;
          const float* M_wtile = M_channel + channels * b;
          const int M_incr = channels * kTiles * batch_size;

          for (auto wTile = 0; wTile < kWinogradTile; wTile++) {
            m[wTile] = *M_wtile;
            M_wtile += M_incr;
          }

          // Calculates transpose(A).temp_m.A
          //    A = [1.0,  0.0],
          //        [1.0,  1.0],
          //        [1.0, -1.0],
          //        [0.0, -1.0]]

          auto o11 = m[0 * 4 + 0] + m[0 * 4 + 1] + m[0 * 4 + 2] + m[1 * 4 + 0] +
                     m[1 * 4 + 1] + m[1 * 4 + 2] + m[2 * 4 + 0] + m[2 * 4 + 1] +
                     m[2 * 4 + 2];

          auto o12 = m[0 * 4 + 1] - m[0 * 4 + 2] - m[0 * 4 + 3] + m[1 * 4 + 1] -
                     m[1 * 4 + 2] - m[1 * 4 + 3] + m[2 * 4 + 1] - m[2 * 4 + 2] -
                     m[2 * 4 + 3];

          auto o21 = m[1 * 4 + 0] + m[1 * 4 + 1] + m[1 * 4 + 2] - m[2 * 4 + 0] -
                     m[2 * 4 + 1] - m[2 * 4 + 2] - m[3 * 4 + 0] - m[3 * 4 + 1] -
                     m[3 * 4 + 2];

          auto o22 = m[1 * 4 + 1] - m[1 * 4 + 2] - m[1 * 4 + 3] - m[2 * 4 + 1] +
                     m[2 * 4 + 2] + m[2 * 4 + 3] - m[3 * 4 + 1] + m[3 * 4 + 2] +
                     m[3 * 4 + 3];

          output_channel[(y)*kWidth + (x)] = o11;
          output_channel[(y)*kWidth + (x + 1)] = o12;
          output_channel[(y + 1) * kWidth + (x)] = o21;
          output_channel[(y + 1) * kWidth + (x + 1)] = o22;
        }
      }
    }
  }
}

void Transforms::WinogradConvolve3(const int batch_size,
                                   const int input_channels,
                                   const int output_channels,
                                   const float* input, const float* weights,
                                   float* V, float* M, float* output) {
  WinogradTransformIn(batch_size, input, V, input_channels);
  WinogradSgemm(batch_size, weights, V, M, input_channels, output_channels);
  WinogradTransformOut(batch_size, M, output, output_channels);
}

template <unsigned int filter_size>
void Transforms::Convolve(const int batch_size, const int input_channels,
                          const int output_channels, const float* input,
                          const float* weights, const float* biases,
                          float* output) {
 constexpr unsigned int filter_len = filter_size * filter_size;
  const auto filter_dim = filter_len * input_channels;
  float col[filter_dim * kWidth * kHeight];

  for (int i = 0; i < batch_size; i++) {
    Im2Col<filter_size>(input_channels, input, col);

    // Weight shape (output, input, filter_size, filter_size)
    // 96 22 3 3
    // outputs[96,8x8] = weights[96,22x3x3] x col[22x3x3,8x8]
    // C←αAB + βC
    // M Number of rows in matrices A and C.
    // N Number of columns in matrices B and C.
    // K Number of columns in matrix A; number of rows in matrix B.
    // lda The size of the first dimention of matrix A; if you are
    // passing a matrix A[m][n], the value should be m.
    //    cblas_sgemm(CblasRowMajor, TransA, TransB, M, N, K, alpha, A, lda, B,
    //                ldb, beta, C, N);

    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                // M                  N            K
                output_channels, kSquares, filter_dim, 1.0f, weights,
                filter_dim, col, kSquares, 0.0f, output, kSquares);

    int index = 0;
    for (unsigned int o = 0; o < output_channels; o++) {
      for (unsigned int b = 0; b < kSquares; b++) {
        output[index++] += biases[o];
      }
    }

    input += 64 * input_channels;
    output += 64 * output_channels;
  }
}

template <>
void Transforms::Convolve<1>(const int batch_size, const int input_channels,
                             const int output_channels, const float* input,
                             const float* weights, const float* biases,
                             float* output) {
  for (int i = 0; i < batch_size; i++) {
    // C←αAB + βC
    // M Number of rows in matrices A and C.
    // N Number of columns in matrices B and C.
    // K Number of columns in matrix A; number of rows in matrix B.
    // lda The size of the first dimention of matrix A; if you are
    // passing a matrix A[m][n], the value should be m.
    //    cblas_sgemm(CblasRowMajor, TransA, TransB, M, N, K, alpha, A, lda, B,
    //                ldb, beta, C, N);

    //             C                               A                     B
    //
    //           outputs             :=          weights        x      input
    //
    //   cols:     64 (N)                   input_channels (K)         64 (N)
    //
    //   rows:  output_channels (M)         output_channels (M)
    //   input_channels (K)

    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                // M              N         K         alpha
                output_channels, 64, input_channels, 1.0f,
                // A     lda
                weights, input_channels,
                // B    ldb   beta,
                input, 64, 0.0f,
                // C   ldc
                output, 64);

    int index = 0;
    for (int o = 0; o < output_channels; o++) {
      const auto bias=biases[o];
      for (unsigned int b = 0; b < 64; b++) {
        output[index++] += bias;
      }
    }

    input += 64 * input_channels;
    output += 64 * output_channels;
  }
}

void Transforms::Innerproduct(int batch_size, const int input_size,
                              const int output_size, const float* inputs,
                              const float* weights, const float* biases,
                              bool apply_relu, float* outputs) {
  if (batch_size == 1) {
    // Just matrix-vecttor multplication
    //
    //             C                A                     B
    //
    //         outputs    :=     weights      x       inputs
    //
    //   cols:   1               input_size            1
    //
    //   rows  output_size      output_size          input_size
    //

    cblas_sgemv(CblasRowMajor, CblasNoTrans,
                // M     K
                output_size, input_size, 1.0f, weights, input_size, inputs, 1,
                0.0f, outputs, 1);
  } else {
    // matrix-matrix multplication
    //
    //             C                     A                         B
    //
    //            outputs      :=       weights        x         inputs
    //
    //   cols:   batch_size (N)       input_size  (K)          batch_size (N)
    //
    //   rows  output_size (M)        output_size (M)         input_size (K)
    //

    // C←αAB + βC
    // M Number of rows in matrices A and C.
    // N Number of columns in matrices B and C.
    // K Number of columns in matrix A; number of rows in matrix B.
    // lda The size of the first dimention of matrix A; if you are
    // passing a matrix A[m][n], the value should be m.
    //    cblas_sgemm(CblasRowMajor, TransA, TransB, M, N, K, alpha, A, lda, B,
    //                ldb, beta, C, N);

    cblas_sgemm(CblasColMajor, CblasTrans, CblasNoTrans,
                // M              N         K         alpha
                output_size, batch_size, input_size, 1.0f,
                // A     lda
                weights, input_size,
                // B    ldb   beta,
                inputs, input_size, 0.0f,
                // C   ldc
                outputs, output_size);
  }
  for (int i = 0; i < batch_size; i++) {
    if (apply_relu) {
      for (int o = 0; o < output_size; o++) {
        float val = biases[o] + outputs[o];
        outputs[o] = val >= 0 ? val : 0;
      }
    } else {
      for (int o = 0; o < output_size; o++) {
        outputs[o] += biases[o];
      }
    }

    outputs += output_size;
    inputs += input_size;
  }
}

void Transforms::Batchnorm(const int batch_size, const int channels,
                           float* data, const float* means,
                           const float* stddivs, const float* eltwise) {
  for (int i = 0; i < batch_size; i++) {
    for (int c = 0; c < channels; ++c) {
      auto mean = means[c];
      auto scale_stddiv = stddivs[c];

      if (eltwise == nullptr) {
        // Classical BN
        auto arr = &data[c * 64];
        for (int b = 0; b < 64; b++) {
          float val = scale_stddiv * (arr[b] - mean);
          arr[b] = val > 0 ? val : 0;
        }
      } else {
        // BN + residual add
        auto arr = &data[c * 64];
        auto res = &eltwise[c * 64];
        for (int b = 0; b < 64; b++) {
          float val = res[b] + (scale_stddiv * (arr[b] - mean));
          arr[b] = val > 0 ? val : 0;
        }
      }
    }
    data += channels * 64;
    if (eltwise != nullptr) eltwise += channels * 64;
  }
}

template <unsigned long filter_size>
void Transforms::Im2Col(const int channels, const float* data_im,
                        float* data_col) {
  constexpr unsigned int height = 8;
  constexpr unsigned int width = 8;
  constexpr unsigned int channel_size = height * width;

  constexpr int pad = (filter_size / 2);
  constexpr unsigned int output_h = height + 2 * pad - filter_size + 1;
  constexpr unsigned int output_w = width + 2 * pad - filter_size + 1;

  for (int channel = channels; channel--; data_im += channel_size) {
    for (unsigned int kernel_row = 0; kernel_row < filter_size; kernel_row++) {
      for (unsigned int kernel_col = 0; kernel_col < filter_size;
           kernel_col++) {
        int input_row = -pad + kernel_row;
        for (int output_rows = output_h; output_rows; output_rows--) {
          if ((unsigned)input_row < height) {
            int input_col = -pad + kernel_col;
            for (int output_col = output_w; output_col; output_col--) {
              if ((unsigned)input_col < width) {
                *(data_col++) = data_im[input_row * width + input_col];
              } else {
                *(data_col++) = 0;
              }
              input_col++;
            }
          } else {
            for (int output_cols = output_w; output_cols; output_cols--) {
              *(data_col++) = 0;
            }
          }
          input_row++;
        }
      }
    }
  }
}

float Transforms::DotProduct(const int size, const float* x, const float* y) {
  // float cblas_sdot(const int __N, const float *__X, const int __incX, const
  // float *__Y, const int __incY);
  return cblas_sdot(size, x, 1, y, 1);
}

void Transforms::Softmax(const int size, const float* input, float* output) {
  auto alpha = *std::max_element(input, input + size);

  auto denom = 0.0f;
  for (int i = 0; i < size; i++) {
    auto val = std::exp(input[i] - alpha);
    output[i] = val;
    denom += val;
  }
  for (int i = 0; i < size; i++) {
    output[i] = output[i] / denom;
  }
}

void Transforms::OffsetBatchNormMeans(std::vector<float>& bn_means,
                                      const std::vector<float>& biases) {
  // Biases are not calculated and are typically zero but some networks might
  // still have non-zero biases.
  // Move biases to batchnorm means to make the output match without having
  // to separately add the biases.
  for (size_t i = 0; i < bn_means.size(); i++) bn_means[i] -= biases[i];
}

void Transforms::InvertBatchNormStddev(std::vector<float>& weights) {
  constexpr float EPSILON = 1e-5f;
  for (auto& w : weights) w = 1.0f / std::sqrt(w + EPSILON);
}

/* Template instantiations and specializations */

template <>
void Transforms::Im2Col<1>(const int channels, const float* input,
                           float* output) {
  constexpr unsigned int boardsize = 8;
  auto outSize = size_t{channels * boardsize * boardsize};
  std::copy(input, input + outSize, output);
}
}
