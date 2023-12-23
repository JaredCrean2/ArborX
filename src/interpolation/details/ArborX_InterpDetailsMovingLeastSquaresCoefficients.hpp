/****************************************************************************
 * Copyright (c) 2023 by the ArborX authors                                 *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the ArborX library. ArborX is                       *
 * distributed under a BSD 3-clause license. For the licensing terms see    *
 * the LICENSE file in the top-level directory.                             *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

#ifndef ARBORX_INTERP_DETAILS_MOVING_LEAST_SQUARES_COEFFICIENTS_HPP
#define ARBORX_INTERP_DETAILS_MOVING_LEAST_SQUARES_COEFFICIENTS_HPP

#include <ArborX_AccessTraits.hpp>
#include <ArborX_DetailsKokkosExtAccessibilityTraits.hpp>
#include <ArborX_GeometryTraits.hpp>
#include <ArborX_HyperPoint.hpp>
#include <ArborX_InterpDetailsPolynomialBasis.hpp>
#include <ArborX_InterpDetailsSymmetricPseudoInverseSVD.hpp>

#include <Kokkos_Core.hpp>
#include <Kokkos_Profiling_ScopedRegion.hpp>

namespace ArborX::Interpolation::Details
{

template <typename CRBF, typename PolynomialDegree, typename CoefficientsType,
          typename MemorySpace, typename ExecutionSpace, typename SourcePoints,
          typename TargetPoints>
Kokkos::View<CoefficientsType **, MemorySpace>
movingLeastSquaresCoefficients(ExecutionSpace const &space,
                               SourcePoints const &source_points,
                               TargetPoints const &target_points)
{
  auto guard =
      Kokkos::Profiling::ScopedRegion("ArborX::MovingLeastSquaresCoefficients");

  static_assert(
      KokkosExt::is_accessible_from<MemorySpace, ExecutionSpace>::value,
      "Memory space must be accessible from the execution space");

  // SourcePoints is a 2D view of points
  static_assert(Kokkos::is_view_v<SourcePoints> && SourcePoints::rank == 2,
                "source points must be a 2D view of points");
  static_assert(
      KokkosExt::is_accessible_from<typename SourcePoints::memory_space,
                                    ExecutionSpace>::value,
      "source points must be accessible from the execution space");
  using src_point = typename SourcePoints::non_const_value_type;
  GeometryTraits::check_valid_geometry_traits(src_point{});
  static_assert(GeometryTraits::is_point<src_point>::value,
                "source points elements must be points");
  static constexpr int dimension = GeometryTraits::dimension_v<src_point>;

  // TargetPoints is an access trait of points
  ArborX::Details::check_valid_access_traits(PrimitivesTag{}, target_points);
  using tgt_acc = AccessTraits<TargetPoints, PrimitivesTag>;
  static_assert(KokkosExt::is_accessible_from<typename tgt_acc::memory_space,
                                              ExecutionSpace>::value,
                "target points must be accessible from the execution space");
  using tgt_point = typename ArborX::Details::AccessTraitsHelper<tgt_acc>::type;
  GeometryTraits::check_valid_geometry_traits(tgt_point{});
  static_assert(GeometryTraits::is_point<tgt_point>::value,
                "target points elements must be points");
  static_assert(dimension == GeometryTraits::dimension_v<tgt_point>,
                "target and source points must have the same dimension");

  int const num_targets = tgt_acc::size(target_points);
  int const num_neighbors = source_points.extent(1);

  // There must be a set of neighbors for each target
  KOKKOS_ASSERT(num_targets == source_points.extent_int(0));

  using point_t = ExperimentalHyperGeometry::Point<dimension, CoefficientsType>;
  static constexpr auto epsilon =
      Kokkos::Experimental::epsilon_v<CoefficientsType>;
  static constexpr int degree = PolynomialDegree::value;
  static constexpr int poly_size = polynomialBasisSize<dimension, degree>();

  Kokkos::Profiling::pushRegion(
      "ArborX::MovingLeastSquaresCoefficients::source_ref_target_fill");

  // The goal is to compute the following line vector for each target point:
  // p(x).[P^T.PHI.P]^-1.P^T.PHI
  // Where:
  // - p(x) is the polynomial basis of point x (line vector).
  // - P is the multidimensional Vandermonde matrix built from the source
  //   points, i.e., each line is the polynomial basis of a source point.
  // - PHI is the diagonal weight matrix / CRBF evaluated at each source point.

  // We first change the origin of the evaluation to be at the target point.
  // This lets us use p(0) which is [1 0 ... 0].
  Kokkos::View<point_t **, MemorySpace> source_ref_target(
      Kokkos::view_alloc(
          space, Kokkos::WithoutInitializing,
          "ArborX::MovingLeastSquaresCoefficients::source_ref_target"),
      num_targets, num_neighbors);
  Kokkos::parallel_for(
      "ArborX::MovingLeastSquaresCoefficients::source_ref_target_fill",
      Kokkos::MDRangePolicy<ExecutionSpace, Kokkos::Rank<2>>(
          space, {0, 0}, {num_targets, num_neighbors}),
      KOKKOS_LAMBDA(int const i, int const j) {
        auto src = source_points(i, j);
        auto tgt = tgt_acc::get(target_points, i);
        point_t t{};

        for (int k = 0; k < dimension; k++)
          t[k] = src[k] - tgt[k];

        source_ref_target(i, j) = t;
      });

  Kokkos::Profiling::popRegion();
  Kokkos::Profiling::pushRegion(
      "ArborX::MovingLeastSquaresCoefficients::radii_computation");

  // We then compute the radius for each target that will be used in evaluating
  // the weight for each source point.
  Kokkos::View<CoefficientsType *, MemorySpace> radii(
      Kokkos::view_alloc(space, Kokkos::WithoutInitializing,
                         "ArborX::MovingLeastSquaresCoefficients::radii"),
      num_targets);
  Kokkos::parallel_for(
      "ArborX::MovingLeastSquaresCoefficients::radii_computation",
      Kokkos::RangePolicy<ExecutionSpace>(space, 0, num_targets),
      KOKKOS_LAMBDA(int const i) {
        CoefficientsType radius = epsilon;

        for (int j = 0; j < num_neighbors; j++)
        {
          CoefficientsType norm =
              ArborX::Details::distance(source_ref_target(i, j), point_t{});
          radius = Kokkos::max(radius, norm);
        }

        // The one at the limit would be valued at 0 due to how CRBFs work
        radii(i) = 1.1 * radius;
      });

  Kokkos::Profiling::popRegion();
  Kokkos::Profiling::pushRegion(
      "ArborX::MovingLeastSquaresCoefficients::phi_computation");

  // This computes PHI given the source points as well as the radius
  Kokkos::View<CoefficientsType **, MemorySpace> phi(
      Kokkos::view_alloc(space, Kokkos::WithoutInitializing,
                         "ArborX::MovingLeastSquaresCoefficients::phi"),
      num_targets, num_neighbors);
  Kokkos::parallel_for(
      "ArborX::MovingLeastSquaresCoefficients::phi_computation",
      Kokkos::MDRangePolicy<ExecutionSpace, Kokkos::Rank<2>>(
          space, {0, 0}, {num_targets, num_neighbors}),
      KOKKOS_LAMBDA(int const i, int const j) {
        CoefficientsType norm =
            ArborX::Details::distance(source_ref_target(i, j), point_t{});
        phi(i, j) = CRBF::evaluate(norm / radii(i));
      });

  Kokkos::Profiling::popRegion();
  Kokkos::Profiling::pushRegion(
      "ArborX::MovingLeastSquaresCoefficients::vandermonde");

  // This builds the Vandermonde (P) matrix
  Kokkos::View<CoefficientsType ***, MemorySpace> p(
      Kokkos::view_alloc(space, Kokkos::WithoutInitializing,
                         "ArborX::MovingLeastSquaresCoefficients::vandermonde"),
      num_targets, num_neighbors, poly_size);
  Kokkos::parallel_for(
      "ArborX::MovingLeastSquaresCoefficients::vandermonde_computation",
      Kokkos::MDRangePolicy<ExecutionSpace, Kokkos::Rank<2>>(
          space, {0, 0}, {num_targets, num_neighbors}),
      KOKKOS_LAMBDA(int const i, int const j) {
        auto basis = evaluatePolynomialBasis<degree>(source_ref_target(i, j));
        for (int k = 0; k < poly_size; k++)
          p(i, j, k) = basis[k];
      });

  Kokkos::Profiling::popRegion();
  Kokkos::Profiling::pushRegion(
      "ArborX::MovingLeastSquaresCoefficients::moment");

  // We then create what is called the moment matrix, which is A = P^T.PHI.P. By
  // construction, A is symmetric.
  Kokkos::View<CoefficientsType ***, MemorySpace> a(
      Kokkos::view_alloc(space, Kokkos::WithoutInitializing,
                         "ArborX::MovingLeastSquaresCoefficients::moment"),
      num_targets, poly_size, poly_size);
  Kokkos::parallel_for(
      "ArborX::MovingLeastSquaresCoefficients::moment_computation",
      Kokkos::MDRangePolicy<ExecutionSpace, Kokkos::Rank<3>>(
          space, {0, 0, 0}, {num_targets, poly_size, poly_size}),
      KOKKOS_LAMBDA(int const i, int const j, int const k) {
        CoefficientsType tmp = 0;
        for (int l = 0; l < num_neighbors; l++)
          tmp += p(i, l, j) * p(i, l, k) * phi(i, l);
        a(i, j, k) = tmp;
      });

  Kokkos::Profiling::popRegion();
  Kokkos::Profiling::pushRegion(
      "ArborX::MovingLeastSquaresCoefficients::pseudo_inverse_svd");

  // We need the inverse of A = P^T.PHI.P, and because A is symmetric, we can
  // use the symmetric SVD algorithm to get it.
  symmetricPseudoInverseSVD(space, a);
  // Now, A = [P^T.PHI.P]^-1

  Kokkos::Profiling::popRegion();
  Kokkos::Profiling::pushRegion(
      "ArborX::MovingLeastSquaresCoefficients::coefficients_computation");

  // Finally, the result is produced by computing p(0).A.P^T.PHI
  Kokkos::View<CoefficientsType **, MemorySpace> coeffs(
      Kokkos::view_alloc(
          space, Kokkos::WithoutInitializing,
          "ArborX::MovingLeastSquaresCoefficients::coefficients"),
      num_targets, num_neighbors);
  Kokkos::parallel_for(
      "ArborX::MovingLeastSquaresCoefficients::coefficients_computation",
      Kokkos::MDRangePolicy<ExecutionSpace, Kokkos::Rank<2>>(
          space, {0, 0}, {num_targets, num_neighbors}),
      KOKKOS_LAMBDA(int const i, int const j) {
        CoefficientsType tmp = 0;
        for (int k = 0; k < poly_size; k++)
          tmp += a(i, 0, k) * p(i, j, k) * phi(i, j);
        coeffs(i, j) = tmp;
      });

  Kokkos::Profiling::popRegion();
  return coeffs;
}

} // namespace ArborX::Interpolation::Details

#endif