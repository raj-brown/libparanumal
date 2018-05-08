#include "elliptic.h"

void ellipticPreconditionerSetup(elliptic_t *elliptic, ogs_t *ogs, dfloat lambda){

  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  mesh2D *mesh = elliptic->mesh;
  precon_t *precon = elliptic->precon;
  setupAide options = elliptic->options;

  if(options.compareArgs("PRECONDITIONER", "FULLALMOND")){ //build full A matrix and pass to Almond
    dlong nnz;
    nonZero_t *A;

    hlong *globalStarts = (hlong*) calloc(size+1, sizeof(hlong));

    int basisNp = mesh->Np;
    dfloat *basis = NULL;

    if (options.compareArgs("BASIS", "BERN")) basis = mesh->VB;

    if (options.compareArgs("DISCRETIZATION", "IPDG")) {
      ellipticBuildIpdg(elliptic, basisNp, basis, lambda, &A, &nnz, globalStarts);
    } else if (options.compareArgs("DISCRETIZATION", "CONTINUOUS")) {
      ellipticBuildContinuous(elliptic,lambda,&A,&nnz, &(precon->ogs), globalStarts);
    }

    hlong *Rows = (hlong *) calloc(nnz, sizeof(hlong));
    hlong *Cols = (hlong *) calloc(nnz, sizeof(hlong));
    dfloat *Vals = (dfloat*) calloc(nnz,sizeof(dfloat));
    
    for (dlong n=0;n<nnz;n++) {
      Rows[n] = A[n].row;
      Cols[n] = A[n].col;
      Vals[n] = A[n].val;
    }

    precon->parAlmond = parAlmondInit(mesh, options);
    parAlmondAgmgSetup(precon->parAlmond,
                       globalStarts,
                       nnz,
                       Rows,
                       Cols,
                       Vals,
                       elliptic->allNeumann,
                       elliptic->allNeumannPenalty);
    free(A); free(Rows); free(Cols); free(Vals);

    if (options.compareArgs("DISCRETIZATION", "CONTINUOUS")) {//tell parAlmond to gather this level
      agmgLevel *baseLevel = precon->parAlmond->levels[0];

      baseLevel->gatherLevel = true;
      baseLevel->Srhs = (dfloat*) calloc(mesh->Np*mesh->Nelements,sizeof(dfloat));
      baseLevel->Sx   = (dfloat*) calloc(mesh->Np*mesh->Nelements,sizeof(dfloat));
      baseLevel->o_Srhs = mesh->device.malloc(mesh->Np*mesh->Nelements*sizeof(dfloat));
      baseLevel->o_Sx   = mesh->device.malloc(mesh->Np*mesh->Nelements*sizeof(dfloat));

      baseLevel->weightedInnerProds = false;

      baseLevel->gatherArgs = (void **) calloc(3,sizeof(void*));  
      baseLevel->gatherArgs[0] = (void *) elliptic;
      baseLevel->gatherArgs[1] = (void *) precon->ogs;
      baseLevel->gatherArgs[2] = (void *) &(baseLevel->o_Sx);
      baseLevel->scatterArgs = baseLevel->gatherArgs;

      baseLevel->device_gather  = ellipticGather;
      baseLevel->device_scatter = ellipticScatter;        
    }

/*
    if (strstr(options,"MATRIXFREE")&&strstr(options,"IPDG")) { //swap the top AMG level ops for matrix free versions
      agmgLevel *baseLevel = precon->parAlmond->levels[0];

      dfloat *vlambda = (dfloat *) calloc(1,sizeof(dfloat));
      *vlambda = lambda;
      baseLevel->AxArgs = (void **) calloc(3,sizeof(void*));
      baseLevel->AxArgs[0] = (void *) elliptic;
      baseLevel->AxArgs[1] = (void *) vlambda;
      baseLevel->AxArgs[2] = (void *) options;
      baseLevel->device_Ax = AxTri2D;

      baseLevel->smoothArgs = (void **) calloc(2,sizeof(void*));
      baseLevel->smoothArgs[0] = (void *) elliptic;
      baseLevel->smoothArgs[1] = (void *) baseLevel;
      baseLevel->device_smooth = smoothTri2D;

      baseLevel->smootherArgs = (void **) calloc(1,sizeof(void*));
      baseLevel->smootherArgs[0] = (void *) elliptic;

      baseLevel->Nrows = mesh->Nelements*mesh->Np;
      baseLevel->Ncols = (mesh->Nelements+mesh->totalHaloPairs)*mesh->Np;

      // extra storage for smoothing op
      baseLevel->o_smootherResidual = mesh->device.malloc(baseLevel->Ncols*sizeof(dfloat),baseLevel->x);

      dfloat rateTolerance;    // 0 - accept not approximate patches, 1 - accept all approximate patches
      if(strstr(options, "EXACT")){
        rateTolerance = 0.0;
      } else {
        rateTolerance = 1.0;
      }

      //set up the fine problem smoothing
      if(strstr(options, "LOCALPATCH")){
        ellipticSetupSmootherLocalPatch(elliptic, precon, baseLevel, tau, lambda, BCType, rateTolerance, options);
      } else { //default to damped jacobi
        ellipticSetupSmootherDampedJacobi(elliptic, precon, baseLevel, tau, lambda, BCType, options);
      }
    }
*/
  } else if (options.compareArgs("PRECONDITIONER", "MASSMATRIX")){

    precon->o_invMM = mesh->device.malloc(mesh->Np*mesh->Np*sizeof(dfloat), mesh->invMM);

  } else if(options.compareArgs("PRECONDITIONER", "MULTIGRID")){

    ellipticMultiGridSetup(elliptic,precon,lambda);

  } else if(options.compareArgs("PRECONDITIONER", "SEMFEM")) {

    ellipticSEMFEMSetup(elliptic,precon,lambda);

  } else if(options.compareArgs("PRECONDITIONER", "JACOBI")) {

    dfloat *invDiagA;
    ellipticBuildJacobi(elliptic,lambda,&invDiagA);
    precon->o_invDiagA = mesh->device.malloc(mesh->Np*mesh->Nelements*sizeof(dfloat), invDiagA);
    free(invDiagA);
  }
}
