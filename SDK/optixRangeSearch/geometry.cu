//
// Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include <optix.h>

#include "optixRangeSearch.h"
#include "helpers.h"

extern "C" {
__constant__ Params params;
}

extern "C" __device__ void intersect_sphere()
{
    // This is called when a ray-bbox intersection is found, but we still can't
    // be sure that the point is within the sphere. It's possible that the
    // point is within the bbox but no within the sphere, and it's also
    // possible that the point is just outside of the bbox and just intersects
    // with the bbox. Note that it's wasteful to do a ray-sphere intersection
    // test and use the intersected Ts to decide whethere a point is inside the
    // sphere or not
    // (https://www.scratchapixel.com/lessons/3d-basic-rendering/minimal-ray-tracer-rendering-simple-shapes/ray-sphere-intersection).

    unsigned int primIdx = optixGetPrimitiveIndex();
    const float3 center = params.points[primIdx];

    const float3  ray_orig = optixGetWorldRayOrigin();

    float3 O = ray_orig - center;

    if (dot(O, O) < params.radius * params.radius) {
      //unsigned int id = optixGetPayload_1();
      //optixSetPayload_1( id+1 );

      unsigned int id = optixGetPayload_1();
      if (id < params.limit) {
        unsigned int queryIdx = optixGetPayload_0();
        unsigned int primIdx = optixGetPrimitiveIndex();
        params.frame_buffer[queryIdx * params.limit + id] = primIdx;
        if (id + 1 == params.limit)
          optixReportIntersection( 0, 0 );
        else optixSetPayload_1( id+1 );
      }
    }
}

extern "C" __global__ void __intersection__sphere_radius()
{
  // The IS program will be called if the ray origin is within a primitive's
  // bbox (even if the actual intersections are beyond the tmin and tmax).

  bool isApprox = false;

  // if d_r2q_map is null and limit is 1, this is the initial run for sorting
  if (params.d_r2q_map == nullptr && params.limit == 1) isApprox = true;

  if (isApprox) {
    unsigned int id = optixGetPayload_1();
    if (id < params.limit) {
      unsigned int queryIdx = optixGetPayload_0();
      unsigned int primIdx = optixGetPrimitiveIndex();
      params.frame_buffer[queryIdx * params.limit + id] = primIdx;
      if (id + 1 == params.limit)
        optixReportIntersection( 0, 0 );
      else optixSetPayload_1( id+1 );
    }
  } else {
    intersect_sphere();
  }
}

extern "C" __device__ void insertTopKQ(float key, unsigned int val)
{
  const unsigned int u0 = optixGetPayload_1();
  const unsigned int u1 = optixGetPayload_2();
  float* keys = reinterpret_cast<float*>( unpackPointer( u0, u1 ) );
  
  const unsigned int u2 = optixGetPayload_3();
  const unsigned int u3 = optixGetPayload_4();
  unsigned int* vals = reinterpret_cast<unsigned int*>( unpackPointer( u2, u3 ) );
  
  float max_key = uint_as_float(optixGetPayload_5());
  unsigned int max_idx = optixGetPayload_6();
  unsigned int _size = optixGetPayload_7();
  
  if (_size < K) {
    keys[_size] = key;
    vals[_size] = val;
  
    if (_size == 0 || key > max_key) {
      optixSetPayload_5( float_as_uint(key) ); //max_key = key;
      optixSetPayload_6( _size ); //max_idx = _size;
    }
    optixSetPayload_7( _size + 1 ); // _size++;
  }
  else if (key < max_key) {
    keys[max_idx] = key;
    vals[max_idx] = val;
  
    // can't directy update payload just yet; we will read max_key in the loop! if we do, then later when we read max_key, we need to use the get payload API. both seem to be using registers -- very similar speed.
    max_key = key;
    //optixSetPayload_5( float_as_uint(key) ); //max_key = key;
    for (unsigned int k = 0; k < K; ++k) {
      float cur_key = keys[k];
  
      //if (cur_key > uint_as_float(optixGetPayload_5())) {
      if (cur_key > max_key) {
        max_key = cur_key;
        max_idx = k;
        //optixSetPayload_5( float_as_uint(cur_key) ); //max_key = cur_key;
        //optixSetPayload_6( k ); //max_idx = k;
      }
    }
    optixSetPayload_5( float_as_uint(max_key) );
    optixSetPayload_6( max_idx );
  }
}

extern "C" __global__ void __intersection__sphere_knn()
{
  // The IS program will be called if the ray origin is within a primitive's
  // bbox (even if the actual intersections are beyond the tmin and tmax).

  bool isApprox = false;

  // if d_r2q_map is null and limit is 1, this is the initial run for sorting
  if (params.d_r2q_map == nullptr && params.limit == 1) isApprox = true;
  //bool isApprox = params.isApprox; // TODO: should use this in the long run

  unsigned int queryIdx = optixGetPayload_0();
  unsigned int primIdx = optixGetPrimitiveIndex();
  if (isApprox) {
    params.frame_buffer[queryIdx * params.limit] = primIdx;
    optixReportIntersection( 0, 0 );
  } else {
    const float3 center = params.points[primIdx];

    const float3  ray_orig = optixGetWorldRayOrigin();
    float3 O = ray_orig - center;
    float sqdist = dot(O, O);

    if ((sqdist > 0) && (sqdist < params.radius * params.radius)) {
      insertTopKQ(sqdist, primIdx);
    }
  }
}

extern "C" __global__ void __anyhit__terminateRay()
{
  optixTerminateRay();
}

