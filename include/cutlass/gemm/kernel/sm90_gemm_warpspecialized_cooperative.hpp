/***************************************************************************************************
 * Copyright (c) 2023 - 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 **************************************************************************************************/
#pragma once

#include "cutlass/cutlass.h"
#include "cutlass/fast_math.h"
#include "cutlass/kernel_hardware_info.hpp"
#include "cute/arch/cluster_sm90.hpp"
#include "cutlass/arch/reg_reconfig.h"
#include "cutlass/arch/mma_sm90.h"
#include "cutlass/epilogue/collective/detail.hpp"
#include "cutlass/gemm/gemm.h"
#include "cutlass/gemm/dispatch_policy.hpp"
#include "cutlass/gemm/kernel/tile_scheduler.hpp"
#include "cutlass/pipeline/pipeline.hpp"
#include "cute/tensor.hpp"

///////////////////////////////////////////////////////////////////////////////

namespace cutlass::gemm::kernel {

///////////////////////////////////////////////////////////////////////////////

template <
  class ProblemShape_,
  class CollectiveMainloop_,
  class CollectiveEpilogue_,
  class TileScheduler_
>
class GemmUniversal<
  ProblemShape_,
  CollectiveMainloop_,
  CollectiveEpilogue_,
  TileScheduler_,
  cute::enable_if_t<cute::is_base_of_v<KernelCpAsyncWarpSpecializedCooperative, typename CollectiveMainloop_::DispatchPolicy::Schedule>>>
{
public:
  //
  // Type Aliases
  //
  using ProblemShape = ProblemShape_;
  static_assert(cute::rank(ProblemShape{}) == 3 or cute::rank(ProblemShape{}) == 4,
    "ProblemShape{} should be <M,N,K> or <M,N,K,L>");
  static constexpr bool IsGdcEnabled = false;
  // Mainloop derived types
  using CollectiveMainloop = CollectiveMainloop_;
  using TileShape = typename CollectiveMainloop::TileShape;
  using TiledMma  = typename CollectiveMainloop::TiledMma;
  using ArchTag   = typename CollectiveMainloop::ArchTag;
  using ElementA  = typename CollectiveMainloop::ElementA;
  using StrideA   = typename CollectiveMainloop::StrideA;
  using ElementB  = typename CollectiveMainloop::ElementB;
  using StrideB   = typename CollectiveMainloop::StrideB;
  using DispatchPolicy = typename CollectiveMainloop::DispatchPolicy;
  using ElementAccumulator = typename CollectiveMainloop::ElementAccumulator;
  using ClusterShape = typename DispatchPolicy::ClusterShape;
  using MainloopArguments = typename CollectiveMainloop::Arguments;
  using MainloopParams = typename CollectiveMainloop::Params;
  static_assert(ArchTag::kMinComputeCapability >= 90);

  // Epilogue derived types
  using CollectiveEpilogue = CollectiveEpilogue_;
  using ElementC = typename CollectiveEpilogue::ElementC;
  using StrideC  = typename CollectiveEpilogue::StrideC;
  using ElementD = typename CollectiveEpilogue::ElementD;
  using StrideD  = typename CollectiveEpilogue::StrideD;
  using EpilogueArguments = typename CollectiveEpilogue::Arguments;
  using EpilogueParams = typename CollectiveEpilogue::Params;

  using TileSchedulerTag = TileScheduler_;
  using TileScheduler = typename detail::TileSchedulerSelector<
    TileScheduler_, ArchTag, TileShape, ClusterShape>::Scheduler;
  using TileSchedulerArguments = typename TileScheduler::Arguments;
  using TileSchedulerParams = typename TileScheduler::Params;

  using GmemTiledCopyA = typename CollectiveMainloop::GmemTiledCopyA;
  using GmemTiledCopyB = typename CollectiveMainloop::GmemTiledCopyB;
  static_assert(cute::size(GmemTiledCopyA{}) == cute::size(GmemTiledCopyB{}), "Number of threads in A/B tiled copies must be the same");

  static constexpr uint32_t NumLoadWarpGroups = cute::size(GmemTiledCopyA{}) / NumThreadsPerWarpGroup;
  static constexpr uint32_t NumMmaWarpGroups = cute::size(TiledMma{}) / NumThreadsPerWarpGroup;
  static constexpr uint32_t NumWarpGroups = NumLoadWarpGroups + NumMmaWarpGroups;
  static_assert(NumWarpGroups == 2 || NumWarpGroups == 3, "Number of warp groups must be 2 or 3 for good performance.");

  static constexpr uint32_t MaxThreadsPerBlock = NumWarpGroups * NumThreadsPerWarpGroup;
  static constexpr uint32_t MinBlocksPerMultiprocessor = 1;

  // Kernel level shared memory storage
  struct SharedStorage {
    struct TensorStorage : cute::aligned_struct<128, _1> {
      using MainloopTensorStorage = typename CollectiveMainloop::TensorStorage;
      using EpilogueTensorStorage = typename CollectiveEpilogue::TensorStorage;

      MainloopTensorStorage mainloop;
      EpilogueTensorStorage epilogue;
    } tensors;

    struct PipelineStorage : cute::aligned_struct<16, _1> {
      using MainloopPipelineStorage = typename CollectiveMainloop::PipelineStorage;
      using EpiLoadPipelineStorage = typename CollectiveEpilogue::PipelineStorage;

      alignas(16) MainloopPipelineStorage mainloop;
      alignas(16) EpiLoadPipelineStorage epi_load;
    } pipelines;
  };

  static constexpr int SharedStorageSize = sizeof(SharedStorage);

  // Device side arguments
  struct Arguments {
    GemmUniversalMode mode{};
    ProblemShape problem_shape{};
    MainloopArguments mainloop{};
    EpilogueArguments epilogue{};
    KernelHardwareInfo hw_info{};
    TileSchedulerArguments scheduler{};
  };

  // Kernel entry point API
  struct Params {
    GemmUniversalMode mode{};
    ProblemShape problem_shape{};
    MainloopParams mainloop{};
    EpilogueParams epilogue{};
    KernelHardwareInfo hw_info{};
    TileSchedulerParams scheduler{};
  };

  //
  // Methods
  //

  // Convert to underlying arguments. In this case, a simple copy for the aliased type.
  static
  Params
  to_underlying_arguments(Arguments const& args, void* workspace) {
    CUTLASS_TRACE_HOST("to_underlying_arguments():");

    auto problem_shape = args.problem_shape;
    if constexpr (detail::Has_SwapAB_v<CollectiveMainloop>) {
      // swap M/N
      get<0>(problem_shape) = get<1>(args.problem_shape);
      get<1>(problem_shape) = get<0>(args.problem_shape);
    }
    auto problem_shape_MNKL = append<4>(problem_shape, 1);

    // Get SM count if needed, otherwise use user supplied SM count
    int sm_count = args.hw_info.sm_count;
    if (sm_count <= 0) {
      CUTLASS_TRACE_HOST("  WARNING: Arguments do not include a valid SM count.\n"
          "  For optimal performance, populate the arguments KernelHardwareInfo struct with the SM count.");
      sm_count = KernelHardwareInfo::query_device_multiprocessor_count(args.hw_info.device_id);
    }
    CUTLASS_TRACE_HOST("to_underlying_arguments(): Setting persistent grid SM count to " << sm_count);

    // Get maximum number of clusters that could co-exist on the target device
    int max_active_clusters = args.hw_info.max_active_clusters;
    if (max_active_clusters <= 0) {
      max_active_clusters = 0;
      CUTLASS_TRACE_HOST("  WARNING: Arguments do not include a valid max cluster count.\n"
          "  For optimal performance, populate the arguments KernelHardwareInfo struct with the max_active_clusters.");
    }
    else {
      CUTLASS_TRACE_HOST("to_underlying_arguments(): Setting persistent grid cluster count to " << max_active_clusters);
    }

    KernelHardwareInfo hw_info{args.hw_info.device_id, sm_count, max_active_clusters};

    TileSchedulerParams scheduler = TileScheduler::to_underlying_arguments(
      problem_shape_MNKL, TileShape{}, ClusterShape{}, hw_info, args.scheduler, workspace);

    return {
      args.mode,
      problem_shape,
      CollectiveMainloop::to_underlying_arguments(args.problem_shape, args.mainloop, workspace),
      CollectiveEpilogue::to_underlying_arguments(args.problem_shape, args.epilogue, workspace),
      hw_info,
      scheduler
    };
  }

  static bool
  can_implement(Arguments const& args) {
    bool implementable = (args.mode == GemmUniversalMode::kGemm) or
        (args.mode == GemmUniversalMode::kBatched && cute::rank(ProblemShape{}) == 4);
    if (!implementable) {
      CUTLASS_TRACE_HOST("  CAN IMPLEMENT: Arguments or Problem Shape don't meet the requirements.\n");
      return implementable;
    }
    implementable &= CollectiveMainloop::can_implement(args.problem_shape, args.mainloop);
    implementable &= CollectiveEpilogue::can_implement(args.problem_shape, args.epilogue);
    implementable &= TileScheduler::can_implement(args.scheduler);

    return implementable;
  }

  static
  size_t
  get_workspace_size(Arguments const& args) {
    TileScheduler t;
    return t.template get_workspace_size<ProblemShape, ElementAccumulator>(
      args.scheduler, args.problem_shape, args.hw_info, NumMmaWarpGroups);
  }

  static
  cutlass::Status
  initialize_workspace(Arguments const& args, void* workspace = nullptr, cudaStream_t stream = nullptr,
    CudaHostAdapter* cuda_adapter = nullptr) {
    TileScheduler t;
    static constexpr uint32_t NumEpilogueSubTiles = 1;
    static constexpr uint32_t NumAccumulatorMtxs = 1;
    return t.template initialize_workspace<ProblemShape, ElementAccumulator>(
      args.scheduler, workspace, stream, args.problem_shape, args.hw_info, NumMmaWarpGroups, NumEpilogueSubTiles, NumAccumulatorMtxs, cuda_adapter);
  }

  // Computes the kernel launch grid shape based on runtime parameters
  static dim3
  get_grid_shape(Params const& params) {
    // Given device SM count, set grid size s.t. we do not launch more thread blocks than we can run concurrently
    TileSchedulerArguments args{};
    if constexpr (!std::is_const_v<decltype(args.max_swizzle_size)>) {
      args.max_swizzle_size = 1 << params.scheduler.log_swizzle_size_;
    }
    return TileScheduler::get_grid_shape(params.scheduler, params.problem_shape, TileShape{}, ClusterShape{}, params.hw_info, args);
  }

  static dim3
  get_block_shape() {
    return dim3(MaxThreadsPerBlock, 1, 1);
  }

  CUTLASS_DEVICE
  void
  operator()(Params const& params, char* smem_buf) {
    using namespace cute;
    using X = Underscore;

// Any Tensor Op MMA Atom in the WGMMA ISA is arch conditional to sm90a.
#if ! defined(__CUDA_ARCH_FEAT_SM90_ALL)
    printf("ERROR : Arch conditional MMA instruction used without targeting sm90a compute capability. Aborting.\n");
#else

    static_assert(cute::rank(StrideA{}) == 3, "StrideA must be rank-3: [M, K, L]. If batch mode is not needed, set L stride to Int<0>.");
    static_assert(cute::rank(StrideB{}) == 3, "StrideB must be rank-3: [N, K, L]. If batch mode is not needed, set L stride to Int<0>.");
    static_assert(cute::rank(StrideC{}) == 3, "StrideC must be rank-3: [M, N, L]. If batch mode is not needed, set L stride to Int<0>.");
    static_assert(cute::rank(StrideD{}) == 3, "StrideD must be rank-3: [M, N, L]. If batch mode is not needed, set L stride to Int<0>.");

    /* In the Cooperative kernel, one or multiple Consumers collaborate on the same tile */
    enum class WarpGroupRole {
      Producer = 0,
      Consumer = 1,
    };

    // Kernel level shared memory storage
    SharedStorage& shared_storage = *reinterpret_cast<SharedStorage*>(smem_buf);

    int thread_idx = int(threadIdx.x);
    int mma_thread_idx = thread_idx % size(TiledMma{});
    int warp_group_thread_idx = thread_idx % NumThreadsPerWarpGroup;
    int warp_group_idx = canonical_warp_group_idx();
    CUTLASS_ASSERT(warp_group_idx < NumWarpGroups);
    WarpGroupRole warp_group_role = warp_group_idx < NumLoadWarpGroups ? WarpGroupRole::Producer : WarpGroupRole::Consumer;

    // Mainloop Load pipeline
    using MainloopPipeline = typename CollectiveMainloop::MainloopPipeline;
    typename MainloopPipeline::Params mainloop_pipeline_params;
    if (warp_group_role == WarpGroupRole::Producer) {
      mainloop_pipeline_params.role = MainloopPipeline::ThreadCategory::Producer;
    }
    if (warp_group_role == WarpGroupRole::Consumer) {
      mainloop_pipeline_params.role = MainloopPipeline::ThreadCategory::Consumer;
    }
    mainloop_pipeline_params.producer_arv_count = NumLoadWarpGroups * NumThreadsPerWarpGroup;
    mainloop_pipeline_params.consumer_arv_count = NumMmaWarpGroups * NumThreadsPerWarpGroup;
    MainloopPipeline mainloop_pipeline(shared_storage.pipelines.mainloop, mainloop_pipeline_params);

    // Epilogue Load pipeline
    using EpiLoadPipeline = typename CollectiveEpilogue::LoadPipeline;
    typename EpiLoadPipeline::Params epi_load_pipeline_params;
    if (warp_group_role == WarpGroupRole::Producer) {
      epi_load_pipeline_params.role = EpiLoadPipeline::ThreadCategory::Producer;
    }
    if (warp_group_role == WarpGroupRole::Consumer) {
      epi_load_pipeline_params.role = EpiLoadPipeline::ThreadCategory::Consumer;
    }
    epi_load_pipeline_params.producer_arv_count = NumLoadWarpGroups * NumThreadsPerWarpGroup;
    epi_load_pipeline_params.consumer_arv_count = NumMmaWarpGroups * NumThreadsPerWarpGroup;
    EpiLoadPipeline epi_load_pipeline(shared_storage.pipelines.epi_load, epi_load_pipeline_params);

    // Epilogue Store pipeline
    using EpiStorePipeline = typename CollectiveEpilogue::StorePipeline;
    typename EpiStorePipeline::Params epi_store_pipeline_params;
    epi_store_pipeline_params.always_wait = true;
    EpiStorePipeline epi_store_pipeline(epi_store_pipeline_params);

    // Initialize starting pipeline states for the collectives
    // Epilogue store pipe is producer-only (consumer is TMA unit, waits via scoreboarding)
    typename CollectiveMainloop::PipelineState mainloop_pipe_consumer_state;
    typename CollectiveEpilogue::LoadPipelineState epi_load_pipe_consumer_state;

    // For the DMA Load (producer) we start with an opposite phase
    // i.e., we skip all waits since we know that the buffer is indeed empty
    PipelineState mainloop_pipe_producer_state = cutlass::make_producer_start_state<MainloopPipeline>();
    PipelineState epi_load_pipe_producer_state = cutlass::make_producer_start_state<EpiLoadPipeline>();
    PipelineState epi_store_pipe_producer_state = cutlass::make_producer_start_state<EpiStorePipeline>();

    // Separate out problem shape for convenience
    // Optionally append 1s until problem shape is rank-4 in case its is only rank-3 (MNK)
    auto problem_shape_MNKL = append<4>(params.problem_shape, Int<1>{});
    auto M = get<0>(problem_shape_MNKL);
    auto N = get<1>(problem_shape_MNKL);
    auto K = get<2>(problem_shape_MNKL);
    auto L = get<3>(problem_shape_MNKL);

    // Represent the full tensors
    Tensor mA_mkl = make_tensor(make_gmem_ptr(params.mainloop.ptr_A), make_shape(M,K,L), params.mainloop.dA); //(m,k,l)
    Tensor mB_nkl = make_tensor(make_gmem_ptr(params.mainloop.ptr_B), make_shape(N,K,L), params.mainloop.dB); //(n,k,l)

    // Get the appropriate blocks for this thread block -- potential for thread block locality
    TiledMma tiled_mma;
    auto blk_shape = TileShape{};                                                                // (BLK_M,BLK_N,BLK_K)

    // Make tiled views, defer the slice
    Tensor gA_mkl = local_tile(mA_mkl, blk_shape, make_coord(_,_,_), Step<_1, X,_1>{});          // (BLK_M,BLK_K,m,k,l)
    Tensor gB_nkl = local_tile(mB_nkl, blk_shape, make_coord(_,_,_), Step< X,_1,_1>{});          // (BLK_N,BLK_K,n,k,l)

    TileScheduler scheduler{params.scheduler};
    auto work_tile_info = scheduler.initial_work_tile_info(ClusterShape{});

    // In a warp specialized kernel, collectives expose data movement and compute operations separately
    CollectiveMainloop collective_mainloop;
    CollectiveEpilogue collective_epilogue{params.epilogue, shared_storage.tensors.epilogue};

    // Wait for all threads in the thread block
    __syncthreads();

    if (warp_group_role == WarpGroupRole::Producer) {

      while (work_tile_info.is_valid()) {
        // Compute m_coord, n_coord, l_coord with the post-tiled m-shape and n-shape
        auto m_coord = idx2crd(work_tile_info.M_idx, shape<2>(gA_mkl));
        auto n_coord = idx2crd(work_tile_info.N_idx, shape<2>(gB_nkl));
        auto l_coord = idx2crd(work_tile_info.L_idx, shape<4>(gB_nkl));
        auto blk_coord = make_coord(m_coord, n_coord, _, l_coord);

        // Slice with our work tile coordinates to construct mainloop tensor views
        Tensor gA = gA_mkl(_,_,m_coord,_,l_coord);                                                   // (BLK_M,BLK_K,k)
        Tensor gB = gB_nkl(_,_,n_coord,_,l_coord);                                                   // (BLK_N,BLK_K,k)

        // Get the number of K tiles to compute for this work as well as the starting K tile offset of the work.
        auto work_k_tile_count = TileScheduler::get_work_k_tile_count(work_tile_info, problem_shape_MNKL, blk_shape);
        auto work_k_tile_start = TileScheduler::get_work_k_tile_start(work_tile_info);
        auto k_tile_iter = cute::make_coord_iterator(idx2crd(work_k_tile_start, shape<2>(gA)), shape<2>(gA));

        // Compute tile residues for predication
        auto m_max_coord = M - size<0>(gA) * get<0>(blk_coord);                             // M - BLK_M * m_coord
        auto n_max_coord = N - size<0>(gB) * get<1>(blk_coord);                             // N - BLK_N * n_coord
        auto k_residue   = K - size<1>(gA) * size<2>(gA);                                   // K - BLK_K * k_coord_max
        auto residue_mnk = make_tuple(m_max_coord, n_max_coord, k_residue);

        collective_mainloop.load(
          mainloop_pipeline,
          mainloop_pipe_producer_state,
          gA,
          gB,
          k_tile_iter, work_k_tile_count,
          residue_mnk,
          thread_idx,
          shared_storage.tensors.mainloop
        );
        // Update starting pipeline state for the next tile
        mainloop_pipe_producer_state.advance(work_k_tile_count);

        if (TileScheduler::compute_epilogue(work_tile_info, params.scheduler) &&
           collective_epilogue.is_producer_load_needed()) {
          epi_load_pipe_producer_state =
          collective_epilogue.load(
            epi_load_pipeline,
            epi_load_pipe_producer_state,
            problem_shape_MNKL,
            blk_shape,
            blk_coord,
            tiled_mma,
            warp_group_thread_idx,
            shared_storage.tensors.epilogue
          );
      }

        // Get next work tile
        auto [next_work_tile_info, increment_pipe] = scheduler.fetch_next_work(work_tile_info);
        work_tile_info = next_work_tile_info;
      } // Scheduler work fetch loop

      // Make sure all Consumer Warp Groups have been waited upon
      collective_mainloop.load_tail(mainloop_pipeline, mainloop_pipe_producer_state);
      
      if (collective_epilogue.is_producer_load_needed()) {
        collective_epilogue.load_tail(epi_load_pipeline, epi_load_pipe_producer_state);
      }
    } // Producer Warp Group End

    else if (warp_group_role == WarpGroupRole::Consumer) {

      bool do_store_tail = false;
      while (work_tile_info.is_valid()) {
        // Compute m_coord, n_coord, l_coord with the post-tiled m-shape and n-shape
        auto m_coord = idx2crd(work_tile_info.M_idx, shape<2>(gA_mkl));
        auto n_coord = idx2crd(work_tile_info.N_idx, shape<2>(gB_nkl));
        auto l_coord = idx2crd(work_tile_info.L_idx, shape<4>(gB_nkl));
        auto blk_coord = make_coord(m_coord, n_coord, _, l_coord);
        auto work_k_tile_count = TileScheduler::get_work_k_tile_count(work_tile_info, problem_shape_MNKL, blk_shape);

        // Allocate the the accumulators for the (M,N) blk_shape
        //
        // MSVC CTAD breaks if we say "Tensor" here, so we use "auto" instead.
        auto accumulators = partition_fragment_C(tiled_mma, take<0,2>(blk_shape));               // (MMA,MMA_M,MMA_N)

        collective_mainloop.mma(
          mainloop_pipeline,
          mainloop_pipe_consumer_state,
          accumulators,
          work_k_tile_count,
          mma_thread_idx,
          shared_storage.tensors.mainloop,
          params.mainloop
        );

        // Make sure the math instructions are done and free buffers before entering the epilogue
        collective_mainloop.mma_tail(
          mainloop_pipeline,
          mainloop_pipe_consumer_state,
          work_k_tile_count
        );

        // Update starting mainloop pipeline state for the next tile
        mainloop_pipe_consumer_state.advance(work_k_tile_count);

        // Index of warp group within consumer warp groups
        int consumer_warp_group_idx = canonical_warp_group_idx() - NumLoadWarpGroups;

        // Perform reduction across splits, if needed
        TileScheduler::fixup(
          params.scheduler, work_tile_info, accumulators, NumMmaWarpGroups, consumer_warp_group_idx);

        if (TileScheduler::compute_epilogue(work_tile_info, params.scheduler)) {
          // Epilogue and write to gD
          auto [epi_load_pipe_consumer_state_next, epi_store_pipe_producer_state_next] =
          collective_epilogue.store(
            epi_load_pipeline,
            epi_load_pipe_consumer_state,
            epi_store_pipeline,
            epi_store_pipe_producer_state,
            problem_shape_MNKL,
            blk_shape,
            blk_coord,
            accumulators,
            tiled_mma,
            mma_thread_idx,
            shared_storage.tensors.epilogue
          );
          epi_load_pipe_consumer_state = epi_load_pipe_consumer_state_next;
          epi_store_pipe_producer_state = epi_store_pipe_producer_state_next;
          do_store_tail = true;
        }

        // Get next work tile
        auto [next_work_tile_info, increment_pipe] = scheduler.fetch_next_work(work_tile_info);
        work_tile_info = next_work_tile_info;
      } // Scheduler work fetch loop

      if (do_store_tail) {
        collective_epilogue.store_tail(
          epi_load_pipeline,
          epi_load_pipe_consumer_state,
          epi_store_pipeline,
          epi_store_pipe_producer_state
        );
      }
    } // Consumer Warp Groups End
#endif
  }

};

///////////////////////////////////////////////////////////////////////////////

} // namespace cutlass::gemm::kernel
