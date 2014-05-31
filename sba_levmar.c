/////////////////////////////////////////////////////////////////////////////////
//// 
////  Expert drivers for sparse bundle adjustment based on the
////  Levenberg - Marquardt minimization algorithm
////  Copyright (C) 2004-2009 Manolis Lourakis (lourakis at ics forth gr)
////  Institute of Computer Science, Foundation for Research & Technology - Hellas
////  Heraklion, Crete, Greece.
////
////  This program is free software; you can redistribute it and/or modify
////  it under the terms of the GNU General Public License as published by
////  the Free Software Foundation; either version 2 of the License, or
////  (at your option) any later version.
////
////  This program is distributed in the hope that it will be useful,
////  but WITHOUT ANY WARRANTY; without even the implied warranty of
////  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
////  GNU General Public License for more details.
////
///////////////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "compiler.h"
#include "sba.h"
#include "sba_chkjac.h"


#define SBA_EPSILON       1E-12
#define SBA_EPSILON_SQ    ( (SBA_EPSILON)*(SBA_EPSILON) )

#define SBA_ONE_THIRD     0.3333333334 /* 1.0/3.0 */


#define emalloc(sz)       emalloc_(__FILE__, __LINE__, sz)

#define FABS(x)           (((x)>=0)? (x) : -(x))

#define ROW_MAJOR         0
#define COLUMN_MAJOR      1
#define MAT_STORAGE       COLUMN_MAJOR


/* contains information necessary for computing a finite difference approximation to a jacobian,
 * e.g. function to differentiate, problem dimensions and pointers to working memory buffers
 */
struct fdj_data_x_ {
  void (*func)(double *p, struct sba_crsm *idxij, int *rcidxs, int *rcsubs, double *hx, void *adata); /* function to differentiate */
  int cnp, pnp, mnp;  /* parameter numbers */
  int *func_rcidxs,
      *func_rcsubs;   /* working memory for func invocations.
                       * Notice that this has to be different
                       * than the working memory used for
                       * evaluating the jacobian!
                       */
  double *hx, *hxx;   /* memory to save results in */
  void *adata;
};

/* auxiliary memory allocation routine with error checking */
inline static void *emalloc_(char *file, int line, size_t sz)
{
void *ptr;

  ptr=(void *)malloc(sz);
  if(ptr==NULL){
    fprintf(stderr, "SBA: memory allocation request for %u bytes failed in file %s, line %d, exiting", sz, file, line);
    exit(1);
  }

  return ptr;
}

/* auxiliary routine for clearing an array of doubles */
inline static void _dblzero(register double *arr, register int count)
{
  while(--count>=0)
    *arr++=0.0;
}

/* auxiliary routine for computing the mean reprojection error; used for debugging */
static double sba_mean_repr_error(int n, int mnp, double *x, double *hx, struct sba_crsm *idxij, int *rcidxs, int *rcsubs)
{
register int i, j;
int nnz, nprojs;
double *ptr1, *ptr2;
double err;

  for(i=nprojs=0, err=0.0; i<n; ++i){
    nnz=sba_crsm_row_elmidxs(idxij, i, rcidxs, rcsubs); /* find nonzero x_ij, j=0...m-1 */
    nprojs+=nnz;
    for(j=0; j<nnz; ++j){ /* point i projecting on camera rcsubs[j] */
      ptr1=x + idxij->val[rcidxs[j]]*mnp;
      ptr2=hx + idxij->val[rcidxs[j]]*mnp;

      err+=sqrt((ptr1[0]-ptr2[0])*(ptr1[0]-ptr2[0]) + (ptr1[1]-ptr2[1])*(ptr1[1]-ptr2[1]));
    }
  }

  return err/((double)(nprojs));
}

/* print the solution in p using sba's text format. If cnp/pnp==0 only points/cameras are printed */
static void sba_print_sol(int n, int m, double *p, int cnp, int pnp, double *x, int mnp, struct sba_crsm *idxij, int *rcidxs, int *rcsubs)
{
register int i, j, ii;
int nnz;
double *ptr;

  if(cnp){
    /* print camera parameters */
    for(j=0; j<m; ++j){
      ptr=p+cnp*j;
      for(ii=0; ii<cnp; ++ii)
        printf("%g ", ptr[ii]);
      printf("\n");
    }
  }

  if(pnp){
    /* 3D & 2D point parameters */
    printf("\n\n\n# X Y Z  nframes  frame0 x0 y0  frame1 x1 y1 ...\n");
    for(i=0; i<n; ++i){
      ptr=p+cnp*m+i*pnp;
      for(ii=0; ii<pnp; ++ii) // print 3D coordinates
        printf("%g ", ptr[ii]);

      nnz=sba_crsm_row_elmidxs(idxij, i, rcidxs, rcsubs); /* find nonzero x_ij, j=0...m-1 */
      printf("%d ", nnz);

      for(j=0; j<nnz; ++j){ /* point i projecting on camera rcsubs[j] */
        ptr=x + idxij->val[rcidxs[j]]*mnp;

        printf("%d ", rcsubs[j]);
        for(ii=0; ii<mnp; ++ii) // print 2D coordinates
          printf("%g ", ptr[ii]);
      }
      printf("\n");
    }
  }
}

/* Compute e=x-y for two n-vectors x and y and return the squared L2 norm of e.
 * e can coincide with either x or y. 
 * Uses loop unrolling and blocking to reduce bookkeeping overhead & pipeline
 * stalls and increase instruction-level parallelism; see http://www.abarnett.demon.co.uk/tutorial.html
 */
static double nrmL2xmy(double *const e, const double *const x, const double *const y, const int n)
{
const int blocksize=8, bpwr=3; /* 8=2^3 */
register int i;
int j1, j2, j3, j4, j5, j6, j7;
int blockn;
register double sum0=0.0, sum1=0.0, sum2=0.0, sum3=0.0;

  /* n may not be divisible by blocksize, 
   * go as near as we can first, then tidy up.
   */
  blockn = (n>>bpwr)<<bpwr; /* (n / blocksize) * blocksize; */

  /* unroll the loop in blocks of `blocksize'; looping downwards gains some more speed */
  for(i=blockn-1; i>0; i-=blocksize){
            e[i ]=x[i ]-y[i ]; sum0+=e[i ]*e[i ];
    j1=i-1; e[j1]=x[j1]-y[j1]; sum1+=e[j1]*e[j1];
    j2=i-2; e[j2]=x[j2]-y[j2]; sum2+=e[j2]*e[j2];
    j3=i-3; e[j3]=x[j3]-y[j3]; sum3+=e[j3]*e[j3];
    j4=i-4; e[j4]=x[j4]-y[j4]; sum0+=e[j4]*e[j4];
    j5=i-5; e[j5]=x[j5]-y[j5]; sum1+=e[j5]*e[j5];
    j6=i-6; e[j6]=x[j6]-y[j6]; sum2+=e[j6]*e[j6];
    j7=i-7; e[j7]=x[j7]-y[j7]; sum3+=e[j7]*e[j7];
  }

  /*
   * There may be some left to do.
   * This could be done as a simple for() loop, 
   * but a switch is faster (and more interesting) 
   */

  i=blockn;
  if(i<n){ 
  /* Jump into the case at the place that will allow
   * us to finish off the appropriate number of items. 
   */
    switch(n - i){ 
      case 7 : e[i]=x[i]-y[i]; sum0+=e[i]*e[i]; ++i;
      case 6 : e[i]=x[i]-y[i]; sum0+=e[i]*e[i]; ++i;
      case 5 : e[i]=x[i]-y[i]; sum0+=e[i]*e[i]; ++i;
      case 4 : e[i]=x[i]-y[i]; sum0+=e[i]*e[i]; ++i;
      case 3 : e[i]=x[i]-y[i]; sum0+=e[i]*e[i]; ++i;
      case 2 : e[i]=x[i]-y[i]; sum0+=e[i]*e[i]; ++i;
      case 1 : e[i]=x[i]-y[i]; sum0+=e[i]*e[i]; ++i;
    }
  }

  return sum0+sum1+sum2+sum3;
}

/* Compute e=W*(x-y) for two n-vectors x and y and return the squared L2 norm of e.
 * This norm equals the squared C norm of x-y with C=W^T*W, W being block diagonal
 * matrix with nvis mnp*mnp blocks: e^T*e=(x-y)^T*W^T*W*(x-y)=||x-y||_C.
 * Note that n=nvis*mnp; e can coincide with either x or y.
 *
 * Similarly to nrmL2xmy() above, uses loop unrolling and blocking
 */
static double nrmCxmy(double *const e, const double *const x, const double *const y,
                      const double *const W, const int mnp, const int nvis)
{
const int n=nvis*mnp;
const int blocksize=8, bpwr=3; /* 8=2^3 */
register int i, ii, k;
int j1, j2, j3, j4, j5, j6, j7;
int blockn, mnpsq;
register double norm, sum;
register const double *Wptr, *auxptr;
register double *eptr;

  /* n may not be divisible by blocksize, 
   * go as near as we can first, then tidy up.
   */
  blockn = (n>>bpwr)<<bpwr; /* (n / blocksize) * blocksize; */

  /* unroll the loop in blocks of `blocksize'; looping downwards gains some more speed */
  for(i=blockn-1; i>0; i-=blocksize){
            e[i ]=x[i ]-y[i ];
    j1=i-1; e[j1]=x[j1]-y[j1];
    j2=i-2; e[j2]=x[j2]-y[j2];
    j3=i-3; e[j3]=x[j3]-y[j3];
    j4=i-4; e[j4]=x[j4]-y[j4];
    j5=i-5; e[j5]=x[j5]-y[j5];
    j6=i-6; e[j6]=x[j6]-y[j6];
    j7=i-7; e[j7]=x[j7]-y[j7];
  }

  /*
   * There may be some left to do.
   * This could be done as a simple for() loop, 
   * but a switch is faster (and more interesting) 
   */

  i=blockn;
  if(i<n){ 
  /* Jump into the case at the place that will allow
   * us to finish off the appropriate number of items. 
   */
    switch(n - i){ 
      case 7 : e[i]=x[i]-y[i]; ++i;
      case 6 : e[i]=x[i]-y[i]; ++i;
      case 5 : e[i]=x[i]-y[i]; ++i;
      case 4 : e[i]=x[i]-y[i]; ++i;
      case 3 : e[i]=x[i]-y[i]; ++i;
      case 2 : e[i]=x[i]-y[i]; ++i;
      case 1 : e[i]=x[i]-y[i]; ++i;
    }
  }

  /* compute w_x_ij e_ij in e and its L2 norm.
   * Since w_x_ij is upper triangular, the products can be safely saved
   * directly in e_ij, without the need for intermediate storage
   */
  mnpsq=mnp*mnp;
  /* Wptr, eptr point to w_x_ij, e_ij below */
  for(i=0, Wptr=W, eptr=e, norm=0.0; i<nvis; ++i, Wptr+=mnpsq, eptr+=mnp){
    for(ii=0, auxptr=Wptr; ii<mnp; ++ii, auxptr+=mnp){ /* auxptr=Wptr+ii*mnp */
      for(k=ii, sum=0.0; k<mnp; ++k) // k>=ii since w_x_ij is upper triangular
        sum+=auxptr[k]*eptr[k]; //Wptr[ii*mnp+k]*eptr[k];
      eptr[ii]=sum;
      norm+=sum*sum;
    }
  }

  return norm;
}

/* search for & print image projection components that are infinite; useful for identifying errors */
static void sba_print_inf(double *hx, int nimgs, int mnp, struct sba_crsm *idxij, int *rcidxs, int *rcsubs)
{
register int i, j, k;
int nnz;
double *phxij;

  for(j=0; j<nimgs; ++j){
    nnz=sba_crsm_col_elmidxs(idxij, j, rcidxs, rcsubs); /* find nonzero hx_ij, i=0...n-1 */
    for(i=0; i<nnz; ++i){
      phxij=hx + idxij->val[rcidxs[i]]*mnp;
      for(k=0; k<mnp; ++k)
        if(!SBA_FINITE(phxij[k]))
          printf("SBA: component %d of the estimated projection of point %d on camera %d is invalid!\n", k, rcsubs[i], j);
    }
  }
}

/* Given a parameter vector p made up of the 3D coordinates of n points and the parameters of m cameras, compute in
 * jac the jacobian of the predicted measurements, i.e. the jacobian of the projections of 3D points in the m images.
 * The jacobian is approximated with the aid of finite differences and is returned in the order
 * (A_11, B_11, ..., A_1m, B_1m, ..., A_n1, B_n1, ..., A_nm, B_nm),
 * where A_ij=dx_ij/da_j and B_ij=dx_ij/db_i (see HZ).
 * Notice that depending on idxij, some of the A_ij, B_ij might be missing
 *
 * Problem-specific information is assumed to be stored in a structure pointed to by "dat".
 *
 * NOTE: The jacobian (for n=4, m=3) in matrix form has the following structure:
 *       A_11  0     0     B_11 0    0    0
 *       0     A_12  0     B_12 0    0    0
 *       0     0     A_13  B_13 0    0    0
 *       A_21  0     0     0    B_21 0    0
 *       0     A_22  0     0    B_22 0    0
 *       0     0     A_23  0    B_23 0    0
 *       A_31  0     0     0    0    B_31 0
 *       0     A_32  0     0    0    B_32 0
 *       0     0     A_33  0    0    B_33 0
 *       A_41  0     0     0    0    0    B_41
 *       0     A_42  0     0    0    0    B_42
 *       0     0     A_43  0    0    0    B_43
 *       To reduce the total number of objective function evaluations, this structure can be
 *       exploited as follows: A certain d is added to the j-th parameters of all cameras and
 *       the objective function is evaluated at the resulting point. This evaluation suffices
 *       to compute the corresponding columns of *all* A_ij through finite differences. A similar
 *       strategy allows the computation of the B_ij. Overall, only cnp+pnp+1 objective function
 *       evaluations are needed to compute the jacobian, much fewer compared to the m*cnp+n*pnp+1
 *       that would be required by the naive strategy of computing one column of the jacobian
 *       per function evaluation. See Nocedal-Wright, ch. 7, pp. 169. Although this approach is
 *       much faster compared to the naive strategy, it is not preferable to analytic jacobians,
 *       since the latter are considerably faster to compute and result in fewer LM iterations.
 */
static void sba_fdjac_x(
    double *p,                /* I: current parameter estimate, (m*cnp+n*pnp)x1 */
    struct sba_crsm *idxij,   /* I: sparse matrix containing the location of x_ij in hx */
    int    *rcidxs,           /* work array for the indexes of nonzero elements of a single sparse matrix row/column */
    int    *rcsubs,           /* work array for the subscripts of nonzero elements in a single sparse matrix row/column */
    double *jac,              /* O: array for storing the approximated jacobian */
    void   *dat)              /* I: points to a "fdj_data_x_" structure */
{
  register int i, j, ii, jj;
  double *pa, *pb, *pqr, *ppt;
  register double *pAB, *phx, *phxx;
  int n, m, nm, nnz, Asz, Bsz, ABsz, idx;

  double *tmpd;
  register double d;

  struct fdj_data_x_ *fdjd;
  void (*func)(double *p, struct sba_crsm *idxij, int *rcidxs, int *rcsubs, double *hx, void *adata);
  double *hx, *hxx;
  int cnp, pnp, mnp;
  void *adata;


  /* retrieve problem-specific information passed in *dat */
  fdjd=(struct fdj_data_x_ *)dat;
  func=fdjd->func;
  cnp=fdjd->cnp; pnp=fdjd->pnp; mnp=fdjd->mnp;
  hx=fdjd->hx;
  hxx=fdjd->hxx;
  adata=fdjd->adata;

  n=idxij->nr; m=idxij->nc;
  pa=p; pb=p+m*cnp;
  Asz=mnp*cnp; Bsz=mnp*pnp; ABsz=Asz+Bsz;

  nm=(n>=m)? n : m; // max(n, m);
  tmpd=(double *)emalloc(nm*sizeof(double));

  (*func)(p, idxij, fdjd->func_rcidxs, fdjd->func_rcsubs, hx, adata); // evaluate supplied function on current solution

  if(cnp){ // is motion varying?
    /* compute A_ij */
    for(jj=0; jj<cnp; ++jj){
      for(j=0; j<m; ++j){
        pqr=pa+j*cnp; // j-th camera parameters
        /* determine d=max(SBA_DELTA_SCALE*|pqr[jj]|, SBA_MIN_DELTA), see HZ */
        d=(double)(SBA_DELTA_SCALE)*pqr[jj]; // force evaluation
        d=FABS(d);
        if(d<SBA_MIN_DELTA) d=SBA_MIN_DELTA;

        tmpd[j]=d;
        pqr[jj]+=d;
      }

      (*func)(p, idxij, fdjd->func_rcidxs, fdjd->func_rcsubs, hxx, adata);

      for(j=0; j<m; ++j){
        pqr=pa+j*cnp; // j-th camera parameters
        pqr[jj]-=tmpd[j]; /* restore */
        d=1.0/tmpd[j]; /* invert so that divisions can be carried out faster as multiplications */

        nnz=sba_crsm_col_elmidxs(idxij, j, rcidxs, rcsubs); /* find nonzero A_ij, i=0...n-1 */
        for(i=0; i<nnz; ++i){
          idx=idxij->val[rcidxs[i]];
          phx=hx + idx*mnp; // set phx to point to hx_ij
          phxx=hxx + idx*mnp; // set phxx to point to hxx_ij
          pAB=jac + idx*ABsz; // set pAB to point to A_ij

          for(ii=0; ii<mnp; ++ii)
            pAB[ii*cnp+jj]=(phxx[ii]-phx[ii])*d;
        }
      }
    }
  }

  if(pnp){ // is structure varying?
    /* compute B_ij */
    for(jj=0; jj<pnp; ++jj){
      for(i=0; i<n; ++i){
        ppt=pb+i*pnp; // i-th point parameters
        /* determine d=max(SBA_DELTA_SCALE*|ppt[jj]|, SBA_MIN_DELTA), see HZ */
        d=(double)(SBA_DELTA_SCALE)*ppt[jj]; // force evaluation
        d=FABS(d);
        if(d<SBA_MIN_DELTA) d=SBA_MIN_DELTA;

        tmpd[i]=d;
        ppt[jj]+=d;
      }

      (*func)(p, idxij, fdjd->func_rcidxs, fdjd->func_rcsubs, hxx, adata);

      for(i=0; i<n; ++i){
        ppt=pb+i*pnp; // i-th point parameters
        ppt[jj]-=tmpd[i]; /* restore */
        d=1.0/tmpd[i]; /* invert so that divisions can be carried out faster as multiplications */

        nnz=sba_crsm_row_elmidxs(idxij, i, rcidxs, rcsubs); /* find nonzero B_ij, j=0...m-1 */
        for(j=0; j<nnz; ++j){
          idx=idxij->val[rcidxs[j]];
          phx=hx + idx*mnp; // set phx to point to hx_ij
          phxx=hxx + idx*mnp; // set phxx to point to hxx_ij
          pAB=jac + idx*ABsz + Asz; // set pAB to point to B_ij

          for(ii=0; ii<mnp; ++ii)
            pAB[ii*pnp+jj]=(phxx[ii]-phx[ii])*d;
        }
      }
    }
  }

  free(tmpd);
}

typedef int (*PLS)(double *A, double *B, double *x, int m, int iscolmaj);

/* Bundle adjustment on camera and structure parameters 
 * using the sparse Levenberg-Marquardt as described in HZ p. 568
 *
 * Returns the number of iterations (>=0) if successfull, SBA_ERROR if failed
 */

int sba_motstr_levmar_x(
    const int n,   /* number of points */
    const int ncon,/* number of points (starting from the 1st) whose parameters should not be modified.
                   * All B_ij (see below) with i<ncon are assumed to be zero
                   */
    const int m,   /* number of images */
    const int mcon,/* number of images (starting from the 1st) whose parameters should not be modified.
					          * All A_ij (see below) with j<mcon are assumed to be zero
					          */
    char *vmask,  /* visibility mask: vmask[i, j]=1 if point i visible in image j, 0 otherwise. nxm */
    double *p,    /* initial parameter vector p0: (a1, ..., am, b1, ..., bn).
                   * aj are the image j parameters, bi are the i-th point parameters,
                   * size m*cnp + n*pnp
                   */
    const int cnp,/* number of parameters for ONE camera; e.g. 6 for Euclidean cameras */
    const int pnp,/* number of parameters for ONE point; e.g. 3 for Euclidean points */
    double *x,    /* measurements vector: (x_11^T, .. x_1m^T, ..., x_n1^T, .. x_nm^T)^T where
                   * x_ij is the projection of the i-th point on the j-th image.
                   * NOTE: some of the x_ij might be missing, if point i is not visible in image j;
                   * see vmask[i, j], max. size n*m*mnp
                   */
    double *covx, /* measurements covariance matrices: (Sigma_x_11, .. Sigma_x_1m, ..., Sigma_x_n1, .. Sigma_x_nm),
                   * where Sigma_x_ij is the mnp x mnp covariance of x_ij stored row-by-row. Set to NULL if no
                   * covariance estimates are available (identity matrices are implicitly used in this case).
                   * NOTE: a certain Sigma_x_ij is missing if the corresponding x_ij is also missing;
                   * see vmask[i, j], max. size n*m*mnp*mnp
                   */
    const int mnp,/* number of parameters for EACH measurement; usually 2 */
    void (*func)(double *p, struct sba_crsm *idxij, int *rcidxs, int *rcsubs, double *hx, void *adata),
                                              /* functional relation describing measurements. Given a parameter vector p,
                                               * computes a prediction of the measurements \hat{x}. p is (m*cnp + n*pnp)x1,
                                               * \hat{x} is (n*m*mnp)x1, maximum
                                               * rcidxs, rcsubs are max(m, n) x 1, allocated by the caller and can be used
                                               * as working memory
                                               */
    void (*fjac)(double *p, struct sba_crsm *idxij, int *rcidxs, int *rcsubs, double *jac, void *adata),
                                              /* function to evaluate the sparse jacobian dX/dp.
                                               * The Jacobian is returned in jac as
                                               * (dx_11/da_1, ..., dx_1m/da_m, ..., dx_n1/da_1, ..., dx_nm/da_m,
                                               *  dx_11/db_1, ..., dx_1m/db_1, ..., dx_n1/db_n, ..., dx_nm/db_n), or (using HZ's notation),
                                               * jac=(A_11, B_11, ..., A_1m, B_1m, ..., A_n1, B_n1, ..., A_nm, B_nm)
                                               * Notice that depending on idxij, some of the A_ij and B_ij might be missing.
                                               * Note also that A_ij and B_ij are mnp x cnp and mnp x pnp matrices resp. and they
                                               * should be stored in jac in row-major order.
                                               * rcidxs, rcsubs are max(m, n) x 1, allocated by the caller and can be used
                                               * as working memory
                                               *
                                               * If NULL, the jacobian is approximated by repetitive func calls and finite
                                               * differences. This is computationally inefficient and thus NOT recommended.
                                               */
    void *adata,       /* pointer to possibly additional data, passed uninterpreted to func, fjac */ 

    const int itmax,   /* I: maximum number of iterations. itmax==0 signals jacobian verification followed by immediate return */
    const int verbose, /* I: verbosity */
    const double opts[SBA_OPTSSZ],
	                     /* I: minim. options [\mu, \epsilon1, \epsilon2, \epsilon3, \epsilon4]. Respectively the scale factor for initial \mu,
                        * stopping thresholds for ||J^T e||_inf, ||dp||_2, ||e||_2 and (||e||_2-||e_new||_2)/||e||_2
                        */
    double info[SBA_INFOSZ]
	                     /* O: information regarding the minimization. Set to NULL if don't care
                        * info[0]=||e||_2 at initial p.
                        * info[1-4]=[ ||e||_2, ||J^T e||_inf,  ||dp||_2, mu/max[J^T J]_ii ], all computed at estimated p.
                        * info[5]= # iterations,
                        * info[6]=reason for terminating: 1 - stopped by small gradient J^T e
                        *                                 2 - stopped by small dp
                        *                                 3 - stopped by itmax
                        *                                 4 - stopped by small relative reduction in ||e||_2
                        *                                 5 - stopped by small ||e||_2
                        *                                 6 - too many attempts to increase damping. Restart with increased mu
                        *                                 7 - stopped by invalid (i.e. NaN or Inf) "func" values. This is a user error
                        * info[7]= # function evaluations
                        * info[8]= # jacobian evaluations
			                  * info[9]= # number of linear systems solved, i.e. number of attempts	for reducing error
                        */
)
{
register int i, j, ii, jj, k, l;
int nvis, nnz, retval;

/* The following are work arrays that are dynamically allocated by sba_motstr_levmar_x() */
double *jac;  /* work array for storing the jacobian, max. size n*m*(mnp*cnp + mnp*pnp) */
double *U;    /* work array for storing the U_j in the order U_1, ..., U_m, size m*cnp*cnp */
double *V;    /* work array for storing the *strictly upper triangles* of V_i in the order V_1, ..., V_n, size n*pnp*pnp.
               * V also stores the lower triangles of (V*_i)^-1 in the order (V*_1)^-1, ..., (V*_n)^-1.
               * Note that diagonal elements of V_1 are saved in diagUV
               */

double *e;    /* work array for storing the e_ij in the order e_11, ..., e_1m, ..., e_n1, ..., e_nm,
                 max. size n*m*mnp */
double *eab;  /* work array for storing the ea_j & eb_i in the order ea_1, .. ea_m eb_1, .. eb_n size m*cnp + n*pnp */

double *E;   /* work array for storing the e_j in the order e_1, .. e_m, size m*cnp */

/* Notice that the blocks W_ij, Y_ij are zero iff A_ij (equivalently B_ij) is zero. This means
 * that the matrix consisting of blocks W_ij is itself sparse, similarly to the
 * block matrix made up of the A_ij and B_ij (i.e. jac)
 */
double *W;    /* work array for storing the W_ij in the order W_11, ..., W_1m, ..., W_n1, ..., W_nm,
                 max. size n*m*cnp*pnp */
double *Yj;   /* work array for storing the Y_ij for a *fixed* j in the order Y_1j, Y_nj,
                 max. size n*cnp*pnp */
double *YWt;  /* work array for storing \sum_i Y_ij W_ik^T, size cnp*cnp */
double *S;    /* work array for storing the block array S_jk, size m*m*cnp*cnp */
double *dp;   /* work array for storing the parameter vector updates da_1, ..., da_m, db_1, ..., db_n, size m*cnp + n*pnp */
double *Wtda; /* work array for storing \sum_j W_ij^T da_j, size pnp */
double *wght= /* work array for storing the weights computed from the covariance inverses, max. size n*m*mnp*mnp */
            NULL;

/* Of the above arrays, jac, e, W, Yj, wght are sparse and
 * U, V, eab, E, S, dp are dense. Sparse arrays (except Yj) are indexed
 * through idxij (see below), that is with the same mechanism as the input 
 * measurements vector x
 */

double *pa, *pb, *ea, *eb, *dpa, *dpb; /* pointers into p, jac, eab and dp respectively */

/* submatrices sizes */
int Asz, Bsz, ABsz, Usz, Vsz,
    Wsz, Ysz, esz, easz, ebsz,
    YWtsz, Wtdasz, Sblsz, covsz;

int Sdim; /* S matrix actual dimension */

register double *ptr1, *ptr2, *ptr3, *ptr4, sum;
struct sba_crsm idxij; /* sparse matrix containing the location of x_ij in x. This is also
                        * the location of A_ij, B_ij in jac, etc.
                        * This matrix can be thought as a map from a sparse set of pairs (i, j) to a continuous
                        * index k and it is used to efficiently lookup the memory locations where the non-zero
                        * blocks of a sparse matrix/vector are stored
                        */
int maxCvis, /* max. of projections of a single point  across cameras, <=m */
    maxPvis, /* max. of projections in a single camera across points,  <=n */
    maxCPvis, /* max. of the above */
    *rcidxs,  /* work array for the indexes corresponding to the nonzero elements of a single row or
                 column in a sparse matrix, size max(n, m) */
    *rcsubs;  /* work array for the subscripts of nonzero elements in a single row or column of a
                 sparse matrix, size max(n, m) */

/* The following variables are needed by the LM algorithm */
register int itno;  /* iteration counter */
int issolved;
/* temporary work arrays that are dynamically allocated */
double *hx,         /* \hat{x}_i, max. size m*n*mnp */
       *diagUV,     /* diagonals of U_j, V_i, size m*cnp + n*pnp */
       *pdp;        /* p + dp, size m*cnp + n*pnp */

double *diagU, *diagV; /* pointers into diagUV */

register double mu,  /* damping constant */
                tmp; /* mainly used in matrix & vector multiplications */
double p_eL2, eab_inf, pdp_eL2; /* ||e(p)||_2, ||J^T e||_inf, ||e(p+dp)||_2 */
double p_L2, dp_L2=DBL_MAX, dF, dL;
double tau=FABS(opts[0]), eps1=FABS(opts[1]), eps2=FABS(opts[2]), eps2_sq=opts[2]*opts[2],
       eps3_sq=opts[3]*opts[3], eps4_sq=opts[4]*opts[4];
double init_p_eL2;
int nu=2, nu2, stop=0, nfev, njev=0, nlss=0;
int nobs, nvars;
const int mmcon=m-mcon;
PLS linsolver=NULL;
int (*matinv)(double *A, int m)=NULL;

struct fdj_data_x_ fdj_data;
void *jac_adata;

/* Initialization */
  mu=eab_inf=0.0; /* -Wall */

  /* block sizes */
  Asz=mnp * cnp; Bsz=mnp * pnp; ABsz=Asz + Bsz;
  Usz=cnp * cnp; Vsz=pnp * pnp;
  Wsz=cnp * pnp; Ysz=cnp * pnp;
  esz=mnp;
  easz=cnp; ebsz=pnp;
  YWtsz=cnp * cnp;
  Wtdasz=pnp;
  Sblsz=cnp * cnp;
  Sdim=mmcon * cnp;
  covsz=mnp * mnp;

  /* count total number of visible image points */
  for(i=nvis=0, jj=n*m; i<jj; ++i)
    nvis+=(vmask[i]!=0);

  nobs=nvis*mnp;
  nvars=m*cnp + n*pnp;
  if(nobs<nvars){
    fprintf(stderr, "SBA: sba_motstr_levmar_x() cannot solve a problem with fewer measurements [%d] than unknowns [%d]\n", nobs, nvars);
    return SBA_ERROR;
  }

  /* allocate & fill up the idxij structure */
  sba_crsm_alloc(&idxij, n, m, nvis);
  for(i=k=0; i<n; ++i){
    idxij.rowptr[i]=k;
    ii=i*m;
    for(j=0; j<m; ++j)
      if(vmask[ii+j]){
        idxij.val[k]=k;
        idxij.colidx[k++]=j;
      }
  }
  idxij.rowptr[n]=nvis;

  /* find the maximum number (for all cameras) of visible image projections coming from a single 3D point */
  for(i=maxCvis=0; i<n; ++i)
    if((k=idxij.rowptr[i+1]-idxij.rowptr[i])>maxCvis) maxCvis=k;

  /* find the maximum number (for all points) of visible image projections in any single camera */
  for(j=maxPvis=0; j<m; ++j){
    for(i=ii=0; i<n; ++i)
      if(vmask[i*m+j]) ++ii;
    if(ii>maxPvis) maxPvis=ii;
  }
  maxCPvis=(maxCvis>=maxPvis)? maxCvis : maxPvis;

#if 0
  /* determine the density of blocks in matrix S */
  for(j=mcon, ii=0; j<m; ++j){
    ++ii; /* block Sjj is surely nonzero */
    for(k=j+1; k<m; ++k)
      if(sba_crsm_common_row(&idxij, j, k)) ii+=2; /* blocks Sjk & Skj are nonzero */
  }
  printf("\nS block density: %.5g\n", ((double)ii)/(mmcon*mmcon)); fflush(stdout);
#endif

  /* allocate work arrays */
  /* W is big enough to hold both jac & W. Note also the extra Wsz, see the initialization of jac below for explanation */
  W=(double *)emalloc((nvis*((Wsz>=ABsz)? Wsz : ABsz) + Wsz)*sizeof(double));
  U=(double *)emalloc(m*Usz*sizeof(double));
  V=(double *)emalloc(n*Vsz*sizeof(double));
  e=(double *)emalloc(nobs*sizeof(double));
  eab=(double *)emalloc(nvars*sizeof(double));
  E=(double *)emalloc(m*cnp*sizeof(double));
  Yj=(double *)emalloc(maxPvis*Ysz*sizeof(double));
  YWt=(double *)emalloc(YWtsz*sizeof(double));
  S=(double *)emalloc(m*m*Sblsz*sizeof(double));
  dp=(double *)emalloc(nvars*sizeof(double));
  Wtda=(double *)emalloc(pnp*sizeof(double));
  rcidxs=(int *)emalloc(maxCPvis*sizeof(int));
  rcsubs=(int *)emalloc(maxCPvis*sizeof(int));
#ifndef SBA_DESTROY_COVS
  if(covx!=NULL) wght=(double *)emalloc(nvis*covsz*sizeof(double));
#else
  if(covx!=NULL) wght=covx;
#endif /* SBA_DESTROY_COVS */


  hx=(double *)emalloc(nobs*sizeof(double));
  diagUV=(double *)emalloc(nvars*sizeof(double));
  pdp=(double *)emalloc(nvars*sizeof(double));

  /* to save resources, W and jac share the same memory: First, the jacobian
   * is computed in some working memory that is then overwritten during the
   * computation of W. To account for the case of W being larger than jac,
   * extra memory is reserved "before" jac.
   * Care must be taken, however, to ensure that storing a certain W_ij
   * does not overwrite the A_ij, B_ij used to compute it. To achieve
   * this is, note that if p1 and p2 respectively point to the first elements
   * of a certain W_ij and A_ij, B_ij pair, we should have p2-p1>=Wsz.
   * There are two cases:
   * a) Wsz>=ABsz: Then p1=W+k*Wsz and p2=jac+k*ABsz=W+Wsz+nvis*(Wsz-ABsz)+k*ABsz
   *    for some k (0<=k<nvis), thus p2-p1=(nvis-k)*(Wsz-ABsz)+Wsz. 
   *    The right side of the last equation is obviously > Wsz for all 0<=k<nvis
   *
   * b) Wsz<ABsz: Then p1=W+k*Wsz and p2=jac+k*ABsz=W+Wsz+k*ABsz and
   *    p2-p1=Wsz+k*(ABsz-Wsz), which is again > Wsz for all 0<=k<nvis
   *
   * In conclusion, if jac is initialized as below, the memory allocated to all
   * W_ij is guaranteed not to overlap with that allocated to their corresponding
   * A_ij, B_ij pairs
   */
  jac=W + Wsz + ((Wsz>ABsz)? nvis*(Wsz-ABsz) : 0);

  /* set up auxiliary pointers */
  pa=p; pb=p+m*cnp;
  ea=eab; eb=eab+m*cnp;
  dpa=dp; dpb=dp+m*cnp;

  diagU=diagUV; diagV=diagUV + m*cnp;

  /* if no jacobian function is supplied, prepare to compute jacobian with finite difference */
  if(!fjac){
    fdj_data.func=func;
    fdj_data.cnp=cnp;
    fdj_data.pnp=pnp;
    fdj_data.mnp=mnp;
    fdj_data.hx=hx;
    fdj_data.hxx=(double *)emalloc(nobs*sizeof(double));
    fdj_data.func_rcidxs=(int *)emalloc(2*maxCPvis*sizeof(int));
    fdj_data.func_rcsubs=fdj_data.func_rcidxs+maxCPvis;
    fdj_data.adata=adata;

    fjac=sba_fdjac_x;
    jac_adata=(void *)&fdj_data;
  }
  else{
    fdj_data.hxx=NULL;
    jac_adata=adata;
  }

  if(itmax==0){ /* verify jacobian */
    sba_motstr_chkjac_x(func, fjac, p, &idxij, rcidxs, rcsubs, ncon, mcon, cnp, pnp, mnp, adata, jac_adata);
    retval=0;
    goto freemem_and_return;
  }

  /* covariances Sigma_x_ij are accommodated by computing the Cholesky decompositions of their
   * inverses and using the resulting matrices w_x_ij to weigh A_ij, B_ij, and e_ij as w_x_ij A_ij,
   * w_x_ij*B_ij and w_x_ij*e_ij. In this way, auxiliary variables as U_j=\sum_i A_ij^T A_ij
   * actually become \sum_i (w_x_ij A_ij)^T w_x_ij A_ij= \sum_i A_ij^T w_x_ij^T w_x_ij A_ij =
   * A_ij^T Sigma_x_ij^-1 A_ij
   *
   * ea_j, V_i, eb_i, W_ij are weighted in a similar manner
   */
  if(covx!=NULL){
    for(i=0; i<n; ++i){
      nnz=sba_crsm_row_elmidxs(&idxij, i, rcidxs, rcsubs); /* find nonzero x_ij, j=0...m-1 */
      for(j=0; j<nnz; ++j){
        /* set ptr1, ptr2 to point to cov_x_ij, w_x_ij resp. */
        ptr1=covx + (k=idxij.val[rcidxs[j]]*covsz);
        ptr2=wght + k;
        if(!sba_mat_cholinv(ptr1, ptr2, mnp)){ /* compute w_x_ij s.t. w_x_ij^T w_x_ij = cov_x_ij^-1 */
			    fprintf(stderr, "SBA: invalid covariance matrix for x_ij (i=%d, j=%d) in sba_motstr_levmar_x()\n", i, rcsubs[j]);
          retval=SBA_ERROR;
          goto freemem_and_return;
        }
      }
    }
    sba_mat_cholinv(NULL, NULL, 0); /* cleanup */
  }

  /* compute the error vectors e_ij in hx */
  (*func)(p, &idxij, rcidxs, rcsubs, hx, adata); nfev=1;
  /* ### compute e=x - f(p) [e=w*(x - f(p)] and its L2 norm */
  if(covx==NULL)
    p_eL2=nrmL2xmy(e, x, hx, nobs); /* e=x-hx, p_eL2=||e|| */
  else
    p_eL2=nrmCxmy(e, x, hx, wght, mnp, nvis); /* e=wght*(x-hx), p_eL2=||e||=||x-hx||_Sigma^-1 */
  if(verbose) printf("initial motstr-SBA error %g [%g]\n", p_eL2, p_eL2/nvis);
  init_p_eL2=p_eL2;
  if(!SBA_FINITE(p_eL2)) stop=7;

  for(itno=0; itno<itmax && !stop; ++itno){
    /* Note that p, e and ||e||_2 have been updated at the previous iteration */

    /* compute derivative submatrices A_ij, B_ij */
    (*fjac)(p, &idxij, rcidxs, rcsubs, jac, jac_adata); ++njev;

    if(covx!=NULL){
      /* compute w_x_ij A_ij and w_x_ij B_ij.
       * Since w_x_ij is upper triangular, the products can be safely saved
       * directly in A_ij, B_ij, without the need for intermediate storage
       */
      for(i=0; i<nvis; ++i){
        /* set ptr1, ptr2, ptr3 to point to w_x_ij, A_ij, B_ij, resp. */
        ptr1=wght + i*covsz;
        ptr2=jac  + i*ABsz;
        ptr3=ptr2 + Asz; // ptr3=jac  + i*ABsz + Asz;

        /* w_x_ij is mnp x mnp, A_ij is mnp x cnp, B_ij is mnp x pnp
         * NOTE: Jamming the outter (i.e., ii) loops did not run faster!
         */
        /* A_ij */
        for(ii=0; ii<mnp; ++ii)
          for(jj=0; jj<cnp; ++jj){
            for(k=ii, sum=0.0; k<mnp; ++k) // k>=ii since w_x_ij is upper triangular
              sum+=ptr1[ii*mnp+k]*ptr2[k*cnp+jj];
            ptr2[ii*cnp+jj]=sum;
          }

        /* B_ij */
        for(ii=0; ii<mnp; ++ii)
          for(jj=0; jj<pnp; ++jj){
            for(k=ii, sum=0.0; k<mnp; ++k) // k>=ii since w_x_ij is upper triangular
              sum+=ptr1[ii*mnp+k]*ptr3[k*pnp+jj];
            ptr3[ii*pnp+jj]=sum;
          }
      }
    }

    /* compute U_j = \sum_i A_ij^T A_ij */ // \Sigma here!
    /* U_j is symmetric, therefore its computation can be sped up by
     * computing only the upper part and then reusing it for the lower one.
     * Recall that A_ij is mnp x cnp
     */
    /* Also compute ea_j = \sum_i A_ij^T e_ij */ // \Sigma here!
    /* Recall that e_ij is mnp x 1
     */
    _dblzero(U, m*Usz); /* clear all U_j */
    _dblzero(ea, m*easz); /* clear all ea_j */
    for(j=mcon; j<m; ++j){
      ptr1=U + j*Usz; // set ptr1 to point to U_j
      ptr2=ea + j*easz; // set ptr2 to point to ea_j

      nnz=sba_crsm_col_elmidxs(&idxij, j, rcidxs, rcsubs); /* find nonzero A_ij, i=0...n-1 */
      for(i=0; i<nnz; ++i){
        /* set ptr3 to point to A_ij, actual row number in rcsubs[i] */
        ptr3=jac + idxij.val[rcidxs[i]]*ABsz;

        /* compute the UPPER TRIANGULAR PART of A_ij^T A_ij and add it to U_j */
        for(ii=0; ii<cnp; ++ii){
          for(jj=ii; jj<cnp; ++jj){
            for(k=0, sum=0.0; k<mnp; ++k)
              sum+=ptr3[k*cnp+ii]*ptr3[k*cnp+jj];
            ptr1[ii*cnp+jj]+=sum;
          }

          /* copy the LOWER TRIANGULAR PART of U_j from the upper one */
          for(jj=0; jj<ii; ++jj)
            ptr1[ii*cnp+jj]=ptr1[jj*cnp+ii];
        }

        ptr4=e + idxij.val[rcidxs[i]]*esz; /* set ptr4 to point to e_ij */
        /* compute A_ij^T e_ij and add it to ea_j */
        for(ii=0; ii<cnp; ++ii){
          for(jj=0, sum=0.0; jj<mnp; ++jj)
            sum+=ptr3[jj*cnp+ii]*ptr4[jj];
          ptr2[ii]+=sum;
        }
      }
    }

    /* compute V_i = \sum_j B_ij^T B_ij */ // \Sigma here!
    /* V_i is symmetric, therefore its computation can be sped up by
     * computing only the upper part and then reusing it for the lower one.
     * Recall that B_ij is mnp x pnp
     */
    /* Also compute eb_i = \sum_j B_ij^T e_ij */ // \Sigma here!
    /* Recall that e_ij is mnp x 1
     */
	  _dblzero(V, n*Vsz); /* clear all V_i */
	  _dblzero(eb, n*ebsz); /* clear all eb_i */
    for(i=ncon; i<n; ++i){
      ptr1=V + i*Vsz; // set ptr1 to point to V_i
      ptr2=eb + i*ebsz; // set ptr2 to point to eb_i

      nnz=sba_crsm_row_elmidxs(&idxij, i, rcidxs, rcsubs); /* find nonzero B_ij, j=0...m-1 */
      for(j=0; j<nnz; ++j){
        /* set ptr3 to point to B_ij, actual column number in rcsubs[j] */
        ptr3=jac + idxij.val[rcidxs[j]]*ABsz + Asz;
      
        /* compute the UPPER TRIANGULAR PART of B_ij^T B_ij and add it to V_i */
        for(ii=0; ii<pnp; ++ii){
          for(jj=ii; jj<pnp; ++jj){
            for(k=0, sum=0.0; k<mnp; ++k)
              sum+=ptr3[k*pnp+ii]*ptr3[k*pnp+jj];
            ptr1[ii*pnp+jj]+=sum;
          }
        }

        ptr4=e + idxij.val[rcidxs[j]]*esz; /* set ptr4 to point to e_ij */
        /* compute B_ij^T e_ij and add it to eb_i */
        for(ii=0; ii<pnp; ++ii){
          for(jj=0, sum=0.0; jj<mnp; ++jj)
            sum+=ptr3[jj*pnp+ii]*ptr4[jj];
          ptr2[ii]+=sum;
        }
      }
    }

    /* compute W_ij =  A_ij^T B_ij */ // \Sigma here!
    /* Recall that A_ij is mnp x cnp and B_ij is mnp x pnp
     */
    for(i=ncon; i<n; ++i){
      nnz=sba_crsm_row_elmidxs(&idxij, i, rcidxs, rcsubs); /* find nonzero W_ij, j=0...m-1 */
      for(j=0; j<nnz; ++j){
        /* set ptr1 to point to W_ij, actual column number in rcsubs[j] */
        ptr1=W + idxij.val[rcidxs[j]]*Wsz;

        if(rcsubs[j]<mcon){ /* A_ij is zero */
          //_dblzero(ptr1, Wsz); /* clear W_ij */
          continue;
        }

        /* set ptr2 & ptr3 to point to A_ij & B_ij resp. */
        ptr2=jac  + idxij.val[rcidxs[j]]*ABsz;
        ptr3=ptr2 + Asz;
        /* compute A_ij^T B_ij and store it in W_ij
         * Recall that storage for A_ij, B_ij does not overlap with that for W_ij,
         * see the comments related to the initialization of jac above
         */
        /* assert(ptr2-ptr1>=Wsz); */
        for(ii=0; ii<cnp; ++ii)
          for(jj=0; jj<pnp; ++jj){
            for(k=0, sum=0.0; k<mnp; ++k)
              sum+=ptr2[k*cnp+ii]*ptr3[k*pnp+jj];
            ptr1[ii*pnp+jj]=sum;
          }
      }
    }

    /* Compute ||J^T e||_inf and ||p||^2 */
    for(i=0, p_L2=eab_inf=0.0; i<nvars; ++i){
      if(eab_inf < (tmp=FABS(eab[i]))) eab_inf=tmp;
      p_L2+=p[i]*p[i];
    }
    //p_L2=sqrt(p_L2);

    /* save diagonal entries so that augmentation can be later canceled.
     * Diagonal entries are in U_j and V_i
     */
    for(j=mcon; j<m; ++j){
      ptr1=U + j*Usz; // set ptr1 to point to U_j
      ptr2=diagU + j*cnp; // set ptr2 to point to diagU_j
      for(i=0; i<cnp; ++i)
        ptr2[i]=ptr1[i*cnp+i];
    }
    for(i=ncon; i<n; ++i){
      ptr1=V + i*Vsz; // set ptr1 to point to V_i
      ptr2=diagV + i*pnp; // set ptr2 to point to diagV_i
      for(j=0; j<pnp; ++j)
        ptr2[j]=ptr1[j*pnp+j];
    }

/*
if(!(itno%100)){
  printf("Current estimate: ");
  for(i=0; i<nvars; ++i)
    printf("%.9g ", p[i]);
  printf("-- errors %.9g %0.9g\n", eab_inf, p_eL2);
}
*/

    /* check for convergence */
    if((eab_inf <= eps1)){
      dp_L2=0.0; /* no increment for p in this case */
      stop=1;
      break;
    }

   /* compute initial damping factor */
    if(itno==0){
      /* find max diagonal element */
      for(i=mcon*cnp, tmp=DBL_MIN; i<m*cnp; ++i)
        if(diagUV[i]>tmp) tmp=diagUV[i];
      for(i=m*cnp + ncon*pnp; i<nvars; ++i) /* tmp is not re-initialized! */
        if(diagUV[i]>tmp) tmp=diagUV[i];
      mu=tau*tmp;
    }

    /* determine increment using adaptive damping */
    while(1){
      /* augment U, V */
      for(j=mcon; j<m; ++j){
        ptr1=U + j*Usz; // set ptr1 to point to U_j
        for(i=0; i<cnp; ++i)
          ptr1[i*cnp+i]+=mu;
      }
      for(i=ncon; i<n; ++i){
        ptr1=V + i*Vsz; // set ptr1 to point to V_i
        for(j=0; j<pnp; ++j)
          ptr1[j*pnp+j]+=mu;

		    /* compute V*_i^-1.
         * Recall that only the upper triangle of the symmetric pnp x pnp matrix V*_i
         * is stored in ptr1; its (also symmetric) inverse is saved in the lower triangle of ptr1
         */
        /* inverting V*_i with LDLT seems to result in faster overall execution compared to when using LU or Cholesky */
        //j=sba_symat_invert_LU(ptr1, pnp); matinv=sba_symat_invert_LU;
        //j=sba_symat_invert_Chol(ptr1, pnp); matinv=sba_symat_invert_Chol;
        j=sba_symat_invert_BK(ptr1, pnp); matinv=sba_symat_invert_BK;
		    if(!j){
			    fprintf(stderr, "SBA: singular matrix V*_i (i=%d) in sba_motstr_levmar_x(), increasing damping\n", i);
          goto moredamping; // increasing damping will eventually make V*_i diagonally dominant, thus nonsingular
          //retval=SBA_ERROR;
          //goto freemem_and_return;
		    }
      }

      _dblzero(E, m*easz); /* clear all e_j */
      /* compute the mmcon x mmcon block matrix S and e_j */

      /* Note that S is symmetric, therefore its computation can be
       * sped up by computing only the upper part and then reusing
       * it for the lower one.
		   */
      for(j=mcon; j<m; ++j){
        int mmconxUsz=mmcon*Usz;

		    nnz=sba_crsm_col_elmidxs(&idxij, j, rcidxs, rcsubs); /* find nonzero Y_ij, i=0...n-1 */

        /* get rid of all Y_ij with i<ncon that are treated as zeros.
         * In this way, all rcsubs[i] below are guaranteed to be >= ncon
         */
        if(ncon){
          for(i=ii=0; i<nnz; ++i){
            if(rcsubs[i]>=ncon){
              rcidxs[ii]=rcidxs[i];
              rcsubs[ii++]=rcsubs[i];
            }
          }
          nnz=ii;
        }

        /* compute all Y_ij = W_ij (V*_i)^-1 for a *fixed* j.
         * To save memory, the block matrix consisting of the Y_ij
         * is not stored. Instead, only a block column of this matrix
         * is computed & used at each time: For each j, all nonzero
         * Y_ij are computed in Yj and then used in the calculations
         * involving S_jk and e_j.
         * Recall that W_ij is cnp x pnp and (V*_i) is pnp x pnp
         */
        for(i=0; i<nnz; ++i){
          /* set ptr3 to point to (V*_i)^-1, actual row number in rcsubs[i] */
          ptr3=V + rcsubs[i]*Vsz;

          /* set ptr1 to point to Y_ij, actual row number in rcsubs[i] */
          ptr1=Yj + i*Ysz;
          /* set ptr2 to point to W_ij resp. */
          ptr2=W + idxij.val[rcidxs[i]]*Wsz;
          /* compute W_ij (V*_i)^-1 and store it in Y_ij.
           * Recall that only the lower triangle of (V*_i)^-1 is stored
           */
          for(ii=0; ii<cnp; ++ii){
            ptr4=ptr2+ii*pnp;
            for(jj=0; jj<pnp; ++jj){
              for(k=0, sum=0.0; k<=jj; ++k)
                sum+=ptr4[k]*ptr3[jj*pnp+k]; //ptr2[ii*pnp+k]*ptr3[jj*pnp+k];
              for( ; k<pnp; ++k)
                sum+=ptr4[k]*ptr3[k*pnp+jj]; //ptr2[ii*pnp+k]*ptr3[k*pnp+jj];
              ptr1[ii*pnp+jj]=sum;
            }
          }
        }

        /* compute the UPPER TRIANGULAR PART of S */
        for(k=j; k<m; ++k){ // j>=mcon
          /* compute \sum_i Y_ij W_ik^T in YWt. Note that for an off-diagonal block defined by j, k
           * YWt (and thus S_jk) is nonzero only if there exists a point that is visible in both the
           * j-th and k-th images
           */
          
          /* Recall that Y_ij is cnp x pnp and W_ik is cnp x pnp */ 
          _dblzero(YWt, YWtsz); /* clear YWt */

          for(i=0; i<nnz; ++i){
            register double *pYWt;

            /* find the min and max column indices of the elements in row i (actually rcsubs[i])
             * and make sure that k falls within them. This test handles W_ik's which are
             * certain to be zero without bothering to call sba_crsm_elmidx()
             */
            ii=idxij.colidx[idxij.rowptr[rcsubs[i]]];
            jj=idxij.colidx[idxij.rowptr[rcsubs[i]+1]-1];
            if(k<ii || k>jj) continue; /* W_ik == 0 */

            /* set ptr2 to point to W_ik */
            l=sba_crsm_elmidxp(&idxij, rcsubs[i], k, j, rcidxs[i]);
            //l=sba_crsm_elmidx(&idxij, rcsubs[i], k);
            if(l==-1) continue; /* W_ik == 0 */

            ptr2=W + idxij.val[l]*Wsz;
            /* set ptr1 to point to Y_ij, actual row number in rcsubs[i] */
            ptr1=Yj + i*Ysz;
            for(ii=0; ii<cnp; ++ii){
              ptr3=ptr1+ii*pnp;
              pYWt=YWt+ii*cnp;

              for(jj=0; jj<cnp; ++jj){
                ptr4=ptr2+jj*pnp;
                for(l=0, sum=0.0; l<pnp; ++l)
                  sum+=ptr3[l]*ptr4[l]; //ptr1[ii*pnp+l]*ptr2[jj*pnp+l];
                pYWt[jj]+=sum; //YWt[ii*cnp+jj]+=sum;
              }
            }
          }
		  
		      /* since the linear system involving S is solved with lapack,
		       * it is preferable to store S in column major (i.e. fortran)
		       * order, so as to avoid unecessary transposing/copying.
           */
#if MAT_STORAGE==COLUMN_MAJOR
          ptr2=S + (k-mcon)*mmconxUsz + (j-mcon)*cnp; // set ptr2 to point to the beginning of block j,k in S
#else
          ptr2=S + (j-mcon)*mmconxUsz + (k-mcon)*cnp; // set ptr2 to point to the beginning of block j,k in S
#endif
		  
          if(j!=k){ /* Kronecker */
            for(ii=0; ii<cnp; ++ii, ptr2+=Sdim)
              for(jj=0; jj<cnp; ++jj)
                ptr2[jj]=
#if MAT_STORAGE==COLUMN_MAJOR
				                -YWt[jj*cnp+ii];
#else
				                -YWt[ii*cnp+jj];
#endif
          }
          else{
            ptr1=U + j*Usz; // set ptr1 to point to U_j

            for(ii=0; ii<cnp; ++ii, ptr2+=Sdim)
              for(jj=0; jj<cnp; ++jj)
                ptr2[jj]=
#if MAT_STORAGE==COLUMN_MAJOR
				                ptr1[jj*cnp+ii] - YWt[jj*cnp+ii];
#else
				                ptr1[ii*cnp+jj] - YWt[ii*cnp+jj];
#endif
          }
        }

        /* copy the LOWER TRIANGULAR PART of S from the upper one */
        for(k=mcon; k<j; ++k){
#if MAT_STORAGE==COLUMN_MAJOR
          ptr1=S + (k-mcon)*mmconxUsz + (j-mcon)*cnp; // set ptr1 to point to the beginning of block j,k in S
          ptr2=S + (j-mcon)*mmconxUsz + (k-mcon)*cnp; // set ptr2 to point to the beginning of block k,j in S
#else
          ptr1=S + (j-mcon)*mmconxUsz + (k-mcon)*cnp; // set ptr1 to point to the beginning of block j,k in S
          ptr2=S + (k-mcon)*mmconxUsz + (j-mcon)*cnp; // set ptr2 to point to the beginning of block k,j in S
#endif
          for(ii=0; ii<cnp; ++ii, ptr1+=Sdim)
            for(jj=0, ptr3=ptr2+ii; jj<cnp; ++jj, ptr3+=Sdim)
              ptr1[jj]=*ptr3;
        }

        /* compute e_j=ea_j - \sum_i Y_ij eb_i */
        /* Recall that Y_ij is cnp x pnp and eb_i is pnp x 1 */
        ptr1=E + j*easz; // set ptr1 to point to e_j

        for(i=0; i<nnz; ++i){
          /* set ptr2 to point to Y_ij, actual row number in rcsubs[i] */
          ptr2=Yj + i*Ysz;

          /* set ptr3 to point to eb_i */
          ptr3=eb + rcsubs[i]*ebsz;
          for(ii=0; ii<cnp; ++ii){
            ptr4=ptr2+ii*pnp;
            for(jj=0, sum=0.0; jj<pnp; ++jj)
              sum+=ptr4[jj]*ptr3[jj]; //ptr2[ii*pnp+jj]*ptr3[jj];
            ptr1[ii]+=sum;
          }
        }

        ptr2=ea + j*easz; // set ptr2 to point to ea_j
        for(i=0; i<easz; ++i)
          ptr1[i]=ptr2[i] - ptr1[i];
      }

#if 0
      if(verbose>1){ /* count the nonzeros in S */
        for(i=ii=0; i<Sdim*Sdim; ++i)
          if(S[i]!=0.0) ++ii;
        printf("\nS density: %.5g\n", ((double)ii)/(Sdim*Sdim)); fflush(stdout);
      }
#endif

      /* solve the linear system S dpa = E to compute the da_j.
       *
       * Note that if MAT_STORAGE==1 S is modified in the following call;
       * this is OK since S is recomputed for each iteration
       */
	    //issolved=sba_Axb_LU(S, E+mcon*cnp, dpa+mcon*cnp, Sdim, MAT_STORAGE); linsolver=sba_Axb_LU;
      issolved=sba_Axb_Chol(S, E+mcon*cnp, dpa+mcon*cnp, Sdim, MAT_STORAGE); linsolver=sba_Axb_Chol;
      //issolved=sba_Axb_BK(S, E+mcon*cnp, dpa+mcon*cnp, Sdim, MAT_STORAGE); linsolver=sba_Axb_BK;
      //issolved=sba_Axb_QRnoQ(S, E+mcon*cnp, dpa+mcon*cnp, Sdim, MAT_STORAGE); linsolver=sba_Axb_QRnoQ;
      //issolved=sba_Axb_QR(S, E+mcon*cnp, dpa+mcon*cnp, Sdim, MAT_STORAGE); linsolver=sba_Axb_QR;
	    //issolved=sba_Axb_SVD(S, E+mcon*cnp, dpa+mcon*cnp, Sdim, MAT_STORAGE); linsolver=sba_Axb_SVD;
	    //issolved=sba_Axb_CG(S, E+mcon*cnp, dpa+mcon*cnp, Sdim, (3*Sdim)/2, 1E-10, SBA_CG_JACOBI, MAT_STORAGE); linsolver=(PLS)sba_Axb_CG;

      ++nlss;

	    _dblzero(dpa, mcon*cnp); /* no change for the first mcon camera params */

      if(issolved){

        /* compute the db_i */
        for(i=ncon; i<n; ++i){
          ptr1=dpb + i*ebsz; // set ptr1 to point to db_i

          /* compute \sum_j W_ij^T da_j */
          /* Recall that W_ij is cnp x pnp and da_j is cnp x 1 */
          _dblzero(Wtda, Wtdasz); /* clear Wtda */
          nnz=sba_crsm_row_elmidxs(&idxij, i, rcidxs, rcsubs); /* find nonzero W_ij, j=0...m-1 */
          for(j=0; j<nnz; ++j){
            /* set ptr2 to point to W_ij, actual column number in rcsubs[j] */
			      if(rcsubs[j]<mcon) continue; /* W_ij is zero */

            ptr2=W + idxij.val[rcidxs[j]]*Wsz;

            /* set ptr3 to point to da_j */
            ptr3=dpa + rcsubs[j]*cnp;

            for(ii=0; ii<pnp; ++ii){
              ptr4=ptr2+ii;
              for(jj=0, sum=0.0; jj<cnp; ++jj)
                sum+=ptr4[jj*pnp]*ptr3[jj]; //ptr2[jj*pnp+ii]*ptr3[jj];
              Wtda[ii]+=sum;
            }
          }

          /* compute eb_i - \sum_j W_ij^T da_j = eb_i - Wtda in Wtda */
          ptr2=eb + i*ebsz; // set ptr2 to point to eb_i
          for(ii=0; ii<pnp; ++ii)
            Wtda[ii]=ptr2[ii] - Wtda[ii];

          /* compute the product (V*_i)^-1 Wtda = (V*_i)^-1 (eb_i - \sum_j W_ij^T da_j).
           * Recall that only the lower triangle of (V*_i)^-1 is stored
           */
          ptr2=V + i*Vsz; // set ptr2 to point to (V*_i)^-1
          for(ii=0; ii<pnp; ++ii){
            for(jj=0, sum=0.0; jj<=ii; ++jj)
              sum+=ptr2[ii*pnp+jj]*Wtda[jj];
            for( ; jj<pnp; ++jj)
              sum+=ptr2[jj*pnp+ii]*Wtda[jj];
            ptr1[ii]=sum;
          }
        }
	      _dblzero(dpb, ncon*pnp); /* no change for the first ncon point params */

        /* parameter vector updates are now in dpa, dpb */

        /* compute p's new estimate and ||dp||^2 */
        for(i=0, dp_L2=0.0; i<nvars; ++i){
          pdp[i]=p[i] + (tmp=dp[i]);
          dp_L2+=tmp*tmp;
        }
        //dp_L2=sqrt(dp_L2);

        if(dp_L2<=eps2_sq*p_L2){ /* relative change in p is small, stop */
        //if(dp_L2<=eps2*(p_L2 + eps2)){ /* relative change in p is small, stop */
          stop=2;
          break;
        }

        if(dp_L2>=(p_L2+eps2)/SBA_EPSILON_SQ){ /* almost singular */
        //if(dp_L2>=(p_L2+eps2)/SBA_EPSILON){ /* almost singular */
          fprintf(stderr, "SBA: the matrix of the augmented normal equations is almost singular in sba_motstr_levmar_x(),\n"
                          "     minimization should be restarted from the current solution with an increased damping term\n");
          retval=SBA_ERROR;
          goto freemem_and_return;
       }

        (*func)(pdp, &idxij, rcidxs, rcsubs, hx, adata); ++nfev; /* evaluate function at p + dp */
        if(verbose>1)
          printf("mean reprojection error %g\n", sba_mean_repr_error(n, mnp, x, hx, &idxij, rcidxs, rcsubs));
        /* ### compute ||e(pdp)||_2 */
        if(covx==NULL)
          pdp_eL2=nrmL2xmy(hx, x, hx, nobs); /* hx=x-hx, pdp_eL2=||hx|| */
        else
          pdp_eL2=nrmCxmy(hx, x, hx, wght, mnp, nvis); /* hx=wght*(x-hx), pdp_eL2=||hx|| */
        if(!SBA_FINITE(pdp_eL2)){
          if(verbose) /* identify the offending point projection */
            sba_print_inf(hx, m, mnp, &idxij, rcidxs, rcsubs);

          stop=7;
          break;
        }

        for(i=0, dL=0.0; i<nvars; ++i)
          dL+=dp[i]*(mu*dp[i]+eab[i]);

        dF=p_eL2-pdp_eL2;

        if(verbose>1)
          printf("\ndamping term %8g, gain ratio %8g, errors %8g / %8g = %g\n", mu, dL!=0.0? dF/dL : dF/DBL_EPSILON, p_eL2/nvis, pdp_eL2/nvis, p_eL2/pdp_eL2);

        if(dL>0.0 && dF>0.0){ /* reduction in error, increment is accepted */
          tmp=(2.0*dF/dL-1.0);
          tmp=1.0-tmp*tmp*tmp;
          mu=mu*( (tmp>=SBA_ONE_THIRD)? tmp : SBA_ONE_THIRD );
          nu=2;

          /* the test below is equivalent to the relative reduction of the RMS reprojection error: sqrt(p_eL2)-sqrt(pdp_eL2)<eps4*sqrt(p_eL2) */
          if(pdp_eL2-2.0*sqrt(p_eL2*pdp_eL2)<(eps4_sq-1.0)*p_eL2) stop=4;
          
          for(i=0; i<nvars; ++i) /* update p's estimate */
            p[i]=pdp[i];

          for(i=0; i<nobs; ++i) /* update e and ||e||_2 */
            e[i]=hx[i];
          p_eL2=pdp_eL2;
          break;
        }
      } /* issolved */

moredamping:
      /* if this point is reached (also via an explicit goto!), either the linear system could
       * not be solved or the error did not reduce; in any case, the increment must be rejected
       */

      mu*=nu;
      nu2=nu<<1; // 2*nu;
      if(nu2<=nu){ /* nu has wrapped around (overflown) */
        fprintf(stderr, "SBA: too many failed attempts to increase the damping factor in sba_motstr_levmar_x()! Singular Hessian matrix?\n");
        //exit(1);
        stop=6;
        break;
      }
      nu=nu2;

#if 0
      /* restore U, V diagonal entries */
      for(j=mcon; j<m; ++j){
        ptr1=U + j*Usz; // set ptr1 to point to U_j
        ptr2=diagU + j*cnp; // set ptr2 to point to diagU_j
        for(i=0; i<cnp; ++i)
          ptr1[i*cnp+i]=ptr2[i];
      }
      for(i=ncon; i<n; ++i){
        ptr1=V + i*Vsz; // set ptr1 to point to V_i
        ptr2=diagV + i*pnp; // set ptr2 to point to diagV_i
        for(j=0; j<pnp; ++j)
          ptr1[j*pnp+j]=ptr2[j];
      }
#endif
    } /* inner while loop */

    if(p_eL2<=eps3_sq) stop=5; // error is small, force termination of outer loop
  }

  if(itno>=itmax) stop=3;

  /* restore U, V diagonal entries */
  for(j=mcon; j<m; ++j){
    ptr1=U + j*Usz; // set ptr1 to point to U_j
    ptr2=diagU + j*cnp; // set ptr2 to point to diagU_j
    for(i=0; i<cnp; ++i)
      ptr1[i*cnp+i]=ptr2[i];
  }
  for(i=ncon; i<n; ++i){
    ptr1=V + i*Vsz; // set ptr1 to point to V_i
    ptr2=diagV + i*pnp; // set ptr2 to point to diagV_i
    for(j=0; j<pnp; ++j)
     ptr1[j*pnp+j]=ptr2[j];
  }

  if(info){
    info[0]=init_p_eL2;
    info[1]=p_eL2;
    info[2]=eab_inf;
    info[3]=dp_L2;
    for(j=mcon, tmp=DBL_MIN; j<m; ++j){
      ptr1=U + j*Usz; // set ptr1 to point to U_j
      for(i=0; i<cnp; ++i)
        if(tmp<ptr1[i*cnp+i]) tmp=ptr1[i*cnp+i];
    }
    for(i=ncon; i<n; ++i){
      ptr1=V + i*Vsz; // set ptr1 to point to V_i
      for(j=0; j<pnp; ++j)
        if(tmp<ptr1[j*pnp+j]) tmp=ptr1[j*pnp+j];
      }
    info[4]=mu/tmp;
    info[5]=itno;
    info[6]=stop;
    info[7]=nfev;
    info[8]=njev;
    info[9]=nlss;
  }
                                                               
  //sba_print_sol(n, m, p, cnp, pnp, x, mnp, &idxij, rcidxs, rcsubs);
  retval=(stop!=7)?  itno : SBA_ERROR;

freemem_and_return: /* NOTE: this point is also reached via a goto! */

   /* free whatever was allocated */
  free(W);   free(U);  free(V);
  free(e);   free(eab);
  free(E);   free(Yj); free(YWt);
  free(S);   free(dp); free(Wtda);
  free(rcidxs); free(rcsubs);
#ifndef SBA_DESTROY_COVS
  if(wght) free(wght);
#else
  /* nothing to do */
#endif /* SBA_DESTROY_COVS */

  free(hx); free(diagUV); free(pdp);
  if(fdj_data.hxx){ // cleanup
    free(fdj_data.hxx);
    free(fdj_data.func_rcidxs);
  }

  sba_crsm_free(&idxij);

  /* free the memory allocated by the matrix inversion & linear solver routines */
  if(matinv) (*matinv)(NULL, 0);
  if(linsolver) (*linsolver)(NULL, NULL, NULL, 0, 0);

  return retval;
}


/* Bundle adjustment on camera parameters only 
 * using the sparse Levenberg-Marquardt as described in HZ p. 568
 *
 * Returns the number of iterations (>=0) if successfull, SBA_ERROR if failed
 */

int sba_mot_levmar_x(
    const int n,   /* number of points */
    const int m,   /* number of images */
    const int mcon,/* number of images (starting from the 1st) whose parameters should not be modified.
					          * All A_ij (see below) with j<mcon are assumed to be zero
					          */
    char *vmask,  /* visibility mask: vmask[i, j]=1 if point i visible in image j, 0 otherwise. nxm */
    double *p,    /* initial parameter vector p0: (a1, ..., am).
                   * aj are the image j parameters, size m*cnp */
    const int cnp,/* number of parameters for ONE camera; e.g. 6 for Euclidean cameras */
    double *x,    /* measurements vector: (x_11^T, .. x_1m^T, ..., x_n1^T, .. x_nm^T)^T where
                   * x_ij is the projection of the i-th point on the j-th image.
                   * NOTE: some of the x_ij might be missing, if point i is not visible in image j;
                   * see vmask[i, j], max. size n*m*mnp
                   */
    double *covx, /* measurements covariance matrices: (Sigma_x_11, .. Sigma_x_1m, ..., Sigma_x_n1, .. Sigma_x_nm),
                   * where Sigma_x_ij is the mnp x mnp covariance of x_ij stored row-by-row. Set to NULL if no
                   * covariance estimates are available (identity matrices are implicitly used in this case).
                   * NOTE: a certain Sigma_x_ij is missing if the corresponding x_ij is also missing;
                   * see vmask[i, j], max. size n*m*mnp*mnp
                   */
    const int mnp,/* number of parameters for EACH measurement; usually 2 */
    void (*func)(double *p, struct sba_crsm *idxij, int *rcidxs, int *rcsubs, double *hx, void *adata),
                                              /* functional relation describing measurements. Given a parameter vector p,
                                               * computes a prediction of the measurements \hat{x}. p is (m*cnp)x1,
                                               * \hat{x} is (n*m*mnp)x1, maximum
                                               * rcidxs, rcsubs are max(m, n) x 1, allocated by the caller and can be used
                                               * as working memory
                                               */
    void (*fjac)(double *p, struct sba_crsm *idxij, int *rcidxs, int *rcsubs, double *jac, void *adata),
                                              /* function to evaluate the sparse jacobian dX/dp.
                                               * The Jacobian is returned in jac as
                                               * (dx_11/da_1, ..., dx_1m/da_m, ..., dx_n1/da_1, ..., dx_nm/da_m), or (using HZ's notation),
                                               * jac=(A_11, ..., A_1m, ..., A_n1, ..., A_nm)
                                               * Notice that depending on idxij, some of the A_ij might be missing.
                                               * Note also that the A_ij are mnp x cnp matrices and they
                                               * should be stored in jac in row-major order.
                                               * rcidxs, rcsubs are max(m, n) x 1, allocated by the caller and can be used
                                               * as working memory
                                               *
                                               * If NULL, the jacobian is approximated by repetitive func calls and finite
                                               * differences. This is computationally inefficient and thus NOT recommended.
                                               */
    void *adata,       /* pointer to possibly additional data, passed uninterpreted to func, fjac */ 

    const int itmax,   /* I: maximum number of iterations. itmax==0 signals jacobian verification followed by immediate return */
    const int verbose, /* I: verbosity */
    const double opts[SBA_OPTSSZ],
	                     /* I: minim. options [\mu, \epsilon1, \epsilon2, \epsilon3, \epsilon4]. Respectively the scale factor for initial \mu,
                        * stopping thresholds for ||J^T e||_inf, ||dp||_2, ||e||_2 and (||e||_2-||e_new||_2)/||e||_2
                        */
    double info[SBA_INFOSZ]
	                     /* O: information regarding the minimization. Set to NULL if don't care
                        * info[0]=||e||_2 at initial p.
                        * info[1-4]=[ ||e||_2, ||J^T e||_inf,  ||dp||_2, mu/max[J^T J]_ii ], all computed at estimated p.
                        * info[5]= # iterations,
                        * info[6]=reason for terminating: 1 - stopped by small gradient J^T e
                        *                                 2 - stopped by small dp
                        *                                 3 - stopped by itmax
                        *                                 4 - stopped by small relative reduction in ||e||_2
                        *                                 5 - stopped by small ||e||_2
                        *                                 6 - too many attempts to increase damping. Restart with increased mu
                        *                                 7 - stopped by invalid (i.e. NaN or Inf) "func" values. This is a user error
                        * info[7]= # function evaluations
                        * info[8]= # jacobian evaluations
			                  * info[9]= # number of linear systems solved, i.e. number of attempts	for reducing error
                        */
)
{
register int i, j, ii, jj, k;
int nvis, nnz, retval;

/* The following are work arrays that are dynamically allocated by sba_mot_levmar_x() */
double *jac; /* work array for storing the jacobian, max. size n*m*mnp*cnp */
double *U;    /* work array for storing the U_j in the order U_1, ..., U_m, size m*cnp*cnp */

double *e;    /* work array for storing the e_ij in the order e_11, ..., e_1m, ..., e_n1, ..., e_nm,
                 max. size n*m*mnp */
double *ea;   /* work array for storing the ea_j in the order ea_1, .. ea_m, size m*cnp */

double *dp;   /* work array for storing the parameter vector updates da_1, ..., da_m, size m*cnp */

double *wght= /* work array for storing the weights computed from the covariance inverses, max. size n*m*mnp*mnp */
            NULL;

/* Of the above arrays, jac, e, wght are sparse and
 * U, ea, dp are dense. Sparse arrays are indexed through
 * idxij (see below), that is with the same mechanism as the input 
 * measurements vector x
 */

/* submatrices sizes */
int Asz, Usz,
    esz, easz, covsz;

register double *ptr1, *ptr2, *ptr3, *ptr4, sum;
struct sba_crsm idxij; /* sparse matrix containing the location of x_ij in x. This is also the location of A_ij 
                        * in jac, e_ij in e, etc.
                        * This matrix can be thought as a map from a sparse set of pairs (i, j) to a continuous
                        * index k and it is used to efficiently lookup the memory locations where the non-zero
                        * blocks of a sparse matrix/vector are stored
                        */
int maxCPvis, /* max. of projections across cameras & projections across points */
    *rcidxs,  /* work array for the indexes corresponding to the nonzero elements of a single row or
                 column in a sparse matrix, size max(n, m) */
    *rcsubs;  /* work array for the subscripts of nonzero elements in a single row or column of a
                 sparse matrix, size max(n, m) */

/* The following variables are needed by the LM algorithm */
register int itno;  /* iteration counter */
int nsolved;
/* temporary work arrays that are dynamically allocated */
double *hx,         /* \hat{x}_i, max. size m*n*mnp */
       *diagU,      /* diagonals of U_j, size m*cnp */
       *pdp;        /* p + dp, size m*cnp */

register double mu,  /* damping constant */
                tmp; /* mainly used in matrix & vector multiplications */
double p_eL2, ea_inf, pdp_eL2; /* ||e(p)||_2, ||J^T e||_inf, ||e(p+dp)||_2 */
double p_L2, dp_L2=DBL_MAX, dF, dL;
double tau=FABS(opts[0]), eps1=FABS(opts[1]), eps2=FABS(opts[2]), eps2_sq=opts[2]*opts[2],
       eps3_sq=opts[3]*opts[3], eps4_sq=opts[4]*opts[4];
double init_p_eL2;
int nu=2, nu2, stop=0, nfev, njev=0, nlss=0;
int nobs, nvars;
PLS linsolver=NULL;

struct fdj_data_x_ fdj_data;
void *jac_adata;

/* Initialization */
  mu=ea_inf=0.0; /* -Wall */

  /* block sizes */
  Asz=mnp * cnp; Usz=cnp * cnp;
  esz=mnp; easz=cnp;
  covsz=mnp * mnp;
  
  /* count total number of visible image points */
  for(i=nvis=0, jj=n*m; i<jj; ++i)
    nvis+=(vmask[i]!=0);

  nobs=nvis*mnp;
  nvars=m*cnp;
  if(nobs<nvars){
    fprintf(stderr, "SBA: sba_mot_levmar_x() cannot solve a problem with fewer measurements [%d] than unknowns [%d]\n", nobs, nvars);
    return SBA_ERROR;
  }

  /* allocate & fill up the idxij structure */
  sba_crsm_alloc(&idxij, n, m, nvis);
  for(i=k=0; i<n; ++i){
    idxij.rowptr[i]=k;
    ii=i*m;
    for(j=0; j<m; ++j)
      if(vmask[ii+j]){
        idxij.val[k]=k;
        idxij.colidx[k++]=j;
      }
  }
  idxij.rowptr[n]=nvis;

  /* find the maximum number of visible image points in any single camera or coming from a single 3D point */
  /* cameras */
  for(i=maxCPvis=0; i<n; ++i)
    if((k=idxij.rowptr[i+1]-idxij.rowptr[i])>maxCPvis) maxCPvis=k;

  /* points, note that maxCPvis is not reinitialized! */
  for(j=0; j<m; ++j){
    for(i=ii=0; i<n; ++i)
      if(vmask[i*m+j]) ++ii;
    if(ii>maxCPvis) maxCPvis=ii;
  }

  /* allocate work arrays */
  jac=(double *)emalloc(nvis*Asz*sizeof(double));
  U=(double *)emalloc(m*Usz*sizeof(double));
  e=(double *)emalloc(nobs*sizeof(double));
  ea=(double *)emalloc(nvars*sizeof(double));
  dp=(double *)emalloc(nvars*sizeof(double));
  rcidxs=(int *)emalloc(maxCPvis*sizeof(int));
  rcsubs=(int *)emalloc(maxCPvis*sizeof(int));
#ifndef SBA_DESTROY_COVS
  if(covx!=NULL) wght=(double *)emalloc(nvis*covsz*sizeof(double));
#else
  if(covx!=NULL) wght=covx;
#endif /* SBA_DESTROY_COVS */


  hx=(double *)emalloc(nobs*sizeof(double));
  diagU=(double *)emalloc(nvars*sizeof(double));
  pdp=(double *)emalloc(nvars*sizeof(double));

  /* if no jacobian function is supplied, prepare to compute jacobian with finite difference */
  if(!fjac){
    fdj_data.func=func;
    fdj_data.cnp=cnp;
    fdj_data.pnp=0;
    fdj_data.mnp=mnp;
    fdj_data.hx=hx;
    fdj_data.hxx=(double *)emalloc(nobs*sizeof(double));
    fdj_data.func_rcidxs=(int *)emalloc(2*maxCPvis*sizeof(int));
    fdj_data.func_rcsubs=fdj_data.func_rcidxs+maxCPvis;
    fdj_data.adata=adata;

    fjac=sba_fdjac_x;
    jac_adata=(void *)&fdj_data;
  }
  else{
    fdj_data.hxx=NULL;
    jac_adata=adata;
  }

  if(itmax==0){ /* verify jacobian */
    sba_mot_chkjac_x(func, fjac, p, &idxij, rcidxs, rcsubs, mcon, cnp, mnp, adata, jac_adata);
    retval=0;
    goto freemem_and_return;
  }

  /* covariances Sigma_x_ij are accommodated by computing the Cholesky decompositions of their
   * inverses and using the resulting matrices w_x_ij to weigh A_ij and e_ij as w_x_ij A_ij
   * and w_x_ij*e_ij. In this way, auxiliary variables as U_j=\sum_i A_ij^T A_ij
   * actually become \sum_i (w_x_ij A_ij)^T w_x_ij A_ij= \sum_i A_ij^T w_x_ij^T w_x_ij A_ij =
   * A_ij^T Sigma_x_ij^-1 A_ij
   *
   * ea_j are weighted in a similar manner
   */
  if(covx!=NULL){
    for(i=0; i<n; ++i){
      nnz=sba_crsm_row_elmidxs(&idxij, i, rcidxs, rcsubs); /* find nonzero x_ij, j=0...m-1 */
      for(j=0; j<nnz; ++j){
        /* set ptr1, ptr2 to point to cov_x_ij, w_x_ij resp. */
        ptr1=covx + (k=idxij.val[rcidxs[j]]*covsz);
        ptr2=wght + k;
        if(!sba_mat_cholinv(ptr1, ptr2, mnp)){ /* compute w_x_ij s.t. w_x_ij^T w_x_ij = cov_x_ij^-1 */
			    fprintf(stderr, "SBA: invalid covariance matrix for x_ij (i=%d, j=%d) in sba_motstr_levmar_x()\n", i, rcsubs[j]);
          retval=SBA_ERROR;
          goto freemem_and_return;
        }
      }
    }
    sba_mat_cholinv(NULL, NULL, 0); /* cleanup */
  }

  /* compute the error vectors e_ij in hx */
  (*func)(p, &idxij, rcidxs, rcsubs, hx, adata); nfev=1;
  /* ### compute e=x - f(p) [e=w*(x - f(p)] and its L2 norm */
  if(covx==NULL)
    p_eL2=nrmL2xmy(e, x, hx, nobs); /* e=x-hx, p_eL2=||e|| */
  else
    p_eL2=nrmCxmy(e, x, hx, wght, mnp, nvis); /* e=wght*(x-hx), p_eL2=||e||=||x-hx||_Sigma^-1 */
  if(verbose) printf("initial mot-SBA error %g [%g]\n", p_eL2, p_eL2/nvis);
  init_p_eL2=p_eL2;
  if(!SBA_FINITE(p_eL2)) stop=7;

  for(itno=0; itno<itmax && !stop; ++itno){
    /* Note that p, e and ||e||_2 have been updated at the previous iteration */

    /* compute derivative submatrices A_ij */
    (*fjac)(p, &idxij, rcidxs, rcsubs, jac, jac_adata); ++njev;

    if(covx!=NULL){
      /* compute w_x_ij A_ij
       * Since w_x_ij is upper triangular, the products can be safely saved
       * directly in A_ij, without the need for intermediate storage
       */
      for(i=0; i<nvis; ++i){
        /* set ptr1, ptr2 to point to w_x_ij, A_ij, resp. */
        ptr1=wght + i*covsz;
        ptr2=jac  + i*Asz;

        /* w_x_ij is mnp x mnp, A_ij is mnp x cnp */
        for(ii=0; ii<mnp; ++ii)
          for(jj=0; jj<cnp; ++jj){
            for(k=ii, sum=0.0; k<mnp; ++k) // k>=ii since w_x_ij is upper triangular
              sum+=ptr1[ii*mnp+k]*ptr2[k*cnp+jj];
            ptr2[ii*cnp+jj]=sum;
          }
      }
    }

    /* compute U_j = \sum_i A_ij^T A_ij */ // \Sigma here!
    /* U_j is symmetric, therefore its computation can be sped up by
     * computing only the upper part and then reusing it for the lower one.
     * Recall that A_ij is mnp x cnp
     */
    /* Also compute ea_j = \sum_i A_ij^T e_ij */ // \Sigma here!
    /* Recall that e_ij is mnp x 1
     */
	  _dblzero(U, m*Usz); /* clear all U_j */
	  _dblzero(ea, m*easz); /* clear all ea_j */
    for(j=mcon; j<m; ++j){
      ptr1=U + j*Usz; // set ptr1 to point to U_j
      ptr2=ea + j*easz; // set ptr2 to point to ea_j

      nnz=sba_crsm_col_elmidxs(&idxij, j, rcidxs, rcsubs); /* find nonzero A_ij, i=0...n-1 */
      for(i=0; i<nnz; ++i){
        /* set ptr3 to point to A_ij, actual row number in rcsubs[i] */
        ptr3=jac + idxij.val[rcidxs[i]]*Asz;

        /* compute the UPPER TRIANGULAR PART of A_ij^T A_ij and add it to U_j */
        for(ii=0; ii<cnp; ++ii){
          for(jj=ii; jj<cnp; ++jj){
            for(k=0, sum=0.0; k<mnp; ++k)
              sum+=ptr3[k*cnp+ii]*ptr3[k*cnp+jj];
            ptr1[ii*cnp+jj]+=sum;
          }

          /* copy the LOWER TRIANGULAR PART of U_j from the upper one */
          for(jj=0; jj<ii; ++jj)
            ptr1[ii*cnp+jj]=ptr1[jj*cnp+ii];
        }

        ptr4=e + idxij.val[rcidxs[i]]*esz; /* set ptr4 to point to e_ij */
        /* compute A_ij^T e_ij and add it to ea_j */
        for(ii=0; ii<cnp; ++ii){
          for(jj=0, sum=0.0; jj<mnp; ++jj)
            sum+=ptr3[jj*cnp+ii]*ptr4[jj];
          ptr2[ii]+=sum;
        }
      }
    }

    /* Compute ||J^T e||_inf and ||p||^2 */
    for(i=0, p_L2=ea_inf=0.0; i<nvars; ++i){
      if(ea_inf < (tmp=FABS(ea[i]))) ea_inf=tmp;
      p_L2+=p[i]*p[i];
    }
    //p_L2=sqrt(p_L2);

    /* save diagonal entries so that augmentation can be later canceled.
     * Diagonal entries are in U_j
     */
    for(j=mcon; j<m; ++j){
      ptr1=U + j*Usz; // set ptr1 to point to U_j
      ptr2=diagU + j*cnp; // set ptr2 to point to diagU_j
      for(i=0; i<cnp; ++i)
        ptr2[i]=ptr1[i*cnp+i];
    }

/*
if(!(itno%100)){
  printf("Current estimate: ");
  for(i=0; i<nvars; ++i)
    printf("%.9g ", p[i]);
  printf("-- errors %.9g %0.9g\n", ea_inf, p_eL2);
}
*/

    /* check for convergence */
    if((ea_inf <= eps1)){
      dp_L2=0.0; /* no increment for p in this case */
      stop=1;
      break;
    }

   /* compute initial damping factor */
    if(itno==0){
      for(i=mcon*cnp, tmp=DBL_MIN; i<nvars; ++i)
        if(diagU[i]>tmp) tmp=diagU[i]; /* find max diagonal element */
      mu=tau*tmp;
    }

    /* determine increment using adaptive damping */
    while(1){
      /* augment U */
      for(j=mcon; j<m; ++j){
        ptr1=U + j*Usz; // set ptr1 to point to U_j
        for(i=0; i<cnp; ++i)
          ptr1[i*cnp+i]+=mu;
      }
 
      /* solve the linear systems U_j da_j = ea_j to compute the da_j */
      _dblzero(dp, mcon*cnp); /* no change for the first mcon camera params */
      for(j=nsolved=mcon; j<m; ++j){
		    ptr1=U + j*Usz; // set ptr1 to point to U_j
		    ptr2=ea + j*easz; // set ptr2 to point to ea_j
		    ptr3=dp + j*cnp; // set ptr3 to point to da_j

		    //nsolved+=sba_Axb_LU(ptr1, ptr2, ptr3, cnp, 0); linsolver=sba_Axb_LU;
		    nsolved+=sba_Axb_Chol(ptr1, ptr2, ptr3, cnp, 0); linsolver=sba_Axb_Chol;
		    //nsolved+=sba_Axb_BK(ptr1, ptr2, ptr3, cnp, 0); linsolver=sba_Axb_BK;
		    //nsolved+=sba_Axb_QRnoQ(ptr1, ptr2, ptr3, cnp, 0); linsolver=sba_Axb_QRnoQ;
		    //nsolved+=sba_Axb_QR(ptr1, ptr2, ptr3, cnp, 0); linsolver=sba_Axb_QR;
		    //nsolved+=sba_Axb_SVD(ptr1, ptr2, ptr3, cnp, 0); linsolver=sba_Axb_SVD;
	      //nsolved+=(sba_Axb_CG(ptr1, ptr2, ptr3, cnp, (3*cnp)/2, 1E-10, SBA_CG_JACOBI, 0) > 0); linsolver=(PLS)sba_Axb_CG;

        ++nlss;
	    }

	    if(nsolved==m){

        /* parameter vector updates are now in dp */

        /* compute p's new estimate and ||dp||^2 */
        for(i=0, dp_L2=0.0; i<nvars; ++i){
          pdp[i]=p[i] + (tmp=dp[i]);
          dp_L2+=tmp*tmp;
        }
        //dp_L2=sqrt(dp_L2);

        if(dp_L2<=eps2_sq*p_L2){ /* relative change in p is small, stop */
        //if(dp_L2<=eps2*(p_L2 + eps2)){ /* relative change in p is small, stop */
          stop=2;
          break;
        }

        if(dp_L2>=(p_L2+eps2)/SBA_EPSILON_SQ){ /* almost singular */
        //if(dp_L2>=(p_L2+eps2)/SBA_EPSILON){ /* almost singular */
          fprintf(stderr, "SBA: the matrix of the augmented normal equations is almost singular in sba_mot_levmar_x(),\n"
                          "     minimization should be restarted from the current solution with an increased damping term\n");
          retval=SBA_ERROR;
          goto freemem_and_return;
        }

        (*func)(pdp, &idxij, rcidxs, rcsubs, hx, adata); ++nfev; /* evaluate function at p + dp */
        if(verbose>1)
          printf("mean reprojection error %g\n", sba_mean_repr_error(n, mnp, x, hx, &idxij, rcidxs, rcsubs));
        /* ### compute ||e(pdp)||_2 */
        if(covx==NULL)
          pdp_eL2=nrmL2xmy(hx, x, hx, nobs); /* hx=x-hx, pdp_eL2=||hx|| */
        else
          pdp_eL2=nrmCxmy(hx, x, hx, wght, mnp, nvis); /* hx=wght*(x-hx), pdp_eL2=||hx|| */
        if(!SBA_FINITE(pdp_eL2)){
          if(verbose) /* identify the offending point projection */
            sba_print_inf(hx, m, mnp, &idxij, rcidxs, rcsubs);

          stop=7;
          break;
        }

        for(i=0, dL=0.0; i<nvars; ++i)
          dL+=dp[i]*(mu*dp[i]+ea[i]);

        dF=p_eL2-pdp_eL2;

        if(verbose>1)
          printf("\ndamping term %8g, gain ratio %8g, errors %8g / %8g = %g\n", mu, dL!=0.0? dF/dL : dF/DBL_EPSILON, p_eL2/nvis, pdp_eL2/nvis, p_eL2/pdp_eL2);

        if(dL>0.0 && dF>0.0){ /* reduction in error, increment is accepted */
          tmp=(2.0*dF/dL-1.0);
          tmp=1.0-tmp*tmp*tmp;
          mu=mu*( (tmp>=SBA_ONE_THIRD)? tmp : SBA_ONE_THIRD );
          nu=2;

          /* the test below is equivalent to the relative reduction of the RMS reprojection error: sqrt(p_eL2)-sqrt(pdp_eL2)<eps4*sqrt(p_eL2) */
          if(pdp_eL2-2.0*sqrt(p_eL2*pdp_eL2)<(eps4_sq-1.0)*p_eL2) stop=4;
          
          for(i=0; i<nvars; ++i) /* update p's estimate */
            p[i]=pdp[i];

          for(i=0; i<nobs; ++i) /* update e and ||e||_2 */
            e[i]=hx[i];
          p_eL2=pdp_eL2;
          break;
        }
      } /* nsolved==m */

      /* if this point is reached, either at least one linear system could not be solved or
       * the error did not reduce; in any case, the increment must be rejected
       */

      mu*=nu;
      nu2=nu<<1; // 2*nu;
      if(nu2<=nu){ /* nu has wrapped around (overflown) */
        fprintf(stderr, "SBA: too many failed attempts to increase the damping factor in sba_mot_levmar_x()! Singular Hessian matrix?\n");
        //exit(1);
        stop=6;
        break;
      }
      nu=nu2;

#if 0
      /* restore U diagonal entries */
      for(j=mcon; j<m; ++j){
        ptr1=U + j*Usz; // set ptr1 to point to U_j
        ptr2=diagU + j*cnp; // set ptr2 to point to diagU_j
        for(i=0; i<cnp; ++i)
          ptr1[i*cnp+i]=ptr2[i];
      }
#endif
    } /* inner while loop */

    if(p_eL2<=eps3_sq) stop=5; // error is small, force termination of outer loop
  }

  if(itno>=itmax) stop=3;

  /* restore U diagonal entries */
  for(j=mcon; j<m; ++j){
    ptr1=U + j*Usz; // set ptr1 to point to U_j
    ptr2=diagU + j*cnp; // set ptr2 to point to diagU_j
    for(i=0; i<cnp; ++i)
      ptr1[i*cnp+i]=ptr2[i];
  }

  if(info){
    info[0]=init_p_eL2;
    info[1]=p_eL2;
    info[2]=ea_inf;
    info[3]=dp_L2;
    for(j=mcon, tmp=DBL_MIN; j<m; ++j){
      ptr1=U + j*Usz; // set ptr1 to point to U_j
      for(i=0; i<cnp; ++i)
        if(tmp<ptr1[i*cnp+i]) tmp=ptr1[i*cnp+i];
    }
    info[4]=mu/tmp;
    info[5]=itno;
    info[6]=stop;
    info[7]=nfev;
    info[8]=njev;
    info[9]=nlss;
  }
  //sba_print_sol(n, m, p, cnp, 0, x, mnp, &idxij, rcidxs, rcsubs);
  retval=(stop!=7)?  itno : SBA_ERROR;
                                                               
freemem_and_return: /* NOTE: this point is also reached via a goto! */

   /* free whatever was allocated */
  free(jac); free(U);
  free(e); free(ea);  
  free(dp);
  free(rcidxs); free(rcsubs);
#ifndef SBA_DESTROY_COVS
  if(wght) free(wght);
#else
  /* nothing to do */
#endif /* SBA_DESTROY_COVS */

  free(hx); free(diagU); free(pdp);
  if(fdj_data.hxx){ // cleanup
    free(fdj_data.hxx);
    free(fdj_data.func_rcidxs);
  }

  sba_crsm_free(&idxij);

  /* free the memory allocated by the linear solver routine */
  if(linsolver) (*linsolver)(NULL, NULL, NULL, 0, 0);

  return retval;
}


/* Bundle adjustment on structure parameters only 
 * using the sparse Levenberg-Marquardt as described in HZ p. 568
 *
 * Returns the number of iterations (>=0) if successfull, SBA_ERROR if failed
 */

int sba_str_levmar_x(
    const int n,   /* number of points */
    const int ncon,/* number of points (starting from the 1st) whose parameters should not be modified.
                   * All B_ij (see below) with i<ncon are assumed to be zero
                   */
    const int m,   /* number of images */
    char *vmask,  /* visibility mask: vmask[i, j]=1 if point i visible in image j, 0 otherwise. nxm */
    double *p,    /* initial parameter vector p0: (b1, ..., bn).
                   * bi are the i-th point parameters, * size n*pnp */
    const int pnp,/* number of parameters for ONE point; e.g. 3 for Euclidean points */
    double *x,    /* measurements vector: (x_11^T, .. x_1m^T, ..., x_n1^T, .. x_nm^T)^T where
                   * x_ij is the projection of the i-th point on the j-th image.
                   * NOTE: some of the x_ij might be missing, if point i is not visible in image j;
                   * see vmask[i, j], max. size n*m*mnp
                   */
    double *covx, /* measurements covariance matrices: (Sigma_x_11, .. Sigma_x_1m, ..., Sigma_x_n1, .. Sigma_x_nm),
                   * where Sigma_x_ij is the mnp x mnp covariance of x_ij stored row-by-row. Set to NULL if no
                   * covariance estimates are available (identity matrices are implicitly used in this case).
                   * NOTE: a certain Sigma_x_ij is missing if the corresponding x_ij is also missing;
                   * see vmask[i, j], max. size n*m*mnp*mnp
                   */
    const int mnp,/* number of parameters for EACH measurement; usually 2 */
    void (*func)(double *p, struct sba_crsm *idxij, int *rcidxs, int *rcsubs, double *hx, void *adata),
                                              /* functional relation describing measurements. Given a parameter vector p,
                                               * computes a prediction of the measurements \hat{x}. p is (n*pnp)x1,
                                               * \hat{x} is (n*m*mnp)x1, maximum
                                               * rcidxs, rcsubs are max(m, n) x 1, allocated by the caller and can be used
                                               * as working memory
                                               */
    void (*fjac)(double *p, struct sba_crsm *idxij, int *rcidxs, int *rcsubs, double *jac, void *adata),
                                              /* function to evaluate the sparse jacobian dX/dp.
                                               * The Jacobian is returned in jac as
                                               * (dx_11/db_1, ..., dx_1m/db_1, ..., dx_n1/db_n, ..., dx_nm/db_n), or (using HZ's notation),
                                               * jac=(B_11, ..., B_1m, ..., B_n1, ..., B_nm)
                                               * Notice that depending on idxij, some of the B_ij might be missing.
                                               * Note also that B_ij are mnp x pnp matrices and they
                                               * should be stored in jac in row-major order.
                                               * rcidxs, rcsubs are max(m, n) x 1, allocated by the caller and can be used
                                               * as working memory
                                               *
                                               * If NULL, the jacobian is approximated by repetitive func calls and finite
                                               * differences. This is computationally inefficient and thus NOT recommended.
                                               */
    void *adata,       /* pointer to possibly additional data, passed uninterpreted to func, fjac */ 

    const int itmax,   /* I: maximum number of iterations. itmax==0 signals jacobian verification followed by immediate return */
    const int verbose, /* I: verbosity */
    const double opts[SBA_OPTSSZ],
	                     /* I: minim. options [\mu, \epsilon1, \epsilon2, \epsilon3, \epsilon4]. Respectively the scale factor for initial \mu,
                        * stopping thresholds for ||J^T e||_inf, ||dp||_2, ||e||_2 and (||e||_2-||e_new||_2)/||e||_2
                        */
    double info[SBA_INFOSZ]
	                     /* O: information regarding the minimization. Set to NULL if don't care
                        * info[0]=||e||_2 at initial p.
                        * info[1-4]=[ ||e||_2, ||J^T e||_inf,  ||dp||_2, mu/max[J^T J]_ii ], all computed at estimated p.
                        * info[5]= # iterations,
                        * info[6]=reason for terminating: 1 - stopped by small gradient J^T e
                        *                                 2 - stopped by small dp
                        *                                 3 - stopped by itmax
                        *                                 4 - stopped by small relative reduction in ||e||_2
                        *                                 5 - stopped by small ||e||_2
                        *                                 6 - too many attempts to increase damping. Restart with increased mu
                        *                                 7 - stopped by invalid (i.e. NaN or Inf) "func" values. This is a user error
                        * info[7]= # function evaluations
                        * info[8]= # jacobian evaluations
			                  * info[9]= # number of linear systems solved, i.e. number of attempts	for reducing error
                        */
)
{
register int i, j, ii, jj, k;
int nvis, nnz, retval;

/* The following are work arrays that are dynamically allocated by sba_str_levmar_x() */
double *jac;  /* work array for storing the jacobian, max. size n*m*mnp*pnp */
double *V;    /* work array for storing the V_i in the order V_1, ..., V_n, size n*pnp*pnp */

double *e;    /* work array for storing the e_ij in the order e_11, ..., e_1m, ..., e_n1, ..., e_nm,
                 max. size n*m*mnp */
double *eb;   /* work array for storing the eb_i in the order eb_1, .. eb_n size n*pnp */

double *dp;   /* work array for storing the parameter vector updates db_1, ..., db_n, size n*pnp */

double *wght= /* work array for storing the weights computed from the covariance inverses, max. size n*m*mnp*mnp */
            NULL;

/* Of the above arrays, jac, e, wght are sparse and
 * V, eb, dp are dense. Sparse arrays are indexed through
 * idxij (see below), that is with the same mechanism as the input 
 * measurements vector x
 */

/* submatrices sizes */
int Bsz, Vsz,
    esz, ebsz, covsz;

register double *ptr1, *ptr2, *ptr3, *ptr4, sum;
struct sba_crsm idxij; /* sparse matrix containing the location of x_ij in x. This is also the location
                        * of B_ij in jac, etc.
                        * This matrix can be thought as a map from a sparse set of pairs (i, j) to a continuous
                        * index k and it is used to efficiently lookup the memory locations where the non-zero
                        * blocks of a sparse matrix/vector are stored
                        */
int maxCPvis, /* max. of projections across cameras & projections across points */
    *rcidxs,  /* work array for the indexes corresponding to the nonzero elements of a single row or
                 column in a sparse matrix, size max(n, m) */
    *rcsubs;  /* work array for the subscripts of nonzero elements in a single row or column of a
                 sparse matrix, size max(n, m) */

/* The following variables are needed by the LM algorithm */
register int itno;  /* iteration counter */
int nsolved;
/* temporary work arrays that are dynamically allocated */
double *hx,         /* \hat{x}_i, max. size m*n*mnp */
       *diagV,      /* diagonals of V_i, size n*pnp */
       *pdp;        /* p + dp, size n*pnp */

register double mu,  /* damping constant */
                tmp; /* mainly used in matrix & vector multiplications */
double p_eL2, eb_inf, pdp_eL2; /* ||e(p)||_2, ||J^T e||_inf, ||e(p+dp)||_2 */
double p_L2, dp_L2=DBL_MAX, dF, dL;
double tau=FABS(opts[0]), eps1=FABS(opts[1]), eps2=FABS(opts[2]), eps2_sq=opts[2]*opts[2],
       eps3_sq=opts[3]*opts[3], eps4_sq=opts[4]*opts[4];
double init_p_eL2;
int nu=2, nu2, stop=0, nfev, njev=0, nlss=0;
int nobs, nvars;
PLS linsolver=NULL;

struct fdj_data_x_ fdj_data;
void *jac_adata;

/* Initialization */
  mu=eb_inf=tmp=0.0; /* -Wall */

  /* block sizes */
  Bsz=mnp * pnp; Vsz=pnp * pnp;
  esz=mnp; ebsz=pnp;
  covsz=mnp * mnp;

  /* count total number of visible image points */
  for(i=nvis=0, jj=n*m; i<jj; ++i)
    nvis+=(vmask[i]!=0);

  nobs=nvis*mnp;
  nvars=n*pnp;
  if(nobs<nvars){
    fprintf(stderr, "SBA: sba_str_levmar_x() cannot solve a problem with fewer measurements [%d] than unknowns [%d]\n", nobs, nvars);
    return SBA_ERROR;
  }

  /* allocate & fill up the idxij structure */
  sba_crsm_alloc(&idxij, n, m, nvis);
  for(i=k=0; i<n; ++i){
    idxij.rowptr[i]=k;
    ii=i*m;
    for(j=0; j<m; ++j)
      if(vmask[ii+j]){
        idxij.val[k]=k;
        idxij.colidx[k++]=j;
      }
  }
  idxij.rowptr[n]=nvis;

  /* find the maximum number of visible image points in any single camera or coming from a single 3D point */
  /* cameras */
  for(i=maxCPvis=0; i<n; ++i)
    if((k=idxij.rowptr[i+1]-idxij.rowptr[i])>maxCPvis) maxCPvis=k;

  /* points, note that maxCPvis is not reinitialized! */
  for(j=0; j<m; ++j){
    for(i=ii=0; i<n; ++i)
      if(vmask[i*m+j]) ++ii;
    if(ii>maxCPvis) maxCPvis=ii;
  }

  /* allocate work arrays */
  jac=(double *)emalloc(nvis*Bsz*sizeof(double));
  V=(double *)emalloc(n*Vsz*sizeof(double));
  e=(double *)emalloc(nobs*sizeof(double));
  eb=(double *)emalloc(nvars*sizeof(double));
  dp=(double *)emalloc(nvars*sizeof(double));
  rcidxs=(int *)emalloc(maxCPvis*sizeof(int));
  rcsubs=(int *)emalloc(maxCPvis*sizeof(int));
#ifndef SBA_DESTROY_COVS
  if(covx!=NULL) wght=(double *)emalloc(nvis*covsz*sizeof(double));
#else
  if(covx!=NULL) wght=covx;
#endif /* SBA_DESTROY_COVS */


  hx=(double *)emalloc(nobs*sizeof(double));
  diagV=(double *)emalloc(nvars*sizeof(double));
  pdp=(double *)emalloc(nvars*sizeof(double));

  /* if no jacobian function is supplied, prepare to compute jacobian with finite difference */
  if(!fjac){
    fdj_data.func=func;
    fdj_data.cnp=0;
    fdj_data.pnp=pnp;
    fdj_data.mnp=mnp;
    fdj_data.hx=hx;
    fdj_data.hxx=(double *)emalloc(nobs*sizeof(double));
    fdj_data.func_rcidxs=(int *)emalloc(2*maxCPvis*sizeof(int));
    fdj_data.func_rcsubs=fdj_data.func_rcidxs+maxCPvis;
    fdj_data.adata=adata;

    fjac=sba_fdjac_x;
    jac_adata=(void *)&fdj_data;
  }
  else{
    fdj_data.hxx=NULL;
    jac_adata=adata;
  }

  if(itmax==0){ /* verify jacobian */
    sba_str_chkjac_x(func, fjac, p, &idxij, rcidxs, rcsubs, ncon, pnp, mnp, adata, jac_adata);
    retval=0;
    goto freemem_and_return;
  }

  /* covariances Sigma_x_ij are accommodated by computing the Cholesky decompositions of their
   * inverses and using the resulting matrices w_x_ij to weigh B_ij and e_ij as
   * w_x_ij*B_ij and w_x_ij*e_ij. In this way, auxiliary variables as V_i=\sum_j B_ij^T B_ij
   * actually become \sum_j (w_x_ij B_ij)^T w_x_ij B_ij= \sum_j B_ij^T w_x_ij^T w_x_ij B_ij =
   * B_ij^T Sigma_x_ij^-1 B_ij
   *
   * eb_i are weighted in a similar manner
   */
  if(covx!=NULL){
    for(i=0; i<n; ++i){
      nnz=sba_crsm_row_elmidxs(&idxij, i, rcidxs, rcsubs); /* find nonzero x_ij, j=0...m-1 */
      for(j=0; j<nnz; ++j){
        /* set ptr1, ptr2 to point to cov_x_ij, w_x_ij resp. */
        ptr1=covx + (k=idxij.val[rcidxs[j]]*covsz);
        ptr2=wght + k;
        if(!sba_mat_cholinv(ptr1, ptr2, mnp)){ /* compute w_x_ij s.t. w_x_ij^T w_x_ij = cov_x_ij^-1 */
			    fprintf(stderr, "SBA: invalid covariance matrix for x_ij (i=%d, j=%d) in sba_motstr_levmar_x()\n", i, rcsubs[j]);
          retval=SBA_ERROR;
          goto freemem_and_return;
        }
      }
    }
    sba_mat_cholinv(NULL, NULL, 0); /* cleanup */
  }

  /* compute the error vectors e_ij in hx */
  (*func)(p, &idxij, rcidxs, rcsubs, hx, adata); nfev=1;
  /* ### compute e=x - f(p) [e=w*(x - f(p)] and its L2 norm */
  if(covx==NULL)
    p_eL2=nrmL2xmy(e, x, hx, nobs); /* e=x-hx, p_eL2=||e|| */
  else
    p_eL2=nrmCxmy(e, x, hx, wght, mnp, nvis); /* e=wght*(x-hx), p_eL2=||e||=||x-hx||_Sigma^-1 */
  if(verbose) printf("initial str-SBA error %g [%g]\n", p_eL2, p_eL2/nvis);
  init_p_eL2=p_eL2;
  if(!SBA_FINITE(p_eL2)) stop=7;

  for(itno=0; itno<itmax && !stop; ++itno){
    /* Note that p, e and ||e||_2 have been updated at the previous iteration */

    /* compute derivative submatrices B_ij */
    (*fjac)(p, &idxij, rcidxs, rcsubs, jac, jac_adata); ++njev;

    if(covx!=NULL){
      /* compute w_x_ij B_ij.
       * Since w_x_ij is upper triangular, the products can be safely saved
       * directly in B_ij, without the need for intermediate storage
       */
      for(i=0; i<nvis; ++i){
        /* set ptr1, ptr2 to point to w_x_ij, B_ij, resp. */
        ptr1=wght + i*covsz;
        ptr2=jac  + i*Bsz;

        /* w_x_ij is mnp x mnp, B_ij is mnp x pnp */
        for(ii=0; ii<mnp; ++ii)
          for(jj=0; jj<pnp; ++jj){
            for(k=ii, sum=0.0; k<mnp; ++k) // k>=ii since w_x_ij is upper triangular
              sum+=ptr1[ii*mnp+k]*ptr2[k*pnp+jj];
            ptr2[ii*pnp+jj]=sum;
          }
      }
    }

    /* compute V_i = \sum_j B_ij^T B_ij */ // \Sigma here!
    /* V_i is symmetric, therefore its computation can be sped up by
     * computing only the upper part and then reusing it for the lower one.
     * Recall that B_ij is mnp x pnp
     */
    /* Also compute eb_i = \sum_j B_ij^T e_ij */ // \Sigma here!
    /* Recall that e_ij is mnp x 1
     */
	  _dblzero(V, n*Vsz); /* clear all V_i */
	  _dblzero(eb, n*ebsz); /* clear all eb_i */
    for(i=ncon; i<n; ++i){
      ptr1=V + i*Vsz; // set ptr1 to point to V_i
      ptr2=eb + i*ebsz; // set ptr2 to point to eb_i

      nnz=sba_crsm_row_elmidxs(&idxij, i, rcidxs, rcsubs); /* find nonzero B_ij, j=0...m-1 */
      for(j=0; j<nnz; ++j){
        /* set ptr3 to point to B_ij, actual column number in rcsubs[j] */
        ptr3=jac + idxij.val[rcidxs[j]]*Bsz;
      
        /* compute the UPPER TRIANGULAR PART of B_ij^T B_ij and add it to V_i */
        for(ii=0; ii<pnp; ++ii){
          for(jj=ii; jj<pnp; ++jj){
            for(k=0, sum=0.0; k<mnp; ++k)
              sum+=ptr3[k*pnp+ii]*ptr3[k*pnp+jj];
            ptr1[ii*pnp+jj]+=sum;
          }

          /* copy the LOWER TRIANGULAR PART of V_i from the upper one */
          for(jj=0; jj<ii; ++jj)
            ptr1[ii*pnp+jj]=ptr1[jj*pnp+ii];
        }

        ptr4=e + idxij.val[rcidxs[j]]*esz; /* set ptr4 to point to e_ij */
        /* compute B_ij^T e_ij and add it to eb_i */
        for(ii=0; ii<pnp; ++ii){
          for(jj=0, sum=0.0; jj<mnp; ++jj)
            sum+=ptr3[jj*pnp+ii]*ptr4[jj];
          ptr2[ii]+=sum;
        }
      }
    }

    /* Compute ||J^T e||_inf and ||p||^2 */
    for(i=0, p_L2=eb_inf=0.0; i<nvars; ++i){
      if(eb_inf < (tmp=FABS(eb[i]))) eb_inf=tmp;
      p_L2+=p[i]*p[i];
    }
    //p_L2=sqrt(p_L2);

    /* save diagonal entries so that augmentation can be later canceled.
     * Diagonal entries are in V_i
     */
    for(i=ncon; i<n; ++i){
      ptr1=V + i*Vsz; // set ptr1 to point to V_i
      ptr2=diagV + i*pnp; // set ptr2 to point to diagV_i
      for(j=0; j<pnp; ++j)
        ptr2[j]=ptr1[j*pnp+j];
    }

/*
if(!(itno%100)){
  printf("Current estimate: ");
  for(i=0; i<nvars; ++i)
    printf("%.9g ", p[i]);
  printf("-- errors %.9g %0.9g\n", eb_inf, p_eL2);
}
*/

    /* check for convergence */
    if((eb_inf <= eps1)){
      dp_L2=0.0; /* no increment for p in this case */
      stop=1;
      break;
    }

   /* compute initial damping factor */
    if(itno==0){
      for(i=ncon*pnp, tmp=DBL_MIN; i<nvars; ++i)
        if(diagV[i]>tmp) tmp=diagV[i]; /* find max diagonal element */
      mu=tau*tmp;
    }

    /* determine increment using adaptive damping */
    while(1){
      /* augment V */
      for(i=ncon; i<n; ++i){
        ptr1=V + i*Vsz; // set ptr1 to point to V_i
        for(j=0; j<pnp; ++j)
          ptr1[j*pnp+j]+=mu;
      }

      /* solve the linear systems V*_i db_i = eb_i to compute the db_i */
      _dblzero(dp, ncon*pnp); /* no change for the first ncon point params */
      for(i=nsolved=ncon; i<n; ++i){
        ptr1=V + i*Vsz; // set ptr1 to point to V_i
        ptr2=eb + i*ebsz; // set ptr2 to point to eb_i
        ptr3=dp + i*pnp; // set ptr3 to point to db_i

        //nsolved+=sba_Axb_LU(ptr1, ptr2, ptr3, pnp, 0); linsolver=sba_Axb_LU;
        nsolved+=sba_Axb_Chol(ptr1, ptr2, ptr3, pnp, 0); linsolver=sba_Axb_Chol;
        //nsolved+=sba_Axb_BK(ptr1, ptr2, ptr3, pnp, 0); linsolver=sba_Axb_BK;
        //nsolved+=sba_Axb_QRnoQ(ptr1, ptr2, ptr3, pnp, 0); linsolver=sba_Axb_QRnoQ;
        //nsolved+=sba_Axb_QR(ptr1, ptr2, ptr3, pnp, 0); linsolver=sba_Axb_QR;
        //nsolved+=sba_Axb_SVD(ptr1, ptr2, ptr3, pnp, 0); linsolver=sba_Axb_SVD;
	      //nsolved+=(sba_Axb_CG(ptr1, ptr2, ptr3, pnp, (3*pnp)/2, 1E-10, SBA_CG_JACOBI, 0) > 0); linsolver=(PLS)sba_Axb_CG;

        ++nlss;
      }

      if(nsolved==n){

        /* parameter vector updates are now in dp */

        /* compute p's new estimate and ||dp||^2 */
        for(i=0, dp_L2=0.0; i<nvars; ++i){
          pdp[i]=p[i] + (tmp=dp[i]);
          dp_L2+=tmp*tmp;
        }
        //dp_L2=sqrt(dp_L2);

        if(dp_L2<=eps2_sq*p_L2){ /* relative change in p is small, stop */
        //if(dp_L2<=eps2*(p_L2 + eps2)){ /* relative change in p is small, stop */
          stop=2;
          break;
        }

        if(dp_L2>=(p_L2+eps2)/SBA_EPSILON_SQ){ /* almost singular */
        //if(dp_L2>=(p_L2+eps2)/SBA_EPSILON){ /* almost singular */
          fprintf(stderr, "SBA: the matrix of the augmented normal equations is almost singular in sba_motstr_levmar_x(),\n"
                          "     minimization should be restarted from the current solution with an increased damping term\n");
          retval=SBA_ERROR;
          goto freemem_and_return;
        }

        (*func)(pdp, &idxij, rcidxs, rcsubs, hx, adata); ++nfev; /* evaluate function at p + dp */
        if(verbose>1)
          printf("mean reprojection error %g\n", sba_mean_repr_error(n, mnp, x, hx, &idxij, rcidxs, rcsubs));
        /* ### compute ||e(pdp)||_2 */
        if(covx==NULL)
          pdp_eL2=nrmL2xmy(hx, x, hx, nobs); /* hx=x-hx, pdp_eL2=||hx|| */
        else
          pdp_eL2=nrmCxmy(hx, x, hx, wght, mnp, nvis); /* hx=wght*(x-hx), pdp_eL2=||hx|| */
        if(!SBA_FINITE(pdp_eL2)){
          if(verbose) /* identify the offending point projection */
            sba_print_inf(hx, m, mnp, &idxij, rcidxs, rcsubs);

          stop=7;
          break;
        }

        for(i=0, dL=0.0; i<nvars; ++i)
          dL+=dp[i]*(mu*dp[i]+eb[i]);

        dF=p_eL2-pdp_eL2;

        if(verbose>1)
          printf("\ndamping term %8g, gain ratio %8g, errors %8g / %8g = %g\n", mu, dL!=0.0? dF/dL : dF/DBL_EPSILON, p_eL2/nvis, pdp_eL2/nvis, p_eL2/pdp_eL2);

        if(dL>0.0 && dF>0.0){ /* reduction in error, increment is accepted */
          tmp=(2.0*dF/dL-1.0);
          tmp=1.0-tmp*tmp*tmp;
          mu=mu*( (tmp>=SBA_ONE_THIRD)? tmp : SBA_ONE_THIRD );
          nu=2;

          /* the test below is equivalent to the relative reduction of the RMS reprojection error: sqrt(p_eL2)-sqrt(pdp_eL2)<eps4*sqrt(p_eL2) */
          if(pdp_eL2-2.0*sqrt(p_eL2*pdp_eL2)<(eps4_sq-1.0)*p_eL2) stop=4;
          
          for(i=0; i<nvars; ++i) /* update p's estimate */
            p[i]=pdp[i];

          for(i=0; i<nobs; ++i) /* update e and ||e||_2 */
            e[i]=hx[i];
          p_eL2=pdp_eL2;
          break;
        }
      } /* nsolved==n */

      /* if this point is reached, either at least one linear system could not be solved or
       * the error did not reduce; in any case, the increment must be rejected
       */

      mu*=nu;
      nu2=nu<<1; // 2*nu;
      if(nu2<=nu){ /* nu has wrapped around (overflown) */
        fprintf(stderr, "SBA: too many failed attempts to increase the damping factor in sba_str_levmar_x()! Singular Hessian matrix?\n");
        //exit(1);
        stop=6;
        break;
      }
      nu=nu2;

#if 0
      /* restore V diagonal entries */
      for(i=ncon; i<n; ++i){
        ptr1=V + i*Vsz; // set ptr1 to point to V_i
        ptr2=diagV + i*pnp; // set ptr2 to point to diagV_i
        for(j=0; j<pnp; ++j)
          ptr1[j*pnp+j]=ptr2[j];
      }
#endif
    } /* inner while loop */

    if(p_eL2<=eps3_sq) stop=5; // error is small, force termination of outer loop
  }

  if(itno>=itmax) stop=3;

  /* restore V diagonal entries */
  for(i=ncon; i<n; ++i){
    ptr1=V + i*Vsz; // set ptr1 to point to V_i
    ptr2=diagV + i*pnp; // set ptr2 to point to diagV_i
    for(j=0; j<pnp; ++j)
      ptr1[j*pnp+j]=ptr2[j];
  }

  if(info){
    info[0]=init_p_eL2;
    info[1]=p_eL2;
    info[2]=eb_inf;
    info[3]=dp_L2;
    for(i=ncon; i<n; ++i){
      ptr1=V + i*Vsz; // set ptr1 to point to V_i
      for(j=0; j<pnp; ++j)
        if(tmp<ptr1[j*pnp+j]) tmp=ptr1[j*pnp+j];
      }
    info[4]=mu/tmp;
    info[5]=itno;
    info[6]=stop;
    info[7]=nfev;
    info[8]=njev;
    info[9]=nlss;
  }
  //sba_print_sol(n, m, p, 0, pnp, x, mnp, &idxij, rcidxs, rcsubs);
  retval=(stop!=7)?  itno : SBA_ERROR;
                                                               
freemem_and_return: /* NOTE: this point is also reached via a goto! */

   /* free whatever was allocated */
  free(jac); free(V);
  free(e); free(eb);  
  free(dp);               
  free(rcidxs); free(rcsubs);
#ifndef SBA_DESTROY_COVS
  if(wght) free(wght);
#else
  /* nothing to do */
#endif /* SBA_DESTROY_COVS */

  free(hx); free(diagV); free(pdp);
  if(fdj_data.hxx){ // cleanup
    free(fdj_data.hxx);
    free(fdj_data.func_rcidxs);
  }

  sba_crsm_free(&idxij);

  /* free the memory allocated by the linear solver routine */
  if(linsolver) (*linsolver)(NULL, NULL, NULL, 0, 0);

  return retval;
}
