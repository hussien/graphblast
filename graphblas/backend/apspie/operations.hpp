#ifndef GRB_BACKEND_APSPIE_OPERATIONS_HPP
#define GRB_BACKEND_APSPIE_OPERATIONS_HPP

namespace graphblas
{
namespace backend
{
  template <typename T>
  class Vector;

  template <typename T>
  class Matrix;

  template <typename c, typename a, typename b, typename m,
            typename BinaryOpT,     typename SemiringT>
  Info mxm( Matrix<c>*       C,
            const Matrix<a>* mask,
            BinaryOpT        accum,
            SemiringT        op,
            const Matrix<a>* A,
            const Matrix<b>* B,
            Descriptor*      desc )
  {
    Storage A_mat_type;
    Storage B_mat_type;
    CHECK( A->getStorage( &A_mat_type ) );
    CHECK( B->getStorage( &B_mat_type ) );

    if( A_mat_type==GrB_SPARSE && B_mat_type==GrB_SPARSE )
    {
      CHECK( C->setStorage( GrB_SPARSE ) );
      CHECK( cusparse_spgemm( &C->sparse_, mask, accum, op, &A->sparse_, 
          &B->sparse_, desc ) );
    }
    else
    {
			std::cout << "Error: SpMM and GEMM not implemented yet!\n";
      /*CHECK( C->setStorage( GrB_DENSE ) );
      if( A_mat_type==GrB_SPARSE && B_mat_type==GrB_DENSE )
      {
        CHECK( spmm( &C->dense_, mask, accum, op, &A->sparse_, 
            &B->dense_, desc ) );
      }
      else if( A_mat_type==GrB_DENSE && B_mat_type==GrB_SPARSE )
      {
        CHECK( spmm( &C->dense_, mask, accum, op, &A->dense_, 
            &B->sparse_, desc ) );
      }
      else
      {
        CHECK( gemm( &C->dense_, mask, accum, op, &A->dense_, 
            &B->dense_, desc ) );
      }*/
    }
    return GrB_SUCCESS;
  }

  template <typename W, typename U, typename a, typename M,
            typename BinaryOpT, typename SemiringT>
  Info vxm( Vector<W>*       w,
            const Vector<M>* mask,
            BinaryOpT        accum,
            SemiringT        op,
            const Vector<U>* u,
            const Matrix<a>* A,
            Descriptor*      desc )
  {
    // Get storage:
    Storage u_vec_type;
    Storage A_mat_type;
    CHECK( u->getStorage( &u_vec_type ) );
    CHECK( A->getStorage( &A_mat_type ) );
    Vector<U>* u_t = const_cast<Vector<U>*>(u);

    // Transpose:
    Desc_value inp0_mode;
    CHECK( desc->get(GrB_INP0, &inp0_mode) );
    if (inp0_mode != GrB_DEFAULT) return GrB_INVALID_VALUE;

    // Treat vxm as an mxv with transposed matrix
    CHECK( desc->toggle( GrB_INP1 ) );

    // 1a) Simple SpMSpV without any load-balancing codepath
    LoadBalanceMode mxv_mode = getEnv("GRB_LOAD_BALANCE_MODE", GrB_LOAD_BALANCE_LB);
    if( desc->debug() )
      std::cout << "Load balance mode: " << mxv_mode << std::endl;
    if (mxv_mode == GrB_LOAD_BALANCE_SIMPLE)
    {
      if( desc->debug() )
        std::cout << "u_vec_type: " << u_vec_type << std::endl;
      if( u_vec_type==GrB_SPARSE )
        CHECK( u_t->sparse2dense(op.identity(), desc) );
      CHECK( w->setStorage(GrB_DENSE) );
      CHECK( spmspvSimple(&w->dense_, mask, accum, op, &A->sparse_, &u->dense_, 
          desc) );
    }
    else if (mxv_mode == GrB_LOAD_BALANCE_LB)
    {
      // 1b) Merge-path and direction-optimizing codepath
      //
      // Conversions:
      // TODO: add tol
			SparseMatrixFormat A_format;
			CHECK( A->getFormat(&A_format) );

      Desc_value vxm_mode, tol;
      CHECK( desc->get( GrB_MXVMODE, &vxm_mode ) );
      CHECK( desc->get( GrB_TOL,     &tol      ) );
      if( desc->debug() )
        std::cout << "Identity: " << op.identity() << std::endl;

			// Fallback for lacking CSC storage overrides any mxvmode selections
			if (A_format == GrB_SPARSE_MATRIX_CSRONLY)
      {
        if (u_vec_type == GrB_DENSE)
          CHECK( u_t->dense2sparse( op.identity(), desc ) );
      }
      else if( vxm_mode==GrB_PUSHPULL )
      {
        CHECK( u_t->convert( op.identity(), desc ) );
      }
      else if( vxm_mode==GrB_PUSHONLY && u_vec_type==GrB_DENSE )
      {
        CHECK( u_t->dense2sparse( op.identity(), desc ) );
      }
      else if( vxm_mode==GrB_PULLONLY && u_vec_type==GrB_SPARSE )
      {
        CHECK( u_t->sparse2dense( op.identity(), desc ) );
      }

      // Check if vector type was changed due to conversion!
      CHECK( u->getStorage( &u_vec_type ) );

      if( desc->debug() )
        std::cout << "u_vec_type: " << u_vec_type << std::endl;

      // Breakdown into 4 cases:
      // 1) SpMSpV: SpMat x SpVec
      // 2) SpMV:   SpMat x DeVec (preferred to 3)
			// 3) SpMSpV: SpMat x SpVec (fallback if CSC representation not available)
      // 4) GeMV:   DeMat x DeVec
			//
			// Note: differs from mxv, because mxv would say instead:
			// 3) "... if CSC representation not available ..."
      if( A_mat_type==GrB_SPARSE && u_vec_type==GrB_SPARSE )
      {
        CHECK( w->setStorage( GrB_SPARSE ) );
        CHECK( spmspv( &w->sparse_, mask, accum, op, &A->sparse_, 
            &u->sparse_, desc ) );
      }
      else
      {
        CHECK( w->setStorage( GrB_DENSE ) );
        if( A_mat_type==GrB_SPARSE )
          CHECK(spmv(&w->dense_, mask, accum, op, &A->sparse_, &u->dense_, 
              desc));
        else
          CHECK(gemv( &w->dense_, mask, accum, op, &A->dense_, &u->dense_, 
              desc));
      }
    }
    else
    {
      std::cout << "Error: Invalid load-balance algorithm!\n";
    }

    // Undo change to desc by toggling again
	  CHECK( desc->toggle( GrB_INP1 ) );

    return GrB_SUCCESS;
  }

  // Only difference between vxm and mxv is an additional check for gemv
  // to transpose A 
  // -this is because w=uA is same as w=A^Tu
  // -i.e. GraphBLAS treats 1xn Vector the same as nx1 Vector
  template <typename W, typename a, typename U, typename M,
            typename BinaryOpT, typename SemiringT>
  Info mxv( Vector<W>*       w,
            const Vector<M>* mask,
            BinaryOpT        accum,
            SemiringT        op,
            const Matrix<a>* A,
            const Vector<U>* u,
            Descriptor*      desc )
  {
    // Get storage:
    Storage u_vec_type;
    Storage A_mat_type;
    CHECK( u->getStorage( &u_vec_type ) );
    CHECK( A->getStorage( &A_mat_type ) );
    Vector<U>* u_t = const_cast<Vector<U>*>(u);

    // Transpose:
    Desc_value inp1_mode;
    CHECK( desc->get(GrB_INP1, &inp1_mode) );
    if( inp1_mode!=GrB_DEFAULT ) return GrB_INVALID_VALUE;

    // 1a) Simple SpMSpV without any load-balancing codepath
    LoadBalanceMode mxv_mode = getEnv("GRB_LOAD_BALANCE_MODE", GrB_LOAD_BALANCE_LB);
    if (mxv_mode == GrB_LOAD_BALANCE_SIMPLE)
    {
      if( u_vec_type==GrB_SPARSE )
        CHECK( u_t->sparse2dense(op.identity(), desc) );
      CHECK( w->setStorage(GrB_DENSE) );
      CHECK( spmspvSimple(&w->dense_, mask, accum, op, &A->sparse_, &u->dense_, 
          desc) );
    }
    else if (mxv_mode == GrB_LOAD_BALANCE_LB)
    {
      // 1b) Merge-path and direction-optimizing codepath
      //
      // Conversions:
			SparseMatrixFormat A_format;
			CHECK( A->getFormat(&A_format) );
      Desc_value mxv_mode;
      Desc_value tol;
      CHECK( desc->get( GrB_MXVMODE, &mxv_mode ) );
      CHECK( desc->get( GrB_TOL,     &tol      ) );

			// Fallback for lacking CSC storage overrides any mxvmode selections
		  if (A_format == GrB_SPARSE_MATRIX_CSRONLY)
      {
        if (u_vec_type == GrB_SPARSE)
          CHECK( u_t->sparse2dense( op.identity(), desc ) );
      }
      else if( mxv_mode==GrB_PUSHPULL )
      {
        CHECK( u_t->convert( op.identity(), desc ) );
      }
      else if( mxv_mode==GrB_PUSHONLY && u_vec_type==GrB_DENSE )
      {
        CHECK( u_t->dense2sparse( op.identity(), desc ) );
      }
      else if( mxv_mode==GrB_PULLONLY && u_vec_type==GrB_SPARSE )
      {
        CHECK( u_t->sparse2dense( op.identity(), desc ) );
      }

      // Check if vector type was changed due to conversion!
      CHECK( u->getStorage( &u_vec_type ) );

      // 3 cases:
      // 1) SpMSpV: SpMat x SpVec (preferred)
      // 2) SpMV:   SpMat x DeVec
			// 3) SpMV:   SpMat x DeVec (fallback if CSC representation not available)
      // 4) GeMV:   DeMat x DeVec
      if( A_mat_type==GrB_SPARSE && u_vec_type==GrB_SPARSE )
      {
        CHECK( w->setStorage( GrB_SPARSE ) );
        CHECK( spmspv( &w->sparse_, mask, accum, op, &A->sparse_, 
            &u->sparse_, desc ) );
      }
      else
      {
        CHECK( w->setStorage( GrB_DENSE ) );
        if( A_mat_type==GrB_SPARSE )
        {
          CHECK( spmv( &w->dense_, mask, accum, op, &A->sparse_, 
              &u->dense_, desc ) );
        }
        else
        {
          CHECK( gemv( &w->dense_, mask, accum, op, &A->dense_, 
              &u->dense_, desc ) );
        }
      }
    }
    else
    {
      std::cout << "Error: Invalid load-balance algorithm!\n";
    }

    return GrB_SUCCESS;
  }

  template <typename W, typename U, typename V, typename M,
            typename BinaryOpT,     typename SemiringT>
  Info eWiseMult( Vector<W>*       w,
                  const Vector<M>* mask,
                  BinaryOpT        accum,
                  SemiringT        op,
                  const Vector<U>* u,
                  const Vector<V>* v,
                  Descriptor*      desc )
  {
    // Use either op->operator() or op->mul() as the case may be
    Storage u_vec_type;
    Storage v_vec_type;
    CHECK( u->getStorage( &u_vec_type ) );
    CHECK( v->getStorage( &v_vec_type ) );

    /* 
     * \brief 4 cases:
     * 1) sparse x sparse
     * 2) dense  x dense
     * 3) sparse x dense
     * 4) dense  x sparse
     */
    if (u_vec_type == GrB_SPARSE && v_vec_type == GrB_SPARSE)
      CHECK( eWiseMult(&w->sparse_, mask, accum, op, &u->sparse_, &v->sparse_, 
          desc) );
    else if (u_vec_type == GrB_DENSE && v_vec_type == GrB_DENSE)
      CHECK( eWiseMult(&w->dense_, mask, accum, op, &u->dense_, &v->dense_, 
          desc) );
    else if (u_vec_type == GrB_SPARSE && v_vec_type == GrB_DENSE)
      CHECK( eWiseMult(&w->sparse_, mask, accum, op, &u->sparse_, &v->dense_,
          desc) );
    else if (u_vec_type == GrB_DENSE && v_vec_type == GrB_SPARSE)
      CHECK( eWiseMult(&w->sparse_, mask, accum, op, &v->sparse_, &u->dense_,
          desc) );
    else
      return GrB_INVALID_OBJECT;

    return GrB_SUCCESS;
  }

  template <typename c, typename a, typename b, typename m,
            typename BinaryOpT,     typename SemiringT>
  Info eWiseMult( Matrix<c>*       C,
                  const Matrix<m>* mask,
                  BinaryOpT        accum,
                  SemiringT        op,
                  const Matrix<a>* A,
                  const Matrix<b>* B,
                  Descriptor*      desc )
  {
    // Use either op->operator() or op->mul() as the case may be
  }

  template <typename W, typename U, typename V, typename M,
            typename BinaryOpT,     typename SemiringT>
  Info eWiseAdd( Vector<W>*       w,
                 const Vector<M>* mask,
                 BinaryOpT        accum,
                 SemiringT        op,
                 const Vector<U>* u,
                 const Vector<V>* v,
                 Descriptor*      desc )
  {
    // Use either op->operator() or op->add() as the case may be
  }

  template <typename c, typename a, typename b, typename m,
            typename BinaryOpT,     typename SemiringT>
  Info eWiseAdd( Matrix<c>*       C,
                 const Matrix<m>* mask,
                 BinaryOpT        accum,
                 SemiringT        op,
                 const Matrix<a>* A,
                 const Matrix<b>* B,
                 Descriptor*      desc )
  {
    // Use either op->operator() or op->add() as the case may be
  }

  template <typename W, typename U, typename M,
            typename BinaryOpT>
  Info extract( Vector<W>*                w,
                const Vector<M>*          mask,
                BinaryOpT                 accum,
                const Vector<U>*          u,
                const std::vector<Index>* indices,
                Index                     nindices,
                Descriptor*               desc )
  {

  }

  template <typename c, typename a, typename m,
            typename BinaryOpT>
  Info extract( Matrix<c>*                C,
                const Matrix<m>*          mask,
                BinaryOpT                 accum,
                const Matrix<a>*          A,
                const std::vector<Index>* row_indices,
                Index                     nrows,
                const std::vector<Index>* col_indices,
                Index                     ncols,
                Descriptor*               desc )
  {

  }

  template <typename W, typename a, typename M,
            typename BinaryOpT>
  Info extract( Vector<W>*                w,
                const Vector<M>*          mask,
                BinaryOpT                 accum,
                const Matrix<a>*          A,
                const std::vector<Index>* row_indices,
                Index                     nrows,
                Index                     col_index,
                Descriptor*               desc )
  {

  }

  template <typename W, typename U, typename M,
            typename BinaryOpT>
  Info assign( Vector<W>*                w,
               const Vector<M>*          mask,
               BinaryOpT                 accum,
               const Vector<U>*          u,
               const std::vector<Index>* indices,
               Index                     nindices,
               Descriptor*               desc )
  {
    return GrB_SUCCESS;
  }

  template <typename c, typename a, typename m,
            typename BinaryOpT>
  Info assign( Matrix<c>*                C,
               const Matrix<m>*          mask,
               BinaryOpT                 accum,
               const Matrix<a>*          A,
               const std::vector<Index>* row_indices,
               Index                     nrows,
               const std::vector<Index>* col_indices,
               Index                     ncols,
               Descriptor*               desc )
  {

  }

  template <typename c, typename U, typename M,
            typename BinaryOpT>
  Info assign( Matrix<c>*                C,
               const Vector<M>*          mask,
               BinaryOpT                 accum,
               const Vector<U>*          u,
               const std::vector<Index>* row_indices,
               Index                     nrows,
               Index                     col_index,
               Descriptor*               desc )
  {

  }

  template <typename c, typename U, typename M,
            typename BinaryOpT>
  Info assign( Matrix<c>*                C,
               const Vector<M>*          mask,
               BinaryOpT                 accum,
               const Vector<U>*          u,
               Index                     row_index,
               const std::vector<Index>* col_indices,
               Index                     ncols,
               Descriptor*               desc )
  {

  }

  template <typename W, typename T, typename M,
            typename BinaryOpT>
  Info assign( Vector<W>*                w,
               const Vector<M>*          mask,
               BinaryOpT                 accum,
               T                         val,
               const std::vector<Index>* indices,
               Index                     nindices,
               Descriptor*               desc )
  {
    // Get storage:
    Storage vec_type;
    CHECK( w->getStorage( &vec_type ) );

    // 2 cases:
    // 1) SpVec
    // 2) DeVec
    if( vec_type==GrB_SPARSE )
    {
      CHECK( w->setStorage( GrB_SPARSE ) );
      CHECK( assignSparse( &w->sparse_, mask, accum, val, indices, nindices, 
          desc) );
    }
    else if( vec_type==GrB_DENSE )
    {
      CHECK( w->setStorage( GrB_DENSE ) );
      CHECK( assignDense( &w->dense_, mask, accum, val, indices, nindices, 
          desc) );
    }

    return GrB_SUCCESS;
  }

  template <typename c, typename T, typename m,
            typename BinaryOpT>
  Info assign( Matrix<c>*                C,
               const Matrix<m>*          mask,
               BinaryOpT                 accum,
               T                         val,
               const std::vector<Index>* row_indices,
               Index                     nrows,
               const std::vector<Index>* col_indices,
               Index                     ncols,
               Descriptor*               desc )
  {

  }

  template <typename W, typename U, typename M,
            typename BinaryOpT,     typename UnaryOpT>
  Info apply( Vector<W>*       w,
              const Vector<M>* mask,
              BinaryOpT        accum,
              UnaryOpT         op,
              const Vector<U>* u,
              Descriptor*      desc )
  {

  }

  template <typename c, typename a, typename m,
            typename BinaryOpT,     typename UnaryOpT>
  Info apply( Matrix<c>*       C,
              const Matrix<m>* mask,
              BinaryOpT        accum,
              UnaryOpT         op,
              const Matrix<a>* A,
              Descriptor*      desc )
  {

  }

  template <typename W, typename a, typename M,
            typename BinaryOpT,     typename MonoidT>
  Info reduce( Vector<W>*       w,
               const Vector<M>* mask,
               BinaryOpT        accum,
               MonoidT          op,
               const Matrix<a>* A,
               Descriptor*      desc )
  {
    // Get storage:
    Storage mat_type;
    CHECK( A->getStorage( &mat_type ) );
		CHECK( w->setStorage( GrB_DENSE ) );

		if( mask == NULL )
		{
    // 2 cases:
    // 1) SpMat
    // 2) DeMat
      if( mat_type==GrB_SPARSE )
      {
        CHECK( reduceInner( &w->dense_, mask, accum, op, &A->sparse_, desc) );
      }
      else if( mat_type==GrB_DENSE )
      {
        CHECK( reduceInner( &w->dense_, mask, accum, op, &A->dense_, desc) );
      }
		}
		else
		{
      std::cout << "Error: Masked reduce not implemented yet!\n";
		}

		return GrB_SUCCESS;
  }

  template <typename T, typename U,
            typename BinaryOpT, typename MonoidT>
  Info reduce( T*               val,
               BinaryOpT        accum,
               MonoidT          op,
               const Vector<U>* u,
               Descriptor*      desc )
  {
    // Get storage:
    Storage vec_type;
    CHECK( u->getStorage( &vec_type ) );

    // 2 cases:
    // 1) SpVec
    // 2) DeVec
    if( vec_type==GrB_SPARSE )
    {
      CHECK( reduceInner( val, accum, op, &u->sparse_, desc) );
    }
    else if( vec_type==GrB_DENSE )
    {
      CHECK( reduceInner( val, accum, op, &u->dense_, desc) );
    }

    return GrB_SUCCESS;
  }

  template <typename T, typename a,
            typename BinaryOpT,     typename MonoidT>
  Info reduce( T*               val,
               BinaryOpT        accum,
               MonoidT          op,
               const Matrix<a>* A,
               Descriptor*      desc )
  {

  }

  template <typename c, typename a, typename m,
            typename BinaryOpT>
  Info transpose( Matrix<c>*       C,
                  const Matrix<m>* mask,
                  BinaryOpT        accum,
                  const Matrix<a>* A,
                  Descriptor*      desc )
  {

  }

}  // backend
}  // graphblas

#endif  // GRB_BACKEND_APSPIE_OPERATIONS_HPP
