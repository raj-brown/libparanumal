#include "massHex3D.h"

void massStartHaloExchange3D(solver_t *solver, occa::memory &o_q, dfloat *sendBuffer, dfloat *recvBuffer){

  mesh3D *mesh = solver->mesh;
  
  // count size of halo for this process
  iint haloBytes = mesh->totalHaloPairs*mesh->Np*sizeof(dfloat);
  iint haloOffset = mesh->Nelements*mesh->Np*sizeof(dfloat);
  
  // extract halo on DEVICE
  if(haloBytes){

    // make sure compute device is ready to perform halo extract
    //    mesh->device.finish();

    // switch to data stream
    //    mesh->device.setStream(solver->dataStream);

    // extract halo on data stream
    mesh->haloExtractKernel(mesh->totalHaloPairs, mesh->Np, mesh->o_haloElementList,
			    o_q, mesh->o_haloBuffer);

    // queue up async copy of halo on data stream
    mesh->o_haloBuffer.copyTo(sendBuffer);
    
    //    mesh->device.setStream(solver->defaultStream);
  }
}

void massInterimHaloExchange3D(solver_t *solver, occa::memory &o_q, dfloat *sendBuffer, dfloat *recvBuffer){

  mesh3D *mesh = solver->mesh;

  // count size of halo for this process
  iint haloBytes = mesh->totalHaloPairs*mesh->Np*sizeof(dfloat);
  iint haloOffset = mesh->Nelements*mesh->Np*sizeof(dfloat);
  
  // extract halo on DEVICE
  if(haloBytes){
    
    // copy extracted halo to HOST
    //    mesh->device.setStream(solver->dataStream);

    // make sure async copy finished
    //    mesh->device.finish(); 
    
    // start halo exchange HOST<>HOST
    meshHaloExchangeStart(mesh,
			  mesh->Np*sizeof(dfloat),
			  sendBuffer,
			  recvBuffer);
    
    //    mesh->device.setStream(solver->defaultStream);

  }
}
    

void massEndHaloExchange3D(solver_t *solver, occa::memory &o_q, dfloat *recvBuffer){

  mesh3D *mesh = solver->mesh;
  
  // count size of halo for this process
  iint haloBytes = mesh->totalHaloPairs*mesh->Np*sizeof(dfloat);
  iint haloOffset = mesh->Nelements*mesh->Np*sizeof(dfloat);
  
  // extract halo on DEVICE
  if(haloBytes){
    // finalize recv on HOST
    meshHaloExchangeFinish(mesh);
    
    // copy into halo zone of o_r  HOST>DEVICE
    //    mesh->device.setStream(solver->dataStream);
    o_q.copyFrom(recvBuffer, haloBytes, haloOffset);
    
    //    mesh->device.setStream(solver->defaultStream);
  }
}