#include <sutil/Exception.h>
#include <sutil/Timing.h>
#include <thrust/device_vector.h>

#include "optixRangeSearch.h"
#include "state.h"
#include "func.h"

void search(WhittedState& state, int batch_id) {
  Timing::startTiming("batch search time");
    Timing::startTiming("search compute");
      unsigned int numQueries = state.numActQueries[batch_id];

      state.params.limit = state.knn;
      thrust::device_ptr<unsigned int> output_buffer = getThrustDevicePtr(numQueries * state.params.limit);

      if (state.qGasSortMode && !state.toGather) state.params.d_r2q_map = state.d_r2q_map[batch_id];
      else state.params.d_r2q_map = nullptr; // if no GAS-sorting or has done gather, this map is null.

      state.params.isApprox = false;
      // approximate in the first batch of radius search. can't approximate in
      // the knn search. note that AABB test is inherently approximately so if
      // we choose to approximate the early batches in radius earch, the result
      // might be incorrect. see:
      // https://forums.developer.nvidia.com/t/numerical-imprecision-in-intersection-test/183665/4.
      // TODO: change it when the batch order changes.
      //if ((state.searchMode == "radius") && state.partition && (batch_id < state.numOfBatches - 1)) state.params.isApprox = true;

      state.params.radius = state.launchRadius[batch_id];

      launchSubframe( thrust::raw_pointer_cast(output_buffer), state, batch_id );
      OMIT_ON_E2EMSR( CUDA_CHECK( cudaStreamSynchronize( state.stream[batch_id] ) ) );
    Timing::stopTiming(true);

    // cudaMallocHost is time consuming; must be hidden behind async launch
    Timing::startTiming("result copy D2H");
      void* data;
      cudaMallocHost(reinterpret_cast<void**>(&data), numQueries * state.params.limit * sizeof(unsigned int));
      state.h_res[batch_id] = data;

      // TODO: do a thrust copy?
      CUDA_CHECK( cudaMemcpyAsync(
                      static_cast<void*>( data ),
                      thrust::raw_pointer_cast(output_buffer),
                      numQueries * state.params.limit * sizeof(unsigned int),
                      cudaMemcpyDeviceToHost,
                      state.stream[batch_id]
                      ) );
      OMIT_ON_E2EMSR( CUDA_CHECK( cudaStreamSynchronize( state.stream[batch_id] ) ) );
    Timing::stopTiming(true);
  Timing::stopTiming(true);

  // TODO: this is free device memory but will block until the previous optix launch finish and the res is written back.
  //CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(output_buffer) ) );
}

thrust::device_ptr<unsigned int> initialTraversal(WhittedState& state, int batch_id) {
  Timing::startTiming("initial traversal");
    unsigned int numQueries = state.numActQueries[batch_id];

    state.params.limit = 1;
    thrust::device_ptr<unsigned int> output_buffer = getThrustDevicePtr(numQueries * state.params.limit);

    state.params.d_r2q_map = nullptr; // contains the index to reorder rays
    state.params.isApprox = true;
    state.params.radius = state.launchRadius[batch_id];

    launchSubframe( thrust::raw_pointer_cast(output_buffer), state, batch_id );
    // TODO: could delay this until sort, but initial traversal is lightweight anyways
    OMIT_ON_E2EMSR( CUDA_CHECK( cudaStreamSynchronize( state.stream[batch_id] ) ) );
  Timing::stopTiming(true);

  return output_buffer;
}

void gasSortSearch(WhittedState& state, int batch_id) {
  // Initial traversal to aggregate the queries
  thrust::device_ptr<unsigned int> d_firsthit_idx_ptr = initialTraversal(state, batch_id);

  // Generate the GAS-sorted query order
  thrust::device_ptr<unsigned int> d_indices_ptr;
  if (state.qGasSortMode == 1)
    d_indices_ptr = sortQueriesByFHCoord(state, d_firsthit_idx_ptr, batch_id);
  else if (state.qGasSortMode == 2)
    d_indices_ptr = sortQueriesByFHIdx(state, d_firsthit_idx_ptr, batch_id);
  state.d_firsthit_idx[batch_id] = reinterpret_cast<void*>(thrust::raw_pointer_cast(d_firsthit_idx_ptr));

  // Actually sort queries in memory if toGather is enabled
  if (state.toGather)
    gatherQueries( state, d_indices_ptr, batch_id );
}
