// Hxt - Copyright (C)
// 2016 - 2020 UCLouvain
//
// See the LICENSE.txt file for license information.
//
// Contributor(s):
//   Célestin Marot

#include "hxt_vertices.h"
#include "hxt_tetNodalSize.h"


HXTStatus hxtNodalSizesInit(HXTMesh* mesh, HXTNodalSizes* nodalSizes)
{
  HXT_CHECK(hxtAlignedMalloc(&nodalSizes->array,mesh->vertices.num*sizeof(double)));

  /*********************************************************************
   first step: compute the missing nodalSizes from triangles and lines *
   *********************************************************************/
  #pragma omp parallel for simd
  for (uint32_t i = 0; i<mesh->vertices.num; i++) {
    if(mesh->vertices.coord[4 * i + 3] <= 0.0) {
      mesh->vertices.coord[4 * i + 3] = 0.0; // we use that as a counter to do the average...
      nodalSizes->array[i] = 0.0;
    }
    else {
      nodalSizes->array[i] = DBL_MAX;
    }
  }

  for (uint32_t i = 0; i<mesh->triangles.num; i++){
    for (uint32_t j = 0; j<2; j++){
      for (uint32_t k = j+1; k<3; k++){
        uint32_t n1 = mesh->triangles.node[3*i+j];
        uint32_t n2 = mesh->triangles.node[3*i+k];

        if(nodalSizes->array[n1] == DBL_MAX &&
           nodalSizes->array[n2] == DBL_MAX) // nothing to do here
          continue;

        double *X1 = &mesh->vertices.coord[4 * n1];
        double *X2 = &mesh->vertices.coord[4 * n2];
        double l = sqrt ((X1[0]-X2[0])*(X1[0]-X2[0])+
                         (X1[1]-X2[1])*(X1[1]-X2[1])+
                         (X1[2]-X2[2])*(X1[2]-X2[2]));

        if(nodalSizes->array[n1] != DBL_MAX) {
          nodalSizes->array[n1]++;
          X1[3] += l;
        }

        if(nodalSizes->array[n2] != DBL_MAX) {
          nodalSizes->array[n2]++;
          X2[3] += l;
        }
      }
    }
  }

  for (uint32_t i = 0; i<mesh->lines.num; i++){
      uint32_t n1 = mesh->lines.node[2*i+0];
      uint32_t n2 = mesh->lines.node[2*i+1];

      if(nodalSizes->array[n1] == DBL_MAX &&
         nodalSizes->array[n2] == DBL_MAX) // nothing to do here
        continue;

      double *X1 = &mesh->vertices.coord[4 * n1];
      double *X2 = &mesh->vertices.coord[4 * n2];
      double l = sqrt ((X1[0]-X2[0])*(X1[0]-X2[0])+
                       (X1[1]-X2[1])*(X1[1]-X2[1])+
                       (X1[2]-X2[2])*(X1[2]-X2[2]));
      if(nodalSizes->array[n1] != DBL_MAX) {
        nodalSizes->array[n1]++;
        X1[3] += l;
      }

      if(nodalSizes->array[n2] != DBL_MAX) {
        nodalSizes->array[n2]++;
        X2[3] += l;
      }
  }

  #pragma omp parallel for simd
  for (uint32_t i=0; i<mesh->vertices.num; i++)
  {
    if(nodalSizes->array[i] == DBL_MAX)
      continue;
    mesh->vertices.coord[4 * i + 3] /= nodalSizes->array[i] * nodalSizes->factor;
  }

  /*********************************************************************
   second step: call the callback function for better values           *
   *********************************************************************/
  if(nodalSizes->callback != NULL) {
    HXT_CHECK( nodalSizes->callback(mesh->vertices.coord, mesh->vertices.num, nodalSizes->userData) );
  }

  #pragma omp parallel for simd
  for (uint32_t i=0; i<mesh->vertices.num; i++) {
    nodalSizes->array[i] = mesh->vertices.coord[4 * i + 3];
  }

  return HXT_STATUS_OK;
}


HXTStatus hxtNodalSizesDestroy(HXTNodalSizes* nodalSizes)
{
  HXT_CHECK( hxtAlignedFree(&nodalSizes->array) );
  return HXT_STATUS_OK;
}
