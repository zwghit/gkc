/*
 * =====================================================================================
 *
 *       Filename:  FFTSolver_fftw3.cpp
 *    Description:  
 *        Version:  0.1
 *         Author:  Paul P. Hilscher (2009, 2011), 
 *
 * =====================================================================================
 */


#include "FFTSolver_fftw3.h"
#include "Plasma.h"

#include <fftw3-mpi.h>


// we have to place plans here to avoid namespace errors 
fftw_plan plan_YForward_Field, plan_YBackward_Field, plan_YForward_PSF, plan_YBackward_PSF;
fftw_plan plan_XForward, plan_XBackward;

fftw_plan plan_XForward_Fields, plan_XBackward_Fields;
fftw_plan plan_FieldTranspose_1, plan_FieldTranspose_2;
fftw_plan plan_AA_YForward, plan_AA_YBackward;



// from http://agentzlerich.blogspot.jp/2010/01/using-fftw-for-in-place-matrix.html
// any license issues ? 
fftw_plan plan_transpose(char storage_type, int rows, int cols, double *in, double *out);


// transpose from C-ordering to Fortran ordering (required by fftw3-mpi) 
void transpose(int Nx, int Ny, int Nz, int Nf, CComplex In[Nf][Nz][Ny][Nx], CComplex OutT[Nx][Ny][Nz][Nf])
{
     //#pragma ivdep
     for(int x=0; x < Nx; x++ ) { for(int y=0; y < Ny; y++ ) {  
     for(int z=0; z < Nz; z++ ) { for(int f=0; f < Nf; f++ ) {  

        OutT[x][y][z][f] = In[f][z][y][x];
   
       } } } }
  
}

// transpose from Fortran ordering to C-ordering (required by fftw3-mpi) 
void transpose_rev(int Nx, int Ny, int Nz, int Nf, CComplex In[Nx][Ny][Nz][Nf], CComplex OutT[Nf][Nz][Ny][Nx])
{
     //#pragma ivdep
     for(int x=0; x < Nx; x++ ) { for(int y=0; y < Ny; y++ ) {  
     for(int z=0; z < Nz; z++ ) { for(int f=0; f < Nf; f++ ) {  

        OutT[f][z][y][x] = In[x][y][z][f];
   
       } } } }
  
}



FFTSolver_fftw3::FFTSolver_fftw3(Setup *setup, Parallel *parallel, Geometry *geo) : FFTSolver(setup, parallel, geo, Nx*(2*Nky-2)*Nz, Nx*(2*Nky-2), Nx,  (2*Nky-2)) {
   

   if(parallel->Coord[DIR_V] == 0) {          // Not Fourier solver required in velocity space

      // Setup plans
      int perf_flag = FFTW_ESTIMATE;
      plan   = setup->get("FFTW3.Plan", "");
      if      (plan == "Estimate"   ) perf_flag = FFTW_ESTIMATE;
      else if (plan == "Measure"    ) perf_flag = FFTW_MEASURE;
      else if (plan == "Exhaustive" ) perf_flag = FFTW_EXHAUSTIVE;

      // Setup wisedom
      wisdom = setup->get("FFTW3.Wisdom", "");
      if     (wisdom == "System") fftw_import_system_wisdom();
      else if(wisdom != ""      ) fftw_import_wisdom_from_filename(wisdom.c_str());
   
      const int nfields = plasma->nfields;


#ifdef PARALLEL_OPENMP
      fftw_init_threads();
      fftw_plan_with_nthreads(parallel->numThreads);
#endif

#ifdef GKC_PARALLEL_MPI
      fftw_mpi_init();
#endif
      // needs for poissons equation
      if(flags & FFT_X) {
         
         // set and check bounds 
         long X_NxLD, X_NxLlD, X_NkxL, X_NkxLlD, X_numElements, X_Nx = Nx; 
         X_numElements = fftw_mpi_local_size_1d(Nx, parallel->Comm[DIR_X], FFTW_FORWARD, 0, &X_NxLD, &X_NxLlD, &X_NkxL, &X_NkxLlD);
        
         FFTSolver::X_NkxL = X_NkxL;
  
         // BUG allocate arrays (factor 2 for saferty (hides BUGS) NyLD or NkyLD ?!
         data_X_kIn  = (Complex *) fftw_alloc_complex(2*X_numElements*NkyLD*NzLD*nfields);
         data_X_kOut = (Complex *) fftw_alloc_complex(2*X_numElements*NkyLD*NzLD*nfields);
         
         data_X_rOut = (Complex *) fftw_alloc_complex(2*X_numElements*NkyLD*NzLD*nfields);
         data_X_rIn  = (Complex *) fftw_alloc_complex(2*X_numElements*NkyLD*NzLD*nfields);
         
         data_X_Transp_1 = (Complex *) fftw_alloc_complex(2*X_numElements*NkyLD*NzLD*nfields);
         data_X_Transp_2 = (Complex *) fftw_alloc_complex(2*X_numElements*NkyLD*NzLD*nfields);
       
         check((NxLD != X_NxLD) ? -1 : 0, DMESG("Bounds to not align")); 
         
         // set and check bounds 
         K1xLlD = X_NkxLlD;       K1xLuD = X_NkxLlD + X_NkxL - 1;
         Rk1xL.setRange(K1xLlD, K1xLuD);
         
         // allocate arrays NOTE : 1D MPI fftw-2 are ALWAYS IN-PLACE TRANSFORMS (but not fftw-3 !)
         //GeneralArrayStorage<4> storage_rX; storage_rX.ordering() = fourthDim, thirdDim,  secondDim, firstDim; storage_rX.base() = NxLlD , NkyLlD, NzLlD, 1;
         //Array4C rXIn_t   ((Complex *) data_X_rIn , shape(X_NxLD , NkyLD, NzLD, nfields), neverDeleteData, storage_rX); rXIn  .reference(rXIn_t ) ; rXIn  = 0.; 
         //Array4C rXOut_t  ((Complex *) data_X_rOut, shape(X_NxLD , NkyLD, NzLD, nfields), neverDeleteData, storage_rX); rXOut .reference(rXOut_t) ; rXOut = 0.;
         
         // OK
         GeneralArrayStorage<4> storage_rX; storage_rX.ordering() = firstDim, secondDim,  thirdDim, fourthDim; storage_rX.base() = NxLlD , NkyLlD, NzLlD, 1;
         Array4C rXIn_t   ((Complex *) data_X_rIn , shape(X_NxLD , NkyLD, NzLD, nfields), neverDeleteData, storage_rX); rXIn  .reference(rXIn_t ) ; rXIn  = 0.; 
         Array4C rXOut_t  ((Complex *) data_X_rOut, shape(X_NxLD , NkyLD, NzLD, nfields), neverDeleteData, storage_rX); rXOut .reference(rXOut_t) ; rXOut = 0.;
           
         // Test
         //GeneralArrayStorage<4> storage_cX; storage_cX.ordering() = fourthDim, thirdDim,  secondDim, firstDim; storage_cX.base() = X_NkxLlD, NkyLlD, NzLlD, 1;
         //Array4C kXOut_t  ((Complex *) data_X_kOut, shape(X_NkxL, NkyLD, NzLD, nfields), neverDeleteData, storage_cX); kXOut.reference(kXOut_t ) ; kXOut = 0.; 
         //Array4C kXIn_t   ((Complex *) data_X_kIn , shape(X_NkxL, NkyLD, NzLD, nfields), neverDeleteData, storage_cX); kXIn .reference(kXIn_t  ) ; kXIn  = 0.;
          
         
         // OK
         GeneralArrayStorage<4> storage_cX; storage_cX.ordering() = firstDim, secondDim,  thirdDim, fourthDim; storage_cX.base() = X_NkxLlD, NkyLlD, NzLlD, 1;
         Array4C kXOut_t  ((Complex *) data_X_kOut, shape(X_NkxL, NkyLD, NzLD, nfields), neverDeleteData, storage_cX); kXOut.reference(kXOut_t ) ; kXOut = 0.; 
         Array4C kXIn_t   ((Complex *) data_X_kIn , shape(X_NkxL, NkyLD, NzLD, nfields), neverDeleteData, storage_cX); kXIn .reference(kXIn_t  ) ; kXIn  = 0.;

         plan_XForward   = fftw_mpi_plan_many_dft(1, &X_Nx, (long) NkyLD * NzLD,  NxLD * nfields, X_NkxL * nfields, (fftw_complex *) rXIn.data(), (fftw_complex *) kXOut.data(), parallel->Comm[DIR_X], FFT_FORWARD, perf_flag);
         plan_XBackward  = fftw_mpi_plan_many_dft(1, &X_Nx, (long) NkyLD * NzLD,  NxLD * nfields, X_NkxL * nfields, (fftw_complex *) kXIn.data(), (fftw_complex *) rXOut.data(), parallel->Comm[DIR_X], FFT_BACKWARD, perf_flag);
        
          // 
          // Note : We should use transformed out to improve parallelization
          // fftw_plan fftw_mpi_plan_many_dft(int rnk, const ptrdiff_t *n,  ptrdiff_t howmany, ptrdiff_t block, ptrdiff_t tblock, fftw_complex *in, fftw_complex *out,
          // MPI_Comm comm, int sign, unsigned flags);
         plan_XForward_Fields   = fftw_mpi_plan_many_dft(1, &X_Nx, (long) NkyLD * NzLD * nfields,  NxLD, X_NkxL, (fftw_complex *) rXIn.data(), (fftw_complex *) kXOut.data(), parallel->Comm[DIR_X], FFT_FORWARD , perf_flag);
         plan_XBackward_Fields  = fftw_mpi_plan_many_dft(1, &X_Nx, (long) NkyLD * NzLD * nfields,  NxLD, X_NkxL, (fftw_complex *) kXIn.data(), (fftw_complex *) rXOut.data(), parallel->Comm[DIR_X], FFT_BACKWARD, perf_flag);

         // Fields have to be continuous in howmanyfields, and thus we have to transpose the array (use in-place) 
         // add factor of 2 because we deal with complex numbers not real numbers
         //plan_FieldTranspose = plan_transpose('R', 2* NxLD, 2 * NkyLD * NzLD * nfields, (double *) kXOut.data(), (double *) kXOut.data());
 
//           plan_FieldTranspose_1 = plan_transpose('R', 2 * NkyLD * NzLD * nfields, 2*   NxLD, (double *) data_X_Transp_1, (double *) data_X_Transp_2);
//           plan_FieldTranspose_2 = plan_transpose('C', 2 * NkyLD * NzLD * nfields, 2 * X_NkxL, (double *) data_X_Transp_1, (double *) data_X_Transp_2);
           
//           check(((plan_FieldTranspose_1 == NULL) || (plan_FieldTranspose_2 == NULL)) ? -1 : 0, DMESG("Transpose planner null"));

      }
           
            
      // Needed to calculate non-linearity in real space
      if(flags & FFT_Y) {
                // array is too big (but for safety, shoukd be 3 but we set 4, maybe there are some roudning errors for AA?!)
                //data_Y_kOut = (Complex *) fftw_alloc_complex(4*(NyLD/2+1)*NxLD*NzLD*nfields_Y);
                const int nfields_Y = 1;
                data_Y_kIn  = (Complex *) fftw_alloc_complex(4*Nky *(NxLD+8));
                data_Y_kOut = (Complex *) fftw_alloc_complex(2*Nky *(NxLD+8));
                data_Y_rOut = (double *)  fftw_alloc_real   (4*NyLD*(NxLD+8));
                data_Y_rIn  = (double *)  fftw_alloc_real   (4*NyLD*(NxLD+8));
                
                GeneralArrayStorage<2> storage_rY; storage_rY.ordering() = secondDim, firstDim; storage_rY.base() = NxLlD, NyLlD;
                Array2R rYIn_t   ( data_Y_rIn,  shape(NxLD+8, NyLD), neverDeleteData, storage_rY); rYIn .reference(rYIn_t ) ; rYIn  = 0.; 
                Array2R rYOut_t  ( data_Y_rOut, shape(NxLD+8, NyLD), neverDeleteData, storage_rY); rYOut.reference(rYOut_t) ; rYOut = 0.;
                
                GeneralArrayStorage<2> storage_cY; storage_cY.ordering() = secondDim, firstDim; storage_cY.base() = NxLlD, NkyLlD;
                Array2C kYOut_t  (data_Y_kOut, shape(NxLD+8, NkyLD), neverDeleteData, storage_cY); kYOut.reference(kYOut_t ) ; kYOut = 0.; 
                Array2C kYIn_t   (data_Y_kIn , shape(NxLD+8, NkyLD), neverDeleteData, storage_cY); kYIn .reference(kYIn_t  ) ; kYIn  = 0.;
                   
                //                                                  howmany                                  stride distance
                //  leave space for boundary conditions
                plan_YForward_Field  = fftw_plan_many_dft_r2c(1, &NyLD, NxLD+8,                 rYIn.data(), NULL, 1, NyLD+4,  (fftw_complex*) kYOut.data(), NULL, 1, NkyLD, perf_flag);
                plan_YBackward_Field = fftw_plan_many_dft_c2r(1, &NyLD, NxLD+8, (fftw_complex*) kYIn.data(), NULL, 1, NkyLD,                  rYOut.data(), NULL, 1, NyLD , perf_flag);
                
                plan_YForward_PSF    = fftw_plan_many_dft_r2c(1, &NyLD, NxLD  ,                 rYIn.data(), NULL, 1, NyLD ,  (fftw_complex*) kYOut.data(), NULL, 1, NkyLD, perf_flag);
                plan_YBackward_PSF   = fftw_plan_many_dft_c2r(1, &NyLD, NxLD  , (fftw_complex*) kYIn.data(), NULL, 1, NkyLD,                  rYOut.data(), NULL, 1, NyLD , perf_flag);
                


                ////////////////////////   Define Anti-Aliased Arrays /////////////////////////////////////
                AA_NkyLD  = 3*Nky/2 ; AA_NyLD   = 2*AA_NkyLD-2;
                AA_NyLlD  = NyLlD   ; AA_NyLuD  = AA_NyLlD  + AA_NyLD  - 1;
                AA_NkyLlD = NkyLlD  ; AA_NkyLuD = AA_NkyLlD + AA_NkyLD - 1;

                Array2R AA_rYIn_t   ( data_Y_rIn , shape(NxLD, AA_NyLD ), neverDeleteData, storage_rY); AA_rYIn .reference(AA_rYIn_t ) ; AA_rYIn  = 0.; 
                Array2R AA_rYOut_t  ( data_Y_rOut, shape(NxLD, AA_NyLD ), neverDeleteData, storage_rY); AA_rYOut.reference(AA_rYOut_t) ; AA_rYOut = 0.;
                
                Array2C AA_kYOut_t  (data_Y_kOut , shape(NxLD, AA_NkyLD), neverDeleteData, storage_cY); AA_kYOut.reference(AA_kYOut_t ) ; AA_kYOut = 0.; 
                Array2C AA_kYIn_t   (data_Y_kIn  , shape(NxLD, AA_NkyLD), neverDeleteData, storage_cY); AA_kYIn .reference(AA_kYIn_t  ) ; AA_kYIn  = 0.;
                   
                plan_AA_YForward  = fftw_plan_many_dft_r2c(1, &AA_NyLD, NxLD*NzLD,                 AA_rYIn.data(), NULL, 1, AA_NyLD ,  (fftw_complex*) AA_kYOut.data(), NULL, 1, AA_NkyLD, perf_flag);
                plan_AA_YBackward = fftw_plan_many_dft_c2r(1, &AA_NyLD, NxLD*NzLD, (fftw_complex*) AA_kYIn.data(), NULL, 1, AA_NkyLD,                  AA_rYOut.data(), NULL, 1, AA_NyLD , perf_flag);
              
   }

     if(flags & FFT_XY ) check(-1, DMESG("XY does not work for fftw3"));
     if(flags & FFT_XYZ) check(-1, DMESG("3D Fourier transformatio not tested yet"));
   

   setNormalizationConstants();
     
   // export it again (only by root job ?!)
   if(wisdom != "") fftw_export_wisdom_to_filename(wisdom.c_str());
   }

}



void FFTSolver_fftw3::solve(const int FFTtype, const int direction, void *in, void *out) 
{
         
         if(FFTtype & FFT_X_FIELDS) {
             
               if(in == nullptr)  check(-1, DMESG("Need Pointer to array"));

               if     (direction == FFT_FORWARD )  {
                transpose(NxLD, NkyLD, NzLD, plasma->nfields      , (A4zz) ((CComplex *) in)             , (A4zz) ((CComplex *) data_X_Transp_1));                
                fftw_mpi_execute_dft(plan_XForward_Fields , (fftw_complex *) data_X_Transp_1 , (fftw_complex *) data_X_Transp_2); 
                transpose_rev(X_NkxL, NkyLD, NzLD, plasma->nfields, (A4zz) ((CComplex *) data_X_Transp_2), (A4zz) ((CComplex *) kXOut.data()));               

               }
               else if(direction == FFT_BACKWARD) {
                  
                transpose(X_NkxL, NkyLD, NzLD, plasma->nfields, (A4zz) ((CComplex *) kXIn.data()), (A4zz) ((CComplex *) data_X_Transp_1));                
                fftw_mpi_execute_dft(plan_XBackward_Fields, (fftw_complex *) data_X_Transp_1, (fftw_complex *) data_X_Transp_2 ); 
                transpose_rev(NxLD, NkyLD, NzLD, plasma->nfields, (A4zz) ((CComplex *) data_X_Transp_2), (A4zz) ((CComplex *) in));                
               }
               else   check(-1, DMESG("No such FFT direction"));

         }  
         else if(FFTtype & FFT_X   & flags) {

                 check(-1, DMESG("What is this ?")); 
             // fftw3 seems to crash when Nx=1, why ? Check small
             if(in != nullptr)    {
               // Need to transpose
               std::cout << "H" << std::endl;
               if     (direction == FFT_FORWARD )  {
//                fftw_execute_r2r(plan_FieldTranspose_1, (double*) p, (double *) data_X_Transp_1);
                transpose(NxLD,   NkyLD, NzLD, plasma->nfields, (A4zz) ((CComplex *) in), (A4zz) ((CComplex *) data_X_Transp_1));                
                fftw_mpi_execute_dft(plan_XForward_Fields , (fftw_complex *) data_X_Transp_1 , (fftw_complex *) data_X_Transp_2); 
                transpose(X_NkxL, NkyLD, NzLD, plasma->nfields, (A4zz) ((CComplex *) data_X_Transp_2), (A4zz) ((CComplex *) kXOut.data()));                
//                fftw_execute_r2r(plan_FieldTranspose_2, (double*) data_X_Transp_2, (double *) kXOut.data());
               }
               else if(direction == FFT_BACKWARD) {
                // use temp array for transpose
//                 fftw_execute_r2r(plan_FieldTranspose_2, (double*) kXIn.data(), (double *) data_X_Transp_1);
                transpose(X_NkxL, NkyLD, NzLD, plasma->nfields, (A4zz) ((CComplex *) kXIn.data()), (A4zz) ((CComplex *) data_X_Transp_1));                
                fftw_mpi_execute_dft(plan_XBackward_Fields, (fftw_complex *) data_X_Transp_1, (fftw_complex *) data_X_Transp_2 ); 
                transpose(NxLD, NkyLD, NzLD, plasma->nfields, (A4zz) ((CComplex *) data_X_Transp_2), (A4zz) ((CComplex *) in));                
//                 fftw_execute_r2r(plan_FieldTranspose_1, (double*) data_X_Transp_2, (double *) p);
               }
               else   check(-1, DMESG("No such FFT direction"));

        } else {

             if     (direction == FFT_FORWARD )  fftw_execute(plan_XForward_Fields ); 
             else if(direction == FFT_BACKWARD)  fftw_execute(plan_XBackward_Fields); 
             else   check(-1, DMESG("No such FFT direction"));

        };
        }
        else if(FFTtype & FFT_XYZ & flags) check(-1, DMESG("No such FFT direction (did you set : FFTSolver.3D = 1 )"));
        else if(FFTtype & FFT_XY  & flags) check(-1, DMESG("No such FFT direction (did you set : FFTSolver.3D = 1 )"));

/* 
            } else {
             if     (direction == FFT_FORWARD )  fftw_execute(plan_XForward ); 
             else if(direction == FFT_BACKWARD)  fftw_execute(plan_XBackward); 
             else   check(-1, DMESG("No such FFT direction"));
            }
 * */
        else if(FFTtype & FFT_Y  & flags) {
            
             // Need to cast between bit comparible complex types
             if     (direction == FFT_FORWARD )  fftw_execute_dft_r2c(plan_YForward_Field , (double   *) in, (fftw_complex *) out); 
             else if(direction == FFT_BACKWARD)  fftw_execute_dft_c2r(plan_YBackward_Field, (fftw_complex *) in, (double   *) out); 
             else   check(-1, DMESG("No such FFT direction"));
        }
        else  check(-1, DMESG("Unknown FFT type or not supported"));

       return;
}


std::string FFTSolver_fftw3::getLibraryName() {
        return std::string(fftw_version) + std::string("-mpi");
}
  

FFTSolver_fftw3::~FFTSolver_fftw3() {

    //  release fftw-3  
    if(flags & FFT_X) {
        fftw_destroy_plan(plan_XForward);
       fftw_destroy_plan(plan_XBackward);
        fftw_destroy_plan(plan_XForward_Fields);
       fftw_destroy_plan(plan_XBackward_Fields);
    }
    if(flags & FFT_Y) {
        fftw_destroy_plan(plan_YForward_Field);
       fftw_destroy_plan(plan_YBackward_Field);
        
        fftw_destroy_plan(plan_AA_YForward);
       fftw_destroy_plan(plan_AA_YBackward);

    }

#ifdef PARALLEL_OPENMP
    fftw_cleanup_threads();
#endif
   
    // clean up arrays 
    fftw_free(data_Y_kIn);
    fftw_free(data_Y_kOut);
    fftw_free(data_Y_rIn);
    fftw_free(data_Y_rOut);
    
    fftw_free(data_X_kIn);
    fftw_free(data_X_rOut);
    fftw_free(data_X_rIn);
    fftw_free(data_X_kOut);
    

    fftw_destroy_plan(plan_FieldTranspose_1);
    fftw_destroy_plan(plan_FieldTranspose_2);

}


// That function let me cry, what a waste of cycles ...
// Any good in-place tranposition ?
//
/*
void FFTSolver_fftw3::transposeFields(CComplex *In, CComplex *T)
{

   const int stride = NkyLD * NzLD * plasma->nfields;

   omp_for(int x = 0, int n = 0; x < NxLD; x++) { simd_for(int m = 0; m < stride; m++, n++) {

      T[n] = In[x+m*NxLD];

   } }


};
  */     
// Note : We need to take care of aliasing
Array3C FFTSolver_fftw3::multiply(Array3C &A, Array3C &B, Array3C  &R) 
{
   return R; 
   // Array A : Copy Values to larger AA-Array (set larger values to zero)
   AA_kYIn = 0.;
   for(int y_k=NkyLlD; y_k<= NkyLuD;y_k++) AA_kYIn(RxLD, y_k, RzLD, 1) = A(RxLD, y_k, RzLD); 
   fftw_execute(plan_AA_YBackward);


   AA_rYIn(RxLD, AA_RyLD, RzLD, 1) = AA_rYOut(RxLD, AA_RyLD, RzLD, 1);
   
   // Array B : Copy Values to larger AA-Array (set larger values to zero)
   AA_kYIn = 0.;
   for(int y_k=NkyLlD; y_k<= NkyLuD;y_k++) AA_kYIn(RxLD, y_k, RzLD, 1) = B(RxLD, y_k, RzLD); 
   fftw_execute(plan_AA_YBackward);


   //////////////////////// Real Space (multiply values) /////////////////// 
   
   const double fft_Norm = 1.5 * Norm_Y_Forward * pow2(Norm_Y_Backward);
   
   for(int z=NzLlD; z<=NzLuD;z++) { for(int y=AA_NyLlD; y<=AA_NyLuD;y++) { for(int x= NxLlD; x <= NxLuD; x++) {
    AA_rYIn(x,y,z,1) *= AA_rYOut(x,y,z,1) / fft_Norm;
   }}}
   
   //////////////////////// End Real Space (multiply values) /////////////////// 
   
   fftw_execute(plan_AA_YForward);
   
   // Array R : Copy Result back to array
   for(int y_k=NkyLlD; y_k<= NkyLuD;y_k++) R(RxLD, y_k, RzLD) = AA_kYOut(RxLD, y_k, RzLD,1); 
   return R;

};

/*  
void somelogic()
{
    char storage_type = 'R';
    int rows          = 3;
    int cols          = 3;
    double *in        = p;
    double *out       = p2;

    // Plan the transpose once; transpose is in-place if in == out 
    fftw_plan transpose = plan_transpose(storage_type, rows, cols, in, out);
    assert(transpose);

    // Execute the plan potentially many times 
    fftw_execute(transpose);

    // FFTW New-array Execute functions should be callable, too 
    // Beware of mixing in-place and out-of-place planning and usage 
    double *another_in  = a;
    double *another_out = b;
    fftw_execute_r2r(transpose, another_in, another_out);

    // Destroy the plan when completely done 
    fftw_destroy_plan(transpose);
}
 *  */


fftw_plan plan_transpose(char storage_type, int rows, int cols, double *in, double *out)
{
    const unsigned flags = FFTW_MEASURE; /* Do not destroy input */

    fftw_iodim howmany_dims[2];
    switch (toupper(storage_type)) {
        case 'R':
            howmany_dims[0].n  = rows;
            howmany_dims[0].is = cols;
            howmany_dims[0].os = 1;
            howmany_dims[1].n  = cols;
            howmany_dims[1].is = 1;
            howmany_dims[1].os = rows;
            break;
        case 'C':
            howmany_dims[0].n  = rows;
            howmany_dims[0].is = 1;
            howmany_dims[0].os = cols;
            howmany_dims[1].n  = cols;
            howmany_dims[1].is = rows;
            howmany_dims[1].os = 1;
            break;
        default:
            return NULL;
    }
    const int howmany_rank = sizeof(howmany_dims)/sizeof(howmany_dims[0]);

    return fftw_plan_guru_r2r(0, NULL, 2, howmany_dims, in, out, NULL, flags);
    //return fftw_plan_guru_r2r(/*rank*/0, /*dims*/NULL, howmany_rank, howmany_dims, in, out, /*kind*/NULL, flags);
}

    
void FFTSolver_fftw3::printOn(ostream &output) const {

         output   << "FFTSolver  |  using fftw-3 interface for (" << std::string(fftw_version) << ")" << std::endl;
         output   << "           |  Plan : " << plan << " Wisdom : " << ((wisdom=="") ? "None" : wisdom) << std::endl;
         
}
