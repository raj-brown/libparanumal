#include "bns.h"
void bnsRestartWrite(bns_t *bns, setupAide &options, dfloat time){

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  mesh_t *mesh = bns->mesh; 
  // Copy Field To Host
  bns->o_q.copyTo(bns->q);

  // Create Binary File Name
  char fname[BUFSIZ];
  string outName;
  options.getArgs("RESTART FILE NAME", outName);
  sprintf(fname, "%s_%04d.dat",(char*)outName.c_str(), rank);
  FILE *fp;
  
  // Overwrite  Binary File
  fp = fopen(fname,"wb");

  // Write q1....qN and all pml variables
  dfloat *elmField  = (dfloat *) calloc(bns->Nfields, sizeof(dfloat)); 
  dfloat *pmlField  = (dfloat *) calloc(bns->Nfields*bns->dim, sizeof(dfloat));

  // First Write the Solution Time
  fwrite(&time, sizeof(dfloat), 1, fp);
  // Write output frame to prevent overwriting vtu files
  fwrite(&bns->frame, sizeof(int), 1, fp);

  // 
 if(options.compareArgs("TIME INTEGRATOR", "LSERK") || 
    options.compareArgs("TIME INTEGRATOR", "SARK")){
  // Write only q works check for MRAB, write history
  for(dlong e = 0; e<mesh->Nelements; e++){
    for(int n=0; n<mesh->Np; n++ ){
      const dlong id = e*bns->Nfields*mesh->Np + n; 
      for(int fld=0; fld<bns->Nfields; fld++){
        elmField[fld] =  bns->q[id + fld*mesh->Np];
      }
      fwrite(elmField, sizeof(dfloat), bns->Nfields, fp);
    }
  }

  if(bns->pmlFlag){
    // Copy Field To Host
    bns->o_pmlqx.copyTo(bns->pmlqx);
    bns->o_pmlqy.copyTo(bns->pmlqy);
    if(bns->dim==3)
      bns->o_pmlqz.copyTo(bns->pmlqz);

    // Write node data
    for(dlong es = 0; es<mesh->pmlNelements; es++)
    {
      dlong e      = mesh->pmlElementIds[es]; 
      dlong pmlId  = mesh->pmlIds[es]; 
      for(int n=0; n<mesh->Np; n++ ){
        const dlong id = pmlId*bns->Nfields*mesh->Np + n;
        for(int fld=0; fld<bns->Nfields; fld++){
          pmlField[fld + 0*bns->Nfields] =  bns->pmlqx[id + fld*mesh->Np];
          pmlField[fld + 1*bns->Nfields] =  bns->pmlqy[id + fld*mesh->Np];
          if(bns->dim==3)
            pmlField[fld + 2*bns->Nfields] =  bns->pmlqz[id + fld*mesh->Np];
        }
      fwrite(pmlField, sizeof(dfloat), bns->Nfields*bns->dim, fp);
      }
    }
  }
}

#if 0
  // Assumes the same dt, there is no interpolation of history
  // will be mofied later if the time step size changes 
   if(options.compareArgs("TIME INTEGRATOR", "MRSAAB")){
   // Keep the same index to prvent order loss at restart
    for(int l = 0; l<mesh->MRABNlevels; l++)
      fwrite(&mesh->MRABshiftIndex[l], sizeof(int),1, fp);

      // Copy right hand side history
      bns->o_rhsq.copyTo(bns->rhsq);

      const dlong offset   = mesh->Nelements*mesh->Np*bns->Nfields; 

      // Write all history, order is not important 
      // as long as reading with the same order
      for(int nrhs = 0; nrhs<bns->Nrhs; nrhs++){
        for(dlong e = 0; e<mesh->Nelements; e++){
          for(int n=0; n<mesh->Np; n++ ){
            const dlong id = e*bns->Nfields*mesh->Np + n + nrhs*offset; 
            for(int fld=0; fld<bns->Nfields; fld++){
              elmField[fld] =  bns->q[id + fld*mesh->Np];
            }
          fwrite(elmField, sizeof(dfloat), bns->Nfields, fp);
          }
        }
      }

    if(bns->pmlFlag){
      // Copy Field To Host
      bns->o_pmlrhsqx.copyTo(bns->pmlrhsqx);
      bns->o_pmlrhsqy.copyTo(bns->pmlrhsqy);
      if(bns->dim==3)
        bns->o_pmlrhsqz.copyTo(bns->pmlrhsqz);

      const dlong pmloffset = mesh->pmlNelements*mesh->Np*bns->Nfields; 

      // Write node data
      for(int nrhs = 0; nrhs<bns->Nrhs; nrhs++){
        for(dlong es = 0; es<mesh->pmlNelements; es++){
          dlong e      = mesh->pmlElementIds[es]; 
          dlong pmlId  = mesh->pmlIds[es]; 
          for(int n=0; n<mesh->Np; n++ ){
            const dlong id = pmlId*bns->Nfields*mesh->Np + n + nrhs*pmloffset;
            for(int fld=0; fld<bns->Nfields; fld++){
              pmlField[fld + 0*bns->Nfields] =  bns->pmlrhsqx[id + fld*mesh->Np];
              pmlField[fld + 1*bns->Nfields] =  bns->pmlrhsqy[id + fld*mesh->Np];
              if(bns->dim==3)
                pmlField[fld + 2*bns->Nfields] =  bns->pmlrhsqz[id + fld*mesh->Np];
              }
            fwrite(pmlField, sizeof(dfloat), bns->Nfields*bns->dim, fp);
          }
        }
      }
    }
   }
#endif

  fclose(fp); 
}




void bnsRestartRead(bns_t *bns, setupAide &options){

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  mesh_t *mesh = bns->mesh; 
  // Create Binary File Name
  char fname[BUFSIZ];
  string outName;
  options.getArgs("RESTART FILE NAME", outName);
  sprintf(fname, "%s_%04d.dat",(char*)outName.c_str(), rank);
  FILE *fp; 

  // Overwrite  Binary File
  fp = fopen(fname,"rb");

  if(fp != NULL){
  
    // Write q1....qN and all pml variables
    dfloat *elmField  = (dfloat *) calloc(bns->Nfields, sizeof(dfloat)); 
    dfloat *pmlField  = (dfloat *) calloc(bns->Nfields*bns->dim, sizeof(dfloat));

    dfloat startTime = 0.0; 
    // Update Start Time
    fread(&startTime, sizeof(dfloat),1, fp);
    // Update frame number to contioune outputs
    fread(&bns->frame, sizeof(int), 1, fp);


    if(rank==0) printf("Restart time: %.4e \n", startTime);

    // Write only q works check for MRAB, write history
    for(dlong e = 0; e<mesh->Nelements; e++){
      for(int n=0; n<mesh->Np; n++ ){
        
        const dlong id = e*bns->Nfields*mesh->Np + n;
        fread(elmField, sizeof(dfloat), bns->Nfields, fp);
        
        for(int fld=0; fld<bns->Nfields; fld++){
          bns->q[id + fld*mesh->Np] = elmField[fld];
        }

      }
    }

    if(bns->pmlFlag){
      
      // Write node data
      for(dlong es = 0; es<mesh->pmlNelements; es++)
      {
        dlong e      = mesh->pmlElementIds[es]; 
        dlong pmlId  = mesh->pmlIds[es]; 
        for(int n=0; n<mesh->Np; n++ ){
          const dlong id = pmlId*bns->Nfields*mesh->Np + n;

          fread(pmlField, sizeof(dfloat), bns->Nfields*bns->dim, fp);
          
          for(int fld=0; fld<bns->Nfields; fld++){          
            bns->pmlqx[id + fld*mesh->Np] = pmlField[fld + 0*bns->Nfields];
            bns->pmlqy[id + fld*mesh->Np] = pmlField[fld + 1*bns->Nfields];
            if(bns->dim==3)
              bns->pmlqz[id + fld*mesh->Np] = pmlField[fld + 2*bns->Nfields];
          }
        }
      } 
    }

  fclose(fp);

  // Update Fields
    bns->o_q.copyFrom(bns->q);

    if(bns->pmlFlag){
      bns->o_pmlqx.copyFrom(bns->pmlqx);
      bns->o_pmlqy.copyFrom(bns->pmlqy);
      if(bns->dim==3)
        bns->o_pmlqz.copyFrom(bns->pmlqz);    
    }

  // Just Update Time Step Size
  bns->startTime = startTime; 
  // Update NtimeSteps and dt
   bns->NtimeSteps = (bns->finalTime-bns->startTime)/bns->dt;
   bns->dt         = (bns->finalTime-bns->startTime)/bns->NtimeSteps; 
}
}