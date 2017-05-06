#include "ellipticTri2D.h"

typedef struct{

  iint localId;
  iint baseId;
  iint haloFlag;
  
} preconGatherInfo_t;

int parallelCompareBaseId(const void *a, const void *b){

  preconGatherInfo_t *fa = (preconGatherInfo_t*) a;
  preconGatherInfo_t *fb = (preconGatherInfo_t*) b;

  if(fa->baseId < fb->baseId) return -1;
  if(fa->baseId > fb->baseId) return +1;

  return 0;

}

typedef struct{

  iint row;
  iint col;
  iint ownerRank;
  dfloat val;

}nonZero_t;

// compare on global indices 
int parallelCompareRowColumn(const void *a, const void *b);

precon_t *ellipticPreconditionerSetupTri2D(mesh2D *mesh, ogs_t *ogs, dfloat lambda, const char *options){

  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  iint Nlocal = mesh->Np*mesh->Nelements;
  iint Nhalo  = mesh->Np*mesh->totalHaloPairs;
  iint Ntrace = mesh->Nfp*mesh->Nfaces*mesh->Nelements;

  precon_t *precon = (precon_t*) calloc(1, sizeof(precon_t));

  if(strstr(options, "OAS")){

    // build gather-scatter
    iint NpP = mesh->Np + mesh->Nfaces*mesh->Nfp;

    // build gather-scatter for overlapping patches
    iint *allNelements = (iint*) calloc(size, sizeof(iint));
    MPI_Allgather(&(mesh->Nelements), 1, MPI_IINT,
  		allNelements, 1, MPI_IINT, MPI_COMM_WORLD);

    // offsets
    iint *startElement = (iint*) calloc(size, sizeof(iint));
    for(iint r=1;r<size;++r){
      startElement[r] = startElement[r-1]+allNelements[r-1];
    }

    // 1-indexed numbering of nodes on this process
    iint *localNums = (iint*) calloc((Nlocal+Nhalo), sizeof(iint));
    for(iint e=0;e<mesh->Nelements;++e){
      for(iint n=0;n<mesh->Np;++n){
        localNums[e*mesh->Np+n] = 1 + e*mesh->Np + n + startElement[rank]*mesh->Np;
      }
    }
    
    if(Nhalo){
      // send buffer for outgoing halo
      iint *sendBuffer = (iint*) calloc(Nhalo, sizeof(iint));

      // exchange node numbers with neighbors
      meshHaloExchange(mesh,
  		     mesh->Np*sizeof(iint),
  		     localNums,
  		     sendBuffer,
  		     localNums+Nlocal);
    }
    
    preconGatherInfo_t *preconGatherInfoDg = 
      (preconGatherInfo_t*) calloc(NpP*mesh->Nelements,
  				 sizeof(preconGatherInfo_t));

    // set local ids
    for(iint n=0;n<mesh->Nelements*NpP;++n)
      preconGatherInfoDg[n].localId = n;

    // numbering of patch interior nodes
    for(iint e=0;e<mesh->Nelements;++e){
      for(iint n=0;n<mesh->Np;++n){
        iint id  = n + e*mesh->Np;
        iint pid = n + e*NpP;

        // all patch interior nodes are local
        preconGatherInfoDg[pid].baseId = localNums[id];
      }
    }

    // add patch boundary nodes
    for(iint e=0;e<mesh->Nelements;++e){
      for(iint f=0;f<mesh->Nfaces;++f){
        // mark halo nodes
        iint rP = mesh->EToP[e*mesh->Nfaces+f];
        iint eP = mesh->EToE[e*mesh->Nfaces+f];
        iint fP = mesh->EToF[e*mesh->Nfaces+f];
        iint bc = mesh->EToB[e*mesh->Nfaces+f];

        for(iint n=0;n<mesh->Nfp;++n){
  	iint id = n + f*mesh->Nfp+e*mesh->Nfp*mesh->Nfaces;
  	iint idP = mesh->vmapP[id];
  	
  	// local numbers
  	iint pidM = e*NpP + mesh->faceNodes[f*mesh->Nfp+n];
  	iint pidP = e*NpP + mesh->Np + f*mesh->Nfp+n;
  	preconGatherInfoDg[pidP].baseId = localNums[idP];

  	if(rP!=-1){
  	  preconGatherInfoDg[pidM].haloFlag = 1;
  	  preconGatherInfoDg[pidP].haloFlag = 1;
  	}
        }
      }
    }

    // sort by rank then base index
    qsort(preconGatherInfoDg, NpP*mesh->Nelements, sizeof(preconGatherInfo_t),
  	parallelCompareBaseId);
      
    // do not gather-scatter nodes labelled zero
    int skip = 0;

    while(preconGatherInfoDg[skip].baseId==0 && skip<NpP*mesh->Nelements){
      ++skip;
    }

    // reset local ids
    iint NlocalDg = NpP*mesh->Nelements - skip;
    iint *gatherLocalIdsDg  = (iint*) calloc(NlocalDg, sizeof(iint));
    iint *gatherBaseIdsDg   = (iint*) calloc(NlocalDg, sizeof(iint));
    iint *gatherHaloFlagsDg = (iint*) calloc(NlocalDg, sizeof(iint));
    for(iint n=0;n<NlocalDg;++n){
      gatherLocalIdsDg[n]  = preconGatherInfoDg[n+skip].localId;
      gatherBaseIdsDg[n]   = preconGatherInfoDg[n+skip].baseId;
      gatherHaloFlagsDg[n] = preconGatherInfoDg[n+skip].haloFlag;
    }
    
    // make preconBaseIds => preconNumbering
    precon->ogsDg = meshParallelGatherScatterSetup(mesh,
  						 NlocalDg,
  						 sizeof(dfloat),
  						 gatherLocalIdsDg,
  						 gatherBaseIdsDg,
  						 gatherHaloFlagsDg);
    
    // -------------------------------------------------------------------------------------------
    // load prebuilt transform matrices
    precon->o_oasForwardDg = mesh->device.malloc(NpP*NpP*sizeof(dfloat), mesh->oasForwardDg);
    precon->o_oasBackDg    = mesh->device.malloc(NpP*NpP*sizeof(dfloat), mesh->oasBackDg);

    dfloat *oasForwardDgT = (dfloat*) calloc(NpP*NpP, sizeof(dfloat));
    dfloat *oasBackDgT = (dfloat*) calloc(NpP*NpP, sizeof(dfloat));
    for(iint n=0;n<NpP;++n){
      for(iint m=0;m<NpP;++m){
        oasForwardDgT[n+m*NpP] = mesh->oasForwardDg[m+n*NpP];
        oasBackDgT[n+m*NpP] = mesh->oasBackDg[m+n*NpP];
      }
    }

    precon->o_oasForwardDgT = mesh->device.malloc(NpP*NpP*sizeof(dfloat), oasForwardDgT);
    precon->o_oasBackDgT    = mesh->device.malloc(NpP*NpP*sizeof(dfloat), oasBackDgT);

    
    /// ---------------------------------------------------------------------------
    
    // hack estimate for Jacobian scaling

    dfloat *diagInvOp = (dfloat*) calloc(NpP*mesh->Nelements, sizeof(dfloat));
    dfloat *diagInvOpDg = (dfloat*) calloc(NpP*mesh->Nelements, sizeof(dfloat));
    for(iint e=0;e<mesh->Nelements;++e){

      // S = Jabc*(wa*wb*wc*lambda + wb*wc*Da'*wa*Da + wa*wc*Db'*wb*Db + wa*wb*Dc'*wc*Dc)
      // S = Jabc*wa*wb*wc*(lambda*I+1/wa*Da'*wa*Da + 1/wb*Db'*wb*Db + 1/wc*Dc'*wc*Dc)
      
      dfloat J = mesh->vgeo[e*mesh->Nvgeo + JID];
      dfloat rx = mesh->vgeo[e*mesh->Nvgeo + RXID];
      dfloat sx = mesh->vgeo[e*mesh->Nvgeo + SXID];
      dfloat ry = mesh->vgeo[e*mesh->Nvgeo + RYID];
      dfloat sy = mesh->vgeo[e*mesh->Nvgeo + SYID];
      
      //metric tensor on this element
      dfloat grr = rx*rx+ry*ry;
      dfloat grs = rx*sx+ry*sy;
      dfloat gss = sx*sx+sy*sy;

      //eigenvalues of the metric
      dfloat eigG1 = 0.5*(grr + gss) - 0.5*sqrt((grr-gss)*(grr-gss) + 4*grs*grs);
      dfloat eigG2 = 0.5*(grr + gss) + 0.5*sqrt((grr-gss)*(grr-gss) + 4*grs*grs);
      
      //TODO Average? Min/Max/Avg Eigenvalue? What works best for the scaling?
      dfloat Jhinv2 = J*(eigG1+eigG2);
      //dfloat Jhinv2 = J*mymax(eigG1,eigG2);
      //dfloat Jhinv2 = J*mymin(eigG1,eigG2);

      for(iint n=0;n<NpP;++n){
        iint pid = n + e*NpP;
  	
        diagInvOpDg[pid] = 1./(J*lambda + Jhinv2*mesh->oasDiagOpDg[n]);
      }
    }
    
    precon->o_oasDiagInvOpDg =
      mesh->device.malloc(NpP*mesh->Nelements*sizeof(dfloat), diagInvOpDg);

    if(Nhalo){
      dfloat *vgeoSendBuffer = (dfloat*) calloc(mesh->totalHaloPairs*mesh->Nvgeo, sizeof(dfloat));
      
      // import geometric factors from halo elements
      mesh->vgeo = (dfloat*) realloc(mesh->vgeo, (mesh->Nelements+mesh->totalHaloPairs)*mesh->Nvgeo*sizeof(dfloat));
      
      meshHaloExchange(mesh,
  		     mesh->Nvgeo*sizeof(dfloat),
  		     mesh->vgeo,
  		     vgeoSendBuffer,
  		     mesh->vgeo + mesh->Nelements*mesh->Nvgeo);
      
      mesh->o_vgeo =
        mesh->device.malloc((mesh->Nelements + mesh->totalHaloPairs)*mesh->Nvgeo*sizeof(dfloat), mesh->vgeo);
    }

    mesh->device.finish();
    occa::tic("CoarsePreconditionerSetup");
    // coarse grid preconditioner (only continous elements)
    // do this here since we need A*x
    ellipticCoarsePreconditionerSetupTri2D(mesh, precon, lambda, options);

    mesh->device.finish();
    occa::toc("CoarsePreconditionerSetup");

  } else if(strstr(options, "FULLALMOND")){

    // ------------------------------------------------------------------------------------
    // 1. Create a contiguous numbering system, starting from the element-vertex connectivity
    iint Nnum = mesh->Np*mesh->Nelements;

    iint *globalOwners = (iint*) calloc(Nnum, sizeof(iint));
    iint *globalStarts = (iint*) calloc(size+1, sizeof(iint));
    
    iint *sendCounts  = (iint*) calloc(size, sizeof(iint));
    iint *sendOffsets = (iint*) calloc(size+1, sizeof(iint));
    iint *recvCounts  = (iint*) calloc(size, sizeof(iint));
    iint *recvOffsets = (iint*) calloc(size+1, sizeof(iint));

    iint *sendSortId = (iint *) calloc(Nnum,sizeof(iint));
    iint *globalSortId;
    iint *compressId;

    iint *globalNumbering = (iint*) calloc(Nnum, sizeof(iint));

    for (iint n=0;n<Nnum;n++) {
      iint id = mesh->gatherLocalIds[n]; 
      globalNumbering[id] = mesh->gatherBaseIds[n];
    }

    // squeeze node numbering
    meshParallelConsecutiveGlobalNumbering(Nnum, globalNumbering, globalOwners, globalStarts,
                                           sendSortId, &globalSortId, &compressId,
                                           sendCounts, sendOffsets, recvCounts, recvOffsets);
    
    // 2. Build non-zeros of stiffness matrix (unassembled)
    iint nnz = mesh->Np*mesh->Np*mesh->Nelements;
    iint   *rows = (iint*) calloc(nnz, sizeof(iint));
    iint   *cols = (iint*) calloc(nnz, sizeof(iint));
    dfloat *vals = (dfloat*) calloc(nnz, sizeof(dfloat));

    nonZero_t *sendNonZeros = (nonZero_t*) calloc(nnz, sizeof(nonZero_t));
    iint *AsendCounts  = (iint*) calloc(size, sizeof(iint));
    iint *ArecvCounts  = (iint*) calloc(size, sizeof(iint));
    iint *AsendOffsets = (iint*) calloc(size+1, sizeof(iint));
    iint *ArecvOffsets = (iint*) calloc(size+1, sizeof(iint));
      
    //build element stiffness matrices
    dfloat *Srr = (dfloat *) calloc(mesh->Np*mesh->Np,sizeof(dfloat));
    dfloat *Srs = (dfloat *) calloc(mesh->Np*mesh->Np,sizeof(dfloat));
    dfloat *Ssr = (dfloat *) calloc(mesh->Np*mesh->Np,sizeof(dfloat));
    dfloat *Sss = (dfloat *) calloc(mesh->Np*mesh->Np,sizeof(dfloat));
    for (iint n=0;n<mesh->Np;n++) {
      for (iint m=0;m<mesh->Np;m++) {
        Srr[m+n*mesh->Np] = 0.;
        Srs[m+n*mesh->Np] = 0.;
        Ssr[m+n*mesh->Np] = 0.;
        Sss[m+n*mesh->Np] = 0.;

        for (iint k=0;k<mesh->Np;k++) {
          for (iint l=0;l<mesh->Np;l++) {
            Srr[m+n*mesh->Np] += mesh->Dr[n+k*mesh->Np]*mesh->MM[k+l*mesh->Np]*mesh->Dr[m+k*mesh->Np];
            Srs[m+n*mesh->Np] += mesh->Dr[n+k*mesh->Np]*mesh->MM[k+l*mesh->Np]*mesh->Ds[m+k*mesh->Np];
            Ssr[m+n*mesh->Np] += mesh->Ds[n+k*mesh->Np]*mesh->MM[k+l*mesh->Np]*mesh->Dr[m+k*mesh->Np];
            Sss[m+n*mesh->Np] += mesh->Ds[n+k*mesh->Np]*mesh->MM[k+l*mesh->Np]*mesh->Ds[m+k*mesh->Np];
          }
        } 
      }
    }

    //Build unassembed non-zeros
    printf("Building full matrix system\n");
    iint cnt =0;
    for (iint e=0;e<mesh->Nelements;e++) {
      for (iint n=0;n<mesh->Np;n++) {
        for (iint m=0;m<mesh->Np;m++) {
          dfloat val = 0.;

          dfloat rx = mesh->vgeo[e*mesh->Nvgeo + RXID];
          dfloat sx = mesh->vgeo[e*mesh->Nvgeo + SXID];
          dfloat ry = mesh->vgeo[e*mesh->Nvgeo + RYID];
          dfloat sy = mesh->vgeo[e*mesh->Nvgeo + SYID];
          dfloat J  = mesh->vgeo[e*mesh->Nvgeo +  JID];

          val += J*(rx*rx+ry*ry)*Srr[m+n*mesh->Np];
          val += J*(rx*sx+ry*sy)*Srs[m+n*mesh->Np];
          val += J*(sx*rx+sy*ry)*Ssr[m+n*mesh->Np];
          val += J*(sx*sx+sy*sy)*Sss[m+n*mesh->Np];
          val += J*lambda*mesh->MM[m+n*mesh->Np];

          vals[cnt] = val;
          rows[cnt] = e*mesh->Np + n;
          cols[cnt] = e*mesh->Np + m;

          // pack non-zero
          sendNonZeros[cnt].val = val;
          sendNonZeros[cnt].row = globalNumbering[e*mesh->Np + n];
          sendNonZeros[cnt].col = globalNumbering[e*mesh->Np + m];
          sendNonZeros[cnt].ownerRank = globalOwners[e*mesh->Np + n];
          cnt++;
        }
      }
    }

    // count how many non-zeros to send to each process
    for(iint n=0;n<cnt;++n)
      AsendCounts[sendNonZeros[n].ownerRank] += sizeof(nonZero_t);

    // sort by row ordering
    qsort(sendNonZeros, cnt, sizeof(nonZero_t), parallelCompareRowColumn);
    
    // find how many nodes to expect (should use sparse version)
    MPI_Alltoall(AsendCounts, 1, MPI_IINT, ArecvCounts, 1, MPI_IINT, MPI_COMM_WORLD);

    // find send and recv offsets for gather
    iint recvNtotal = 0;
    for(iint r=0;r<size;++r){
      AsendOffsets[r+1] = AsendOffsets[r] + AsendCounts[r];
      ArecvOffsets[r+1] = ArecvOffsets[r] + ArecvCounts[r];
      recvNtotal += ArecvCounts[r]/sizeof(nonZero_t);
    }

    nonZero_t *recvNonZeros = (nonZero_t*) calloc(recvNtotal, sizeof(nonZero_t));
    
    // determine number to receive
    MPI_Alltoallv(sendNonZeros, AsendCounts, AsendOffsets, MPI_CHAR,
      recvNonZeros, ArecvCounts, ArecvOffsets, MPI_CHAR,
      MPI_COMM_WORLD);

    // sort received non-zero entries by row block (may need to switch compareRowColumn tests)
    qsort(recvNonZeros, recvNtotal, sizeof(nonZero_t), parallelCompareRowColumn);

    // compress duplicates
    cnt = 0;
    for(iint n=1;n<recvNtotal;++n){
      if(recvNonZeros[n].row == recvNonZeros[cnt].row &&
         recvNonZeros[n].col == recvNonZeros[cnt].col){
        recvNonZeros[cnt].val += recvNonZeros[n].val;
      }
      else{
        ++cnt;
        recvNonZeros[cnt] = recvNonZeros[n];
      }
    }
    recvNtotal = cnt+1;

    iint *recvRows = (iint *) calloc(recvNtotal,sizeof(iint));
    iint *recvCols = (iint *) calloc(recvNtotal,sizeof(iint));
    dfloat *recvVals = (dfloat *) calloc(recvNtotal,sizeof(dfloat));
    
    for (iint n=0;n<recvNtotal;n++) {
      recvRows[n] = recvNonZeros[n].row;
      recvCols[n] = recvNonZeros[n].col;
      recvVals[n] = recvNonZeros[n].val;
    }
    
    //collect global assembled matrix
    iint *globalnnz       = (iint *) calloc(size  ,sizeof(iint));
    iint *globalnnzOffset = (iint *) calloc(size+1,sizeof(iint));
    MPI_Allgather(&recvNtotal, 1, MPI_IINT, 
                  globalnnz, 1, MPI_IINT, MPI_COMM_WORLD);
    globalnnzOffset[0] = 0;
    for (iint n=0;n<size;n++)
      globalnnzOffset[n+1] = globalnnzOffset[n]+globalnnz[n];

    iint globalnnzTotal = globalnnzOffset[size];

    iint *globalRecvCounts  = (iint *) calloc(size,sizeof(iint));
    iint *globalRecvOffsets = (iint *) calloc(size,sizeof(iint));
    for (iint n=0;n<size;n++){
      globalRecvCounts[n] = globalnnz[n]*sizeof(nonZero_t);
      globalRecvOffsets[n] = globalnnzOffset[n]*sizeof(nonZero_t);
    }
    nonZero_t *globalNonZero = (nonZero_t*) calloc(globalnnzTotal, sizeof(nonZero_t));

    MPI_Allgatherv(recvNonZeros, recvNtotal*sizeof(nonZero_t), MPI_CHAR, 
                  globalNonZero, globalRecvCounts, globalRecvOffsets, MPI_CHAR, MPI_COMM_WORLD);
    

    iint *globalIndex = (iint *) calloc(globalnnzTotal, sizeof(iint));
    iint *globalRows = (iint *) calloc(globalnnzTotal, sizeof(iint));
    iint *globalCols = (iint *) calloc(globalnnzTotal, sizeof(iint));
    dfloat *globalVals = (dfloat*) calloc(globalnnzTotal,sizeof(dfloat));

    for (iint n=0;n<globalnnzTotal;n++) {
      globalRows[n] = globalNonZero[n].row;
      globalCols[n] = globalNonZero[n].col;
      globalVals[n] = globalNonZero[n].val;
    }

    precon->parAlmond = almondGlobalSetup(mesh->device, 
                                         Nnum, 
                                         globalStarts, 
                                         globalnnzTotal,      
                                         globalRows,        
                                         globalCols,        
                                         globalVals,
                                         sendSortId, 
                                         globalSortId, 
                                         compressId,
                                         sendCounts, 
                                         sendOffsets, 
                                         recvCounts, 
                                         recvOffsets,    
                                         0);             // 0 if no null space

    precon->o_r1 = mesh->device.malloc(Nnum*sizeof(dfloat));
    precon->o_z1 = mesh->device.malloc(Nnum*sizeof(dfloat));
    precon->r1 = (dfloat*) malloc(Nnum*sizeof(dfloat));
    precon->z1 = (dfloat*) malloc(Nnum*sizeof(dfloat));
    
    free(AsendCounts);
    free(ArecvCounts);
    free(AsendOffsets);
    free(ArecvOffsets);

    if(strstr(options, "UBERGRID")){
      iint coarseTotal;

      iint* coarseNp      = (iint *) calloc(size,sizeof(iint));
      iint* coarseOffsets = (iint *) calloc(size+1,sizeof(iint));

      iint nnz2;
      iint *globalNumbering2;
      iint *rowsA2;
      iint *colsA2;
      dfloat *valsA2;  

      almondGlobalCoarseSetup(precon->parAlmond,coarseNp,coarseOffsets,&globalNumbering2,
                              &nnz2,&rowsA2,&colsA2, &valsA2);

      precon->coarseNp = coarseNp[rank];
      precon->coarseTotal = coarseOffsets[size];
      coarseTotal = coarseOffsets[size];
      precon->coarseOffsets = coarseOffsets;

      // need to create numbering for really coarse grid on each process for xxt
      precon->xxt2 = xxtSetup(coarseTotal,
          globalNumbering2,
          nnz2,
          rowsA2,
          colsA2,
          valsA2,
          0,
          iintString,
          dfloatString);

      almondSetCoarseSolve(precon->parAlmond, xxtSolve,precon->xxt2,
                      precon->coarseTotal,
                      precon->coarseOffsets[rank]);

      printf("Done UberCoarse setup\n"); 
    }
  }
  
  return precon;
}
