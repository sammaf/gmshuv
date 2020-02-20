#include <hxt_octree.h>

#include <math.h>
#include <iostream>

double ALPHA;

#define P8EST_QMAXLEVEL 8
#define P4EST_QMAXLEVEL 8

#ifdef HAVE_P4EST

int globCount = 0;

p4est_connectivity_t *p8est_connectivity_new_cube (HXTForestOptions *forestOptions);
  
static double hxtOctreeBulkSize(double x, double y, double z, double hBulk){
  return hBulk;
}

static HXTStatus hxtOctreeGetCenter(p4est_t *p4est, p4est_topidx_t which_tree, p4est_quadrant_t *q, double xyz[3])
{
  p4est_qcoord_t      half_length = P4EST_QUADRANT_LEN (q->level) / 2;
  p4est_qcoord_to_vertex (p4est->connectivity, which_tree,
                          q->x + half_length, q->y + half_length,
  #ifdef P4_TO_P8
                          q->z + half_length,
  #endif
                          xyz);

  return HXT_STATUS_OK;
}

static HXTStatus hxtOctreeGetBboxOctant(p4est_t *p4est, p4est_topidx_t which_tree, p4est_quadrant_t *q, double min[3], double max[3])
{
  p4est_qcoord_t      length = P4EST_QUADRANT_LEN (q->level);
  p4est_qcoord_to_vertex (p4est->connectivity, which_tree,
                          q->x, q->y,
  #ifdef P4_TO_P8
                          q->z,
  #endif
                          min);

  p4est_qcoord_to_vertex (p4est->connectivity, which_tree,
                          q->x + length, q->y + length,
  #ifdef P4_TO_P8
                          q->z + length,
  #endif
                          max);

  return HXT_STATUS_OK;
}

static HXTStatus hxtOctreeGetOctantSize(p4est_t *p4est, p4est_topidx_t which_tree, p4est_quadrant_t *q, double *h)
{
  double min[3], max[3];
  p4est_qcoord_t      length = P4EST_QUADRANT_LEN (q->level);
  p4est_qcoord_to_vertex (p4est->connectivity, which_tree,
                          q->x, q->y,
  #ifdef P4_TO_P8
                          q->z,
  #endif
                          min);

  p4est_qcoord_to_vertex (p4est->connectivity, which_tree,
                          q->x + length, q->y + length,
  #ifdef P4_TO_P8
                          q->z + length,
  #endif
                          max);
  
  // h designe la longueur d'un cote de l'octant
  // Normalement les trois côtés ont la même taille
  *h = fmax(max[0] - min[0],fmax(max[1] - min[1],max[2] - min[2]));

  return HXT_STATUS_OK;
}

static void hxtOctreeSetInitialSize(p4est_t* p4est, p4est_topidx_t which_tree, p4est_quadrant_t *q){

  HXTForestOptions  *forestOptions = (HXTForestOptions *) p4est->user_pointer;
  size_data_t       *data = (size_data_t *) q->p.user_data;

  double center[3];

  hxtOctreeGetCenter(p4est, which_tree, q, center);

  double (*size_fun)(double, double, double, double) = (double (*)(double, double, double, double)) forestOptions->sizeFunction;

  data->size = size_fun(center[0], center[1], center[2], forestOptions->hbulk);

  for(int i = 0; i < P4EST_DIM; ++i){
      data->ds[i] = 0.0;
  }

  data->d2s = 0;

  // Initialisation des tailles à partir des donnees du demi-quadrant;
  hxtOctreeGetOctantSize(p4est, which_tree, q, &(data->h));

  data->h_xL = data->h/2;
  data->h_xR = data->h/2;
  data->h_yD = data->h/2;
  data->h_yU = data->h/2;
  data->h_zB = data->h/2;
  data->h_zT = data->h/2;

  data->h_xavg = 0;
  data->h_yavg = 0;
  data->h_zavg = 0;

  data->refineFlag = 0;
  data->coarsenFlag = 0;
}

HXTStatus hxtForestOptionsCreate(HXTForestOptions **forestOptions){

  HXT_CHECK( hxtMalloc (forestOptions, sizeof(HXTForestOptions)) );
    if(*forestOptions == NULL) return HXT_ERROR(HXT_STATUS_OUT_OF_MEMORY);

  return HXT_STATUS_OK;
}

HXTStatus hxtForestOptionsDelete(HXTForestOptions **forestOptions){

  HXT_CHECK(hxtFree(forestOptions));

  return HXT_STATUS_OK;
}

HXTStatus hxtForestCreate(int argc, char **argv, HXTForest **forest, const char* filename, HXTForestOptions *forestOptions){

    HXT_CHECK( hxtMalloc (forest, sizeof(HXTForest)) );
    if(*forest == NULL) return HXT_ERROR(HXT_STATUS_OUT_OF_MEMORY);

    int mpiret;
    int balance;
    sc_MPI_Comm mpicomm;
    p4est_connectivity_t *connect;

    /* Initialize MPI; see sc_mpi.h.
     * If configure --enable-mpi is given these are true MPI calls.
     * Else these are dummy functions that simulate a single-processor run. */
    mpiret = sc_MPI_Init (&argc, &argv);
    SC_CHECK_MPI(mpiret);
    mpicomm = sc_MPI_COMM_WORLD;

    /* These functions are optional.  If called they store the MPI rank as a
     * static variable so subsequent global p4est log messages are only issued
     * from processor zero.  Here we turn off most of the logging; see sc.h. */
    sc_init(mpicomm, 1, 1, NULL, SC_LP_ESSENTIAL);
    p4est_init(NULL, SC_LP_PRODUCTION);

    /* Create a forest from the bounding box */
    connect = p8est_connectivity_new_cube(forestOptions);

    if(connect == NULL)return HXT_ERROR(HXT_STATUS_FILE_CANNOT_BE_OPENED);

    #ifdef P4EST_WITH_METIS
    //  Use metis (if p4est is compiled with the flag '--with-metis') to
    //  * reorder the connectivity for better parititioning of the forest
    //  * across processors. 
    p4est_connectivity_reorder(mpicomm, 0, conn, P4EST_CONNECT_FACE);
    #endif /* P4EST_WITH_METIS */

    /* Create a forest that is not refined; it consists of the root octant. */
    if(forestOptions->sizeFunction == NULL) forestOptions->sizeFunction = &hxtOctreeBulkSize;

    ALPHA = forestOptions->gradMax;

    (*forest)->p4est = p4est_new(mpicomm, connect, sizeof(size_data_t), hxtOctreeSetInitialSize, (void *)forestOptions);
    (*forest)->forestOptions = forestOptions;

    return HXT_STATUS_OK;
}

HXTStatus hxtForestDelete(HXTForest **forest){
    /* Destroy the p4est structure. */
    p4est_connectivity_destroy((*forest)->p4est->connectivity);
    p4est_destroy((*forest)->p4est);
    /* Verify that allocations internal to p4est and sc do not leak memory.
     * This should be called if sc_init () has been called earlier. */
    sc_finalize();
    /* This is standard MPI programs.  Without --enable-mpi, this is a dummy. */
    int mpiret = sc_MPI_Finalize();
    SC_CHECK_MPI(mpiret);

    HXT_CHECK(hxtFree(forest));

    return HXT_STATUS_OK;
}

static int refineToBulkSizeCallback(p4est_t *p4est, p4est_topidx_t which_tree, p4est_quadrant_t *q){
  HXTForestOptions *forestOptions = (HXTForestOptions *) p4est->user_pointer;
  size_data_t *data = (size_data_t *) q->p.user_data;
  return data->h > forestOptions->hbulk;
}

HXTStatus hxtOctreeRefineToBulkSize(HXTForest *forest){
  p4est_refine(forest->p4est, 1, refineToBulkSizeCallback, hxtOctreeSetInitialSize);
  return HXT_STATUS_OK;
}

HXTStatus hxtOctreeBalance(HXTForest *forest){
  p4est_balance(forest->p4est, P4EST_CONNECT_FACE, hxtOctreeSetInitialSize);
  return HXT_STATUS_OK;
}

static void hxtOctreeComputeGradientCenter(p4est_iter_face_info_t * info, void *user_data){

    p4est_iter_face_side_t *side[2];
    sc_array_t             *sides = &(info->sides);
    size_data_t            *size_data;
    size_data_t            *size_data_opp;
    double                  s_avg;
    double                  h;
    int                     which_face;
    int                     which_face_opp;

    // Indice de l'autre côté de la face (0 si 1 et 1 si 0)
    int                     iOpp; 

    side[0] = p4est_iter_fside_array_index_int (sides, 0);
    side[1] = p4est_iter_fside_array_index_int (sides, 1);

    if(sides->elem_count == 2){

        for(int i = 0; i < 2; i++) {
            // Indice dans side[] de la face opposée
            iOpp = 1 - i;

            which_face_opp = side[iOpp]->face; /* 0,1 == -+x, 2,3 == -+y, 4,5 == -+z */

            s_avg = 0;
            if (side[i]->is_hanging) {
                /* there are 2^(dim-1) (P4EST_HALF) subfaces */
                for(int j = 0; j < P4EST_HALF; j++) {
                    size_data = (size_data_t *) side[i]->is.hanging.quad[j]->p.user_data;
                    s_avg += size_data->size;
                }

                // Calcul de la valeur moyenne sur les P4EST_HALF quadrants courants
                s_avg /= P4EST_HALF;

                size_data_opp = (size_data_t *) side[iOpp]->is.full.quad->p.user_data;

                switch(which_face_opp){
                    case 0 :    size_data_opp->ds[0] -= 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_xL + size_data->h_xR); break;
                    case 1 :    size_data_opp->ds[0] += 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_xR + size_data->h_xL); break;
                    case 2 :    size_data_opp->ds[1] -= 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_yD + size_data->h_yU); break;
                    case 3 :    size_data_opp->ds[1] += 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_yU + size_data->h_yD); break;
                #ifdef P4_TO_P8
                    case 4 :    size_data_opp->ds[2] -= 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_zB + size_data->h_zT); break;
                    case 5 :    size_data_opp->ds[2] += 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_zT + size_data->h_zB); break;
                #endif
                    default :
                        std::cout<<"Valeur inattendue : "<<which_face_opp<<std::endl;
                }
            }
            else {

                size_data = (size_data_t *) side[i]->is.full.quad->p.user_data;
                s_avg = size_data->size;

                if(side[iOpp]->is_hanging){
                    // Full - Oppose hanging
                    for(int j = 0; j < P4EST_HALF; ++j){
                        size_data_opp = (size_data_t *) side[iOpp]->is.hanging.quad[j]->p.user_data;

                        switch(which_face_opp){
                            case 0 :    size_data_opp->ds[0] -= 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_xL + size_data->h_xR); break;
                            case 1 :    size_data_opp->ds[0] += 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_xR + size_data->h_xL); break;
                            case 2 :    size_data_opp->ds[1] -= 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_yD + size_data->h_yU); break;
                            case 3 :    size_data_opp->ds[1] += 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_yU + size_data->h_yD); break;
                        #ifdef P4_TO_P8
                            case 4 :    size_data_opp->ds[2] -= 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_zB + size_data->h_zT); break;
                            case 5 :    size_data_opp->ds[2] += 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_zT + size_data->h_zB); break;
                        #endif
                            default :
                                std::cout<<"Valeur inattendue : "<<which_face_opp<<std::endl;
                        }
                    }
                    
                }
                else{
                    // Full - Oppose full
                    size_data_opp = (size_data_t *) side[iOpp]->is.full.quad->p.user_data;

                    switch(which_face_opp){
                        case 0 :    size_data_opp->ds[0] -= 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_xL + size_data->h_xR); break;
                        case 1 :    size_data_opp->ds[0] += 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_xR + size_data->h_xL); break;
                        case 2 :    size_data_opp->ds[1] -= 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_yD + size_data->h_yU); break;
                        case 3 :    size_data_opp->ds[1] += 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_yU + size_data->h_yD); break;
                    #ifdef P4_TO_P8
                        case 4 :    size_data_opp->ds[2] -= 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_zB + size_data->h_zT); break;
                        case 5 :    size_data_opp->ds[2] += 0.5 * (s_avg - size_data_opp->size)/(size_data_opp->h_zT + size_data->h_zB); break;
                    #endif
                        default :
                            std::cout<<"Valeur inattendue : "<<which_face_opp<<std::endl;
                    }
                }
                  
            }
        }
    }
    else{
        size_data = (size_data_t *) side[0]->is.full.quad->p.user_data;

        which_face = side[0]->face;

        // double center[3];

        // get_center(info->p4est, side[0]->treeid, side[0]->is.full.quad, center);

        // h = (double) P4EST_QUADRANT_LEN (side[0]->is.full.quad->level) / (double) P4EST_ROOT_LEN /2;

        // double s_out;

        // switch(which_face){
        //     case 0 :
        //         s_out = myFunction(center[0] - h, center[1], center[2]);
        //         size_data->ds[0] -= s_out;
        //         break;
        //     case 1 :
        //         s_out = myFunction(center[0] + h, center[1], center[2]);
        //         size_data->ds[0] += s_out;
        //         break;
        //     case 2 : 
        //         s_out = myFunction(center[0], center[1] - h, center[2]);
        //         size_data->ds[1] -= s_out;
        //         break;
        //     case 3 : 
        //         s_out = myFunction(center[0], center[1] + h, center[2]);
        //         size_data->ds[1] += s_out;
        //         break;
        //     default :
        //         std::cout<<"Valeur inattendue : "<<which_face_opp<<std::endl;
        //         exit(-1);
        // }

        // Differences finies aux faces (decentrees) si le quadrant est a la frontiere
        // switch(which_face){
        //     case 0 :    size_data->ds[0] -= size_data->size; break;
        //     case 1 :    size_data->ds[0] += size_data->size; break;
        //     case 2 :    size_data->ds[1] -= size_data->size; break;
        //     case 3 :    size_data->ds[1] += size_data->size; break;
        // #ifdef P4_TO_P8
        //     case 4 :    size_data->ds[2] -= size_data->size; break;
        //     case 5 :    size_data->ds[2] += size_data->size; break;
        // #endif
        //     default :
        //         std::cout<<"Valeur inattendue : "<<which_face_opp<<std::endl;
        // }
    }
}

static void hxtOctreeComputeGradientCenterBoundary(p4est_iter_face_info_t * info, void *user_data){

    p4est_iter_face_side_t *side[2];
    sc_array_t             *sides = &(info->sides);
    size_data_t            *size_data;
    int                     which_dir;

    side[0] = p4est_iter_fside_array_index_int (sides, 0);
    side[1] = p4est_iter_fside_array_index_int (sides, 1);

    if(sides->elem_count == 1){
        size_data = (size_data_t *) side[0]->is.full.quad->p.user_data;
        which_dir = side[0]->face / 2;
        size_data->ds[which_dir] *= 2.0; // Sera divise par hMin dans laplacian_volume !
    }
}

static void hxtOctreeComputeSizeMin(p4est_iter_face_info_t * info, void *user_data){

    p4est_iter_face_side_t *side[2];
    sc_array_t             *sides = &(info->sides);
    size_data_t            *ghost_data = (size_data_t *) user_data;
    size_data_t            *size_data;
    size_data_t            *size_data_opp;
    p4est_quadrant_t       *quad;
    p4est_quadrant_t       *quadOpp;
    double                  h;
    int                     which_face;
    int                     which_face_opp;

    // Indice de l'autre côté de la face (0 si 1 et 1 si 0)
    int                     iOpp; 

    side[0] = p4est_iter_fside_array_index_int (sides, 0);
    side[1] = p4est_iter_fside_array_index_int (sides, 1);

    if(sides->elem_count == 2){

        for(int i = 0; i < 2; i++) {

            // Indice dans side[] de la face opposée
            iOpp = 1 - i;

            which_face_opp = side[iOpp]->face;     /* 0,1 == x, 2,3 == y, 4,5 == z */

            if (side[i]->is_hanging) {

                // Taille identique pour les deux quadrants hanging
                quad = side[i]->is.hanging.quad[0];
                // h = (double) P4EST_QUADRANT_LEN (quad->level) / (double) P4EST_ROOT_LEN /2;
                // hxtOctreeGetOctantSize(info->p4est, quad->p.which_tree, quad, &h);
                // h /= 2.0;
                size_data = (size_data_t *) quad->p.user_data;
                h = size_data->h;
                // Le quadrant oppose ne peut pas etre hanging, sinon j'imagine que 
                // la face est divisee en deux. Comme on est hanging, l'oppose est
                // presumement full.
                size_data_opp = (size_data_t *) side[iOpp]->is.full.quad->p.user_data;

                // Contribution pour la taille dans le quadrant oppose
                switch(which_face_opp){
                    case 0 :    size_data_opp->h_xL += h/2 ; break;
                    case 1 :    size_data_opp->h_xR += h/2 ; break;
                    case 2 :    size_data_opp->h_yD += h/2 ; break;
                    case 3 :    size_data_opp->h_yU += h/2 ; break;
                #ifdef P4_TO_P8
                    case 4 :    size_data_opp->h_zB += h/2 ; break;
                    case 5 :    size_data_opp->h_zT += h/2 ; break;
                #endif
                    default :
                        std::cout<<"Valeur inattendue : "<<which_face_opp<<std::endl;
                        exit(-1); // Brutal
                }
            }
            else {
                // Taille du quadrant courant
                quad = side[i]->is.full.quad;
                // h = (double) P4EST_QUADRANT_LEN (quad->level) / (double) P4EST_ROOT_LEN /2;
                // hxtOctreeGetOctantSize(info->p4est, quad->p.which_tree, quad, &h);
                // h /= 2.0;
                size_data = (size_data_t *) quad->p.user_data;
                h = size_data->h;

                // Contribution pour la taille dans le quadrant oppose
                if(side[iOpp]->is_hanging){
                    // Full - Oppose Hanging
                    for(int j = 0; j < P4EST_HALF; ++j){
                        size_data_opp = (size_data_t *) side[iOpp]->is.hanging.quad[j]->p.user_data;

                        switch(which_face_opp){
                            case 0 :    size_data_opp->h_xL += h/2 ; break;
                            case 1 :    size_data_opp->h_xR += h/2 ; break;
                            case 2 :    size_data_opp->h_yD += h/2 ; break;
                            case 3 :    size_data_opp->h_yU += h/2 ; break;
                        #ifdef P4_TO_P8
                            case 4 :    size_data_opp->h_zB += h/2 ; break;
                            case 5 :    size_data_opp->h_zT += h/2 ; break;
                        #endif
                            default :
                                std::cout<<"Valeur inattendue : "<<which_face_opp<<std::endl;
                                exit(-1); // Brutal
                        }
                    }
                }
                else{
                    // Full - Oppose Full
                    size_data_opp = (size_data_t *) side[iOpp]->is.full.quad->p.user_data;

                    switch(which_face_opp){
                        case 0 :    size_data_opp->h_xL += h/2 ; break;
                        case 1 :    size_data_opp->h_xR += h/2 ; break;
                        case 2 :    size_data_opp->h_yD += h/2 ; break;
                        case 3 :    size_data_opp->h_yU += h/2 ; break;
                    #ifdef P4_TO_P8
                        case 4 :    size_data_opp->h_zB += h/2 ; break;
                        case 5 :    size_data_opp->h_zT += h/2 ; break;
                    #endif
                        default :
                            std::cout<<"Valeur inattendue : "<<which_face_opp<<std::endl;
                            exit(-1); // Brutal
                    }  
                }  

            }
        }
    }
    else{
        // Frontière
        size_data = (size_data_t *) side[0]->is.full.quad->p.user_data;

        which_face = side[0]->face;

        // h = (double) P4EST_QUADRANT_LEN (side[0]->is.full.quad->level) / (double) P4EST_ROOT_LEN /2;
        // hxtOctreeGetOctantSize(info->p4est, quad->p.which_tree, quad, &h);
        // h /= 2.0;
        h = size_data->h;

        switch(which_face){
            case 0 :    size_data->h_xL += h/2; break;
            case 1 :    size_data->h_xR += h/2; break;
            case 2 :    size_data->h_yD += h/2; break;
            case 3 :    size_data->h_yU += h/2; break;
        #ifdef P4_TO_P8
            case 4 :    size_data->h_zB += h/2 ; break;
            case 5 :    size_data->h_zT += h/2 ; break;
        #endif
            default :
                std::cout<<"Valeur inattendue : "<<which_face_opp<<std::endl;
                exit(-1); // Brutal
        }
    }
}

static void hxtOctreeResetLaplacian(p4est_iter_volume_info_t * info, void *user_data){

    size_data_t *size_data = (size_data_t *) info->quad->p.user_data;
    // double h = (double) P4EST_QUADRANT_LEN (info->quad->level) / (double) P4EST_ROOT_LEN / 2;

    size_data->d2s = 0.0;

    for(int i = 0; i < P4EST_DIM; ++i){
        size_data->ds[i] = 0.0;
    }
    
    size_data->h_xL = size_data->h/2;
    size_data->h_xR = size_data->h/2;
    size_data->h_yU = size_data->h/2;
    size_data->h_yD = size_data->h/2;
#ifdef P4_TO_P8
    size_data->h_zB = size_data->h/2;
    size_data->h_zT = size_data->h/2;
#endif
    
    size_data->h_xavg = 0.0;
    size_data->h_yavg = 0.0;

    size_data->hMin = 0.0;
}

HXTStatus hxtOctreeComputeGradient(HXTForest *forest){

  // Remet à 0 les derivees, h et laplacien sur chaque quadrant
    p4est_iterate(forest->p4est, NULL, NULL, hxtOctreeResetLaplacian, NULL,
            #ifdef P4_TO_P8
                        NULL,
            #endif
                        NULL);

    // Calcule les tailles
    p4est_iterate(forest->p4est, NULL, NULL, NULL, hxtOctreeComputeSizeMin,
        #ifdef P4_TO_P8
                    NULL,
        #endif
                    NULL);

    // Calcule le gradient au centre de chaque quadrant
    p4est_iterate(forest->p4est, NULL, NULL, NULL, hxtOctreeComputeGradientCenter,
        #ifdef P4_TO_P8
                    NULL,
        #endif
                    NULL);

    // // Finalise le calcul gradient sur les quadrants aux bords
    // p4est_iterate(forest->p4est, NULL, NULL, NULL, hxtOctreeComputeGradientCenterBoundary,
    //     #ifdef P4_TO_P8
    //                 NULL,
    //     #endif
    //                 NULL);
}

void min_size (p4est_iter_volume_info_t * info, void *user_data)
{
  p4est_quadrant_t   *q = info->quad;
  size_data_t        *data = (size_data_t *) q->p.user_data;
  double              size_min = *((double *) user_data);

  size_min = SC_MIN (data->size, size_min);

  *((double *) user_data) = size_min;
}

void max_size (p4est_iter_volume_info_t * info, void *user_data)
{
  p4est_quadrant_t   *q = info->quad;
  size_data_t        *data = (size_data_t *) q->p.user_data;
  double              size_max = *((double *) user_data);

  size_max = SC_MAX (data->size, size_max);

  *((double *) user_data) = size_max;
}

void max_dsdx (p4est_iter_volume_info_t * info, void *user_data)
{
  p4est_quadrant_t   *q = info->quad;
  size_data_t        *data = (size_data_t *) q->p.user_data;
  double              umax = *((double *) user_data);

  umax = SC_MAX (fabs(data->ds[0]), umax);

  *((double *) user_data) = umax;
}

void max_dsdy (p4est_iter_volume_info_t * info, void *user_data)
{
  p4est_quadrant_t   *q = info->quad;
  size_data_t        *data = (size_data_t *) q->p.user_data;
  double              umax = *((double *) user_data);

  umax = SC_MAX (fabs(data->ds[1]), umax);

  *((double *) user_data) = umax;
}

// #ifdef P4_TO_P8
void max_dsdz (p4est_iter_volume_info_t * info, void *user_data)
{
  p4est_quadrant_t   *q = info->quad;
  size_data_t        *data = (size_data_t *) q->p.user_data;
  double              umax = *((double *) user_data);

  umax = SC_MAX (fabs(data->ds[2]), umax);

  *((double *) user_data) = umax;
}
// #endif

HXTStatus hxtOctreeComputeMinimumSize(HXTForest *forest, double *size_min){
  p4est_iterate (forest->p4est, NULL, (void *) size_min, min_size, NULL,
            #ifdef P4_TO_P8
                       NULL,
            #endif
                       NULL);
  return HXT_STATUS_OK;
}

HXTStatus hxtOctreeComputeMaximumSize(HXTForest *forest, double *size_max){
  p4est_iterate (forest->p4est, NULL, (void *) size_max, max_size, NULL,
            #ifdef P4_TO_P8
                       NULL,
            #endif
                       NULL);
  return HXT_STATUS_OK;
}

HXTStatus hxtOctreeComputeMaxGradientX(HXTForest *forest, double *dsdx_max){
	p4est_iterate (forest->p4est, NULL, (void *) dsdx_max, max_dsdx, NULL,
            #ifdef P4_TO_P8
                       NULL,
            #endif
                       NULL);
	return HXT_STATUS_OK;
}

HXTStatus hxtOctreeComputeMaxGradientY(HXTForest *forest, double *dsdy_max){
	p4est_iterate (forest->p4est, NULL, (void *) dsdy_max, max_dsdy, NULL,
            #ifdef P4_TO_P8
                       NULL,
            #endif
                       NULL);
	return HXT_STATUS_OK;
}

HXTStatus hxtOctreeComputeMaxGradientZ(HXTForest *forest, double *dsdz_max){
	p4est_iterate (forest->p4est, NULL, (void *) dsdz_max, max_dsdz, NULL,
            #ifdef P4_TO_P8
                       NULL,
            #endif
                       NULL);
	return HXT_STATUS_OK;
}

void set_max_gradient_faces(p4est_iter_face_info_t * info, void *user_data){
    p4est_iter_face_side_t *side[2];
    sc_array_t             *sides = &(info->sides);
    size_data_t            *ghost_data = (size_data_t *) user_data;
    size_data_t            *size_data;
    size_data_t            *size_data_opp1;
    size_data_t            *size_data_opp2;
    p4est_quadrant_t       *quad;
    double                  s_avg;
    int                     which_dir;
    int                     which_face;
    int                     which_face_opp;
    int                     iOpp;

    side[0] = p4est_iter_fside_array_index_int (sides, 0);
    side[1] = p4est_iter_fside_array_index_int (sides, 1);

    if(sides->elem_count==2){

      for(int i = 0; i < 2; ++i){

        iOpp = 1 - i;
        which_dir = side[i]->face / 2; // Direction x (0), y (1) ou z(2)
        which_face_opp = side[iOpp]->face;

        if(side[i]->is_hanging){

          size_data_opp1 = (size_data_t *) side[iOpp]->is.full.quad->p.user_data;

          for(int j = 0; j < P4EST_HALF; ++j){

            size_data = (size_data_t *) side[i]->is.hanging.quad[j]->p.user_data;

            if(fabs(size_data->ds[which_dir]) > ALPHA-1){

              if(size_data->size > size_data_opp1->size){
                  // size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * size_data_opp1->hMin);
                switch(which_face_opp){
                  case 0 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_xL + size_data->h_xR)); break;
                  case 1 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_xR + size_data->h_xL)); break;
                  case 2 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_yD + size_data->h_yU)); break;
                  case 3 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_yU + size_data->h_yD)); break;
                  case 4 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_zB + size_data->h_zT)); break;
                  case 5 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_zT + size_data->h_zB)); break;
                }
              }else{
                  // size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * size_data->hMin);
                switch(which_face_opp){
                  case 0 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_xL + size_data->h_xR)); break;
                  case 1 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_xR + size_data->h_xL)); break;
                  case 2 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_yD + size_data->h_yU)); break;
                  case 3 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_yU + size_data->h_yD)); break;
                  case 4 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_zB + size_data->h_zT)); break;
                  case 5 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_zT + size_data->h_zB)); break;
                }
              }
            } // if ds > alpha-1
          } // for j hanging
        } // if hanging
        else{

          size_data = (size_data_t *) side[i]->is.full.quad->p.user_data;

          if(fabs(size_data->ds[which_dir]) > ALPHA-1){
            if(side[iOpp]->is_hanging){
                // Full - Oppose hanging
              for(int j = 0; j < P4EST_HALF; ++j){
                size_data_opp1 = (size_data_t *) side[iOpp]->is.hanging.quad[j]->p.user_data;
                // size_data_opp2 = (size_data_t *) side[iOpp]->is.hanging.quad[1]->p.user_data;

                // if(size_data->size > fmin(size_data_opp1->size, size_data_opp2->size) ){
                if(size_data->size > size_data_opp1->size){
                    // size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * size_data_opp1->hMin);
                    // size_data->size = fmin(size_data->size, size_data_opp2->size + (ALPHA-1) * size_data_opp2->hMin);
                  switch(which_face_opp){
                    case 0 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_xL + size_data->h_xR)); break;
                    case 1 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_xR + size_data->h_xL)); break;
                    case 2 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_yD + size_data->h_yU)); break;
                    case 3 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_yU + size_data->h_yD)); break;
                    case 4 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_zB + size_data->h_zT)); break;
                    case 5 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_zT + size_data->h_zB)); break;
                  }
                }
                else{
                    // size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * size_data->hMin);
                    // size_data_opp2->size = fmin(size_data_opp2->size, size_data->size + (ALPHA-1) * size_data->hMin);
                  switch(which_face_opp){
                    case 0 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_xL + size_data->h_xR)); break;
                    case 1 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_xR + size_data->h_xL)); break;
                    case 2 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_yD + size_data->h_yU)); break;
                    case 3 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_yU + size_data->h_yD)); break;
                    case 4 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_zB + size_data->h_zT)); break;
                    case 5 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_zT + size_data->h_zB)); break;
                  }
                }
              }
            }
            else{
                // Full - Oppose full
                size_data_opp1 = (size_data_t *) side[iOpp]->is.full.quad->p.user_data;

                if(size_data->size > size_data_opp1->size){
                    // size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * size_data_opp1->hMin);
                  switch(which_face_opp){
                    case 0 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_xL + size_data->h_xR)); break;
                    case 1 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_xR + size_data->h_xL)); break;
                    case 2 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_yD + size_data->h_yU)); break;
                    case 3 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_yU + size_data->h_yD)); break;
                    case 4 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_zB + size_data->h_zT)); break;
                    case 5 : size_data->size = fmin(size_data->size, size_data_opp1->size + (ALPHA-1) * (size_data_opp1->h_zT + size_data->h_zB)); break;
                  }
                }
                else{
                    // size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * size_data->hMin);
                  switch(which_face_opp){
                    case 0 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_xL + size_data->h_xR)); break;
                    case 1 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_xR + size_data->h_xL)); break;
                    case 2 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_yD + size_data->h_yU)); break;
                    case 3 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_yU + size_data->h_yD)); break;
                    case 4 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_zB + size_data->h_zT)); break;
                    case 5 : size_data_opp1->size = fmin(size_data_opp1->size, size_data->size + (ALPHA-1) * (size_data_opp1->h_zT + size_data->h_zB)); break;
                  }
                }
            }  
          } // if gradient trop grand
        } // else
      }
    }
    else{
        // size_data = (size_data_t *) side[0]->is.full.quad->p.user_data;

        // which_dir = side[0]->face / 2;

        // if(fabs(size_data->ds[which_dir]) > ALPHA-1){
        //     size_data->ds[which_dir] = fmin(size_data->ds[which_dir], ALPHA-1);
        //     size_data->ds[which_dir] = nan("");

        // }
    }
}

HXTStatus hxtOctreeSetMaxGradient(HXTForest *forest){
	p4est_iterate (forest->p4est, NULL, NULL, NULL, set_max_gradient_faces,
            #ifdef P4_TO_P8
                       NULL,
            #endif
                       NULL);
	return HXT_STATUS_OK;
}

static void fill_size_vec(p4est_iter_volume_info_t * info, void *user_data){
    sc_array_t         *size_vec = (sc_array_t *) user_data;      /* we passed the array of values to fill as the user_data in the call to p4est_iterate */
    p4est_t            *p4est = info->p4est;
    p4est_quadrant_t   *q = info->quad;
    p4est_topidx_t      which_tree = info->treeid;
    p4est_locidx_t      local_id = info->quadid;  /* this is the index of q *within its tree's numbering*.  We want to convert it its index for all the quadrants on this process, which we do below */
    p4est_tree_t       *tree;
    size_data_t        *data = (size_data_t *) q->p.user_data;
    p4est_locidx_t      arrayoffset;
    double              this_size;
    double             *this_size_ptr;

    tree = p4est_tree_array_index(p4est->trees, which_tree);
    local_id += tree->quadrants_offset;   /* now the id is relative to the MPI process */
    arrayoffset = local_id;      /* Chaque quadrant a une donne pour l'offset*/

    // On copie la valeur de la taille sur la cellule dans size_vec, en tenant compte de l'offset
    this_size = data->size;
    this_size_ptr = (double *) sc_array_index(size_vec, arrayoffset);
    this_size_ptr[0] = this_size;
}

static void fill_dsdx_vec(p4est_iter_volume_info_t * info, void *user_data){
    sc_array_t         *dsdx_vec = (sc_array_t *) user_data;      /* we passed the array of values to fill as the user_data in the call to p4est_iterate */
    p4est_t            *p4est = info->p4est;
    p4est_quadrant_t   *q = info->quad;
    p4est_topidx_t      which_tree = info->treeid;
    p4est_locidx_t      local_id = info->quadid;  /* this is the index of q *within its tree's numbering*.  We want to convert it its index for all the quadrants on this process, which we do below */
    p4est_tree_t       *tree;
    size_data_t        *data = (size_data_t *) q->p.user_data;
    p4est_locidx_t      arrayoffset;
    double             *this_dsdx_ptr;

    tree = p4est_tree_array_index(p4est->trees, which_tree);
    local_id += tree->quadrants_offset;   /* now the id is relative to the MPI process */
    arrayoffset = local_id;      /* Chaque quadrant deux valeurs pour la derivee (offset dans le vecteur)*/

    this_dsdx_ptr = (double *) sc_array_index(dsdx_vec, arrayoffset);
    this_dsdx_ptr[0] = data->ds[0];
    // std::cout<<data->ds[0]<<"  -  "<<data->ds[1]<<std::endl;
}

static void fill_dsdy_vec(p4est_iter_volume_info_t * info, void *user_data){
    sc_array_t         *dsdy_vec = (sc_array_t *) user_data;      /* we passed the array of values to fill as the user_data in the call to p4est_iterate */
    p4est_t            *p4est = info->p4est;
    p4est_quadrant_t   *q = info->quad;
    p4est_topidx_t      which_tree = info->treeid;
    p4est_locidx_t      local_id = info->quadid;  /* this is the index of q *within its tree's numbering*.  We want to convert it its index for all the quadrants on this process, which we do below */
    p4est_tree_t       *tree;
    size_data_t        *data = (size_data_t *) q->p.user_data;
    p4est_locidx_t      arrayoffset;
    double             *this_dsdy_ptr;

    tree = p4est_tree_array_index(p4est->trees, which_tree);
    local_id += tree->quadrants_offset;   /* now the id is relative to the MPI process */
    arrayoffset = local_id;      /* Chaque quadrant deux valeurs pour la derivee (offset dans le vecteur)*/

    this_dsdy_ptr = (double *) sc_array_index(dsdy_vec, arrayoffset);
    this_dsdy_ptr[0] = data->ds[1];
}

#ifdef P4_TO_P8
static void fill_dsdz_vec(p4est_iter_volume_info_t * info, void *user_data){
    sc_array_t         *dsdz_vec = (sc_array_t *) user_data;      /* we passed the array of values to fill as the user_data in the call to p4est_iterate */
    p4est_t            *p4est = info->p4est;
    p4est_quadrant_t   *q = info->quad;
    p4est_topidx_t      which_tree = info->treeid;
    p4est_locidx_t      local_id = info->quadid;  /* this is the index of q *within its tree's numbering*.  We want to convert it its index for all the quadrants on this process, which we do below */
    p4est_tree_t       *tree;
    size_data_t        *data = (size_data_t *) q->p.user_data;
    p4est_locidx_t      arrayoffset;
    double             *this_dsdz_ptr;

    tree = p4est_tree_array_index(p4est->trees, which_tree);
    local_id += tree->quadrants_offset;   /* now the id is relative to the MPI process */
    arrayoffset = local_id;      /* Chaque quadrant deux valeurs pour la derivee (offset dans le vecteur)*/

    this_dsdz_ptr = (double *) sc_array_index(dsdz_vec, arrayoffset);
    this_dsdz_ptr[0] = data->ds[2];
}
#endif

static void fill_d2s_vec(p4est_iter_volume_info_t * info, void *user_data){
    sc_array_t         *d2s_vec = (sc_array_t *) user_data;      /* we passed the array of values to fill as the user_data in the call to p4est_iterate */
    p4est_t            *p4est = info->p4est;
    p4est_quadrant_t   *q = info->quad;
    p4est_topidx_t      which_tree = info->treeid;
    p4est_locidx_t      local_id = info->quadid;  /* this is the index of q *within its tree's numbering*.  We want to convert it its index for all the quadrants on this process, which we do below */
    p4est_tree_t       *tree;
    size_data_t        *data = (size_data_t *) q->p.user_data;
    p4est_locidx_t      arrayoffset;
    double             *this_d2s_ptr;

    tree = p4est_tree_array_index(p4est->trees, which_tree);
    local_id += tree->quadrants_offset;   /* now the id is relative to the MPI process */
    arrayoffset = local_id;      /* Chaque quadrant deux valeurs pour la derivee (offset dans le vecteur)*/

    this_d2s_ptr = (double *) sc_array_index(d2s_vec, arrayoffset);
    this_d2s_ptr[0] = data->d2s;
    // std::cout<<data->d2s<<std::endl;
}

static void fill_refineFlag(p4est_iter_volume_info_t * info, void *user_data){
    sc_array_t         *refineFlag = (sc_array_t *) user_data;      /* we passed the array of values to fill as the user_data in the call to p4est_iterate */
    p4est_t            *p4est = info->p4est;
    p4est_quadrant_t   *q = info->quad;
    p4est_topidx_t      which_tree = info->treeid;
    p4est_locidx_t      local_id = info->quadid;  /* this is the index of q *within its tree's numbering*.  We want to convert it its index for all the quadrants on this process, which we do below */
    p4est_tree_t       *tree;
    size_data_t        *data = (size_data_t *) q->p.user_data;
    p4est_locidx_t      arrayoffset;
    double             *this_h_ptr;

    tree = p4est_tree_array_index(p4est->trees, which_tree);
    local_id += tree->quadrants_offset;   /* now the id is relative to the MPI process */
    arrayoffset = local_id;      /* Chaque quadrant deux valeurs pour la derivee (offset dans le vecteur)*/

    this_h_ptr = (double *) sc_array_index(refineFlag, arrayoffset);
    this_h_ptr[0] = data->refineFlag;
    // std::cout<<data->d2s<<std::endl;
}

static void write_4est_to_vtk(p4est_t *p4est, const char *filename){
    p4est_vtk_write_file(p4est, NULL, filename);
}

static void write_size_to_vtk(p4est_t *p4est, const char *filename){

    p4est_locidx_t numquads = p4est->local_num_quadrants;

    /* Chaque quadrant a une place a allouer dans size_vec (info par cellule) */
    sc_array_t *size_vec = sc_array_new_size (sizeof (double), numquads);

    p4est_iterate (p4est, NULL,   /* we don't need any ghost quadrants for this loop */
                 (void *) size_vec,     /* user_data : pass in size_vec so that we can fill it */
                 fill_size_vec,    /* callback function that interpolates from the cell center to the cell corners, defined above */
                 NULL,          /* there is no callback for the faces between quadrants */
        #ifdef P4_TO_P8
                 NULL,          /* there is no callback for the edges between quadrants */
        #endif
                 NULL);         /* there is no callback for the corners between quadrants */

    /* create VTK output context and set its parameters */
    p4est_vtk_context_t *context = p4est_vtk_context_new(p4est, filename);
    p4est_vtk_context_set_scale (context, 1.0);  /* quadrant at almost full scale */

    /* begin writing the output files */
    context = p4est_vtk_write_header (context);
    SC_CHECK_ABORT (context != NULL, P4EST_STRING "_vtk: Error writing vtk header");

    /* do not write the tree id's of each quadrant
     * (there is only one tree in this example) */
    context = p4est_vtk_write_cell_dataf(context, 0, 1,  /* do write the refinement level of each quadrant */
                                        1,      /* do write the mpi process id of each quadrant */
                                        0,      /* do not wrap the mpi rank (if this were > 0, the modulus of the rank relative to this number would be written instead of the rank) */
                                        1,      /* there is no custom cell scalar data. */
                                        0,      /* there is no custom cell vector data. */
                                        "Size",
                                        size_vec,
                                        context);       /* mark the end of the variable cell data. */
    SC_CHECK_ABORT (context != NULL, P4EST_STRING "_vtk: Error writing cell data");

    const int retval = p4est_vtk_write_footer (context);
    SC_CHECK_ABORT (!retval, P4EST_STRING "_vtk: Error writing footer");

    sc_array_destroy (size_vec);
}

void write_ds_to_vtk(p4est_t *p4est, const char *filename){

    p4est_locidx_t numquads = p4est->local_num_quadrants;

    /* Chaque quadrant a une place a allouer dans size_vec (info par cellule) */
    sc_array_t *s_vec = sc_array_new_size (sizeof (double), numquads);
    sc_array_t *dsdx_vec = sc_array_new_size (sizeof (double), numquads);
    sc_array_t *dsdy_vec = sc_array_new_size (sizeof (double), numquads);
#ifdef P4_TO_P8
    sc_array_t *dsdz_vec = sc_array_new_size (sizeof (double), numquads);
#endif
    sc_array_t *d2s_vec = sc_array_new_size (sizeof (double), numquads);
    sc_array_t *refineFlag = sc_array_new_size (sizeof (double), numquads);

    p4est_iterate (p4est, NULL,   /* we don't need any ghost quadrants for this loop */
                 (void *) s_vec,     /* user_data : pass in size_vec so that we can fill it */
                 fill_size_vec,    /* callback function that interpolates from the cell center to the cell corners, defined above */
                 NULL,          /* there is no callback for the faces between quadrants */
        #ifdef P4_TO_P8
                 NULL,          /* there is no callback for the edges between quadrants */
        #endif
                 NULL);         /* there is no callback for the corners between quadrants */

    p4est_iterate (p4est, NULL,   /* we don't need any ghost quadrants for this loop */
                 (void *) dsdx_vec,     /* user_data : pass in size_vec so that we can fill it */
                 fill_dsdx_vec,    /* callback function that interpolates from the cell center to the cell corners, defined above */
                 NULL,          /* there is no callback for the faces between quadrants */
        #ifdef P4_TO_P8
                 NULL,          /* there is no callback for the edges between quadrants */
        #endif
                 NULL);         /* there is no callback for the corners between quadrants */

    p4est_iterate (p4est, NULL,   /* we don't need any ghost quadrants for this loop */
                 (void *) dsdy_vec,     /* user_data : pass in size_vec so that we can fill it */
                 fill_dsdy_vec,    /* callback function that interpolates from the cell center to the cell corners, defined above */
                 NULL,          /* there is no callback for the faces between quadrants */
        #ifdef P4_TO_P8
                 NULL,          /* there is no callback for the edges between quadrants */
        #endif
                 NULL);         /* there is no callback for the corners between quadrants */

#ifdef P4_TO_P8
    p4est_iterate (p4est, NULL,   /* we don't need any ghost quadrants for this loop */
                 (void *) dsdz_vec,     /* user_data : pass in size_vec so that we can fill it */
                 fill_dsdz_vec,    /* callback function that interpolates from the cell center to the cell corners, defined above */
                 NULL,          /* there is no callback for the faces between quadrants */
                 NULL,          /* there is no callback for the edges between quadrants */
                 NULL);         /* there is no callback for the corners between quadrants */
#endif

    p4est_iterate (p4est, NULL,   /* we don't need any ghost quadrants for this loop */
                 (void *) d2s_vec,     /* user_data : pass in size_vec so that we can fill it */
                 fill_d2s_vec,    /* callback function that interpolates from the cell center to the cell corners, defined above */
                 NULL,          /* there is no callback for the faces between quadrants */
        #ifdef P4_TO_P8
                 NULL,          /* there is no callback for the edges between quadrants */
        #endif
                 NULL);         /* there is no callback for the corners between quadrants */

    p4est_iterate (p4est, NULL,   /* we don't need any ghost quadrants for this loop */
                 (void *) refineFlag,     /* user_data : pass in size_vec so that we can fill it */
                 fill_refineFlag,    /* callback function that interpolates from the cell center to the cell corners, defined above */
                 NULL,          /* there is no callback for the faces between quadrants */
        #ifdef P4_TO_P8
                 NULL,          /* there is no callback for the edges between quadrants */
        #endif
                 NULL);         /* there is no callback for the corners between quadrants */

    /* create VTK output context and set its parameters */
    p4est_vtk_context_t *context = p4est_vtk_context_new(p4est, filename);
    p4est_vtk_context_set_scale (context, 1.0);  /* quadrant at almost full scale */

    /* begin writing the output files */
    context = p4est_vtk_write_header (context);
    SC_CHECK_ABORT (context != NULL, P4EST_STRING "_vtk: Error writing vtk header");

#ifdef P4_TO_P8
    int nScalarFields = 6;
#else
    int nScalarFields = 5;
#endif

    /* do not write the tree id's of each quadrant
     * (there is only one tree in this example) */
    context = p4est_vtk_write_cell_dataf(context, 0, 1,  /* do write the refinement level of each quadrant */
                                        1,      /* do write the mpi process id of each quadrant */
                                        0,      /* do not wrap the mpi rank (if this were > 0, the modulus of the rank relative to this number would be written instead of the rank) */
                                        nScalarFields,      /* there is no custom cell scalar data. */
                                        0,
                                        "Size",
                                        s_vec,
                                        "dsdx",
                                        dsdx_vec,
                                        "dsdy",
                                        dsdy_vec,
                                    #ifdef P4_TO_P8
                                        "dsdz",
                                        dsdz_vec,
                                    #endif
                                        "d2s",
                                        d2s_vec,
                                        "refineFlag",
                                        refineFlag,
                                        context);       /* mark the end of the variable cell data. */
    SC_CHECK_ABORT (context != NULL, P4EST_STRING "_vtk: Error writing cell data");

    const int retval = p4est_vtk_write_footer (context);
    SC_CHECK_ABORT (!retval, P4EST_STRING "_vtk: Error writing footer");

    sc_array_destroy(s_vec);
    sc_array_destroy(dsdx_vec);
    sc_array_destroy(dsdy_vec);
#ifdef P4_TO_P8
    sc_array_destroy(dsdz_vec);
#endif
    sc_array_destroy(d2s_vec);
    sc_array_destroy(refineFlag);
}

int finalizeP4est(p4est_t *p4est, p4est_connectivity_t *connect)
{
    int mpiret;
    /* Destroy the p4est and the connectivity structure. */
    p4est_destroy(p4est);
    p4est_connectivity_destroy(connect);

    /* Verify that allocations internal to p4est and sc do not leak memory.
     * This should be called if sc_init () has been called earlier. */
    sc_finalize();

    /* This is standard MPI programs.  Without --enable-mpi, this is a dummy. */
    mpiret = sc_MPI_Finalize();
    SC_CHECK_MPI(mpiret);
    return mpiret;
}

inline static bool isPoint(double x, double y, double z, size_point_t *p){
  return (fabs(p->x - x) < 1e-12 && fabs(p->y - y) < 1e-12 && fabs(p->z - z) < 1e-12);
}

static int hxtOctreeSearchCallback(p4est_t * p4est, p4est_topidx_t which_tree, p4est_quadrant_t * q, p4est_locidx_t local_num, void *point){

    bool in_box;
    int is_match;
    int is_leaf = local_num >= 0;
    
    size_data_t  *data = (size_data_t *) q->p.user_data;
    size_point_t *p    = (size_point_t *) point;

    // We have to recompute the cell dimension h for the root (non leaves) octants 
    // because it seems to be undefined. Otherwise it's contained in q->p.user_data.
    double h;
    if(!is_leaf) hxtOctreeGetOctantSize(p4est, which_tree, q, &h);
    else h = data->h;
    

    double center[3];
    hxtOctreeGetCenter(p4est, which_tree, q, center);

    double epsilon = 1e-13;
    in_box  = (p->x < center[0] + h/2. + epsilon) && (p->x > center[0] - h/2. - epsilon);
    in_box &= (p->y < center[1] + h/2. + epsilon) && (p->y > center[1] - h/2. - epsilon);
#ifdef P4_TO_P8
    in_box &= (p->z < center[2] + h/2. + epsilon) && (p->z > center[2] - h/2. - epsilon);
#endif

    // A point can be on the exact boundary of two cells, hence we take the min.
    if(in_box && is_leaf){
        p->size = fmin(p->size, data->size);
        p->isFound = true;
    }

    return in_box;
}

/* Search for a single point in the tree structure and returns its size.
   See hxtOctreeSearch for the detailed comments. */
HXTStatus hxtOctreeSearchOne(HXTForest *forest, double x, double y, double z, double *size){
  
  sc_array_t *points = sc_array_new_size(sizeof(size_point_t), 1);

  size_point_t *p = (size_point_t *) sc_array_index(points, 0);
  p->x = x;
  p->y = y;
  p->z = z;
  p->size = 1.0e22;
  p->isFound = false;
  
  p4est_search(forest->p4est, NULL, hxtOctreeSearchCallback, points);

  if(!p->isFound) printf("Point (%f,%f,%f) n'a pas été trouvé dans l'octree 8-|\n", x,y,z);
  *size = p->size;

  sc_array_destroy(points);

  return HXT_STATUS_OK;
}

static int hxtOctreeReplaceCallback(p4est_t * p4est, p4est_topidx_t which_tree, p4est_quadrant_t * q, p4est_locidx_t local_num, void *point){

    bool in_box;
    int is_match;
    int is_leaf = local_num >= 0;
    
    size_data_t  *data = (size_data_t *) q->p.user_data;
    size_point_t *p    = (size_point_t *) point;

    // We have to recompute the cell dimension h for the root (non leaves) octants 
    // because it seems to be undefined. Otherwise it's contained in q->p.user_data.
    double h;
    if(!is_leaf) hxtOctreeGetOctantSize(p4est, which_tree, q, &h);
    else h = data->h;
    

    double center[3];
    hxtOctreeGetCenter(p4est, which_tree, q, center);

    double epsilon = 1e-13;
    in_box  = (p->x < center[0] + h/2 + epsilon) && (p->x > center[0] - h/2 - epsilon);
    in_box &= (p->y < center[1] + h/2 + epsilon) && (p->y > center[1] - h/2 - epsilon);
#ifdef P4_TO_P8
    in_box &= (p->z < center[2] + h/2 + epsilon) && (p->z > center[2] - h/2 - epsilon);
#endif

    if(in_box && is_leaf){
        data->size = fmin(data->size, p->size);
        data->refineFlag = p->surfaceFlag;
        // printf("Taille remplacée dans l'octree à la position %f - %f - %f \n",p->x,p->y,p->z);
        // printf("dans le quadrant centré en %f - %f - %f de côté %f \n", center[0], center[1], center[2], h);
    }

    return in_box;
}

HXTStatus hxtOctreeSearch(HXTForest *forest, std::vector<double> *x, std::vector<double> *y, std::vector<double> *z, std::vector<double> *size){
  
  // Array of size_point_t to search in the tree
  sc_array_t *points = sc_array_new_size(sizeof(size_point_t), (*x).size());
  size_point_t *p;

  for(int i = 0; i < (*x).size(); ++i){
    p = (size_point_t *) sc_array_index(points, i);
    p->x = (*x)[i];
    p->y = (*y)[i];
    p->z = (*z)[i];
    p->size = -1.0;
  }
  
  // Search on all cells
  p4est_search(forest->p4est, NULL, hxtOctreeSearchCallback, points);

  // Get the sizes
  for(int i = 0; i < (*x).size(); ++i){ 
    p = (size_point_t *) sc_array_index(points, i);
    (*size)[i] = p->size;
  }

  // Clean up
  sc_array_destroy(points);

  return HXT_STATUS_OK;
}

static bool rtreeCallback(uint64_t id, void *ctx) {
  std::vector<uint64_t>* vec = reinterpret_cast< std::vector<uint64_t>* >(ctx);
  vec->push_back(id);
  return true;
}

static void hxtOctreeAssignSizeAfterRefinement(p4est_iter_volume_info_t * info, void *user_data){

  p4est_t            *p4est = info->p4est;
  p4est_quadrant_t   *q = info->quad;
  p4est_topidx_t      which_tree = info->treeid;
  size_data_t        *data = (size_data_t *) q->p.user_data;
  HXTForestOptions   *forestOptions = (HXTForestOptions *) user_data;

  double min[3], max[3];
  hxtOctreeGetBboxOctant(p4est, which_tree, q, min, max);

  std::vector<uint64_t> candidates;  
  forestOptions->triRTree->Search(min, max, rtreeCallback, &candidates);

  if(!candidates.empty()){
    // printf("candidates.size() = %d\n", candidates.size());
    double kmax = -1.0e22;
    for(std::vector<uint64_t>::iterator tri = candidates.begin(); tri != candidates.end(); ++tri){
      for(int i = 0; i < 3; ++i){
          int node = forestOptions->mesh->triangles.node[(size_t) 3*(*tri)+i];

          double *v1 = forestOptions->nodalCurvature + 6*node;
          double *v2 = forestOptions->nodalCurvature + 6*node + 3;

          double k1, k2;
          hxtNorm2V3(v1, &k1);
          hxtNorm2V3(v2, &k2);

          kmax = fmax(kmax,fmax(k1,k2));
          // if(kmax > 100) printf("%f\n", kmax);
        }
    }
    data->size = fmax(forestOptions->hmin, fmin(forestOptions->hmax, 2*M_PI/(forestOptions->nodePerTwoPi * kmax)));
    // data->size = kmax;
    // data->size = 1.0;
  }
  else{
    data->size = fmax(forestOptions->hmin, fmin(forestOptions->hmax, data->size));
  }
}

/** Initialize the state variables of incoming quadrants from outgoing
 * quadrants.
 *
 * The functions p4est_refine_ext(), p4est_coarsen_ext(), and
 * p4est_balance_ext() take as an argument a p4est_replace_t callback function,
 * which allows one to setup the quadrant data of incoming quadrants from the
 * data of outgoing quadrants, before the outgoing data is destroyed.  This
 * function matches the p4est_replace_t prototype.
 *
 * In this example, we linearly interpolate the state variable of a quadrant
 * that is refined to its children, and we average the midpoints of children
 * that are being coarsened to the parent.
 *
 * \param [in] p4est          the forest
 * \param [in] which_tree     the tree in the forest containing \a children
 * \param [in] num_outgoing   the number of quadrants that are being replaced:
 *                            either 1 if a quadrant is being refined, or
 *                            P4EST_CHILDREN if a family of children are being
 *                            coarsened.
 * \param [in] outgoing       the outgoing quadrants
 * \param [in] num_incoming   the number of quadrants that are being added:
 *                            either P4EST_CHILDREN if a quadrant is being refined, or
 *                            1 if a family of children are being
 *                            coarsened.
 * \param [in,out] incoming   quadrants whose data are initialized.
 */
static void hxtOctreeCurvatureReplaceOctants(p4est_t * p4est, p4est_topidx_t which_tree,
                     int num_outgoing,
                     p4est_quadrant_t * outgoing[],
                     int num_incoming, p4est_quadrant_t * incoming[])
{
  size_data_t       *parent_data, *child_data;
  int                 i, j;
  double              h;
  double              du_old, du_est;

  if (num_outgoing > 1) {
    /* this is coarsening */
    parent_data = (size_data_t*) incoming[0]->p.user_data;

    for (i = 0; i < P4EST_CHILDREN; i++) {
      child_data = (size_data_t*) outgoing[i]->p.user_data;
      parent_data->size += child_data->size / P4EST_CHILDREN;
      // for (j = 0; j < P4EST_DIM; j++) {
      //   du_old = parent_data->du[j];
      //   du_est = child_data->du[j];

      //   if (du_old == du_old) {
      //     if (du_est * du_old >= 0.) {
      //       if (fabs (du_est) < fabs (du_old)) {
      //         parent_data->du[j] = du_est;
      //       }
      //     }
      //     else {
      //       parent_data->du[j] = 0.;
      //     }
      //   }
      //   else {
      //     parent_data->du[j] = du_est;
      //   }
      // }
    }
  }
  else {
    parent_data = (size_data_t *) outgoing[0]->p.user_data;
    // h = (double) P4EST_QUADRANT_LEN (outgoing[0]->level) / (double) P4EST_ROOT_LEN;

    for (i = 0; i < P4EST_CHILDREN; i++) {
      child_data = (size_data_t *) incoming[i]->p.user_data;
      child_data->size = parent_data->size;
      // for (j = 0; j < P4EST_DIM; j++) {
      //   child_data->du[j] = parent_data->du[j];
      //   child_data->u +=
      //     (h / 4.) * parent_data->du[j] * ((i & (1 << j)) ? 1. : -1);
      // }
    }
  }
}

static int hxtCurvatureRefineCallback(p4est_t *p4est, p4est_topidx_t which_tree, p4est_quadrant_t *q){
  // std::cout<<((size_data_t *) q->p.user_data)->refineFlag<<std::endl;
  // return ((size_data_t *) q->p.user_data)->refineFlag;


  // p4est_t            *p4est = info->p4est;
  // p4est_quadrant_t   *q = info->quad;
  // p4est_topidx_t      which_tree = info->treeid;
  size_data_t        *data = (size_data_t *) q->p.user_data;
  HXTForestOptions   *forestOptions = (HXTForestOptions *) p4est->user_pointer;

  double min[3], max[3];
  hxtOctreeGetBboxOctant(p4est, which_tree, q, min, max);

  std::vector<uint64_t> candidates;  
  forestOptions->triRTree->Search(min, max, rtreeCallback, &candidates);

  if(!candidates.empty()){
    double kmax = -1.e22;
    double kmin =  1.e22;
    for(std::vector<uint64_t>::iterator tri = candidates.begin(); tri != candidates.end(); ++tri){
      for(int i = 0; i < 3; ++i){
        int node = forestOptions->mesh->triangles.node[(size_t) 3*(*tri)+i];

        double *v1 = forestOptions->nodalCurvature + 6*node;
        double *v2 = forestOptions->nodalCurvature + 6*node + 3;

        double k1, k2;
        hxtNorm2V3(v1, &k1);
        hxtNorm2V3(v2, &k2);

        kmax = fmax(kmax,fmax(k1,k2));
        kmin = fmin(kmin,fmin(k1,k2));
      }
    }

    double h;
    hxtOctreeGetOctantSize(p4est, which_tree, q, &h);

    // Pas de courbure
    if(fabs(kmin) < 1e-3 && fabs(kmax) < 1e-3){
      if(2*h > forestOptions->hbulk){
        return 1;
      } else{
        return 0;
      }
    } else{
      // Taille cible
      double hc = 2*M_PI/(forestOptions->nodePerTwoPi * kmax);
      int nElemPerCell = 1;

      if(2*h > nElemPerCell * hc){
        return 1;
      } else{
        return 0;
      }
    }

  } else{ // candidates.empty()
    return 0;
  }


}

static int hxtCurvatureCoarsenCallback(p4est_t *p4est, p4est_topidx_t which_tree, p4est_quadrant_t *children[]){
  int flag = 1;

  HXTForestOptions *forestOptions = (HXTForestOptions *) p4est->user_pointer;

  for(int n = 0; n < P4EST_CHILDREN; ++n){
    size_data_t *data = (size_data_t *) children[n]->p.user_data;
    
    double min[3], max[3];
    hxtOctreeGetBboxOctant(p4est, which_tree, children[n], min, max);

    std::vector<uint64_t> candidates;  
    forestOptions->triRTree->Search(min, max, rtreeCallback, &candidates);

    // On ne coarsen pas si une cellule touche le maillage de surface
    if(!candidates.empty()){
      flag = 0;
    }

    // On ne coarsen pas si le nouvel élément sera plus grand que hbulk
    if(2.0*data->h > forestOptions->hbulk){
      flag = 0;
    }
  }

  return flag;
}

HXTStatus hxtOctreeCurvatureRefine(HXTForest *forest, int nMax){

  // Refine recursively with respect to the curvature
  // p4est_refine_ext(forest->p4est, 1, P4EST_QMAXLEVEL, hxtCurvatureRefineCallback, hxtOctreeSetInitialSize, hxtOctreeCurvatureReplaceOctants);
  p4est_refine_ext(forest->p4est, 1, P4EST_QMAXLEVEL, hxtCurvatureRefineCallback, hxtOctreeSetInitialSize, NULL);

  // Coarsen
  p4est_coarsen_ext(forest->p4est, 1, 0, hxtCurvatureCoarsenCallback, hxtOctreeSetInitialSize, NULL);

  // Balance the octree to get 2:1 ratio between adjacent cells
  p4est_balance_ext(forest->p4est, P4EST_CONNECT_FACE, hxtOctreeSetInitialSize, hxtOctreeCurvatureReplaceOctants);

  // Print octree in VTK
  // std::string fileVTK = "/Users/arthur/Documents/Code/Mesh_octree/results_octree/dummy_3D_rtree_curvature_refine" + std::to_string(i);
  // std::string fileVTK = "/home/bawina/Documents/paraview_octree/curvature_refine_2sphere_fixed";
  // write_ds_to_vtk(forest->p4est, fileVTK.c_str()); 

  p4est_iterate(forest->p4est, NULL, forest->forestOptions, hxtOctreeAssignSizeAfterRefinement, NULL, NULL, NULL);

  return HXT_STATUS_OK;
}

static double det3x3(const double mat[3][3])
{
  return (mat[0][0] * (mat[1][1] * mat[2][2] - mat[1][2] * mat[2][1]) -
      mat[0][1] * (mat[1][0] * mat[2][2] - mat[1][2] * mat[2][0]) +
      mat[0][2] * (mat[1][0] * mat[2][1] - mat[1][1] * mat[2][0]));
}

static HXTStatus inv3x3(const double mat[3][3], double inv[3][3], double *det)
{
  *det = det3x3(mat);
  if(det) {
    double ud = 1. / *det;
    inv[0][0] = (mat[1][1] * mat[2][2] - mat[1][2] * mat[2][1]) * ud;
    inv[1][0] = -(mat[1][0] * mat[2][2] - mat[1][2] * mat[2][0]) * ud;
    inv[2][0] = (mat[1][0] * mat[2][1] - mat[1][1] * mat[2][0]) * ud;
    inv[0][1] = -(mat[0][1] * mat[2][2] - mat[0][2] * mat[2][1]) * ud;
    inv[1][1] = (mat[0][0] * mat[2][2] - mat[0][2] * mat[2][0]) * ud;
    inv[2][1] = -(mat[0][0] * mat[2][1] - mat[0][1] * mat[2][0]) * ud;
    inv[0][2] = (mat[0][1] * mat[1][2] - mat[0][2] * mat[1][1]) * ud;
    inv[1][2] = -(mat[0][0] * mat[1][2] - mat[0][2] * mat[1][0]) * ud;
    inv[2][2] = (mat[0][0] * mat[1][1] - mat[0][1] * mat[1][0]) * ud;
  } else {
    return HXT_STATUS_ERROR;
  }
  return HXT_STATUS_OK;
}

/*
 distance to triangle

 P(p) = p1 + t1 xi + t2 eta

 t1 = (p2-p1) ; t2 = (p3-p1) ;

 (P(p) - p) = d n

 (p1 + t1 xi + t2 eta - p) = d n
 t1 xi + t2 eta - d n = p - p1

 | t1x t2x -nx | |xi  |   |px-p1x|
 | t1y t2y -ny | |eta | = |py-p1y|
 | t1z t2z -nz | |d   |   |pz-p1z|

 distance to segment

 P(p) = p1 + t (p2-p1)

 (p - P(p)) * (p2-p1) = 0
 (p - p1 - t (p2-p1) ) * (p2-p1) = 0
 - t ||p2-p1||^2 + (p-p1)(p2-p1) = 0

 t = (p-p1)*(p2-p1)/||p2-p1||^2
*/
void signedDistancePointTriangle2(const SPoint3 &p1, const SPoint3 &p2,
                                const SPoint3 &p3, const SPoint3 &p, double &d,
                                SPoint3 &closePt)
{
 SVector3 t1 = p2 - p1;
 SVector3 t2 = p3 - p1;
 SVector3 t3 = p3 - p2;
 SVector3 n = crossprod(t1, t2);
 n.normalize();
 const double n2t1 = dot(t1, t1);
 const double n2t2 = dot(t2, t2);
 const double n2t3 = dot(t3, t3);
 double mat[3][3] = {{t1.x(), t2.x(), -n.x()},
                     {t1.y(), t2.y(), -n.y()},
                     {t1.z(), t2.z(), -n.z()}};
 double inv[3][3];
 double det;
 inv3x3(mat, inv, &det);
 if(det == 0.0) return;

 double u, v;
 SVector3 pp1 = p - p1;
 u = (inv[0][0] * pp1.x() + inv[0][1] * pp1.y() + inv[0][2] * pp1.z());
 v = (inv[1][0] * pp1.x() + inv[1][1] * pp1.y() + inv[1][2] * pp1.z());
 d = (inv[2][0] * pp1.x() + inv[2][1] * pp1.y() + inv[2][2] * pp1.z());
 double sign = (d > 0) ? 1. : -1.;
 if(d == 0.) sign = 1.;
 if(u >= 0. && v >= 0. && 1. - u - v >= 0.0) { // P(p) inside triangle
   closePt = p1 + (p2 - p1) * u + (p3 - p1) * v;
 }
 else {
   const double t12 = dot(pp1, t1) / n2t1;
   const double t13 = dot(pp1, t2) / n2t2;
   SVector3 pp2 = p - p2;
   const double t23 = dot(pp2, t3) / n2t3;
   d = 1.e10;
   if(t12 >= 0 && t12 <= 1.) {
     d = sign * std::min(fabs(d), p.distance(p1 + (p2 - p1) * t12));
     closePt = p1 + (p2 - p1) * t12;
   }
   if(t13 >= 0 && t13 <= 1.) {
     if(p.distance(p1 + (p3 - p1) * t13) < fabs(d))
       closePt = p1 + (p3 - p1) * t13;
     d = sign * std::min(fabs(d), p.distance(p1 + (p3 - p1) * t13));
   }
   if(t23 >= 0 && t23 <= 1.) {
     if(p.distance(p2 + (p3 - p2) * t23) < fabs(d))
       closePt = p2 + (p3 - p2) * t23;
     d = sign * std::min(fabs(d), p.distance(p2 + (p3 - p2) * t23));
   }
   if(p.distance(p1) < fabs(d)) {
     closePt = p1;
     d = sign * std::min(fabs(d), p.distance(p1));
   }
   if(p.distance(p2) < fabs(d)) {
     closePt = p2;
     d = sign * std::min(fabs(d), p.distance(p2));
   }
   if(p.distance(p3) < fabs(d)) {
     closePt = p3;
     d = sign * std::min(fabs(d), p.distance(p3));
   }
 }
}

HXTStatus hxtDistanceToTriangles(HXTForest *forest, std::vector<uint64_t> *candidates, const SPoint3 &p, double &d, uint64_t &closestTri){

  SPoint3 p1 = SPoint3();
  SPoint3 p2 = SPoint3();
  SPoint3 p3 = SPoint3();
  SPoint3 closePt = SPoint3();

  d = DBL_MAX;

  double x,y,z;

  for(std::vector<uint64_t>::iterator tri = candidates->begin(); tri != candidates->end(); ++tri){
    // Coordonnees des points du triangle
    int node1 = forest->forestOptions->mesh->triangles.node[(size_t) 3*(*tri)  ];
    int node2 = forest->forestOptions->mesh->triangles.node[(size_t) 3*(*tri)+1];
    int node3 = forest->forestOptions->mesh->triangles.node[(size_t) 3*(*tri)+2];
      
    x = forest->forestOptions->mesh->vertices.coord[(size_t) 4*node1  ];
    y = forest->forestOptions->mesh->vertices.coord[(size_t) 4*node1+1];
    z = forest->forestOptions->mesh->vertices.coord[(size_t) 4*node1+2];
    p1.setPosition(x,y,z);
    x = forest->forestOptions->mesh->vertices.coord[(size_t) 4*node2  ];
    y = forest->forestOptions->mesh->vertices.coord[(size_t) 4*node2+1];
    z = forest->forestOptions->mesh->vertices.coord[(size_t) 4*node2+2];
    p2.setPosition(x,y,z);
    x = forest->forestOptions->mesh->vertices.coord[(size_t) 4*node3  ];
    y = forest->forestOptions->mesh->vertices.coord[(size_t) 4*node3+1];
    z = forest->forestOptions->mesh->vertices.coord[(size_t) 4*node3+2];
    p3.setPosition(x,y,z);

    double d_tmp;
    signedDistancePointTriangle2(p1, p2, p3, p, d_tmp, closePt);
    if(d_tmp <= d) closestTri = *tri;
    d = fmin(d, fabs(d_tmp));
  }

  return HXT_STATUS_OK;

}

// Algorithme breadth first search pour eliminer les triangles de candidates 
// qui sont proches de node par rapport a la topologie de la triangulation (!= distance euclidienne)
// In : - candidates, le vecteur des triangles qui intersectent la boite de cote h autour de node
//      - node, le noeud courant dans SurfacesProches
void hxtBFSTriangles(HXTForest *forest, std::vector<uint64_t> *candidates, int node){

  double *n = forest->forestOptions->nodeNormals + 3*node;
  // printf("Normale au noeud : (%f, %f, %f)\n", n[0], n[1], n[2]);
  // Contient des noeuds (il faut partir de node)
  std::queue<int> q; 
  // Determiner toutes les couleurs du noeud
  int currentColor;
  std::vector<int> nodeColors, allColors;
  for(std::vector<uint64_t>::iterator tri = candidates->begin(); tri != candidates->end(); ++tri){
    currentColor = forest->forestOptions->mesh->triangles.colors[(size_t)(*tri)];
    if(std::find(allColors.begin(), allColors.end(), currentColor) == allColors.end())
      allColors.push_back(currentColor);
    for(int i = 0; i < 3; ++i){
      if(forest->forestOptions->mesh->triangles.node[(size_t) 3*(*tri)+i] == node){
        if(std::find(nodeColors.begin(), nodeColors.end(), currentColor) == nodeColors.end()){
          nodeColors.push_back(currentColor);
        }
      }
    }
  }
  // printf("Nombre de couleurs pour le noeud %d : %d\n", node, nodeColors.size());
  std::vector<uint64_t> savedCandidates;
  // Partir de node (ajouter dans la file)
  q.push(node);
  while(!q.empty()){
    // Prendre tous les triangles de candidates qui contiennent node, puis les retirer de candidates. 
    // Prendre tous les noeuds de ces triangles et les ajouter dans la file
    for(std::vector<uint64_t>::iterator tri = candidates->begin(); tri != candidates->end(); ){

      // std::cout << "Couleur = " << forest->forestOptions->mesh->triangles.colors[(size_t) (*tri)] << std::endl;
      currentColor = forest->forestOptions->mesh->triangles.colors[(size_t)(*tri)];

      bool flag = false;
      for(int i = 0; i < 3; ++i){
        int local_node = forest->forestOptions->mesh->triangles.node[(size_t) 3*(*tri)+i];
        if(local_node == q.front()) flag = true;
      }

      // Si q.front() est dans le triangle courant, on ajoute les deux autres sommets et on retire le triangle de la liste
      if(flag){
        for(int i = 0; i < 3; ++i){
          int local_node = forest->forestOptions->mesh->triangles.node[(size_t) 3*(*tri)+i];
          if(local_node != q.front()) q.push(local_node);
        }

        if(std::count(nodeColors.begin(), nodeColors.end(), currentColor) == 0){
          savedCandidates.push_back(*tri);
        }
        
        tri = candidates->erase(tri);

      } else{
        ++tri;
      }
    }
    q.pop();
  }

  if(allColors.size() > 2){
    for(std::vector<uint64_t>::iterator it = savedCandidates.begin(); it < savedCandidates.end(); ++it){
      candidates->push_back(*it);
    }
  }

  for(std::vector<uint64_t>::iterator it = candidates->begin(); it < candidates->end(); ){
    double *v0 = &forest->forestOptions->nodeNormals[3*forest->forestOptions->mesh->triangles.node[3*(*it)+0]];
    double *v1 = &forest->forestOptions->nodeNormals[3*forest->forestOptions->mesh->triangles.node[3*(*it)+1]];
    double *v2 = &forest->forestOptions->nodeNormals[3*forest->forestOptions->mesh->triangles.node[3*(*it)+2]];

    double cos0 = v0[0]*n[0] + v0[1]*n[1] + v0[2]*n[2]; // Les normales sont censees etre unitaires
    double cos1 = v1[0]*n[0] + v1[1]*n[1] + v1[2]*n[2]; // Les normales sont censees etre unitaires
    double cos2 = v2[0]*n[0] + v2[1]*n[1] + v2[2]*n[2]; // Les normales sont censees etre unitaires

    double cosMin = 0.7; // Plus grand que sqrt(2)/2 pour les coins à 45° ?
    int areNormalsNotAligned = (fabs(cos0) < cosMin) + (fabs(cos1) < cosMin) + (fabs(cos2) < cosMin);
    int areNormalsInRightDirection = (cos0 > 0) + (cos1 > 0) + (cos2 > 0);

    if(areNormalsNotAligned >= 1 || areNormalsInRightDirection >= 1){
      it = candidates->erase(it);
    }else{
      ++it;
    }
  }

  if(!candidates->empty()){
    globCount++;
  }


   // std::cout<<"Elements restants"<<std::endl;
   // for(auto &val : *candidates)
     // std::cout<<val<<std::endl;
}

HXTStatus hxtOctreeSurfacesProches(HXTForest *forest){

  // Pour chaque noeud : recuperer sa taille dans l'octree et prendre les triangles dans la boule de rayon h
  SPoint3 p = SPoint3();
  // sc_array_t *points = sc_array_new_size(sizeof(size_point_t), forest->forestOptions->mesh->vertices.num);
  sc_array_t *points = sc_array_new_size(sizeof(size_point_t), 1);
  size_point_t *p_tmp;

  // La taille du noeud courant
  double size, x, y, z; 
  double min[3], max[3];

  bool debug = true;
  FILE* file = fopen("pointsConnectesAvecAngles.pos", "w");
    if(file==NULL)
      return HXT_ERROR(HXT_STATUS_FILE_CANNOT_BE_OPENED);
  if(debug){
    fprintf(file, "View \"pointsConnectesAvecAngles\" {\n");
  }

  int percents = 10, steps = forest->forestOptions->mesh->vertices.num / 10;
  for(uint64_t i = 0; i < forest->forestOptions->mesh->vertices.num; ++i){
    p_tmp = (size_point_t *) sc_array_index(points, 0);

    p_tmp->x = x = forest->forestOptions->mesh->vertices.coord[(size_t) 4*i  ];
    p_tmp->y = y = forest->forestOptions->mesh->vertices.coord[(size_t) 4*i+1];
    p_tmp->z = z = forest->forestOptions->mesh->vertices.coord[(size_t) 4*i+2];

    HXT_CHECK(hxtOctreeSearchOne(forest, x, y, z, &size));

    // Boite autour du point de taille h
    min[0] = x - size; max[0] = x + size;
    min[1] = y - size; max[1] = y + size;
    min[2] = z - size; max[2] = z + size;
    std::vector<uint64_t> candidates;  
    forest->forestOptions->triRTree->Search(min, max, rtreeCallback, &candidates);

    if(!candidates.empty()){

      hxtBFSTriangles(forest, &candidates, i);

      if(!candidates.empty()){
        uint64_t closestTri;
        p.setPosition(x,y,z);
        hxtDistanceToTriangles(forest, &candidates, p, size, closestTri);

        if(debug){
          double x_avg, y_avg, z_avg;
          // for(std::vector<uint64_t>::iterator it = candidates.begin(); it < candidates.end(); ++it){
            x_avg = y_avg = z_avg = 0.0;
            for(int i = 0; i < 3; ++i){
              // uint64_t node = forest->forestOptions->mesh->triangles.node[(size_t) 3*(*it)+i];
              uint64_t node = forest->forestOptions->mesh->triangles.node[(size_t) 3*closestTri+i];
              x_avg += 1.0/3.0 * forest->forestOptions->mesh->vertices.coord[(size_t) 4*node];
              y_avg += 1.0/3.0 * forest->forestOptions->mesh->vertices.coord[(size_t) 4*node+1];
              z_avg += 1.0/3.0 * forest->forestOptions->mesh->vertices.coord[(size_t) 4*node+2];
            }
            fprintf(file, "SL(%f,%f,%f,%f,%f,%f){%f,%f};\n", x, y, z, x_avg, y_avg, z_avg, size, size);
          // }
        }

        size = fmin(size, size/forest->forestOptions->nodePerGap);
        size = fmax(size, forest->forestOptions->hmin);
        // printf("Taille corrigée au noeud %d = %f \n", i+1, size);
        p_tmp->size = size;
        // p_tmp->size = 0.0;
        p_tmp->surfaceFlag = 2;

        // On cherche dans l'octree et on remplace dans les quadrants associes aux noeuds
        p4est_search(forest->p4est, NULL, hxtOctreeReplaceCallback, points);
      }
    }

    if(i%steps==0){
      printf("[%d%]\n", percents);
      percents += 10;
    }

  }

  if(debug){
    fprintf(file, "};");
    fclose(file);
  }

  printf("%d listes non vides\n", globCount);

  sc_array_destroy(points);

  return HXT_STATUS_OK;

}

void elementEstimateCallback(p4est_iter_volume_info_t * info, void *user_data)
{
  p4est_quadrant_t   *q = info->quad;
  size_data_t        *data = (size_data_t *) q->p.user_data;

  p4est_t            *p4est = info->p4est;
  p4est_topidx_t      which_tree = info->treeid;

  double center[3];
  hxtOctreeGetCenter(p4est, which_tree, q, center);

  double octantVolume = data->h * data->h * data->h;
  double tetVolume = data->size * data->size * data->size * sqrt(2) / 12.0;

  if(sqrt(center[0]*center[0] + center[1]*center[1] + center[2]*center[2]) <= 1){
    *((double *) user_data) += octantVolume/tetVolume;
  }

}

HXTStatus hxtOctreeElementEstimation(HXTForest *forest, double *elemEstimate){

  p4est_iterate (forest->p4est, NULL, (void *) elemEstimate, elementEstimateCallback, NULL,
            #ifdef P4_TO_P8
                       NULL,
            #endif
                       NULL);

  return HXT_STATUS_OK;
}

// Deprecated
// HXTStatus hxtForestWriteBBoxMesh(HXTBbox *bbox, const char* filename){
//   FILE* file = fopen(filename,"w");
//   if(file==NULL)
//     return HXT_ERROR_MSG(HXT_STATUS_FILE_CANNOT_BE_OPENED,
//       "Cannot open mesh file \"%s\"",(filename==NULL)?"(null)":filename);

//   /* Writing a simple ABAQUS (.inp) file with a single element*/
//   fprintf(file,"*Heading\n %s\n",filename);

//   for(int i = 0; i < 3; ++i){
//     bbox->min[i] *= 1.3;
//     bbox->max[i] *= 1.3;
//   }
  
//   /* print the nodes */
//   fprintf(file,"*NODE\n");
//   fprintf(file,"%u, %f, %f, %f\n",1, bbox->min[0], bbox->min[1], bbox->min[2]);
//   fprintf(file,"%u, %f, %f, %f\n",2, bbox->max[0], bbox->min[1], bbox->min[2]);
//   fprintf(file,"%u, %f, %f, %f\n",3, bbox->max[0], bbox->max[1], bbox->min[2]);
//   fprintf(file,"%u, %f, %f, %f\n",4, bbox->min[0], bbox->max[1], bbox->min[2]);
//   fprintf(file,"%u, %f, %f, %f\n",5, bbox->min[0], bbox->min[1], bbox->max[2]);
//   fprintf(file,"%u, %f, %f, %f\n",6, bbox->max[0], bbox->min[1], bbox->max[2]);
//   fprintf(file,"%u, %f, %f, %f\n",7, bbox->max[0], bbox->max[1], bbox->max[2]);
//   fprintf(file,"%u, %f, %f, %f\n",8, bbox->min[0], bbox->max[1], bbox->max[2]);

//   fprintf(file,"******* E L E M E N T S *************\n");
//   fprintf(file,"*ELEMENT, type=C3D8, ELSET=Volume8\n");
//   fprintf(file,"1, 1, 2, 3, 4, 5, 6, 7, 8\n");

//   fclose(file);
//   return HXT_STATUS_OK;
// }

p4est_connectivity_t *
// p8est_connectivity_new_cube (double cMax, double cMin)
p8est_connectivity_new_cube (HXTForestOptions *forestOptions)
{
  const p4est_topidx_t num_vertices = 8;
  const p4est_topidx_t num_trees = 1;
  const p4est_topidx_t num_ett = 0;
  const p4est_topidx_t num_ctt = 0;

  double centreX = (forestOptions->bbox[0]+forestOptions->bbox[3])/2;
  double centreY = (forestOptions->bbox[1]+forestOptions->bbox[4])/2;
  double centreZ = (forestOptions->bbox[2]+forestOptions->bbox[5])/2;
  double cX = (forestOptions->bbox[3]-forestOptions->bbox[0])/2;
  double cY = (forestOptions->bbox[4]-forestOptions->bbox[1])/2;
  double cZ = (forestOptions->bbox[5]-forestOptions->bbox[2])/2;
  double c = 1.5*fmax(fmax(cX,cY),cZ);

  // const double        vertices[8 * 3] = {
  //   cMin, cMin, cMin,
  //   cMax, cMin, cMin,
  //   cMin, cMax, cMin,
  //   cMax, cMax, cMin,
  //   cMin, cMin, cMax,
  //   cMax, cMin, cMax,
  //   cMin, cMax, cMax,
  //   cMax, cMax, cMax,
  // };
  const double        vertices[8 * 3] = {
    centreX-c, centreY-c, centreZ-c,
    centreX+c, centreY-c, centreZ-c,
    centreX-c, centreY+c, centreZ-c,
    centreX+c, centreY+c, centreZ-c,
    centreX-c, centreY-c, centreZ+c,
    centreX+c, centreY-c, centreZ+c,
    centreX-c, centreY+c, centreZ+c,
    centreX+c, centreY+c, centreZ+c,
  };
  const p4est_topidx_t tree_to_vertex[1 * 8] = {
    0, 1, 2, 3, 4, 5, 6, 7,
  };
  const p4est_topidx_t tree_to_tree[1 * 6] = {
    0, 0, 0, 0, 0, 0,
  };
  const int8_t        tree_to_face[1 * 6] = {
    0, 1, 2, 3, 4, 5,
  };
  return p4est_connectivity_new_copy (num_vertices, num_trees, 0, 0,
				      vertices, tree_to_vertex,
				      tree_to_tree, tree_to_face,
				      NULL, &num_ett, NULL, NULL,
				      NULL, &num_ctt, NULL, NULL);
}

void exportToTetraCallback(p4est_iter_volume_info_t * info, void *user_data)
{
  p4est_quadrant_t   *q = info->quad;
  size_data_t        *data = (size_data_t *) q->p.user_data;

  p4est_t            *p4est = info->p4est;
  p4est_topidx_t      which_tree = info->treeid;

  HXTForestOptions   *forestOptions = (HXTForestOptions *) p4est->user_pointer;

  FILE* f = (FILE*) user_data;

  double center[3], x[8], y[8], z[8];
  hxtOctreeGetCenter(p4est, which_tree, q, center);

  double h = data->h, s = data->size;
  x[0] = x[1] = x[4] = x[5] = center[0]-h;
  x[2] = x[3] = x[6] = x[7] = center[0]+h;
  y[0] = y[3] = y[4] = y[7] = center[1]-h;
  y[1] = y[2] = y[5] = y[6] = center[1]+h;
  z[0] = z[1] = z[2] = z[3] = center[2]-h;
  z[4] = z[5] = z[6] = z[7] = center[2]+h;
  
  fprintf(f, "SS(%f,%f,%f, %f,%f,%f, %f,%f,%f, %f,%f,%f){%f,%f,%f,%f};\n", 
                 x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2], x[4], y[4], z[4], s, s, s, s);
  fprintf(f, "SS(%f,%f,%f, %f,%f,%f, %f,%f,%f, %f,%f,%f){%f,%f,%f,%f};\n", 
                 x[0], y[0], z[0], x[2], y[2], z[2], x[3], y[3], z[3], x[4], y[4], z[4], s, s, s, s);
  fprintf(f, "SS(%f,%f,%f, %f,%f,%f, %f,%f,%f, %f,%f,%f){%f,%f,%f,%f};\n", 
                 x[1], y[1], z[1], x[2], y[2], z[2], x[4], y[4], z[4], x[5], y[5], z[5], s, s, s, s);
  fprintf(f, "SS(%f,%f,%f, %f,%f,%f, %f,%f,%f, %f,%f,%f){%f,%f,%f,%f};\n", 
                 x[2], y[2], z[2], x[3], y[3], z[3], x[4], y[4], z[4], x[6], y[6], z[6], s, s, s, s);
  fprintf(f, "SS(%f,%f,%f, %f,%f,%f, %f,%f,%f, %f,%f,%f){%f,%f,%f,%f};\n", 
                 x[2], y[2], z[2], x[4], y[4], z[4], x[5], y[5], z[5], x[6], y[6], z[6], s, s, s, s);
  fprintf(f, "SS(%f,%f,%f, %f,%f,%f, %f,%f,%f, %f,%f,%f){%f,%f,%f,%f};\n", 
                 x[3], y[3], z[3], x[4], y[4], z[4], x[6], y[6], z[6], x[7], y[7], z[7], s, s, s, s);
}

void exportToHexCallback(p4est_iter_volume_info_t * info, void *user_data)
{
  p4est_quadrant_t   *q = info->quad;
  size_data_t        *data = (size_data_t *) q->p.user_data;

  p4est_t            *p4est = info->p4est;
  p4est_topidx_t      which_tree = info->treeid;

  HXTForestOptions   *forestOptions = (HXTForestOptions *) p4est->user_pointer;

  FILE* f = (FILE*) user_data;

  double center[3], x[8], y[8], z[8];
  hxtOctreeGetCenter(p4est, which_tree, q, center);

  double h = data->h/2, s = data->size;
  x[0] = x[3] = x[4] = x[7] = center[0]-h;
  x[1] = x[2] = x[5] = x[6] = center[0]+h;
  y[0] = y[1] = y[4] = y[5] = center[1]-h;
  y[2] = y[3] = y[6] = y[7] = center[1]+h;
  z[0] = z[1] = z[2] = z[3] = center[2]-h;
  z[4] = z[5] = z[6] = z[7] = center[2]+h;
  
  fprintf(f, "SH(%f,%f,%f, %f,%f,%f, %f,%f,%f, %f,%f,%f,%f,%f,%f, %f,%f,%f, %f,%f,%f, %f,%f,%f){%f,%f,%f,%f,%f,%f,%f,%f};\n", 
    x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2], x[3], y[3], z[3], 
    x[4], y[4], z[4], x[5], y[5], z[5], x[6], y[6], z[6], x[7], y[7], z[7], 
    s, s, s, s, s, s, s, s);
}

HXTStatus hxtOctreeExport(HXTForest *forest){

  FILE* f = fopen(forest->forestOptions->filename, "w");
  if(f==NULL)
    return HXT_ERROR(HXT_STATUS_FILE_CANNOT_BE_OPENED);

  fprintf(f, "View \"sizeField\" {\n");
  
  p4est_iterate(forest->p4est, NULL, (void*) f, exportToHexCallback, NULL,
            #ifdef P4_TO_P8
                        NULL,
            #endif
                        NULL);

  fprintf(f, "};");
  fclose(f);
}

#else // HAVE_P4EST

HXTStatus hxtOctreeSearchOne(HXTForest *forest, double x, double y, double z, double *size) {

  *size = 1.e22;
  static int count = 0;
  if (count ++ < 20)
    return HXT_ERROR_MSG(HXT_STATUS_ERROR,"HXT needs P4EST to compute automatic size fields");
  return HXT_STATUS_ERROR;
}

#endif // HAVE_P4EST
