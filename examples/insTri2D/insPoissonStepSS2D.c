#include "ins2D.h"

// complete a time step using LSERK4
void insPoissonStepSS2D(ins_t *ins, iint tstep, iint haloBytes,
				               dfloat * sendBuffer, dfloat * recvBuffer,
				                char   * options){

  mesh2D *mesh = ins->mesh;
  solver_t *solver = ins->pSolver;
  dfloat t = tstep*ins->dt + ins->dt;

  //hard coded for 3 stages.
  int index0 = (ins->index+0)%3; // Still in current time

  iint offset = index0*(mesh->Nelements+mesh->totalHaloPairs);


  // Compute Curl(Curl(U))  Nodal not in weak sense
  ins->poissonRhsCurlKernel(mesh->Nelements,
                        offset,
                        mesh->o_vgeo,
                        mesh->o_DrT,
                        mesh->o_DsT,
                        ins->o_U,
                        ins->o_V,
                        ins->o_rhsU,
                        ins->o_rhsV);


  // Compute derived Neumann data : result is lifted and multiplied with invJ
  ins->poissonRhsNeumannKernel(mesh->Nelements,
                                ins->index,
                                ins->NtotalElements,
                                ins->dt,
                                ins->a0,
                                ins->a1,
                                ins->a2,
                                mesh->o_sgeo,
                                mesh->o_LIFTT,
                                mesh->o_vmapM,
                                mesh->o_vmapP,
                                mesh->o_EToB,
                                t,
                                mesh->o_x,
                                mesh->o_y,
                                ins->o_NU,
                                ins->o_NV,
                                ins->o_rhsU, // holds curl of Vorticity X
                                ins->o_rhsV, // holds curl of Vorticity Y
                                ins->o_WN,   //
                                ins->o_rhsP); 


if(strstr(options,"BROKEN")){
  // compute div(ut) and store on rhsU
  ins->divergenceKernel(mesh->Nelements,
                        mesh->o_vgeo,
                        mesh->o_DrT,
                        mesh->o_DsT,
                        ins->o_Ut,
                        ins->o_Vt,
                        ins->o_rhsU);  
}
  
 // compute all forcing i.e. f^(n+1) - grad(Pr)
  ins->poissonRhsForcingKernel(mesh->Nelements,
                              ins->dt,  
                              ins->g0,
                              mesh->o_vgeo,
                              mesh->o_MM,
                              ins->o_rhsU, // div(Ut)
                              ins->o_rhsP);

 //
  const iint pressure_solve = 1; // Solve for Pressure
  ins->poissonRhsIpdgBCKernel(mesh->Nelements,
                                pressure_solve,
                                mesh->o_vmapM,
                                mesh->o_vmapP,
                                ins->tau,
                                t,
                                ins->dt,
                                mesh->o_x,
                                mesh->o_y,
                                mesh->o_vgeo,
                                mesh->o_sgeo,
                                mesh->o_EToB,
                                mesh->o_DrT,
                                mesh->o_DsT,
                                mesh->o_LIFTT,
                                mesh->o_MM,
                                ins->o_rhsP);




  iint Ntotal = (mesh->Nelements+mesh->totalHaloPairs)*mesh->Np;
  ins->o_PH.copyFrom(ins->o_P,Ntotal*sizeof(dfloat),0,ins->index*Ntotal*sizeof(dfloat));
  iint Niter;
  printf("Solving for Pr: Niter= ");
  Niter= ellipticSolveTri2D(solver, 0.0, ins->o_rhsP, ins->o_PH,  ins->pSolverOptions); 
  printf("%d\n", Niter); 

  int index1 = (ins->index+1)%3; //hard coded for 3 stages
  ins->o_PH.copyTo(ins->o_P,Ntotal*sizeof(dfloat),index1*Ntotal*sizeof(dfloat),0);


if(strstr(options,"BROKEN")){
  // compute div(ut) and store on rhsU
  ins->gradientKernel(mesh->Nelements,
                        mesh->o_vgeo,
                        mesh->o_DrT,
                        mesh->o_DsT,
                        ins->o_PH,
                        ins->o_rhsU, // Store dP/dx
                        ins->o_rhsV  // Store dP/dy
                        );  
}
  

ins->poissonUpdateKernel(mesh->Nelements,
                            ins->dt,
                            ins->g0,
                            ins->o_rhsU,
                            ins->o_rhsV,
                            ins->o_Ut,
                            ins->o_Vt);

}
