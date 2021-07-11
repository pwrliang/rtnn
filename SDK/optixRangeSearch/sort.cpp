#include <cuda_runtime.h>

#include <sutil/Exception.h>
#include <sutil/vec_math.h>
#include <sutil/Timing.h>

#include <thrust/device_vector.h>
#include <thrust/sort.h>
#include <thrust/copy.h>
#include <thrust/sequence.h>
#include <thrust/gather.h>

#include <algorithm>
#include <iomanip>
#include <cstring>
#include <fstream>
#include <string>
#include <random>
#include <cstdlib>
#include <queue>
#include <unordered_set>

#include "optixRangeSearch.h"
#include "func.h"
#include "state.h"
#include "grid.h"

void computeMinMax(WhittedState& state, ParticleType type)
{
  unsigned int N;
  float3* particles;
  if (type == POINT) {
    N = state.numPoints;
    particles = state.params.points;
  } else {
    N = state.numQueries;
    particles = state.params.queries;
  }

  // TODO: maybe use long since we are going to convert a float to its floor value?
  thrust::host_vector<int3> h_MinMax(2);
  h_MinMax[0] = make_int3(std::numeric_limits<int>().max(), std::numeric_limits<int>().max(), std::numeric_limits<int>().max());
  h_MinMax[1] = make_int3(std::numeric_limits<int>().min(), std::numeric_limits<int>().min(), std::numeric_limits<int>().min());

  thrust::device_vector<int3> d_MinMax = h_MinMax;

  unsigned int threadsPerBlock = 64;
  unsigned int numOfBlocks = N / threadsPerBlock + 1;
  // compare only the ints since atomicAdd has only int version
  kComputeMinMax(numOfBlocks,
                 threadsPerBlock,
                 particles,
                 N,
                 thrust::raw_pointer_cast(&d_MinMax[0]),
                 thrust::raw_pointer_cast(&d_MinMax[1])
                 );

  h_MinMax = d_MinMax;

  // minCell encloses the scene but maxCell doesn't (floor and int in the kernel) so increment by 1 to enclose the scene.
  // TODO: consider minus 1 for minCell too to avoid the numerical precision issue
  int3 minCell = h_MinMax[0];
  int3 maxCell = h_MinMax[1] + make_int3(1, 1, 1);
 
  state.Min.x = minCell.x;
  state.Min.y = minCell.y;
  state.Min.z = minCell.z;
 
  state.Max.x = maxCell.x;
  state.Max.y = maxCell.y;
  state.Max.z = maxCell.z;

  //fprintf(stdout, "\tcell boundary: (%d, %d, %d), (%d, %d, %d)\n", minCell.x, minCell.y, minCell.z, maxCell.x, maxCell.y, maxCell.z);
  //fprintf(stdout, "\tscene boundary: (%f, %f, %f), (%f, %f, %f)\n", state.Min.x, state.Min.y, state.Min.z, state.Max.x, state.Max.y, state.Max.z);
}

void genMask (unsigned int* h_CellParticleCounts, unsigned int numberOfCells, WhittedState& state, GridInfo& gridInfo, unsigned int N) {
  std::vector<unsigned int> cellSearchSize(numberOfCells, 0);

  for (int x = 0; x < gridInfo.GridDimension.x; x++) {
    for (int y = 0; y < gridInfo.GridDimension.y; y++) {
      for (int z = 0; z < gridInfo.GridDimension.z; z++) {
        // now let's check;
        int cellIndex = (x * gridInfo.GridDimension.y + y) * gridInfo.GridDimension.z + z;
        if (h_CellParticleCounts[cellIndex] == 0) continue;
        //fprintf(stdout, "%d, %u\n", cellIndex, h_CellParticleCounts[cellIndex]);

        int iter = 0;
        int count = 0;
        int maxWidth = state.params.radius / sqrt(2) * 2;
        while(1) {
          for (int ix = x-iter; ix <= x+iter; ix++) {
            for (int iy = y-iter; iy <= y+iter; iy++) {
              for (int iz = z-iter; iz <= z+iter; iz++) {
                if (ix < 0 || ix >= gridInfo.GridDimension.x || iy < 0 || iy >= gridInfo.GridDimension.y || iz < 0 || iz >= gridInfo.GridDimension.z) continue;
                else {
                  unsigned int iCellIdx = (ix * gridInfo.GridDimension.y + iy) * gridInfo.GridDimension.z + iz;
                  count += h_CellParticleCounts[iCellIdx];
                }
              }
            }
          }
          // TODO: there could be corner cases here, e.g., maxWidth is very small, cellSize will be 0 (same as uninitialized).
          int width = (iter * 2 + 1) * state.params.radius / state.crRatio;
          if (width > maxWidth) {
            cellSearchSize[cellIndex] = 0; // if width > maxWidth, we need to do a full search.
            break;
          }
          else if (count >= state.params.knn) {
            cellSearchSize[cellIndex] = iter + 1; // + 1 so that iter being 0 doesn't become full search.
            break;
          }
          else {
            iter++;
            count = 0;
          }
        }
        //fprintf(stdout, "%u, %u\n", cellIndex, cellSearchSize[cellIndex]);
      }
    }
  }

  state.cellMask = new bool[numberOfCells];
  unsigned int numOfActiveQueries = 0;
  for (unsigned int i = 0; i < numberOfCells; i++) {
    //if (cellSearchSize[i] != 0) fprintf(stdout, "%u, %u, %u, %f\n", i, cellSearchSize[i], h_CellParticleCounts[i], (cellSearchSize[i] * 2 - 1) * state.params.radius/state.crRatio);
    if (cellSearchSize[i] > 0 && cellSearchSize[i] <= 2) {
       state.cellMask[i] = true;
       numOfActiveQueries += h_CellParticleCounts[i];
    }
    else state.cellMask[i] = false;
  }

  unsigned int numOfActiveCells = std::count(state.cellMask, state.cellMask + numberOfCells, true);
  fprintf(stdout, "# of Act Cells: %u\n# of Act Queries: %u\n", numOfActiveCells, numOfActiveQueries);
}

unsigned int genGridInfo(WhittedState& state, unsigned int N, GridInfo& gridInfo) {
  float3 sceneMin = state.Min;
  float3 sceneMax = state.Max;

  gridInfo.ParticleCount = N;
  gridInfo.GridMin = sceneMin;

  float cellSize = state.params.radius/state.crRatio; // TODO: change cellSize as a input parameter to the function
  float3 gridSize = sceneMax - sceneMin;
  gridInfo.GridDimension.x = static_cast<unsigned int>(ceilf(gridSize.x / cellSize));
  gridInfo.GridDimension.y = static_cast<unsigned int>(ceilf(gridSize.y / cellSize));
  gridInfo.GridDimension.z = static_cast<unsigned int>(ceilf(gridSize.z / cellSize));

  // Adjust grid size to multiple of cell size
  gridSize.x = gridInfo.GridDimension.x * cellSize;
  gridSize.y = gridInfo.GridDimension.y * cellSize;
  gridSize.z = gridInfo.GridDimension.z * cellSize;

  gridInfo.GridDelta.x = gridInfo.GridDimension.x / gridSize.x;
  gridInfo.GridDelta.y = gridInfo.GridDimension.y / gridSize.y;
  gridInfo.GridDelta.z = gridInfo.GridDimension.z / gridSize.z;

  // morton code can only be correctly calcuated for a cubic, where each
  // dimension is of the same size. currently we generate the largely meta_grid
  // possible, which would divice the entire grid into multiple meta grids.
  gridInfo.meta_grid_dim = std::min({gridInfo.GridDimension.x, gridInfo.GridDimension.y, gridInfo.GridDimension.z});
  gridInfo.meta_grid_size = gridInfo.meta_grid_dim * gridInfo.meta_grid_dim * gridInfo.meta_grid_dim;

  // One meta grid cell contains meta_grid_dim^3 cells. The morton curve is
  // calculated for each metagrid, and the order of metagrid is raster order.
  // So if meta_grid_dim is 1, this is basically the same as raster order
  // across all cells. If meta_grid_dim is the same as GridDimension, this
  // calculates one single morton curve for the entire grid.
  gridInfo.MetaGridDimension.x = static_cast<unsigned int>(ceilf(gridInfo.GridDimension.x / (float)gridInfo.meta_grid_dim));
  gridInfo.MetaGridDimension.y = static_cast<unsigned int>(ceilf(gridInfo.GridDimension.y / (float)gridInfo.meta_grid_dim));
  gridInfo.MetaGridDimension.z = static_cast<unsigned int>(ceilf(gridInfo.GridDimension.z / (float)gridInfo.meta_grid_dim));

  // metagrids will slightly increase the total cells
  unsigned int numberOfCells = (gridInfo.MetaGridDimension.x * gridInfo.MetaGridDimension.y * gridInfo.MetaGridDimension.z) * gridInfo.meta_grid_size;
  //fprintf(stdout, "\tGrid dimension (without meta grids): %u, %u, %u\n", gridInfo.GridDimension.x, gridInfo.GridDimension.y, gridInfo.GridDimension.z);
  //fprintf(stdout, "\tGrid dimension (with meta grids): %u, %u, %u\n", gridInfo.MetaGridDimension.x * gridInfo.meta_grid_dim, gridInfo.MetaGridDimension.y * gridInfo.meta_grid_dim, gridInfo.MetaGridDimension.z * gridInfo.meta_grid_dim);
  //fprintf(stdout, "\tMeta Grid dimension: %u, %u, %u\n", gridInfo.MetaGridDimension.x, gridInfo.MetaGridDimension.y, gridInfo.MetaGridDimension.z);
  //fprintf(stdout, "\tLength of a meta grid: %u\n", gridInfo.meta_grid_dim);
  fprintf(stdout, "\tNumber of cells: %u\n", numberOfCells);
  fprintf(stdout, "\tCell size: %f\n", cellSize);

  // update GridDimension so that it can be used in the kernels (otherwise raster order is incorrect)
  gridInfo.GridDimension.x = gridInfo.MetaGridDimension.x * gridInfo.meta_grid_dim;
  gridInfo.GridDimension.y = gridInfo.MetaGridDimension.y * gridInfo.meta_grid_dim;
  gridInfo.GridDimension.z = gridInfo.MetaGridDimension.z * gridInfo.meta_grid_dim;

  return numberOfCells;
}

void gridSort(WhittedState& state, ParticleType type, bool morton) {
  unsigned int N;
  float3* particles;
  float3* h_particles;
  if (type == POINT) {
    N = state.numPoints;
    particles = state.params.points;
    h_particles = state.h_points;
  } else {
    N = state.numQueries;
    particles = state.params.queries;
    h_particles = state.h_queries;
  }

  GridInfo gridInfo;
  unsigned int numberOfCells = genGridInfo(state, N, gridInfo);

  thrust::device_ptr<unsigned int> d_ParticleCellIndices_ptr = getThrustDevicePtr(N);
  thrust::device_ptr<unsigned int> d_CellParticleCounts_ptr = getThrustDevicePtr(numberOfCells); // this takes a lot of memory
  fillByValue(d_CellParticleCounts_ptr, numberOfCells, 0);
  thrust::device_ptr<unsigned int> d_LocalSortedIndices_ptr = getThrustDevicePtr(N);

  unsigned int threadsPerBlock = 64;
  unsigned int numOfBlocks = N / threadsPerBlock + 1;
  kInsertParticles(numOfBlocks,
                   threadsPerBlock,
                   gridInfo,
                   particles,
                   thrust::raw_pointer_cast(d_ParticleCellIndices_ptr),
                   thrust::raw_pointer_cast(d_CellParticleCounts_ptr),
                   thrust::raw_pointer_cast(d_LocalSortedIndices_ptr),
                   morton
                   );

  bool debug = false;
  //if (debug) {
  //  for (unsigned int i = 0; i < numberOfCells; i++) {
  //    if (h_CellParticleCounts[i] != 0) fprintf(stdout, "%u, %u\n", i, h_CellParticleCounts[i]);
  //  }
  //}

  thrust::device_ptr<unsigned int> d_CellOffsets_ptr = getThrustDevicePtr(numberOfCells);
  fillByValue(d_CellOffsets_ptr, numberOfCells, 0); // need to initialize it even for exclusive scan
  exclusiveScan(d_CellParticleCounts_ptr, numberOfCells, d_CellOffsets_ptr);

  thrust::device_ptr<unsigned int> d_posInSortedPoints_ptr = getThrustDevicePtr(N);
  // if samepq and partition is enabled, do it here. we are partitioning points, but it's the same as queries.
  if (state.partition && state.samepq) {
    thrust::host_vector<unsigned int> h_CellParticleCounts(numberOfCells);
    thrust::copy(d_CellParticleCounts_ptr, d_CellParticleCounts_ptr + numberOfCells, h_CellParticleCounts.begin());

    genMask(h_CellParticleCounts.data(), numberOfCells, state, gridInfo, N);

    // TODO: free them
    thrust::device_ptr<bool> d_rayMask = getThrustDeviceBoolPtr(state.numQueries);
    thrust::device_ptr<bool> d_cellMask = getThrustDeviceBoolPtr(numberOfCells);
    thrust::copy(state.cellMask, state.cellMask + numberOfCells, d_cellMask);

    kCountingSortIndices_genMask(numOfBlocks,
                         threadsPerBlock,
                         gridInfo,
                         thrust::raw_pointer_cast(d_ParticleCellIndices_ptr),
                         thrust::raw_pointer_cast(d_CellOffsets_ptr),
                         thrust::raw_pointer_cast(d_LocalSortedIndices_ptr),
                         thrust::raw_pointer_cast(d_posInSortedPoints_ptr),
                         thrust::raw_pointer_cast(d_cellMask),
                         thrust::raw_pointer_cast(d_rayMask)
                         );

    // make a copy of the keys since they are useless after the first sort. no need to use stable sort since the keys are unique, so masks and the queries are gauranteed to be sorted in exactly the same way.
    // TODO: Can we do away with th extra copy by replacing sort by key with scatter? that'll need new space too...
    thrust::device_ptr<unsigned int> d_posInSortedPoints_ptr_copy = getThrustDevicePtr(N);
    //thrust::copy(d_posInSortedPoints_ptr, d_posInSortedPoints_ptr + N, d_posInSortedPoints_ptr_copy); // not sure why this doesn't link.
    CUDA_CHECK( cudaMemcpy(
                reinterpret_cast<void*>( thrust::raw_pointer_cast(d_posInSortedPoints_ptr_copy) ),
                thrust::raw_pointer_cast(d_posInSortedPoints_ptr),
                N * sizeof( unsigned int ),
                cudaMemcpyDeviceToDevice
    ) );
    // sort the ray masks as well, but do that after the query sorting so that we get the nice query order.
    sortByKey(d_posInSortedPoints_ptr_copy, d_rayMask, N);

    unsigned int numOfActiveQueries = countByPred(d_rayMask, N, true);
    state.numQueries = numOfActiveQueries;
    std::cout << state.numQueries << std::endl;

    // TODO: this is basically discarding the rest of the queries; OK for now...
    thrust::device_ptr<float3> d_curQs = getThrustDeviceF3Ptr(numOfActiveQueries);
    copyIfStencilTrue(state.params.queries, N, d_rayMask, d_curQs); // use N since state.numQueries is updated now
    state.params.queries = thrust::raw_pointer_cast(d_curQs);
    CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_posInSortedPoints_ptr_copy) ) );

    // Copy the active queries to host.
    // TODO: do this in a stream
    state.h_queries = new float3[numOfActiveQueries];
    thrust::copy(d_curQs, d_curQs + numOfActiveQueries, state.h_queries);
    printf("%f, %f, %f\n", state.h_queries[1192803].x, state.h_queries[1192803].y, state.h_queries[1192803].z);
  } else {
    kCountingSortIndices(numOfBlocks,
                         threadsPerBlock,
                         gridInfo,
                         thrust::raw_pointer_cast(d_ParticleCellIndices_ptr),
                         thrust::raw_pointer_cast(d_CellOffsets_ptr),
                         thrust::raw_pointer_cast(d_LocalSortedIndices_ptr),
                         thrust::raw_pointer_cast(d_posInSortedPoints_ptr)
                         );
  }
  // in-place sort; no new device memory is allocated
  sortByKey(d_posInSortedPoints_ptr, thrust::device_pointer_cast(particles), N);

  // copy particles to host, regardless of partition. for POINT, this is to make sure the points in device are consistent with the host points used to build the GAS. for QUERY and POINT, this also makes sure the sanity check passes.
  // TODO: do this in a stream
  thrust::device_ptr<float3> d_particles_ptr = thrust::device_pointer_cast(particles);
  thrust::copy(d_particles_ptr, d_particles_ptr + N, h_particles);

  CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_ParticleCellIndices_ptr) ) );
  CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_posInSortedPoints_ptr) ) );
  CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_CellOffsets_ptr) ) );
  CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_LocalSortedIndices_ptr) ) );
  CUDA_CHECK( cudaFree( (void*)thrust::raw_pointer_cast(d_CellParticleCounts_ptr) ) );

  debug = false;
  if (debug) {
    thrust::host_vector<uint> temp(N);
    thrust::copy(d_posInSortedPoints_ptr, d_posInSortedPoints_ptr + N, temp.begin());
    for (unsigned int i = 0; i < N; i++) {
      fprintf(stdout, "%u (%f, %f, %f)\n", temp[i], h_particles[i].x, h_particles[i].y, h_particles[i].z);
    }
  }
}

void oneDSort ( WhittedState& state, ParticleType type ) {
  // sort points/queries based on coordinates (x/y/z)
  unsigned int N;
  float3* particles;
  float3* h_particles;
  if (type == POINT) {
    N = state.numPoints;
    particles = state.params.points;
    h_particles = state.h_points;
  } else {
    N = state.numQueries;
    particles = state.params.queries;
    h_particles = state.h_queries;
  }

  // TODO: do this whole thing on GPU.
  // create 1d points as the sorting key and upload it to device memory
  thrust::host_vector<float> h_key(N);
  for(unsigned int i = 0; i < N; i++) {
    h_key[i] = h_particles[i].x;
  }

  float* d_key = nullptr;
  CUDA_CHECK( cudaMalloc(
      reinterpret_cast<void**>(&d_key),
      N * sizeof(float) ) );
  thrust::device_ptr<float> d_key_ptr = thrust::device_pointer_cast(d_key);
  thrust::copy(h_key.begin(), h_key.end(), d_key_ptr);

  // actual sort
  thrust::device_ptr<float3> d_particles_ptr = thrust::device_pointer_cast(particles);
  sortByKey( d_key_ptr, d_particles_ptr, N );
  CUDA_CHECK( cudaFree( (void*)d_key ) );

  // TODO: lift it outside of this function and combine with other sorts?
  // copy the sorted queries to host so that we build the GAS in the same order
  // note that the h_queries at this point still point to what h_points points to
  thrust::copy(d_particles_ptr, d_particles_ptr + N, h_particles);
}

void sortParticles ( WhittedState& state, ParticleType type, int sortMode ) {
  if (!sortMode) return;

  // the semantices of the two sort functions are: sort data in device, and copy the sorted data back to host.
  std::string typeName = ((type == POINT) ? "points" : "queries");
  Timing::startTiming("sort " + typeName);
    if (sortMode == 3) {
      oneDSort(state, type);
    } else {
      computeMinMax(state, type);

      bool morton; // false for raster order
      if (sortMode == 1) morton = true;
      else {
        assert(sortMode == 2);
        morton = false;
      }
      gridSort(state, type, morton);
    }
  Timing::stopTiming(true);
}

thrust::device_ptr<unsigned int> sortQueriesByFHCoord( WhittedState& state, thrust::device_ptr<unsigned int> d_firsthit_idx_ptr ) {
  // this is sorting queries by the x/y/z coordinate of the first hit primitives.

  Timing::startTiming("gas-sort queries init");
    // allocate device memory for storing the keys, which will be generated by a gather and used in sort_by_keys
    float* d_key;
    cudaMalloc(reinterpret_cast<void**>(&d_key),
               state.numQueries * sizeof(float) );
    thrust::device_ptr<float> d_key_ptr = thrust::device_pointer_cast(d_key);
    state.d_key = d_key; // just so we have a handle to free it later
  
    // create indices for gather and upload to device
    thrust::host_vector<float> h_orig_points_1d(state.numQueries);
    // TODO: do this in CUDA
    for (unsigned int i = 0; i < state.numQueries; i++) {
      h_orig_points_1d[i] = state.h_points[i].z; // could be other dimensions
    }
    thrust::device_vector<float> d_orig_points_1d = h_orig_points_1d;

    // initialize a sequence to be sorted, which will become the r2q map.
    // TODO: need to free this.
    thrust::device_ptr<unsigned int> d_r2q_map_ptr = genSeqDevice(state.numQueries);
  Timing::stopTiming(true);
  
  Timing::startTiming("gas-sort queries");
    // TODO: do thrust work in a stream: https://forums.developer.nvidia.com/t/thrust-and-streams/53199
    // first use a gather to generate the keys, then sort by keys
    gatherByKey(d_firsthit_idx_ptr, &d_orig_points_1d, d_key_ptr, state.numQueries);
    sortByKey( d_key_ptr, d_r2q_map_ptr, state.numQueries );
    state.d_r2q_map = thrust::raw_pointer_cast(d_r2q_map_ptr);
  Timing::stopTiming(true);
  
  // if debug, copy the sorted keys and values back to host
  bool debug = false;
  if (debug) {
    thrust::host_vector<unsigned int> h_vec_val(state.numQueries);
    thrust::copy(d_r2q_map_ptr, d_r2q_map_ptr+state.numQueries, h_vec_val.begin());

    thrust::host_vector<float> h_vec_key(state.numQueries);
    thrust::copy(d_key_ptr, d_key_ptr+state.numQueries, h_vec_key.begin());
    
    for (unsigned int i = 0; i < h_vec_val.size(); i++) {
      std::cout << h_vec_key[i] << "\t" 
                << h_vec_val[i] << "\t" 
                << state.h_queries[h_vec_val[i]].x << "\t"
                << state.h_queries[h_vec_val[i]].y << "\t"
                << state.h_queries[h_vec_val[i]].z
                << std::endl;
    }
  }

  return d_r2q_map_ptr;
}

thrust::device_ptr<unsigned int> sortQueriesByFHIdx( WhittedState& state, thrust::device_ptr<unsigned int> d_firsthit_idx_ptr ) {
  // this is sorting queries just by the first hit primitive IDs

  // initialize a sequence to be sorted, which will become the r2q map
  Timing::startTiming("gas-sort queries init");
    thrust::device_ptr<unsigned int> d_r2q_map_ptr = genSeqDevice(state.numQueries);
  Timing::stopTiming(true);

  Timing::startTiming("gas-sort queries");
    sortByKey( d_firsthit_idx_ptr, d_r2q_map_ptr, state.numQueries );
    // thrust can't be used in kernel code since NVRTC supports only a
    // limited subset of C++, so we would have to explicitly cast a
    // thrust device vector to its raw pointer. See the problem discussed
    // here: https://github.com/cupy/cupy/issues/3728 and
    // https://github.com/cupy/cupy/issues/3408. See how cuNSearch does it:
    // https://github.com/InteractiveComputerGraphics/cuNSearch/blob/master/src/cuNSearchDeviceData.cu#L152
    state.d_r2q_map = thrust::raw_pointer_cast(d_r2q_map_ptr);
  Timing::stopTiming(true);

  bool debug = false;
  if (debug) {
    thrust::host_vector<unsigned int> h_vec_val(state.numQueries);
    thrust::copy(d_r2q_map_ptr, d_r2q_map_ptr+state.numQueries, h_vec_val.begin());

    thrust::host_vector<unsigned int> h_vec_key(state.numQueries);
    thrust::copy(d_firsthit_idx_ptr, d_firsthit_idx_ptr+state.numQueries, h_vec_key.begin());

    for (unsigned int i = 0; i < h_vec_val.size(); i++) {
      std::cout << h_vec_key[i] << "\t"
                << h_vec_val[i] << "\t"
                << state.h_queries[h_vec_val[i]].x << "\t"
                << state.h_queries[h_vec_val[i]].y << "\t"
                << state.h_queries[h_vec_val[i]].z
                << std::endl;
    }
  }

  return d_r2q_map_ptr;
}

void gatherQueries( WhittedState& state, thrust::device_ptr<unsigned int> d_indices_ptr ) {
  // Perform a device gather before launching the actual search, which by
  // itself is not useful, since we access each query only once (in the RG
  // program) anyways. in reality we see little gain by gathering queries. but
  // if queries and points point to the same device memory, gathering queries
  // effectively reorders the points too. we access points in the IS program
  // (get query origin using the hit primIdx), and so it would be nice to
  // coalesce memory by reordering points. but note two things. First, we
  // access only one point and only once in each IS program and the bulk of
  // memory access is to the BVH which is out of our control, so better memory
  // coalescing has less effect than in traditional grid search. Second, if the
  // points are already sorted in a good order (raster scan or z-order), this
  // reordering has almost zero effect. empirically, we get 10% search time
  // reduction for large point clouds and the points originally are poorly
  // ordered. but this comes at a chilling overhead that we need to rebuild the
  // GAS (to make sure the ID of a box in GAS is the ID of the sphere in device
  // memory; otherwise IS program is in correct), which is on the critical path
  // and whose overhead can't be hidden. so almost always this optimization
  // leads to performance degradation, both toGather and reorderPoints are
  // disabled by default.

  Timing::startTiming("gather queries");
    // allocate device memory for reordered/gathered queries
    float3* d_reordered_queries;
    // TODO: change this to thrust device vector
    cudaMalloc(reinterpret_cast<void**>(&d_reordered_queries),
               state.numQueries * sizeof(float3) );
    thrust::device_ptr<float3> d_reord_queries_ptr = thrust::device_pointer_cast(d_reordered_queries);

    // get pointer to original queries in device memory
    thrust::device_ptr<float3> d_orig_queries_ptr = thrust::device_pointer_cast(state.params.queries);

    // gather by key, which is generated by the previous sort
    gatherByKey(d_indices_ptr, d_orig_queries_ptr, d_reord_queries_ptr, state.numQueries);

    // if not samepq, then we can free the original query device memory
    if (!state.samepq) CUDA_CHECK( cudaFree( (void*)state.params.queries ) );
    state.params.queries = thrust::raw_pointer_cast(d_reord_queries_ptr);
    assert(state.params.points != state.params.queries);
  Timing::stopTiming(true);

  // Copy reordered queries to host for sanity check
  thrust::host_vector<float3> host_reord_queries(state.numQueries);
  // if not samepq, free the original query host memory first
  if (!state.samepq) delete state.h_queries;
  state.h_queries = new float3[state.numQueries]; // don't overwrite h_points
  thrust::copy(d_reord_queries_ptr, d_reord_queries_ptr+state.numQueries, state.h_queries);
  assert (state.h_points != state.h_queries);

  // if samepq, we could try reordering points according to the new query
  // layout. see caveats in the note above.
  if (state.samepq && state.reorderPoints) {
    // will rebuild the GAS later
    delete state.h_points;
    state.h_points = state.h_queries;
    CUDA_CHECK( cudaFree( (void*)state.params.points ) );
    state.params.points = state.params.queries;
  }
}

