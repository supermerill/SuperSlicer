// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "repdiag.h"
#include <vector>

template <typename T>
IGL_INLINE void igl::repdiag(
  const Eigen::SparseMatrix<T>& A,
  const int d,
  Eigen::SparseMatrix<T>& B)
{
  using namespace std;
  using namespace Eigen;
  int m = A.rows();
  int n = A.cols();
#if false
  vector<Triplet<T> > IJV;
  IJV.reserve(A.nonZeros()*d);
  // Loop outer level
  for (int k=0; k<A.outerSize(); ++k)
  {
    // loop inner level
    for (typename Eigen::SparseMatrix<T>::InnerIterator it(A,k); it; ++it)
    {
      for(int i = 0;i<d;i++)
      {
        IJV.push_back(Triplet<T>(i*m+it.row(),i*n+it.col(),it.value()));
      }
    }
  }
  B.resize(m*d,n*d);
  B.setFromTriplets(IJV.begin(),IJV.end());
#else
  // This will not work for RowMajor
  B.resize(m*d,n*d);
  Eigen::VectorXi per_col = Eigen::VectorXi::Zero(n*d);
  for (int k=0; k<A.outerSize(); ++k)
  {
    for (typename Eigen::SparseMatrix<T>::InnerIterator it(A,k); it; ++it)
    {
      for(int r = 0;r<d;r++) per_col(n*r + k)++;
    }
  }
  B.reserve(per_col);
  for(int r = 0;r<d;r++)
  {
    const int mr = m*r;
    const int nr = n*r;
    for (int k=0; k<A.outerSize(); ++k)
    {
      for (typename Eigen::SparseMatrix<T>::InnerIterator it(A,k); it; ++it)
      {
        B.insert(it.row()+mr,k+nr) = it.value();
      }
    }
  }
  B.makeCompressed();
#endif
}

template <typename T>
IGL_INLINE void igl::repdiag(
  const Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & A,
  const int d,
  Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & B)
{
  int m = A.rows();
  int n = A.cols();
  B.resize(m*d,n*d);
  B.array() *= 0;
  for(int i = 0;i<d;i++)
  {
    B.block(i*m,i*n,m,n) = A;
  }
}

// Wrapper with B as output
template <class Mat>
IGL_INLINE Mat igl::repdiag(const Mat & A, const int d)
{
  Mat B;
  repdiag(A,d,B);
  return B;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::repdiag<double>(Eigen::SparseMatrix<double, 0, int> const&, int, Eigen::SparseMatrix<double, 0, int>&);
// generated by autoexplicit.sh
template Eigen::SparseMatrix<double, 0, int> igl::repdiag<Eigen::SparseMatrix<double, 0, int> >(Eigen::SparseMatrix<double, 0, int> const&, int);
#endif
