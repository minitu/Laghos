// Copyright (c) 2017, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-734707. All Rights
// reserved. See files LICENSE and NOTICE for details.
//
// This file is part of CEED, a collection of benchmarks, miniapps, software
// libraries and APIs for efficient high-order finite element and spectral
// element discretizations for exascale applications. For more information and
// source code availability see http://github.com/ceed.
//
// The CEED research is supported by the Exascale Computing Project 17-SC-20-SC,
// a collaborative effort of two U.S. Department of Energy organizations (Office
// of Science and the National Nuclear Security Administration) responsible for
// the planning and preparation of a capable exascale ecosystem, including
// software, applications, hardware, advanced system engineering and early
// testbed platforms, in support of the nation's exascale computing imperative.

#include "qupdate.hpp"

namespace mfem {

namespace hydrodynamics {

   // **************************************************************************
   __device__ double Det(const size_t dim, const double *J){
      assert(dim==2);
      return J[0] * J[3] - J[1] * J[2];
   }
   
   // **************************************************************************
   __device__ double norml2(const int size, const double *data) {
      if (0 == size) return 0.0;
      if (1 == size) return std::abs(data[0]);
      double scale = 0.0;
      double sum = 0.0;
      for (int i = 0; i < size; i++) {
         if (data[i] != 0.0)
         {
            const double absdata = fabs(data[i]);
            if (scale <= absdata)
            {
               const double sqr_arg = scale / absdata;
               sum = 1.0 + sum * (sqr_arg * sqr_arg);
               scale = absdata;
               continue;
            } // end if scale <= absdata
            const double sqr_arg = absdata / scale;
            sum += (sqr_arg * sqr_arg); // else scale > absdata
         } // end if data[i] != 0
      }
      return scale * sqrt(sum);
   }
   
   // **************************************************************************
   template<const int dim>
   __kernel__ void qkernel(const int nzones,
                           const int nqp,
                           const int nqp1D,
                           const double gamma,
                           const bool use_viscosity,
                           const double h0,
                           const double h1order,
                           const double cfl,
                           const double infinity,
                           
                           const double *weights,
                           const double *Jacobians,
                           const double *rho0DetJ0w,
                           const double *e_quads,
                           const double *grad_v_ext,
                           const double *Jac0inv,
                           double *dt_est,
                           double *stressJinvT){
      double min_detJ = infinity;
#ifdef __NVCC__
      const int _z = blockDim.x * blockIdx.x + threadIdx.x;
      //if (z < nzones)
      if (_z != 0) return;
      for (int z = 0; z < nzones; z++)
#else
      for (int z = 0; z < nzones; z++)
#endif
      {
         // ********************************************************************
         for (int q = 0; q < nqp; q++) {
            const int zdx = z * nqp + q;
            const double weight =  weights[q];
            const double inv_weight = 1. / weight;
            const double *J = Jacobians + zdx*dim*dim;
            const double detJ = Det(dim,J);
            min_detJ = fmin(min_detJ,detJ);
            double Jinv[dim*dim];
            calcInverse2D(dim,J,Jinv);
            // *****************************************************************
            const double rho = inv_weight * rho0DetJ0w[zdx] / detJ;
            const double e   = fmax(0.0, e_quads[zdx]);
            const double p  = (gamma - 1.0) * rho * e;
            const double sound_speed = sqrt(gamma * (gamma-1.0) * e);
            //printf("\n\t[%d,%d] %f %f %f %f",z,q,detJ,rho,p,sound_speed);
            // *****************************************************************
            double stress[dim*dim];
            for (int k = 0; k < dim*dim;k+=1) stress[k] = 0.0;
            for (int d = 0; d < dim; d++) stress[d*dim+d] = -p;
            // *****************************************************************
            double visc_coeff = 0.0;
            if (use_viscosity) {
               // Compression-based length scale at the point. The first
               // eigenvector of the symmetric velocity gradient gives the
               // direction of maximal compression. This is used to define the
               // relative change of the initial length scale.
               const double *dV = grad_v_ext + zdx*dim*dim;
               double sgrad_v[dim*dim];
               mult(dim,dim,dim, dV, Jinv, sgrad_v);
               symmetrize(dim,sgrad_v);
               double eig_val_data[3];
               double eig_vec_data[9];
               if (dim==1) {
                  eig_val_data[0] = sgrad_v[0];
                  eig_vec_data[0] = 1.;
               }
               else {
                  calcEigenvalues(dim, sgrad_v, eig_val_data, eig_vec_data);
               }
               double compr_dir[dim];
               for(int k=0;k<dim;k+=1) compr_dir[k]=eig_vec_data[k];
               // Computes the initial->physical transformation Jacobian.
               double Jpi[dim*dim];
               mult(dim,dim,dim, J, Jac0inv+zdx*dim*dim, Jpi);
               double ph_dir[dim];
               multV(dim, dim, Jpi, compr_dir, ph_dir);
               // Change of the initial mesh size in the compression direction.
               const double h = h0 * norml2(dim,ph_dir) / norml2(dim,compr_dir);
               // Measure of maximal compression.
               const double mu = eig_val_data[0];
               visc_coeff = 2.0 * rho * h * h * fabs(mu);
               if (mu < 0.0) { visc_coeff += 0.5 * rho * h * sound_speed; }
               add(dim, dim, visc_coeff, sgrad_v, stress);
            }
            // Time step estimate at the point. Here the more relevant length
            // scale is related to the actual mesh deformation; we use the min
            // singular value of the ref->physical Jacobian. In addition, the
            // time step estimate should be aware of the presence of shocks.
            const double sv = calcSingularvalue(dim, dim-1, J);
            const double h_min = sv / h1order;
            const double inv_h_min = 1. / h_min;
            const double inv_rho_inv_h_min_sq = inv_h_min * inv_h_min / rho ;
            const double inv_dt = sound_speed * inv_h_min
               + 2.5 * visc_coeff * inv_rho_inv_h_min_sq;
            if (min_detJ < 0.0) {
               // This will force repetition of the step with smaller dt.
               *dt_est = 0.0;
            } else {
               *dt_est = fmin(*dt_est, cfl * (1.0 / inv_dt) );
            }
            // Quadrature data for partial assembly of the force operator.
            double stressJiT[dim*dim];
            multABt(dim, dim, dim, stress, Jinv, stressJiT);
            for(int k=0;k<dim*dim;k+=1) stressJiT[k] *= weight * detJ;
            for (int vd = 0 ; vd < dim; vd++) {
               for (int gd = 0; gd < dim; gd++) {
                  const size_t offset = zdx + nqp*nzones*(gd+vd*dim);
                  stressJinvT[offset] = stressJiT[vd+gd*dim];
               }
            }
         }
      }
   }
   
   // **************************************************************************
   // * Last kernel QUpdate
   // **************************************************************************
   void QUpdate(const int dim,
                const int nzones,
                const int l2dofs_cnt,
                const int h1dofs_cnt,
                const bool use_viscosity,
                const bool p_assembly,
                const double cfl,
                TimingData &timer,
                Coefficient *material_pcf,
                const IntegrationRule &ir,
                ParFiniteElementSpace &H1FESpace,
                ParFiniteElementSpace &L2FESpace,
                const Vector &S,
                bool &quad_data_is_current,
                QuadratureData &quad_data) {
      push();
      assert(dim==2);
      assert(p_assembly);
      assert(material_pcf);

      // ***********************************************************************
      ElementTransformation *T = H1FESpace.GetElementTransformation(0);
      const IntegrationPoint &ip = ir.IntPoint(0);
      const double gamma = material_pcf->Eval(*T,ip);

      // ***********************************************************************
      if (quad_data_is_current) return;

      // ***********************************************************************
      timer.sw_qdata.Start();
      Vector* sptr = (Vector*) &S;
      const mfem::FiniteElement& fe = *H1FESpace.GetFE(0);
      const int numDofs  = fe.GetDof();
      const int nqp = ir.GetNPoints();
      dbg("numDofs=%d, nqp=%d, nzones=%d",numDofs,nqp,nzones);
      const size_t H1_size = H1FESpace.GetVSize();
      const size_t L2_size = L2FESpace.GetVSize();
      const int nqp1D = tensors1D->LQshape1D.Width();
      
      // Energy dof => quads ***************************************************
      dbg("Energy dof => quads (L2FESpace)");
      const double *h_e_data = sptr->GetData()+2*H1_size;
      for(size_t k=0;k<L2_size;k+=1) dbg("\te[%d]=%f",k,h_e_data[k]);
      double *d_e_data =
         (double*)mfem::kernels::kmalloc<double>::operator new(L2_size);
      mfem::kernels::kmemcpy::rHtoD(d_e_data, h_e_data, L2_size*sizeof(double));
      double *d_e_quads_data;
      Dof2QuadScalar(L2FESpace, ir, d_e_data, &d_e_quads_data);

      // Coords to Jacobians ***************************************************
      dbg("Refresh Geom J, invJ & detJ");
      const double *h_x_data = sptr->GetData() + 0;
      double *d_x_data =
         (double*)mfem::kernels::kmalloc<double>::operator new(H1_size);
      mfem::kernels::kmemcpy::rHtoD(d_x_data, h_x_data, H1_size*sizeof(double));
      double *d_grad_x_data;
      Dof2QuadGrad(H1FESpace,ir,d_x_data,&d_grad_x_data);
      
      // Integration Points Weights (tensor) ***********************************
      dbg("Integration Points Weights (tensor,H1FESpace)");
      const kernels::kDofQuadMaps* maps = kernels::kDofQuadMaps::Get(H1FESpace,ir);
      
      // Velocity **************************************************************
      dbg("Velocity H1_size=%d",H1_size);
      const double *h_v_data = sptr->GetData() + H1_size;
      double *d_v_data =
         (double*)mfem::kernels::kmalloc<double>::operator new(H1_size);
      mfem::kernels::kmemcpy::rHtoD(d_v_data, h_v_data, H1_size*sizeof(double));
      double *d_grad_v_data;
      Dof2QuadGrad(H1FESpace,ir,d_v_data,&d_grad_v_data);
      
      // ***********************************************************************      
      const double h1order = (double) H1FESpace.GetOrder(0);
      const double infinity = std::numeric_limits<double>::infinity();

      dbg("rho0DetJ0w");
      const size_t rho0DetJ0w_sz = nzones * nqp;
      double *d_rho0DetJ0w =
         (double*)mfem::kernels::kmalloc<double>::operator new(rho0DetJ0w_sz);
      mfem::kernels::kmemcpy::rHtoD(d_rho0DetJ0w,
                                    quad_data.rho0DetJ0w.GetData(),
                                    rho0DetJ0w_sz*sizeof(double));

      dbg("Jac0inv");
      const size_t Jac0inv_sz = dim * dim * nzones * nqp;
      double *d_Jac0inv =
         (double*)mfem::kernels::kmalloc<double>::operator new(Jac0inv_sz);
      mfem::kernels::kmemcpy::rHtoD(d_Jac0inv,
                                    quad_data.Jac0inv.Data(),
                                    Jac0inv_sz*sizeof(double));

      dbg("dt_est");
      double *d_dt_est =
         (double*)mfem::kernels::kmalloc<double>::operator new(1);
      mfem::kernels::kmemcpy::rHtoD(d_dt_est, &quad_data.dt_est, sizeof(double));

      dbg("stressJinvT");
      const size_t stressJinvT_sz = nzones * nqp * dim * dim;
      double *d_stressJinvT =
         (double*)mfem::kernels::kmalloc<double>::operator new(stressJinvT_sz);

      dbg("qkernel");
      qkernel<2> __config(nzones) (nzones,
                                   nqp,
                                   nqp1D,
                                   gamma,
                                   use_viscosity,
                                   quad_data.h0,
                                   h1order,
                                   cfl,
                                   infinity,
                                   
                                   maps->quadWeights,
                                   d_grad_x_data,
                                   d_rho0DetJ0w,
                                   d_e_quads_data,
                                   d_grad_v_data,
                                   d_Jac0inv,
                                   d_dt_est,
                                   d_stressJinvT);
      
      mfem::kernels::kmemcpy::rDtoH(quad_data.stressJinvT.Data(),
                                    d_stressJinvT, stressJinvT_sz*sizeof(double));
      mfem::kernels::kmemcpy::rDtoH(&quad_data.dt_est, d_dt_est, sizeof(double));
      dbg("\033[7mdt_est=%.15e",quad_data.dt_est);
      //assert(false);
      quad_data_is_current = true;
      timer.sw_qdata.Stop();
      timer.quad_tstep += nzones;
   }

} // namespace hydrodynamics

} // namespace mfem
