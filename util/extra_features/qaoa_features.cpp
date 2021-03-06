//------------------------------------------------------------------------------
// Copyright (C) 2019 Intel Corporation 
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//------------------------------------------------------------------------------

#include "qaoa_features.hpp"

#include <cassert>		// pow

#ifdef INTELQS_HAS_MPI
#include <mpi.h>
#endif

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////

namespace qaoa
{

/////////////////////////////////////////////////////////////////////////////////////////

namespace utility
{

/////////////////////////////////////////////////////////////////////////////////////////

/// Function to convert a decimal number into a binary number (expressed as vector).
/// The 0-component of the vector represents the least significant bit.
/// This convention has been used in qHiPSTER but may be unusual in quantum computation
/// where qubit zero-th is usually associated to the most significant bit.
template<typename T_decimal, typename T_bit>
void ConvertToBinary( T_decimal k, std::vector<T_bit> &z )
{
  // If decimal number is too large to be converted into a binary.
  if ( k >= (T_decimal)((T_decimal)1<<(T_decimal)(z.size()) ) )
  {
      std::cout << "Too large decimal number:\n"
                << "decimal = " << k << " , but binary has " << z.size() << "bits.\n";
      assert( 0 );
  }
  for (unsigned pos=0; pos<z.size(); ++pos)
  {
      z[pos] = k%(T_decimal)(2);
      k=k/(T_decimal)(2);
  }
}

template void ConvertToBinary<std::size_t,int> (std::size_t, std::vector<int> &);
template void ConvertToBinary<std::size_t,unsigned> (std::size_t, std::vector<unsigned> &);

////////////////////////////////////////////////////////////////////////////////

/// Function to convert a binary number (expressed as vector) into a decimal number.
/// The 0-componenet of the vector represents the least significant bit (i.e. associated
/// to the factor 2^0 in power expension).
template<typename T_bit, typename T_decimal>
void ConvertToDecimal( std::vector<T_bit> &z , T_decimal &k )
{
  k=0;
  for (unsigned pos=z.size()-1; pos>0; --pos)
  {
      k += (T_decimal)(z[pos]);
      k=k*(T_decimal)(2);
  }
  k += (T_decimal)(z[0]);
}

template void ConvertToDecimal<int,std::size_t> (std::vector<int> &, std::size_t &);
template void ConvertToDecimal<unsigned,std::size_t> (std::vector<unsigned> &, std::size_t &);

////////////////////////////////////////////////////////////////////////////////

}	// end namespace 'utility'

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

template<typename Type>
int InitializeVectorAsMaxCutCostFunction(QubitRegister<Type> & diag,
                                         std::vector<int> & adjacency)
{
  int num_vertices = diag.NumQubits();
  int num_edges = 0;

  // A few preliminary checks on the adjacency matrix.
  // - it should have the right size:
  assert(adjacency.size() == num_vertices*num_vertices);
  // - it should have a null diagonal:
  for (int v=0; v<num_vertices; ++v)
      assert(adjacency[v*num_vertices+v]==0);
  // - each edge is counted twice
  for (int e=0; e<adjacency.size(); ++e)
      num_edges += adjacency[e];
  assert(num_edges%2==0);
  num_edges /= 2;

  // Denote (x)^T the row vector: {-1,1,1,-1,1,...,1}
  // It indicates how the vertices of the graph are colored (either +1 or -1).
  // In this case,
  //   (x)^T.ADJ.(x) = 2*(num_uncut_edges - num_cut_edges)
  // Therefore:
  //   num_cut_edges = ( num_edges - x^T.ADJ.x /2 ) /2 
 
  std::size_t myrank = qhipster::mpi::Environment::GetStateRank();
  std::size_t glb_start = UL(myrank) * diag.LocalSize();
  int max_cut = 0;

#pragma omp parallel
  {
      std::size_t x;
      std::vector<int> xbin(num_vertices);
      int cut;
      #pragma omp for reduction(max: max_cut)
      for(std::size_t i = 0; i < diag.LocalSize(); i++)
      {
         x = glb_start + i;
         // From decimal to binary vector of {0,1}.
         utility::ConvertToBinary(x,xbin);
         // From binary vector of {0,1} to binary vector of {-1,1}.
         for (int v=0; v<num_vertices; ++v)
             if (xbin[v]==0)
                 xbin[v]=-1;
         // Compute x^T.ADJ.x
         cut = 0;
         for (int v=0; v<num_vertices; ++v)
             for (int u=0; u<num_vertices; ++u)
                 cut += adjacency[v*num_vertices + u] * xbin[v] * xbin[u];
         assert(cut%2==0);
         cut = ( num_edges - cut/2);
         assert(cut%2==0);
         cut /= 2;
         diag[i] = Type(cut,0);
         if (cut>max_cut)
             max_cut = cut;
      }
  }

#ifdef INTELQS_HAS_MPI
  int lcl_max_cut = max_cut;
  MPI_Comm comm = qhipster::mpi::Environment::GetStateComm();
  MPI_Allreduce(&lcl_max_cut, &max_cut, 1, MPI_INT, MPI_MAX, comm);
#endif

  return max_cut;
}

template int InitializeVectorAsMaxCutCostFunction<ComplexDP>
    (QubitRegister<ComplexDP> &, std::vector<int> & );
template int InitializeVectorAsMaxCutCostFunction<ComplexSP>
    (QubitRegister<ComplexSP> &, std::vector<int> & );

/////////////////////////////////////////////////////////////////////////////////////////

template<typename Type>
void ImplementQaoaLayerBasedOnCostFunction(QubitRegister<Type> & psi,
                                           QubitRegister<Type> & diag,
                                           typename QubitRegister<Type>::BaseType gamma)
{
  assert( psi.LocalSize( ) == diag.LocalSize( ) );
  assert( psi.GlobalSize() == diag.GlobalSize() );

  // NOTE: cosine and sine for all values of the cost function could be computed once
  //       and stored in a vector.

  // exp(-i gamma H_problem)
  #pragma omp parallel for
  for (std::size_t i=0; i < psi.LocalSize(); ++i)
       psi[i] *= Type( std::cos(gamma* diag[i].real()) , -std::sin(gamma* diag[i].real()) );
}

template void ImplementQaoaLayerBasedOnCostFunction<ComplexDP>
    (QubitRegister<ComplexDP> &, QubitRegister<ComplexDP> &, double );
template void ImplementQaoaLayerBasedOnCostFunction<ComplexSP>
    (QubitRegister<ComplexSP> &, QubitRegister<ComplexSP> &, float );

/////////////////////////////////////////////////////////////////////////////////////////

template<typename Type>
typename QubitRegister<Type>::BaseType
GetExpectationValueFromCostFunction(const QubitRegister<Type> & psi,
                                    const QubitRegister<Type> & diag)
{
  // Extract basic type from IQS objects.
  typename QubitRegister<Type>::BaseType global_expectation, local_expectation = 0.;

  #pragma omp parallel for reduction(+: local_expectation)
  for ( size_t i=0 ; i < psi.LocalSize(); ++i)
  {
      local_expectation += diag[i].real() * norm(psi[i]) ;
  }

#ifdef INTELQS_HAS_MPI
  MPI_Comm comm = qhipster::mpi::Environment::GetStateComm();
  qhipster::mpi::MPI_Allreduce_x(&local_expectation, &global_expectation, 1, MPI_SUM, comm);
#else
  global_expectation = local_expectation;
#endif
  return global_expectation;
}

template double GetExpectationValueFromCostFunction<ComplexDP>
    (const QubitRegister<ComplexDP> &, const QubitRegister<ComplexDP> & );
template float GetExpectationValueFromCostFunction<ComplexSP>
    (const QubitRegister<ComplexSP> &, const QubitRegister<ComplexSP> & );

/////////////////////////////////////////////////////////////////////////////////////////

template<typename Type>
std::vector<typename QubitRegister<Type>::BaseType>
GetHistogramFromCostFunction( const QubitRegister<Type> & psi,
                              const QubitRegister<Type> & diag, int max_value)
{
  // Extract basic type from IQS objects.
  typedef typename QubitRegister<Type>::BaseType Basetype;

  // A few preliminary checks:
  assert( psi.LocalSize( ) == diag.LocalSize( ) );	// Vectors with equal local size.
  assert( psi.GlobalSize() == diag.GlobalSize() );	// Vectors with equal global size.
  assert(max_value>0);					// The max_value must be positive.

  int my_rank = qhipster::mpi::Environment::GetStateRank();

  // Histogram of the specific MPI (state) rank.
  Basetype local_hist[max_value+1] ;	// Initialize all elements to 0 (only with C++)
  for (int n=0; n<=max_value; ++n)
      local_hist[n]=0;

  #pragma omp parallel
  {
      int cut;
      int index_bin;
      // Histogram of the specific thread.
      Basetype private_hist[max_value+1] ;
      for (int n=0; n<=max_value; ++n)
          private_hist[n]=0;

      #pragma omp for
      for ( size_t i=0 ; i < psi.LocalSize(); ++i)
      {
          cut = (int)(diag[i].real());
          assert( cut>=0 && cut <= max_value );
          // The next line can be changed to have wider bins. In general not required.
          index_bin = cut;
          private_hist[index_bin] += norm(psi[i]) ;
      }
      #pragma omp critical
      {
          for (int n=0; n<=max_value; ++n)
              local_hist[n] += private_hist[n];
      }
  }

  // Global histogram.
  std::vector<Basetype> global_hist(max_value+1,0);
#ifdef INTELQS_HAS_MPI
  // Sum local histograms into (state) global histogram.
  MPI_Comm comm = qhipster::mpi::Environment::GetStateComm();
  qhipster::mpi::MPI_Allreduce_x(local_hist, global_hist.data(), max_value+1, MPI_SUM, comm);
#else
  for ( int n=0; n<=max_value; ++n)
      global_hist[n]=local_hist[n];
#endif

  return global_hist;
}

template std::vector<double> GetHistogramFromCostFunction<ComplexDP>
    (const QubitRegister<ComplexDP> &, const QubitRegister<ComplexDP> &, int );
template std::vector<float>  GetHistogramFromCostFunction<ComplexSP>
    (const QubitRegister<ComplexSP> &, const QubitRegister<ComplexSP> &, int );

/////////////////////////////////////////////////////////////////////////////////////////

}	// close namespace qaoa

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
