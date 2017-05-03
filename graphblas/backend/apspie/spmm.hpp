#ifndef GRB_BACKEND_APSPIE_SPMM_HPP
#define GRB_BACKEND_APSPIE_SPMM_HPP

#include <iostream>

#include <cuda.h>
#include <cusparse.h>

#include "graphblas/backend/apspie/SparseMatrix.hpp"
#include "graphblas/backend/apspie/DenseMatrix.hpp"
#include "graphblas/types.hpp"

#define VT 16
#define NV 32
#define NT 128

namespace graphblas
{
namespace backend
{
	template<typename c>
	__global__ void spmm_row_kernel( const Index A_nrows, const Index B_ncols, 
			const Index A_ncols, const Index A_nvals, 
			const Index* A_csrRowPtr, const Index* A_csrColInd, const c* A_csrVal, 
			const c* B_denseVal, c* C_denseVal );

	template<typename c>
  __global__ void spmm_col_kernel( const Index A_nrows, const Index B_ncols, 
	  	const Index A_ncols, const Index A_nvals, 
		  const Index* A_csrRowPtr, const Index* A_csrColInd, const c* A_csrVal, 
		  const c* B_denseVal, c* C_denseVal );

  template<typename c, typename a, typename b>
	Info spmm( DenseMatrix<c>&        C,
             const Semiring&        op,
             const SparseMatrix<a>& A,
             const DenseMatrix<b>&  B )
  {
    Index A_nrows, A_ncols, A_nvals;
    Index B_nrows, B_ncols;
    Index C_nrows, C_ncols;

    A.nrows( A_nrows );
    A.ncols( A_ncols );
    A.nvals( A_nvals );
    B.nrows( B_nrows );
    B.ncols( B_ncols );
    C.nrows( C_nrows );
    C.ncols( C_ncols );

    // Dimension compatibility check
    if( (A_ncols != B_nrows) || (C_ncols != B_ncols) || (C_nrows != A_nrows ) )
    {
      std::cout << "Dim mismatch" << std::endl;
      std::cout << A_ncols << " " << B_nrows << std::endl;
      std::cout << C_ncols << " " << B_ncols << std::endl;
      std::cout << C_nrows << " " << A_nrows << std::endl;
      return GrB_DIMENSION_MISMATCH;
    }

    // Domain compatibility check
    // TODO: add domain compatibility check

    // Computation
    Info err;
    const int T        = VT;
    const int NTHREADS = NT;
    const int NBLOCKS  = (T*A_nrows+NTHREADS-1)/NTHREADS;
    spmm_col_kernel<<<NBLOCKS,NTHREADS>>>( A_nrows, B_ncols, A_ncols, A_nvals,
      A.d_csrRowPtr, A.d_csrColInd, A.d_csrVal, B.d_denseVal, C.d_denseVal );

		C.need_update = true;
		return GrB_SUCCESS;
	}

	// Naive implementation (row major)
	// Notes: performs much worse than cuSPARSE col major csrmm
	template<typename c>
	__global__ void spmm_row_kernel( const Index A_nrows, const Index B_ncols, 
			const Index A_ncols, const Index A_nvals, 
			const Index* A_csrRowPtr, const Index* A_csrColInd, const c* A_csrVal, 
			const c* B_denseVal, c* C_denseVal )
	{
		const Index idx = blockIdx.x*blockDim.x + threadIdx.x;
    const int   idb = threadIdx.x;
		const int   T   = VT;
    const int   L_c = NV;
		const Index i   = idx/T;
		const int   sid = idb/T;  // equivalent to (idx%blk)/T
    const int   idp = idb%T;
		const int   blk = NT;

		c sv[L_c];
		__shared__ c sdata[blk/T*L_c];

    if( i<A_nrows ) {
			const int max_batch = B_ncols/L_c;
			for( int batch=0; batch<max_batch; batch++ ) {
        sv[0] = 0.0; sv[1] = 0.0; sv[2] = 0.0; sv[3] = 0.0;
			  //const int max = (A_csrRowPtr[i+1]-A_csrRowPtr[i])/T;
			  const int max = (A_csrRowPtr[i+1]-A_csrRowPtr[i]+T-1)/T;
			  for( int j=0; j<max; j++ ) {
          Index ind = A_csrRowPtr[i]+j*T+idp;
				  if( ind<A_csrRowPtr[i+1] ) {
				    c     val = A_csrVal[ind];
				    Index col = A_csrColInd[ind];
				    sv[0] += val*B_denseVal[0+batch*L_c+col*B_ncols];
				    sv[1] += val*B_denseVal[1+batch*L_c+col*B_ncols];
				    sv[2] += val*B_denseVal[2+batch*L_c+col*B_ncols];
				    sv[3] += val*B_denseVal[3+batch*L_c+col*B_ncols];
            //printf("tid:%d,row:%d,col:%d,val:%f,sv0:%f,sv1:%f,sv2:%f,sv3:%f\n", idb, i, col, val, sv[0], sv[1], sv[2], sv[3] );
			  }}
			  if( idp!=0 ) {
		      sdata[sid*L_c+0] = sv[0];
	  		  sdata[sid*L_c+1] = sv[1];
		  	  sdata[sid*L_c+2] = sv[2];
			    sdata[sid*L_c+3] = sv[3];
          //printf("tid:%d,row:%d,sv0:%f,sv1:%f,sv2:%f,sv3:%f\n", idb, i, sv[0], sv[1], sv[2], sv[3] );
        }
        __syncthreads();
			  if( idp==0 ) {
				  C_denseVal[0+batch*L_c+i*B_ncols] = sdata[sid*L_c+0]+sv[0];
			    C_denseVal[1+batch*L_c+i*B_ncols] = sdata[sid*L_c+1]+sv[1];
			    C_denseVal[2+batch*L_c+i*B_ncols] = sdata[sid*L_c+2]+sv[2];
			    C_denseVal[3+batch*L_c+i*B_ncols] = sdata[sid*L_c+3]+sv[3];
        //printf("tid:%d,row:%d,sv0:%d,sv1:%d,sv2:%d,sv3:%d\n", idb, i, 0*A_ncols+i, 1*A_ncols+i, 2*A_ncols+i, 3*A_ncols+i );
        //printf("tid:%d,row:%d,sv0:%f,sv1:%f,sv2:%f,sv3:%f\n", idb, i, C_denseVal[0*A_ncols+i], C_denseVal[1*A_ncols+i], C_denseVal[2*A_ncols+i], C_denseVal[3*A_ncols+i] );
			}}
			const int rem = B_ncols-max_batch*L_c;
			//if( idb==0 ) printf("Remainder:%d\n", rem);
			if( rem!=0 ) {
        sv[0] = 0.0; sv[1] = 0.0; sv[2] = 0.0;
			  const int max = (A_csrRowPtr[i+1]-A_csrRowPtr[i]+T-1)/T;
			  for( int j=0; j<max; j++ ) {
          Index ind = A_csrRowPtr[i]+j*T+idp;
			    if( ind<A_csrRowPtr[i+1] ) {
		        c     val = A_csrVal[ind];
				    Index col = A_csrColInd[ind];
				                sv[0] += val*B_denseVal[0+max_batch*L_c+col*B_ncols];
				    if( rem>1 ) sv[1] += val*B_denseVal[1+max_batch*L_c+col*B_ncols];
				    if( rem>2 ) sv[2] += val*B_denseVal[2+max_batch*L_c+col*B_ncols];
            //printf("tid:%d,row:%d,col:%d,val:%f,sv0:%f,sv1:%f,sv2:%f,sv3:%f\n", idb, i, col, val, sv[0], sv[1], sv[2], sv[3] );
			    }}
			    if( idp!=0 ) {
		                    sdata[sid*L_c+0] = sv[0];
	  		    if( rem>1 ) sdata[sid*L_c+1] = sv[1];
		  	    if( rem>2 )sdata[sid*L_c+2] = sv[2];
            //printf("tid:%d,row:%d,sv0:%f,sv1:%f,sv2:%f,sv3:%f\n", idb, i, sv[0], sv[1], sv[2], sv[3] );
          }
          __syncthreads();
			    if( idp==0 ) {
								        C_denseVal[0+max_batch*L_c+i*B_ncols] = sdata[sid*L_c+0]+sv[0];
						if( rem>1 ) C_denseVal[1+max_batch*L_c+i*B_ncols] = sdata[sid*L_c+1]+sv[1];
			      if( rem>2 ) C_denseVal[2+max_batch*L_c+i*B_ncols] = sdata[sid*L_c+2]+sv[2];
        //printf("tid:%d,row:%d,sv0:%d,sv1:%d,sv2:%d,sv3:%d\n", idb, i, 0*A_ncols+i, 1*A_ncols+i, 2*A_ncols+i, 3*A_ncols+i );
        //printf("tid:%d,row:%d,sv0:%f,sv1:%f,sv2:%f,sv3:%f\n", idb, i, C_denseVal[0*A_ncols+i], C_denseVal[1*A_ncols+i], C_denseVal[2*A_ncols+i], C_denseVal[3*A_ncols+i] );
	}}}}

	// Naive implementation (col major)
	// Notes: performs much worse than cuSPARSE col major csrmm
	template<typename c>
	__global__ void spmm_col_kernel( const Index A_nrows, const Index B_ncols, 
			const Index A_ncols, const Index A_nvals, 
			const Index* A_csrRowPtr, const Index* A_csrColInd, const c* A_csrVal, 
			const c* B_denseVal, c* C_denseVal )
	{
		const Index idx = blockIdx.x*blockDim.x + threadIdx.x;
    const int   idb = threadIdx.x;
		const int   T   = VT;
    const int   L_c = NV;
		const Index i   = idx/T;
		const int   sid = idb/T;     // equivalent to (idx%blk)/T
    const int   idp = idb&(T-1); // equivalent to (idb%T)
		const int   blk = NT;

		c sv[L_c];
		__shared__ c sdata[blk/T*L_c];

    if( i<A_nrows ) {
			const int max_batch = B_ncols/L_c;
			for( int batch=0; batch<max_batch; batch++ ) {
        #pragma unroll
			  for( int k=0; k<L_c; k++ )
					sv[k] = 0.0;
			  //const int max = (A_csrRowPtr[i+1]-A_csrRowPtr[i])/T;
			  const int max = (A_csrRowPtr[i+1]-A_csrRowPtr[i]+T-1)/T;
			  for( int j=0; j<max; j++ ) {
          Index ind = A_csrRowPtr[i]+j*T+idp;
				  if( ind<A_csrRowPtr[i+1] ) {
				    c     val = A_csrVal[ind];
				    Index col = A_csrColInd[ind];
            #pragma unroll
						for( int k=0; k<L_c; k++ )
							sv[k] += val*B_denseVal[(k+batch*L_c)*A_ncols+col];
            //printf("tid:%d,row:%d,col:%d,val:%f,sv0:%f,sv1:%f,sv2:%f,sv3:%f\n", idb, i, col, val, sv[0], sv[1], sv[2], sv[3] );
			  }}
			  if( idp!=0 ) {
          #pragma unroll
					for( int k=0; k<L_c; k++ )
						sdata[sid*L_c+k] = sv[k];
          //printf("tid:%d,row:%d,sv0:%f,sv1:%f,sv2:%f,sv3:%f\n", idb, i, sv[0], sv[1], sv[2], sv[3] );
        }
        __syncthreads();
			  if( idp==0 ) {
          #pragma unroll
					for( int k=0; k<L_c; k++ )
						C_denseVal[(k+batch*L_c)*A_nrows+i] = sdata[sid*L_c+k]+sv[k];
        //printf("tid:%d,row:%d,sv0:%d,sv1:%d,sv2:%d,sv3:%d\n", idb, i, 0*A_ncols+i, 1*A_ncols+i, 2*A_ncols+i, 3*A_ncols+i );
        //printf("tid:%d,row:%d,sv0:%f,sv1:%f,sv2:%f,sv3:%f\n", idb, i, C_denseVal[0*A_ncols+i], C_denseVal[1*A_ncols+i], C_denseVal[2*A_ncols+i], C_denseVal[3*A_ncols+i] );
			}}
			/*const int rem = B_ncols-max_batch*L_c;
			//if( idb==0 ) printf("Remainder:%d\n", rem);
			if( rem!=0 ) {
        #pragma unroll
			  for( int k=0; k<L_c-1; k++ )
					sv[k] = 0.0;
			  const int max = (A_csrRowPtr[i+1]-A_csrRowPtr[i]+T-1)/T;
			  for( int j=0; j<max; j++ ) {
          Index ind = A_csrRowPtr[i]+j*T+idp;
			    if( ind<A_csrRowPtr[i+1] ) {
		        c     val = A_csrVal[ind];
				    Index col = A_csrColInd[ind];
				                sv[0] += val*B_denseVal[(0+max_batch*L_c)*A_ncols+col];
				    if( rem>1 ) sv[1] += val*B_denseVal[(1+max_batch*L_c)*A_ncols+col];
				    if( rem>2 ) sv[2] += val*B_denseVal[(2+max_batch*L_c)*A_ncols+col];
            //printf("tid:%d,row:%d,col:%d,val:%f,sv0:%f,sv1:%f,sv2:%f,sv3:%f\n", idb, i, col, val, sv[0], sv[1], sv[2], sv[3] );
			    }}
			    if( idp!=0 ) {
		                    sdata[sid*L_c+0] = sv[0];
	  		    if( rem>1 ) sdata[sid*L_c+1] = sv[1];
		  	    if( rem>2 ) sdata[sid*L_c+2] = sv[2];
            //printf("tid:%d,row:%d,sv0:%f,sv1:%f,sv2:%f,sv3:%f\n", idb, i, sv[0], sv[1], sv[2], sv[3] );
          }
          __syncthreads();
			    if( idp==0 ) {
								        C_denseVal[(0+max_batch*L_c)*A_nrows+i] = sdata[sid*L_c+0]+sv[0];
						if( rem>1 ) C_denseVal[(1+max_batch*L_c)*A_nrows+i] = sdata[sid*L_c+1]+sv[1];
			      if( rem>2 ) C_denseVal[(2+max_batch*L_c)*A_nrows+i] = sdata[sid*L_c+2]+sv[2];
        //printf("tid:%d,row:%d,sv0:%d,sv1:%d,sv2:%d,sv3:%d\n", idb, i, 0*A_ncols+i, 1*A_ncols+i, 2*A_ncols+i, 3*A_ncols+i );
        //printf("tid:%d,row:%d,sv0:%f,sv1:%f,sv2:%f,sv3:%f\n", idb, i, C_denseVal[0*A_ncols+i], C_denseVal[1*A_ncols+i], C_denseVal[2*A_ncols+i], C_denseVal[3*A_ncols+i] );}}*/
	}}
	
	
  template<typename c, typename a, typename b>
  Info cusparse_spmm( DenseMatrix<c>&        C,
                      const Semiring&        op,
                      const SparseMatrix<a>& A,
                      const DenseMatrix<b>&  B )
  {
    Index A_nrows, A_ncols, A_nvals;
    Index B_nrows, B_ncols;
    Index C_nrows, C_ncols;

    A.nrows( A_nrows );
    A.ncols( A_ncols );
    A.nvals( A_nvals );
    B.nrows( B_nrows );
    B.ncols( B_ncols );
    C.nrows( C_nrows );
    C.ncols( C_ncols );

    // Dimension compatibility check
    if( (A_ncols != B_nrows) || (C_ncols != B_ncols) || (C_nrows != A_nrows ) )
    {
      std::cout << "Dim mismatch" << std::endl;
      std::cout << A_ncols << " " << B_nrows << std::endl;
      std::cout << C_ncols << " " << B_ncols << std::endl;
      std::cout << C_nrows << " " << A_nrows << std::endl;
      return GrB_DIMENSION_MISMATCH;
    }

    // Domain compatibility check
    // TODO: add domain compatibility check

    // Computation
    cusparseHandle_t handle;
    cusparseCreate( &handle );
    cusparseSetPointerMode( handle, CUSPARSE_POINTER_MODE_HOST );

    cusparseMatDescr_t descr;
    cusparseCreateMatDescr( &descr );

    cusparseSetMatType( descr, CUSPARSE_MATRIX_TYPE_GENERAL );
    cusparseSetMatIndexBase( descr, CUSPARSE_INDEX_BASE_ZERO );

    cusparseSetMatType( descr, CUSPARSE_MATRIX_TYPE_GENERAL );
    cusparseSetMatIndexBase( descr, CUSPARSE_INDEX_BASE_ZERO );
    cusparseStatus_t status;
    float alpha = 1.0;
    float beta  = 0.0;
    status = cusparseScsrmm( handle,
        CUSPARSE_OPERATION_NON_TRANSPOSE, A_nrows, B_ncols, A_ncols, A_nvals,
        &alpha, descr, A.d_csrVal, A.d_csrRowPtr, A.d_csrColInd, B.d_denseVal,
        A_ncols,      // ldb = max(1,k) since op(A) = A
        &beta, C.d_denseVal,
        A_nrows );    // ldc = max(1,m) since op(A) = A

    switch( status ) {
        case CUSPARSE_STATUS_SUCCESS:
            std::cout << "SpMM successful!\n";
            break;
        case CUSPARSE_STATUS_NOT_INITIALIZED:
            std::cout << "Error: Library not initialized.\n";
            break;
        case CUSPARSE_STATUS_INVALID_VALUE:
            std::cout << "Error: Invalid parameters m, n, or nnz.\n";
            break;
        case CUSPARSE_STATUS_EXECUTION_FAILED:
            std::cout << "Error: Failed to launch GPU.\n";
            break;
        case CUSPARSE_STATUS_ALLOC_FAILED:
            std::cout << "Error: Resources could not be allocated.\n";
            break;
        case CUSPARSE_STATUS_ARCH_MISMATCH:
            std::cout << "Error: Device architecture does not support.\n";
            break;
        case CUSPARSE_STATUS_INTERNAL_ERROR:
            std::cout << "Error: An internal operation failed.\n";
            break;
        case CUSPARSE_STATUS_MATRIX_TYPE_NOT_SUPPORTED:
            std::cout << "Error: Matrix type not supported.\n";
    }

    C.need_update = true;  // Set flag that we need to copy data from GPU
    return GrB_SUCCESS;
  }
}  // backend
}  // graphblas

#endif  // GRB_BACKEND_APSPIE_SPMM_HPP
