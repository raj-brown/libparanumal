#include "ellipticQuad2D.h"


void ellipticStartHaloExchange2D(mesh2D *mesh, occa::memory &o_q, dfloat *sendBuffer, dfloat *recvBuffer);

void ellipticEndHaloExchange2D(mesh2D *mesh, occa::memory &o_q, dfloat *recvBuffer);

void ellipticParallelGatherScatterQuad2D(mesh2D *mesh, ogs_t *ogs, occa::memory &o_q, occa::memory &o_gsq, const char *type, const char *op);

void ellipticOperator2D(solver_t *solver, dfloat lambda,
      occa::memory &o_q, occa::memory &o_Aq, const char *options){

  mesh_t *mesh = solver->mesh;

  occaTimerTic(mesh->device,"AxKernel");

  dfloat *sendBuffer = solver->sendBuffer;
  dfloat *recvBuffer = solver->recvBuffer;

  if(strstr(options, "CONTINUOUS")){
    // compute local element operations and store result in o_Aq
    solver->AxKernel(mesh->Nelements, mesh->o_ggeo, mesh->o_D, lambda, o_q, o_Aq);

  } else{

    ellipticStartHaloExchange2D(mesh, o_q, sendBuffer, recvBuffer);

    ellipticEndHaloExchange2D(mesh, o_q, recvBuffer);

    // need start/end elements then can split into two parts
    iint allNelements = mesh->Nelements+mesh->totalHaloPairs;
    solver->gradientKernel(allNelements, mesh->o_vgeo, mesh->o_D, o_q, solver->o_grad);

    solver->ipdgKernel(mesh->Nelements,
         mesh->o_vmapM,
         mesh->o_vmapP,
         lambda,
         solver->tau,
         mesh->o_vgeo,
         mesh->o_sgeo,
         mesh->o_EToB,
         mesh->o_D,
         solver->o_grad,
         o_Aq);

  }

  if(strstr(options, "CONTINUOUS")||strstr(options, "PROJECT"))
    // parallel gather scatter
    ellipticParallelGatherScatterQuad2D(mesh, solver->ogs, o_Aq, o_Aq, dfloatString, "add");

  occaTimerToc(mesh->device,"AxKernel");
}

void ellipticMatrixFreeAx(void **args, occa::memory o_q, occa::memory o_Aq, const char* options) {

  solver_t *solver = (solver_t *) args[0];
  dfloat  *lambda  = (dfloat *)  args[1];

  mesh_t *mesh = solver->mesh;
  dfloat *sendBuffer = solver->sendBuffer;
  dfloat *recvBuffer = solver->recvBuffer;

  if(strstr(options, "CONTINUOUS")){
    // compute local element operations and store result in o_Aq
    solver->AxKernel(mesh->Nelements, mesh->o_ggeo, mesh->o_D, lambda, o_q, o_Aq);

  } else{

    ellipticStartHaloExchange2D(mesh, o_q, sendBuffer, recvBuffer);

    ellipticEndHaloExchange2D(mesh, o_q, recvBuffer);

    // need start/end elements then can split into two parts
    iint allNelements = mesh->Nelements+mesh->totalHaloPairs;
    solver->gradientKernel(allNelements, mesh->o_vgeo, mesh->o_D, o_q, solver->o_grad);

    solver->ipdgKernel(mesh->Nelements,
         mesh->o_vmapM,
         mesh->o_vmapP,
         lambda,
         solver->tau,
         mesh->o_vgeo,
         mesh->o_sgeo,
         mesh->o_EToB,
         mesh->o_D,
         solver->o_grad,
         o_Aq);

  }
}

void ellipticScaledAdd(solver_t *solver, dfloat alpha, occa::memory &o_a, dfloat beta, occa::memory &o_b){

  mesh_t *mesh = solver->mesh;

  iint Ntotal = mesh->Nelements*mesh->Np;

  // b[n] = alpha*a[n] + beta*b[n] n\in [0,Ntotal)
  occaTimerTic(mesh->device,"scaledAddKernel");
  solver->scaledAddKernel(Ntotal, alpha, o_a, beta, o_b);
  occaTimerToc(mesh->device,"scaledAddKernel");
}

dfloat ellipticWeightedInnerProduct(solver_t *solver,
            occa::memory &o_w,
            occa::memory &o_a,
            occa::memory &o_b,
            const char *options){

  mesh_t *mesh = solver->mesh;
  dfloat *tmp = solver->tmp;
  iint Nblock = solver->Nblock;
  iint Ntotal = mesh->Nelements*mesh->Np;

  occa::memory &o_tmp = solver->o_tmp;

  occaTimerTic(mesh->device,"weighted inner product2");

  if(strstr(options,"CONTINUOUS")||strstr(options, "PROJECT"))
    solver->weightedInnerProduct2Kernel(Ntotal, o_w, o_a, o_b, o_tmp);
  else
    solver->innerProductKernel(Ntotal, o_a, o_b, o_tmp);

  occaTimerToc(mesh->device,"weighted inner product2");

  o_tmp.copyTo(tmp);

  dfloat wab = 0;
  for(iint n=0;n<Nblock;++n){
    wab += tmp[n];
  }

  dfloat globalwab = 0;
  MPI_Allreduce(&wab, &globalwab, 1, MPI_DFLOAT, MPI_SUM, MPI_COMM_WORLD);

  return globalwab;
}

dfloat ellipticLocalInnerProduct(solver_t *solver,
         occa::memory &o_a,
         occa::memory &o_b){

  mesh_t *mesh = solver->mesh;
  dfloat *tmp = solver->tmp;
  iint Nblock = solver->Nblock;
  iint Ntotal = mesh->Nelements*mesh->Np;

  occa::memory &o_tmp = solver->o_tmp;

  occaTimerTic(mesh->device,"inner product2");
  solver->innerProductKernel(Ntotal, o_a, o_b, o_tmp);
  occaTimerToc(mesh->device,"inner product2");

  o_tmp.copyTo(tmp);

  dfloat ab = 0;
  for(iint n=0;n<Nblock;++n)
    ab += tmp[n];

  return ab;
}

void ellipticPreconditioner2D(solver_t *solver,
            dfloat lambda,
            occa::memory &o_r,
            occa::memory &o_zP,
            occa::memory &o_z,
            const char *options){

  mesh_t *mesh = solver->mesh;
  precon_t *precon = solver->precon;
  ogs_t    *ogs = solver->ogs; // C0 Gather ScatterTri info

  dfloat *sendBuffer = solver->sendBuffer;
  dfloat *recvBuffer = solver->recvBuffer;


  if(strstr(options, "OAS")){
    //L2 project weighting
    if(strstr(options,"CONTINUOUS")||strstr(options,"PROJECT")) {
      ellipticParallelGatherScatterQuad2D(mesh,ogs,o_r,o_r,dfloatString,"add");
      solver->dotMultiplyKernel(mesh->Np*mesh->Nelements,mesh->o_projectL2,o_r,o_r);
    }

    ellipticStartHaloExchange2D(mesh, o_r, sendBuffer, recvBuffer);

    ellipticEndHaloExchange2D(mesh, o_r, recvBuffer);

    // compute local precon on DEVICE
    occaTimerTic(mesh->device,"OASpreconKernel");
    if(strstr(options, "CONTINUOUS")||strstr(options,"PROJECT")) {
      precon->preconKernel(mesh->Nelements,
         precon->o_vmapPP,
         precon->o_faceNodesP,
         precon->o_oasForward,
         precon->o_oasDiagInvOp,
         precon->o_oasBack,
         o_r,
         o_zP);
      ellipticParallelGatherScatterQuad2D(mesh, precon->ogsP, o_zP, o_zP, dfloatString, "add");
      solver->dotMultiplyKernel(mesh->NqP*mesh->NqP*mesh->Nelements,precon->o_invDegreeP,o_zP,o_zP);
    }
    else{
      precon->preconKernel(mesh->Nelements,
         mesh->o_vmapP,
         precon->o_faceNodesP,
         precon->o_oasForwardDg,
         precon->o_oasDiagInvOpDg,
         precon->o_oasBackDg,
         o_r,
         o_zP);
      ellipticParallelGatherScatterQuad2D(mesh, precon->ogsDg, o_zP, o_zP, dfloatString, "add");
      solver->dotMultiplyKernel(mesh->NqP*mesh->NqP*mesh->Nelements,precon->o_invDegreeDGP,o_zP,o_zP);
    }
    occaTimerToc(mesh->device,"OASpreconKernel");

    // extract block interiors on DEVICE
    occaTimerTic(mesh->device,"restrictKernel");
    precon->restrictKernel(mesh->Nelements, o_zP, o_z);
    occaTimerToc(mesh->device,"restrictKernel");

    if(strstr(options, "COARSEGRID")){ // should split into two parts
      occaTimerTic(mesh->device,"coarseGrid");

      // Z1*Z1'*PL1*(Z1*z1) = (Z1*rL)  HMMM
      occaTimerTic(mesh->device,"coarsenKernel");
      precon->coarsenKernel(mesh->Nelements, precon->o_V1, o_r, precon->o_r1);
      occaTimerToc(mesh->device,"coarsenKernel");

      if(strstr(options,"XXT")){
        precon->o_r1.copyTo(precon->r1);
        xxtSolve(precon->z1, precon->xxt,precon->r1);
        precon->o_z1.copyFrom(precon->z1);
      }

      if(strstr(options,"ALMOND")){
        occaTimerTic(mesh->device,"parAlmond");
        parAlmondPrecon(precon->o_z1, precon->parAlmond, precon->o_r1);
        occaTimerToc(mesh->device,"parAlmond");
      }

      occaTimerTic(mesh->device,"prolongateKernel");
      precon->prolongateKernel(mesh->Nelements, precon->o_V1, precon->o_z1, solver->o_res);
      occaTimerToc(mesh->device,"prolongateKernel");

      // do we have to DG gatherscatter here
      dfloat one = 1.;
      ellipticScaledAdd(solver, one, solver->o_res, one, o_z);
      occaTimerToc(mesh->device,"coarseGrid");
    }

    //project weighting
    if(strstr(options,"CONTINUOUS")||strstr(options,"PROJECT")) {
      solver->dotMultiplyKernel(mesh->Np*mesh->Nelements,mesh->o_projectL2,o_z,o_z);
      ellipticParallelGatherScatterQuad2D(mesh, ogs, o_z, o_z, dfloatString, "add");
    }
  } else if(strstr(options, "OMS")){
    //L2 project weighting
    if(strstr(options,"CONTINUOUS")||strstr(options,"PROJECT")) {
      ellipticParallelGatherScatterQuad2D(mesh,ogs,o_r,o_r,dfloatString,"add");
      solver->dotMultiplyKernel(mesh->Np*mesh->Nelements,mesh->o_projectL2,o_r,o_r);
    }

    ellipticStartHaloExchange2D(mesh, o_r, sendBuffer, recvBuffer);

    ellipticEndHaloExchange2D(mesh, o_r, recvBuffer);

    // compute local precon on DEVICE
    occaTimerTic(mesh->device,"OMSpreconKernel");
    if(strstr(options, "CONTINUOUS")||strstr(options,"PROJECT")) {
      precon->preconKernel(mesh->Nelements,
         precon->o_vmapPP,
         precon->o_faceNodesP,
         precon->o_oasForward,
         precon->o_oasDiagInvOp,
         precon->o_oasBack,
         o_r,
         o_zP);
      ellipticParallelGatherScatterQuad2D(mesh, precon->ogsP, o_zP, o_zP, dfloatString, "add");
      solver->dotMultiplyKernel(mesh->NqP*mesh->NqP*mesh->Nelements,precon->o_invDegreeP,o_zP,o_zP);
    }
    else{
      precon->preconKernel(mesh->Nelements,
         mesh->o_vmapP,
         precon->o_faceNodesP,
         precon->o_oasForwardDg,
         precon->o_oasDiagInvOpDg,
         precon->o_oasBackDg,
         o_r,
         o_zP);
      ellipticParallelGatherScatterQuad2D(mesh, precon->ogsDg, o_zP, o_zP, dfloatString, "add");
      solver->dotMultiplyKernel(mesh->NqP*mesh->NqP*mesh->Nelements,precon->o_invDegreeDGP,o_zP,o_zP);
    }
    occaTimerToc(mesh->device,"OMSpreconKernel");

    // extract the degrees of freedom
    occaTimerTic(mesh->device,"restrictKernel");
    precon->restrictKernel(mesh->Nelements, o_zP, o_z);
    occaTimerToc(mesh->device,"restrictKernel");

    dfloat one = 1.; dfloat mone = -1.;
    if(strstr(options, "COARSEGRID")){
      occaTimerTic(mesh->device,"coarseGrid");
      //res = r-Az
      ellipticOperator2D(solver, lambda, o_z, solver->o_res, options);
      ellipticScaledAdd(solver, one, o_r, mone, solver->o_res);

      // restrict to coarse problem
      occaTimerTic(mesh->device,"coarsenKernel");
      precon->coarsenKernel(mesh->Nelements, precon->o_V1, solver->o_res, precon->o_r1);
      occaTimerToc(mesh->device,"coarsenKernel");

      occaTimerTic(mesh->device,"ALMOND");
      parAlmondPrecon(precon->o_z1, precon->parAlmond, precon->o_r1);
      occaTimerToc(mesh->device,"ALMOND");

      // prolongate back to fine problem
      occaTimerTic(mesh->device,"prolongateKernel");
      precon->prolongateKernel(mesh->Nelements, precon->o_V1, precon->o_z1, solver->o_res);
      occaTimerToc(mesh->device,"prolongateKernel");
      ellipticScaledAdd(solver, one, solver->o_res, one, o_z);
    }

    //do another fine smoothing
    //res = r-Az
    ellipticOperator2D(solver, lambda, o_z, solver->o_res, options);
    ellipticScaledAdd(solver, one, o_r, mone, solver->o_res);

    //L2 project weighting
    if(strstr(options,"CONTINUOUS")||strstr(options,"PROJECT")) {
      ellipticParallelGatherScatterQuad2D(mesh,ogs,solver->o_res,solver->o_res,dfloatString,"add");
      solver->dotMultiplyKernel(mesh->Np*mesh->Nelements,mesh->o_projectL2,solver->o_res,solver->o_res);
    }

    // compute local precon on DEVICE
    occaTimerTic(mesh->device,"OMSpreconKernel");
    if(strstr(options, "CONTINUOUS")||strstr(options,"PROJECT")) {
      precon->preconKernel(mesh->Nelements,
         precon->o_vmapPP,
         precon->o_faceNodesP,
         precon->o_oasForward,
         precon->o_oasDiagInvOp,
         precon->o_oasBack,
         solver->o_res,
         o_zP);
      ellipticParallelGatherScatterQuad2D(mesh, precon->ogsP, o_zP, o_zP, dfloatString, "add");
      solver->dotMultiplyKernel(mesh->NqP*mesh->NqP*mesh->Nelements,precon->o_invDegreeP,o_zP,o_zP);
    }
    else{
      precon->preconKernel(mesh->Nelements,
         mesh->o_vmapP,
         precon->o_faceNodesP,
         precon->o_oasForwardDg,
         precon->o_oasDiagInvOpDg,
         precon->o_oasBackDg,
         solver->o_res,
         o_zP);
      ellipticParallelGatherScatterQuad2D(mesh, precon->ogsDg, o_zP, o_zP, dfloatString, "add");
      solver->dotMultiplyKernel(mesh->NqP*mesh->NqP*mesh->Nelements,precon->o_invDegreeDGP,o_zP,o_zP);
    }
    occaTimerToc(mesh->device,"OMSpreconKernel");

    // extract the degrees of freedom
    occaTimerTic(mesh->device,"restrictKernel");
    precon->restrictKernel(mesh->Nelements, o_zP, solver->o_res);
    occaTimerToc(mesh->device,"restrictKernel");

    ellipticScaledAdd(solver, one, solver->o_res, one, o_z);

  } else if (strstr(options, "FULLALMOND")) {

    occaTimerTic(mesh->device,"parALMOND");
    parAlmondPrecon(o_z, precon->parAlmond, o_r);
    occaTimerToc(mesh->device,"parALMOND");

  } else if(strstr(options, "JACOBI")){

    iint Ntotal = mesh->Np*mesh->Nelements;
    // Jacobi preconditioner
    occaTimerTic(mesh->device,"dotDivideKernel");
    mesh->dotDivideKernel(Ntotal, o_r, precon->o_diagA, o_z);
    occaTimerToc(mesh->device,"dotDivideKernel");
  }
  else {// turn off preconditioner
    o_z.copyFrom(o_r);
  }
}


int ellipticSolveQuad2D(solver_t *solver, dfloat lambda, occa::memory &o_r, occa::memory &o_x, const char *options){

  mesh_t *mesh = solver->mesh;

  iint rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  // convergence tolerance (currently absolute)
  const dfloat tol = 1e-8;

  // placeholder conjugate gradient:
  // https://en.wikipedia.org/wiki/Conjugate_gradient_method

  // placeholder preconditioned conjugate gradient
  // https://en.wikipedia.org/wiki/Conjugate_gradient_method#The_preconditioned_conjugate_gradient_method

  occa::memory &o_p  = solver->o_p;
  occa::memory &o_z  = solver->o_z;
  occa::memory &o_zP = solver->o_zP;
  occa::memory &o_Ap = solver->o_Ap;
  occa::memory &o_Ax = solver->o_Ax;

  occaTimerTic(mesh->device,"PCG");

  // gather-scatter rhs
  if(strstr(options, "CONTINUOUS")||strstr(options, "PROJECT"))
    ellipticParallelGatherScatterQuad2D(mesh, solver->ogs, o_r, o_r, dfloatString, "add");

  // compute A*x
  ellipticOperator2D(solver, lambda, o_x, o_Ax, options);

  // subtract r = b - A*x
  ellipticScaledAdd(solver, -1.f, o_Ax, 1.f, o_r);

  dfloat rdotr0 = ellipticWeightedInnerProduct(solver, solver->o_invDegree, o_r, o_r, options);
  iint Niter = 0;
  //sanity check
  if (rdotr0<=(tol*tol)) {
    printf("iter=0 norm(r) = %g\n", sqrt(rdotr0));
    return 0;
  }

  occaTimerTic(mesh->device,"Preconditioner");
  if(strstr(options,"PCG")){

    // Precon^{-1} (b-A*x)
    ellipticPreconditioner2D(solver, lambda, o_r, o_zP, o_z, options); // r => rP => zP => z

    // p = z
    o_p.copyFrom(o_z); // PCG
  }
  else{
    // p = r
    o_p.copyFrom(o_r); // CG
  }
  occaTimerToc(mesh->device,"Preconditioner");


  // dot(r,r)
  dfloat rdotz0 = ellipticWeightedInnerProduct(solver, solver->o_invDegree, o_r, o_z, options);
  dfloat rdotr1 = 0;
  dfloat rdotz1 = 0;
  dfloat pAp = 0;

  dfloat alpha, beta;

  if(rank==0)
    printf("rdotr0 = %g, rdotz0 = %g\n", rdotr0, rdotz0);

  while(rdotr0>(tol*tol)){
    // A*p
    ellipticOperator2D(solver, lambda, o_p, o_Ap, options);

    //diagnostic(Ntotal, o_p, "o_Ap");

    // dot(p,A*p)
    pAp = ellipticWeightedInnerProduct(solver, solver->o_invDegree,o_p, o_Ap, options);

    if(strstr(options,"PCG"))
      // alpha = dot(r,z)/dot(p,A*p)
      alpha = rdotz0/pAp;
    else
      // alpha = dot(r,r)/dot(p,A*p)
      alpha = rdotr0/pAp;

    // x <= x + alpha*p
    ellipticScaledAdd(solver,  alpha, o_p,  1.f, o_x);

    // r <= r - alpha*A*p
    ellipticScaledAdd(solver, -alpha, o_Ap, 1.f, o_r);

    // dot(r,r)
    rdotr1 = ellipticWeightedInnerProduct(solver, solver->o_invDegree, o_r, o_r, options);

    if(rdotr1 < tol*tol) {
      rdotr0 = rdotr1;
      Niter++;
      break;
    }

    occaTimerTic(mesh->device,"Preconditioner");
    if(strstr(options,"PCG")){

      // z = Precon^{-1} r
      ellipticPreconditioner2D(solver, lambda, o_r, o_zP, o_z, options);

      // dot(r,z)
      rdotz1 = ellipticWeightedInnerProduct(solver, solver->o_invDegree, o_r, o_z, options);

      // flexible pcg beta = (z.(-alpha*Ap))/zdotz0
      if(strstr(options,"FLEXIBLE")){
        dfloat zdotAp = ellipticWeightedInnerProduct(solver, solver->o_invDegree, o_z, o_Ap, options);
        beta = -alpha*zdotAp/rdotz0;
      }
      else{
        beta = rdotz1/rdotz0;
      }

      // p = z + beta*p
      ellipticScaledAdd(solver, 1.f, o_z, beta, o_p);

      // switch rdotz0 <= rdotz1
      rdotz0 = rdotz1;
    }
    else{
      beta = rdotr1/rdotr0;

      // p = r + beta*p
      ellipticScaledAdd(solver, 1.f, o_r, beta, o_p);
    }
    occaTimerToc(mesh->device,"Preconditioner");

    // switch rdotr0 <= rdotr1
    rdotr0 = rdotr1;

    if((rank==0)&&(strstr(options,"VERBOSE")))
      printf("iter=%05d pAp = %g norm(r) = %g\n", Niter, pAp, sqrt(rdotr0));

    ++Niter;

  }

  if(rank==0)
    printf("iter=%05d pAp = %g norm(r) = %g\n", Niter, pAp, sqrt(rdotr0));

  occaTimerToc(mesh->device,"PCG");

  occa::printTimer();

  return Niter;

}