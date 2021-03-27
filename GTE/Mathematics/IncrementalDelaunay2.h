// David Eberly, Geometric Tools, Redmond WA 98052
// Copyright (c) 1998-2021
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt
// https://www.geometrictools.com/License/Boost/LICENSE_1_0.txt
// Version: 4.8.2021.03.26

#pragma once

// Incremental insertion and removal of vertices in a Delaunay triangulation.
// The triangles are counterclockwise ordered. The insertion code is that of
// template <typename T> class Delaunay2<T> in Delaunay2.h. For now, the code
// was copy-pasted here. Any changes made to this code must be made in both
// places. The duplication is
//   using Triangle = VETManifoldMesh::Triangle;
//   using DirectedEdgeKeySet = std::set<EdgeKey<true>>;
//   using TrianglePtrSet = std::set<std::shared_ptr<Triangle>>;
//   VETManifoldMesh mGraph;
//   std::array<std::array<size_t, 2>, 3> const mIndex;
//   bool GetContainingTriangle(size_t, std::shared_ptr<Triangle>&) const;
//   void GetAndRemoveInsertionPolygon(size_t,
//            TrianglePtrSet&, DirectedEdgeKeySet&);
//   void Update(size_t);
//   static ComputeRational const& Copy(InputRational const&,
//            ComputeRational&);
//   int32_t ToLine(size_t, size_t, size_t) const;
//   int32_t ToTriangle(size_t, size_t, size_t, size_t) const;
//   int32_t ToCircumcircle(size_t, size_t, size_t, size_t) const;
//
// The removal code is an implementation of the algorithm in
//     Olivier Devillers,
//     "On Deletion in Delaunay Triangulations",
//     International Journal of Computational Geometry and Applications,
//     World Scientific Publishing, 2002, 12, pp. 193-205.
//     https://hal.inria.fr/inria-00167201/document
// The weight function for the priority queue, implemented as a min-heap, is
// the negative of the function power(p,circle(q0,q1,q2)) function described
// in the paper.
// 
// The paper appears to assume that the removal point is an interior point of
// the trianglation. Just as the insertion algorithms are different for
// interior points and for boundary points, the removal algorithms are
// different for interior points and for boundary points.
// 
// The paper mentions that degeneracies (colinear points, cocircular points)
// are handled by jittering. Although one hopes that jittering prevents
// degeneracies--and perhaps probabilistically this is acceptable, the only
// guarantee for a correct result is to use exact arithmetic on the input
// points. The implementation here uses a blend of interval and rational
// arithmetic for exactness; the input points are not jittered.
//
// The details of the algorithms and implementation are provided in
// https://www.geometrictools.com/Documentation/IncrementalDelaunayTriangulation.pdf

#include <Mathematics/ArbitraryPrecision.h>
#include <Mathematics/MinHeap.h>
#include <Mathematics/SWInterval.h>
#include <Mathematics/Vector2.h>
#include <Mathematics/VETManifoldMesh.h>
#include <functional>
#include <map>
#include <set>

namespace gte
    // The input type must be 'float' or 'double'. The compute type is defined
    // internally and has enough bits of precision to handle any
    // floating-point inputs.
{
    template <typename T>
    class IncrementalDelaunay2
    {
    public:
        // Construction and destruction. A bounding rectangle for the input
        // points must be specified.
        IncrementalDelaunay2(T const& xMin, T const& yMin, T const& xMax, T const& yMax)
            :
            mXMin(xMin),
            mYMin(yMin),
            mXMax(xMax),
            mYMax(yMax),
            mCRPool(maxNumCRPool),
            mGraph{},
            mIndex{ { { 0, 1 }, { 1, 2 }, { 2, 0 } } }
        {
            LogAssert(
                mXMin < mXMax&& mYMin < mYMax,
                "Invalid bounding rectangle.");

            mToLineWrapper = [this](size_t vPrev, size_t vCurr, size_t vNext)
            {
                return ToLine(vPrev, vCurr, vNext);
            };

            // Create a supertriangle that contains the input rectangle.
            T xDelta = mXMax - mXMin;
            T yDelta = mYMax - mYMin;
            T x0 = mXMin - xDelta;
            T y0 = mYMin - yDelta;
            T x1 = mXMin + static_cast<T>(5) * xDelta;
            T y1 = y0;
            T x2 = x0;
            T y2 = mYMin + static_cast<T>(5) * yDelta;
            Vector2<T> supervertex0{ x0, y0 };
            Vector2<T> supervertex1{ x1, y1 };
            Vector2<T> supervertex2{ x2, y2 };

            // Insert the supertriangle vertices into the vertex storage.
            mVertexIndexMap.emplace(supervertex0, 0);
            mVertexIndexMap.emplace(supervertex1, 1);
            mVertexIndexMap.emplace(supervertex2, 2);
            mVertices.emplace_back(supervertex0);
            mVertices.emplace_back(supervertex1);
            mVertices.emplace_back(supervertex2);
            mIRVertices.emplace_back(IRVector{ x0, y0 });
            mIRVertices.emplace_back(IRVector{ x1, y1 });
            mIRVertices.emplace_back(IRVector{ x2, y2 });

            // Insert the supertriangle into the triangulation.
            auto inserted = mGraph.Insert(0, 1, 2);
            LogAssert(
                inserted != nullptr,
                "Failed to insert supertriangle.");
        }

        ~IncrementalDelaunay2() = default;

        // Insert a point into the triangulation. The return value is the
        // index associated with the vertex in the vertex map. The
        // supertriangle vertices are at indices 0, 1, and 2. If the input
        // point already exists, its vertex-map index is simply returned. If
        // the position is outside the domain specified in the constructor,
        // an exception is thrown.
        size_t Insert(Vector2<T> const& position)
        {
            LogAssert(
                mXMin <= position[0] && position[0] <= mXMax &&
                mYMin <= position[1] && position[1] <= mYMax,
                "The position is outside the domain specified in the constructor.");

            auto iter = mVertexIndexMap.find(position);
            if (iter != mVertexIndexMap.end())
            {
                // The vertex already exists.
                return iter->second;
            }

            // Store the position in the various pools.
            size_t posIndex = mVertices.size();
            mVertexIndexMap.emplace(position, posIndex);
            mVertices.emplace_back(position);
            mIRVertices.emplace_back(IRVector{ position[0], position[1] });

            Update(posIndex);
            return posIndex;
        }

        // Remove a point from the triangulation. The return value is the index
        // associated with the vertex in the vertex map when that vertex exists.
        // If the vertex does not exist, the return value is
        // std::numeric_limit<size_t>::max().
        size_t Remove(Vector2<T> const& position)
        {
            auto iter = mVertexIndexMap.find(position);
            if (iter == mVertexIndexMap.end())
            {
                // The position is not a vertex of the triangulation.
                return invalid;
            }
            int32_t vRemovalIndex = static_cast<int32_t>(iter->second);

            if (mVertexIndexMap.size() == 4)
            {
                // Only a single point has been inserted previously into the
                // triangulation.
                for (int32_t i0 = 2, i1 = 0; i1 < 3; i0 = i1++)
                {
                    auto removed = mGraph.Remove(vRemovalIndex, i0, i1);
                    LogAssert(
                        removed,
                        "Unexpected removal failure.");
                }

                auto inserted = mGraph.Insert(0, 1, 2);
                LogAssert(
                    inserted != nullptr,
                    "Failed to insert supertriangle.");

                mVertexIndexMap.erase(iter);
                return static_cast<size_t>(vRemovalIndex);
            }

            // Locate the position in the vertices of the graph.
            auto const& vMap = mGraph.GetVertices();
            auto vIter = vMap.find(vRemovalIndex);
            LogAssert(
                vIter != vMap.end(),
                "Expecting to find the to-be-removed vertex in the triangulation.");

            bool removalPointOnBoundary = false;
            for (auto vIndex : vIter->second->VAdjacent)
            {
                if (IsSupervertex(vIndex))
                {
                    // The triangle has a supervertex, so the removal point
                    // is on the boundary of the Delaunay triangulation.
                    removalPointOnBoundary = true;
                    break;
                }
            }

            auto const& adjacents = vIter->second->TAdjacent;
            std::vector<int32_t> polygon;
            DeleteRemovalPolygon(vRemovalIndex, adjacents, polygon);

            if (removalPointOnBoundary)
            {
                RetriangulateBoundaryRemovalPolygon(vRemovalIndex, polygon);
            }
            else
            {
                RetriangulateInteriorRemovalPolygon(vRemovalIndex, polygon);
            }

            mVertexIndexMap.erase(iter);
            return static_cast<size_t>(vRemovalIndex);
        }

        // Get the current triangulation including the supervertices and
        // triangles containing supervertices.
        void GetTriangulation(std::vector<Vector2<T>>& vertices,
            std::vector<std::array<size_t, 3>>& triangles)
        {
            vertices.resize(mVertices.size());
            std::copy(mVertices.begin(), mVertices.end(), vertices.begin());

            auto const& tMap = mGraph.GetTriangles();
            triangles.reserve(tMap.size());
            triangles.clear();
            for (auto const& tri : tMap)
            {
                auto const& tKey = tri.first;
                triangles.push_back({
                    static_cast<size_t>(tKey.V[0]),
                    static_cast<size_t>(tKey.V[1]),
                    static_cast<size_t>(tKey.V[2]) });
            }
        }

    private:
        static size_t constexpr invalid = std::numeric_limits<size_t>::max();

        // The minimum-size rational type of the input points.
        static int32_t constexpr InputNumWords = std::is_same<T, float>::value ? 2 : 4;
        using InputRational = BSNumber<UIntegerFP32<InputNumWords>>;
        using IRVector = Vector2<InputRational>;

        // The compute type used for exact sign classification.
        static int32_t constexpr ComputeNumWords = std::is_same<T, float>::value ? 36 : 264;
        using ComputeRational = BSNumber<UIntegerFP32<ComputeNumWords>>;
        using CRVector = Vector2<ComputeRational>;

        // The rectangular domain in which all input points live.
        T mXMin, mYMin, mXMax, mYMax;

        // The current vertices.
        std::map<Vector2<T>, size_t> mVertexIndexMap;
        std::vector<Vector2<T>> mVertices;
        std::vector<IRVector> mIRVertices;

        // Sufficient storage for the expression trees related to computing
        // the exact signs in ToLine(...) and ToCircumcircle(...).
        static size_t constexpr maxNumCRPool = 43;
        mutable std::vector<ComputeRational> mCRPool;

        // Support for inserting a point into the triangulation.  The graph
        // is the current triangulation. The mIndex array provides indexing
        using Triangle = VETManifoldMesh::Triangle;
        using DirectedEdgeKeySet = std::set<EdgeKey<true>>;
        using TrianglePtrSet = std::set<std::shared_ptr<Triangle>>;
        VETManifoldMesh mGraph;

        // Indexing for the vertices of the triangle adjacent to a vertex.
        // The edge adjacent to vertex j is <mIndex[j][0], mIndex[j][1]> and
        // is listed so that the triangle interior is to your left as you walk
        // around the edges.
        std::array<std::array<size_t, 2>, 3> const mIndex;

        // Wrap the ToLine function for use in retriangulating the removal
        // polygon.
        std::function<int32_t(size_t, size_t, size_t)> mToLineWrapper;


        template <typename IntegerType>
        inline bool IsDelaunayVertex(IntegerType vIndex) const
        {
            return vIndex >= 3;
        }

        template <typename IntegerType>
        inline bool IsSupervertex(IntegerType vIndex) const
        {
            return vIndex < 3;
        }

        bool GetContainingTriangle(size_t pIndex, std::shared_ptr<Triangle>& tri) const
        {
            size_t const numTriangles = mGraph.GetTriangles().size();
            for (size_t t = 0; t < numTriangles; ++t)
            {
                size_t j;
                for (j = 0; j < 3; ++j)
                {
                    size_t v0Index = static_cast<size_t>(tri->V[mIndex[j][0]]);
                    size_t v1Index = static_cast<size_t>(tri->V[mIndex[j][1]]);
                    if (ToLine(pIndex, v0Index, v1Index) > 0)
                    {
                        // Point i sees edge <v0,v1> from outside the triangle.
                        auto adjTri = tri->T[j].lock();
                        if (adjTri)
                        {
                            // Traverse to the triangle sharing the face.
                            tri = adjTri;
                            break;
                        }
                        else
                        {
                            // We reached a hull edge, so the point is outside
                            // the hull.
                            return false;
                        }
                    }

                }

                if (j == 3)
                {
                    // The point is inside all four edges, so the point is
                    // inside a triangle.
                    return true;
                }
            }

            LogError(
                "Unexpected termination of loop while searching for a triangle.");
        }

        void GetAndRemoveInsertionPolygon(size_t pIndex,
            TrianglePtrSet& candidates, DirectedEdgeKeySet& boundary)
        {
            // Locate the triangles that make up the insertion polygon.
            ETManifoldMesh polygon;
            while (candidates.size() > 0)
            {
                std::shared_ptr<Triangle> tri = *candidates.begin();
                candidates.erase(candidates.begin());

                for (size_t j = 0; j < 3; ++j)
                {
                    auto adj = tri->T[j].lock();
                    if (adj && candidates.find(adj) == candidates.end())
                    {
                        size_t v0Index = adj->V[0];
                        size_t v1Index = adj->V[1];
                        size_t v2Index = adj->V[2];
                        if (ToCircumcircle(pIndex, v0Index, v1Index, v2Index) <= 0)
                        {
                            // Point P is in the circumcircle.
                            candidates.insert(adj);
                        }
                    }
                }

                auto inserted = polygon.Insert(tri->V[0], tri->V[1], tri->V[2]);
                LogAssert(
                    inserted != nullptr,
                    "Unexpected insertion failure.");

                auto removed = mGraph.Remove(tri->V[0], tri->V[1], tri->V[2]);
                LogAssert(
                    removed,
                    "Unexpected removal failure.");
            }

            // Get the boundary edges of the insertion polygon.
            for (auto const& element : polygon.GetTriangles())
            {
                std::shared_ptr<Triangle> tri = element.second;
                for (size_t j = 0; j < 3; ++j)
                {
                    if (!tri->T[j].lock())
                    {
                        EdgeKey<true> ekey(tri->V[mIndex[j][0]], tri->V[mIndex[j][1]]);
                        boundary.insert(ekey);
                    }
                }
            }
        }

        void Update(size_t pIndex)
        {
            auto const& tmap = mGraph.GetTriangles();
            std::shared_ptr<Triangle> tri = tmap.begin()->second;
            if (GetContainingTriangle(pIndex, tri))
            {
                // The point is inside the convex hull. The insertion polygon
                // contains only triangles in the current triangulation; the
                // hull does not change.

                // Use a depth-first search for those triangles whose
                // circumcircles contain point P.
                TrianglePtrSet candidates;
                candidates.insert(tri);

                // Get the boundary of the insertion polygon C that contains
                // the triangles whose circumcircles contain point P. Polygon
                // Polygon C contains this point.
                DirectedEdgeKeySet boundary;
                GetAndRemoveInsertionPolygon(pIndex, candidates, boundary);

                // The insertion polygon consists of the triangles formed by
                // point P and the faces of C.
                for (auto const& key : boundary)
                {
                    size_t v0Index = static_cast<size_t>(key.V[0]);
                    size_t v1Index = static_cast<size_t>(key.V[1]);
                    if (ToLine(pIndex, v0Index, v1Index) < 0)
                    {
                        auto inserted = mGraph.Insert(static_cast<int32_t>(pIndex),
                            key.V[0], key.V[1]);
                        LogAssert(
                            inserted != nullptr,
                            "Unexpected insertion failure.");
                    }
                }
            }
            else
            {
                // The point is outside the convex hull. The insertion
                // polygon is formed by point P and any triangles in the
                // current triangulation whose circumcircles contain point P.

                // Locate the convex hull of the triangles.
                DirectedEdgeKeySet hull;
                for (auto const& element : tmap)
                {
                    std::shared_ptr<Triangle> t = element.second;
                    for (size_t j = 0; j < 3; ++j)
                    {
                        if (!t->T[j].lock())
                        {
                            hull.insert(EdgeKey<true>(t->V[mIndex[j][0]], t->V[mIndex[j][1]]));
                        }
                    }
                }

                // Iterate over all the hull edges and use the ones visible to
                // point P to locate the insertion polygon.
                auto const& emap = mGraph.GetEdges();
                TrianglePtrSet candidates;
                DirectedEdgeKeySet visible;
                for (auto const& key : hull)
                {
                    size_t v0Index = static_cast<size_t>(key.V[0]);
                    size_t v1Index = static_cast<size_t>(key.V[1]);
                    if (ToLine(pIndex, v0Index, v1Index) > 0)
                    {
                        auto iter = emap.find(EdgeKey<false>(key.V[0], key.V[1]));
                        if (iter != emap.end() && iter->second->T[1].lock() == nullptr)
                        {
                            auto adj = iter->second->T[0].lock();
                            if (adj && candidates.find(adj) == candidates.end())
                            {
                                size_t a0Index = static_cast<size_t>(adj->V[0]);
                                size_t a1Index = static_cast<size_t>(adj->V[1]);
                                size_t a2Index = static_cast<size_t>(adj->V[2]);
                                if (ToCircumcircle(pIndex, a0Index, a1Index, a2Index) <= 0)
                                {
                                    // Point P is in the circumcircle.
                                    candidates.insert(adj);
                                }
                                else
                                {
                                    // Point P is not in the circumcircle but
                                    // the hull edge is visible.
                                    visible.insert(key);
                                }
                            }
                        }
                        else
                        {
                            LogError(
                                "This condition should not occur for rational arithmetic.");
                        }
                    }
                }

                // Get the boundary of the insertion subpolygon C that
                // contains the triangles whose circumcircles contain point P.
                DirectedEdgeKeySet boundary;
                GetAndRemoveInsertionPolygon(pIndex, candidates, boundary);

                // The insertion polygon P consists of the triangles formed by
                // point i and the back edges of C and by the visible edges of
                // mGraph-C.
                for (auto const& key : boundary)
                {
                    size_t v0Index = static_cast<size_t>(key.V[0]);
                    size_t v1Index = static_cast<size_t>(key.V[1]);
                    if (ToLine(pIndex, v0Index, v1Index) < 0)
                    {
                        // This is a back edge of the boundary.
                        auto inserted = mGraph.Insert(static_cast<int32_t>(pIndex),
                            key.V[0], key.V[1]);
                        LogAssert(
                            inserted != nullptr,
                            "Unexpected insertion failure.");
                    }
                }
                for (auto const& key : visible)
                {
                    auto inserted = mGraph.Insert(static_cast<int32_t>(pIndex),
                        key.V[1], key.V[0]);
                    LogAssert(
                        inserted != nullptr,
                        "Unexpected insertion failure.");
                }
            }
        }

        static ComputeRational const& Copy(InputRational const& source,
            ComputeRational& target)
        {
            target.SetSign(source.GetSign());
            target.SetBiasedExponent(source.GetBiasedExponent());
            target.GetUInteger().CopyFrom(source.GetUInteger());
            return target;
        }

        // Given a line with origin V0 and direction <V0,V1> and a query
        // point P, ToLine returns
        //   +1, P on right of line
        //   -1, P on left of line
        //    0, P on the line
        int32_t ToLine(size_t pIndex, size_t v0Index, size_t v1Index) const
        {
            // The expression tree has 13 nodes consisting of 6 input
            // leaves and 7 compute nodes.

            // Use interval arithmetic to determine the sign if possible.
            Vector2<T> const& inP = mVertices[pIndex];
            Vector2<T> const& inV0 = mVertices[v0Index];
            Vector2<T> const& inV1 = mVertices[v1Index];

            auto x0 = SWInterval<T>::Sub(inP[0], inV0[0]);
            auto y0 = SWInterval<T>::Sub(inP[1], inV0[1]);
            auto x1 = SWInterval<T>::Sub(inV1[0], inV0[0]);
            auto y1 = SWInterval<T>::Sub(inV1[1], inV0[1]);
            auto x0y1 = x0 * y1;
            auto x1y0 = x1 * y0;
            auto det = x0y1 - x1y0;

            T constexpr zero = 0;
            if (det[0] > zero)
            {
                return +1;
            }
            else if (det[1] < zero)
            {
                return -1;
            }

            // The exact sign of the determinant is not known, so compute
            // the determinant using rational arithmetic.

            // Name the nodes of the expression tree.
            auto const& irP = mIRVertices[pIndex];
            Vector2<InputRational> const& irV0 = mIRVertices[v0Index];
            Vector2<InputRational> const& irV1 = mIRVertices[v1Index];

            auto const& crP0 = Copy(irP[0], mCRPool[0]);
            auto const& crP1 = Copy(irP[1], mCRPool[1]);
            auto const& crV00 = Copy(irV0[0], mCRPool[2]);
            auto const& crV01 = Copy(irV0[1], mCRPool[3]);
            auto const& crV10 = Copy(irV1[0], mCRPool[4]);
            auto const& crV11 = Copy(irV1[1], mCRPool[5]);
            auto& crX0 = mCRPool[6];
            auto& crY0 = mCRPool[7];
            auto& crX1 = mCRPool[8];
            auto& crY1 = mCRPool[9];
            auto& crX0Y1 = mCRPool[10];
            auto& crX1Y0 = mCRPool[11];
            auto& crDet = mCRPool[12];

            // Evaluate the expression tree.
            crX0 = crP0 - crV00;
            crY0 = crP1 - crV01;
            crX1 = crV10 - crV00;
            crY1 = crV11 - crV01;
            crX0Y1 = crX0 * crY1;
            crX1Y0 = crX1 * crY0;
            crDet = crX0Y1 - crX1Y0;
            return crDet.GetSign();
        }

        // For a triangle with counterclockwise vertices V0, V1 and V2, operator()
        // returns
        //   +1, P outside triangle
        //   -1, P inside triangle
        //    0, P on triangle
        int32_t ToTriangle(size_t pIndex, size_t v0Index, size_t v1Index, size_t v2Index) const
        {
            int32_t sign0 = ToLine(pIndex, v1Index, v2Index);
            if (sign0 > 0)
            {
                return +1;
            }

            int32_t sign1 = ToLine(pIndex, v0Index, v2Index);
            if (sign1 < 0)
            {
                return +1;
            }

            int32_t sign2 = ToLine(pIndex, v0Index, v1Index);
            if (sign2 > 0)
            {
                return +1;
            }

            return ((sign0 && sign1 && sign2) ? -1 : 0);
        }

        // For a triangle with counterclockwise vertices V0, V1 and V2 and a
        // query point P, ToCircumcircle returns
        //   +1, P outside circumcircle of triangle
        //   -1, P inside circumcircle of triangle
        //    0, P on circumcircle of triangle
        int32_t ToCircumcircle(size_t pIndex, size_t v0Index, size_t v1Index, size_t v2Index) const
        {
            // The expression tree has 43 nodes consisting of 8 input
            // leaves and 35 compute nodes.

            // Use interval arithmetic to determine the sign if possible.
            auto const& inP = mVertices[pIndex];
            Vector2<T> const& inV0 = mVertices[v0Index];
            Vector2<T> const& inV1 = mVertices[v1Index];
            Vector2<T> const& inV2 = mVertices[v2Index];

            auto x0 = SWInterval<T>::Sub(inV0[0], inP[0]);
            auto y0 = SWInterval<T>::Sub(inV0[1], inP[1]);
            auto s00 = SWInterval<T>::Add(inV0[0], inP[0]);
            auto s01 = SWInterval<T>::Add(inV0[1], inP[1]);
            auto x1 = SWInterval<T>::Sub(inV1[0], inP[0]);
            auto y1 = SWInterval<T>::Sub(inV1[1], inP[1]);
            auto s10 = SWInterval<T>::Add(inV1[0], inP[0]);
            auto s11 = SWInterval<T>::Add(inV1[1], inP[1]);
            auto x2 = SWInterval<T>::Sub(inV2[0], inP[0]);
            auto y2 = SWInterval<T>::Sub(inV2[1], inP[1]);
            auto s20 = SWInterval<T>::Add(inV2[0], inP[0]);
            auto s21 = SWInterval<T>::Add(inV2[1], inP[1]);
            auto t00 = s00 * x0;
            auto t01 = s01 * y0;
            auto t10 = s10 * x1;
            auto t11 = s11 * y1;
            auto t20 = s20 * x2;
            auto t21 = s21 * y2;
            auto z0 = t00 + t01;
            auto z1 = t10 + t11;
            auto z2 = t20 + t21;
            auto y0z1 = y0 * z1;
            auto y0z2 = y0 * z2;
            auto y1z0 = y1 * z0;
            auto y1z2 = y1 * z2;
            auto y2z0 = y2 * z0;
            auto y2z1 = y2 * z1;
            auto c0 = y1z2 - y2z1;
            auto c1 = y2z0 - y0z2;
            auto c2 = y0z1 - y1z0;
            auto x0c0 = x0 * c0;
            auto x1c1 = x1 * c1;
            auto x2c2 = x2 * c2;
            auto det = x0c0 + x1c1 + x2c2;

            T constexpr zero = 0;
            if (det[0] > zero)
            {
                return -1;
            }
            else if (det[1] < zero)
            {
                return +1;
            }

            // The exact sign of the determinant is not known, so compute
            // the determinant using rational arithmetic.

            // Name the nodes of the expression tree.
            auto const& irP = mIRVertices[pIndex];
            Vector2<InputRational> const& irV0 = mIRVertices[v0Index];
            Vector2<InputRational> const& irV1 = mIRVertices[v1Index];
            Vector2<InputRational> const& irV2 = mIRVertices[v2Index];

            auto const& crP0 = Copy(irP[0], mCRPool[0]);
            auto const& crP1 = Copy(irP[1], mCRPool[1]);
            auto const& crV00 = Copy(irV0[0], mCRPool[2]);
            auto const& crV01 = Copy(irV0[1], mCRPool[3]);
            auto const& crV10 = Copy(irV1[0], mCRPool[4]);
            auto const& crV11 = Copy(irV1[1], mCRPool[5]);
            auto const& crV20 = Copy(irV2[0], mCRPool[6]);
            auto const& crV21 = Copy(irV2[1], mCRPool[7]);

            auto& crX0 = mCRPool[8];
            auto& crY0 = mCRPool[9];
            auto& crS00 = mCRPool[10];
            auto& crS01 = mCRPool[11];
            auto& crT00 = mCRPool[12];
            auto& crT01 = mCRPool[13];
            auto& crZ0 = mCRPool[14];

            auto& crX1 = mCRPool[15];
            auto& crY1 = mCRPool[16];
            auto& crS10 = mCRPool[17];
            auto& crS11 = mCRPool[18];
            auto& crT10 = mCRPool[19];
            auto& crT11 = mCRPool[20];
            auto& crZ1 = mCRPool[21];

            auto& crX2 = mCRPool[22];
            auto& crY2 = mCRPool[23];
            auto& crS20 = mCRPool[24];
            auto& crS21 = mCRPool[25];
            auto& crT20 = mCRPool[26];
            auto& crT21 = mCRPool[27];
            auto& crZ2 = mCRPool[28];

            auto& crY0Z1 = mCRPool[29];
            auto& crY0Z2 = mCRPool[30];
            auto& crY1Z0 = mCRPool[31];
            auto& crY1Z2 = mCRPool[32];
            auto& crY2Z0 = mCRPool[33];
            auto& crY2Z1 = mCRPool[34];

            auto& crC0 = mCRPool[35];
            auto& crC1 = mCRPool[36];
            auto& crC2 = mCRPool[37];
            auto& crX0C0 = mCRPool[38];
            auto& crX1C1 = mCRPool[39];
            auto& crX2C2 = mCRPool[40];
            auto& crTerm = mCRPool[41];
            auto& crDet = mCRPool[42];

            // Evaluate the expression tree.
            crX0 = crV00 - crP0;
            crY0 = crV01 - crP1;
            crS00 = crV00 + crP0;
            crS01 = crV01 + crP1;
            crT00 = crS00 * crX0;
            crT01 = crS01 * crY0;
            crZ0 = crT00 + crT01;

            crX1 = crV10 - crP0;
            crY1 = crV11 - crP1;
            crS10 = crV10 + crP0;
            crS11 = crV11 + crP1;
            crT10 = crS10 * crX1;
            crT11 = crS11 * crY1;
            crZ1 = crT10 + crT11;

            crX2 = crV20 - crP0;
            crY2 = crV21 - crP1;
            crS20 = crV20 + crP0;
            crS21 = crV21 + crP1;
            crT20 = crS20 * crX2;
            crT21 = crS21 * crY2;
            crZ2 = crT20 + crT21;

            crY0Z1 = crY0 * crZ1;
            crY0Z2 = crY0 * crZ2;
            crY1Z0 = crY1 * crZ0;
            crY1Z2 = crY1 * crZ2;
            crY2Z0 = crY2 * crZ0;
            crY2Z1 = crY2 * crZ1;

            crC0 = crY1Z2 - crY2Z1;
            crC1 = crY2Z0 - crY0Z2;
            crC2 = crY0Z1 - crY1Z0;
            crX0C0 = crX0 * crC0;
            crX1C1 = crX1 * crC1;
            crX2C2 = crX2 * crC2;
            crTerm = crX0C0 + crX1C1;
            crDet = crTerm + crX2C2;
            return -crDet.GetSign();
        }

    private:
        // Support for triangulating the removal polygon.

        // Let Vc be a vertex in the removal polygon. If Vc is not an ear, its
        // weight is +infinity. If Vc is an ear, let Vp be its predecessor and
        // let Vn be its successor when traversing counterclockwise. Let P be
        // the removal point. The weight is
        //   W = -H(Vp, Vc, Vn, P) / D(Vp, Vc, Vn) = WNumer / WDenom
        // where
        //           +-                -+
        //           | Vp.x  Vc.x  Vn.x |       +-                    -+
        //   D = det | Vp.y  Vc.y  Vn.y | = det | Vc.x-Vp.x  Vn.x-Vp.x |
        //           |   1     1     1  |       | Vc.y-Vp.y  Vn.y-Vp.y |
        //           +-                -+       +-                    -+
        // and
        //           +-                             -+
        //           | Vp.x    Vc.x    Vn.x    P.x   |
        //   H = det | Vp.y    Vc.y    Vn.y    P.y   |
        //           | |Vp|^2  |Vc|^2  |Vn|^2  |P|^2 |
        //           |   1       1       1       1   |
        //           +-                             -+
        //
        //            +-                                          -+
        //            | Vc.x-Vp.x      Vn.x-Vp.x      P.x-Vp.x     |
        //     = -det | Vc.y-Vp.y      Vn.y-Vp.y      P.y-Vp.y     |
        //            | |Vc|^2-|Vp|^2  |Vn|^2-|Vp|^2  |P|^2-|Vp|^2 |
        //            +-                                          -+
        // 
        // To use BSNumber-based rationals, the weight is a ratio stored as a
        // pair (WNumer, WDenom) with WDenom > 0. The comparison of weights is
        // WN0/WD0 < WN1/WD1, implemented as WN0*WD1 < WN1*WD0. Additionally,
        // a Boolean flag isFinite is used to distinguish between a finite
        // ratio (for convex vertices) and a weight that is infinite (for
        // reflex vertices).
        class RPWeight
        {
        public:
            enum class Type
            {
                finite,
                infinite,
                unmodifiable
            };

            RPWeight(Type inType = Type::unmodifiable)
                :
                numerator(0),
                denominator(inType == Type::finite ? 1 : 0),
                type(inType)
            {
            }

            bool operator<(RPWeight const& other) const
            {
                // finite < infinite < unmodifiable

                if (type == Type::finite)
                {
                    if (other.type == Type::finite)
                    {
                        ComputeRational lhs = numerator * other.denominator;
                        ComputeRational rhs = other.numerator * denominator;
                        return lhs < rhs;
                    }
                    else // other.type is infinite or unmodifiable
                    {
                        return true;
                    }
                }
                else if (type == Type::infinite)
                {
                    if (other.type == Type::finite)
                    {
                        return false;
                    }
                    else  // other.type is infinite or unmodifiable
                    {
                        return other.type == Type::unmodifiable;
                    }
                }
                else  // type is unmodifiable
                {
                    return false;
                }
            }

            bool operator<=(RPWeight const& other) const
            {
                return !(other < *this);
            }

            // The finite weight is numerator/denominator with a nonnegative
            // numerator and a positive denominator. If the weight is
            // infinite, the numerator and denominator values are invalid.
            ComputeRational numerator, denominator;
            Type type;
        };

        class RPVertex
        {
        public:
            RPVertex()
                :
                vIndex(invalid),
                isConvex(false),
                iPrev(invalid),
                iNext(invalid),
                record(nullptr)
            {
            }

            // The index relative to IncrementalDelaunay2 mVertices[].
            size_t vIndex;

            // A vertex is either convex or reflex.
            bool isConvex;

            // Vertex indices for the polygon. These are indices relative to
            // RPPolygon mVertices[].
            size_t iPrev, iNext;

            // Support for the priority queue of ears.
            typename MinHeap<size_t, RPWeight>::Record* record;
        };

        class RPPolygon
        {
        public:
            RPPolygon(std::vector<int32_t> const& polygon,
                std::function<int32_t(size_t, size_t, size_t)> const& ToLine)
                :
                mNumActive(polygon.size()),
                mVertices(polygon.size())
            {
                // Create a circular list of the polygon vertices for dynamic
                // removal of vertices.
                size_t const numVertices = mVertices.size();
                for (size_t i = 0; i < numVertices; ++i)
                {
                    RPVertex& vertex = mVertices[i];
                    vertex.vIndex = static_cast<size_t>(polygon[i]);
                    vertex.iPrev = (i > 0 ? i - 1 : numVertices - 1);
                    vertex.iNext = (i < numVertices - 1 ? i + 1 : 0);
                }

                // Create a linear list of the polygon convex vertices and a
                // linear list of the polygon reflex vertices, both used for
                // dynamic removal of vertices.
                for (size_t i = 0; i < numVertices; ++i)
                {
                    // Determine whether the vertex is convex or reflex.
                    size_t vPrev = invalid, vCurr = invalid, vNext = invalid;
                    GetTriangle(i, vPrev, vCurr, vNext);
                    mVertices[i].isConvex = (ToLine(vPrev, vCurr, vNext) < 0);
                }
            }

            inline RPVertex const& Vertex(size_t i) const
            {
                return mVertices[i];
            }

            inline RPVertex& Vertex(size_t i)
            {
                return mVertices[i];
            }

            void GetTriangle(size_t i, size_t& vPrev, size_t& vCurr, size_t& vNext) const
            {
                RPVertex const& vertex = mVertices[i];
                vCurr = vertex.vIndex;
                vPrev = mVertices[vertex.iPrev].vIndex;
                vNext = mVertices[vertex.iNext].vIndex;
            }

            void Classify(size_t i,
                std::function<int32_t(size_t, size_t, size_t)> const& ToLine)
            {
                size_t vPrev = invalid, vCurr = invalid, vNext = invalid;
                GetTriangle(i, vPrev, vCurr, vNext);
                mVertices[i].isConvex = (ToLine(vPrev, vCurr, vNext) < 0);
            }

            inline size_t GetNumActive() const
            {
                return mNumActive;
            }

            size_t GetActive() const
            {
                for (size_t i = 0; i < mVertices.size(); ++i)
                {
                    if (mVertices[i].iPrev != invalid)
                    {
                        return i;
                    }
                }

                LogError(
                    "Expecting to find an active vertex.");
            }

            inline void Remove(size_t i)
            {
                // Remove the vertex from the polygon.
                RPVertex& vertex = mVertices[i];
                size_t iPrev = vertex.iPrev;
                size_t iNext = vertex.iNext;
                mVertices[iPrev].iNext = iNext;
                mVertices[iNext].iPrev = iPrev;

                vertex.vIndex = invalid;
                vertex.isConvex = false;
                vertex.iPrev = invalid;
                vertex.iNext = invalid;
                vertex.record = nullptr;

                --mNumActive;
            }

        private:
            size_t mNumActive;
            std::vector<RPVertex> mVertices;
        };

        RPWeight ComputeWeight(size_t iConvexIndex, int32_t vRemovalIndex,
            RPPolygon& rpPolygon)
        {
            // Get the triangle <VP,VC,VN> with convex vertex VC.
            size_t vPrev = invalid, vCurr = invalid, vNext = invalid;
            rpPolygon.GetTriangle(iConvexIndex, vPrev, vCurr, vNext);

            auto const& irVP = mIRVertices[vPrev];
            auto const& irVC = mIRVertices[vCurr];
            auto const& irVN = mIRVertices[vNext];
            auto const& irPR = mIRVertices[vRemovalIndex];

            CRVector VP, VC, VN, PR;
            Copy(irVP[0], VP[0]);
            Copy(irVP[1], VP[1]);
            Copy(irVC[0], VC[0]);
            Copy(irVC[1], VC[1]);
            Copy(irVN[0], VN[0]);
            Copy(irVN[1], VN[1]);
            Copy(irPR[0], PR[0]);
            Copy(irPR[1], PR[1]);

            auto subVCVP = VC - VP;
            auto subVNVP = VN - VP;
            auto subPRVP = PR - VP;
            auto addVCVP = VC + VP;
            auto addVNVP = VN + VP;
            auto addPRVP = PR + VP;
            auto c20 = DotPerp(subVNVP, subPRVP);
            auto c21 = DotPerp(subPRVP, subVCVP);
            auto c22 = DotPerp(subVCVP, subVNVP);
            auto a20 = Dot(subVCVP, addVCVP);
            auto a21 = Dot(subVNVP, addVNVP);
            auto a22 = Dot(subPRVP, addPRVP);

            RPWeight weight(RPWeight::Type::finite);
            weight.numerator = a20 * c20 + a21 * c21 + a22 * c22;
            weight.numerator.SetSign(-weight.numerator.GetSign());
            weight.denominator = std::move(c22);
            if (weight.denominator.GetSign() < 0)
            {
                weight.numerator.SetSign(-weight.numerator.GetSign());
                weight.denominator.SetSign(1);
            }
            return weight;
        }

        void DoEarClipping(MinHeap<size_t, RPWeight>& earHeap,
            std::function<RPWeight(size_t)> const& WeightFunction,
            RPPolygon& rpPolygon)
        {
            // Remove the finite-weight vertices from the priority queue,
            // one at a time.
            size_t i;
            RPWeight weight{};
            while (earHeap.GetNumElements() >= 3)
            {
                // Get the ear of minimum weight. The vertex at index i must
                // be convex.
                (void)earHeap.GetMinimum(i, weight);
                if (weight.type != RPWeight::Type::finite)
                {
                    break;
                }
                earHeap.Remove(i, weight);

                // Get the triangle associated with the ear.
                size_t vPrev = invalid, vCurr = invalid, vNext = invalid;
                rpPolygon.GetTriangle(i, vPrev, vCurr, vNext);

                // Insert the triangle into the graph.
                auto inserted = mGraph.Insert(
                    static_cast<int32_t>(vPrev),
                    static_cast<int32_t>(vCurr),
                    static_cast<int32_t>(vNext));
                LogAssert(
                    inserted != nullptr,
                    "Unexpected insertion failure.");
                if (earHeap.GetNumElements() < 3)
                {
                    earHeap.Reset(0);
                    break;
                }

                // Remove the vertex from the polygon. The previous and next
                // neighbor indices are required to update the adjacent
                // vertices after the removal.
                RPVertex const& vertex = rpPolygon.Vertex(i);
                size_t iPrev = vertex.iPrev;
                size_t iNext = vertex.iNext;
                rpPolygon.Remove(i);

                // Removal of the ear can cause an adjacent vertex to become
                // an ear or to stop being an ear.
                bool wasConvex, nowConvex;

                RPVertex& vertexP = rpPolygon.Vertex(iPrev);
                wasConvex = vertexP.isConvex;
                rpPolygon.Classify(iPrev, mToLineWrapper);
                nowConvex = vertexP.isConvex;
                if (wasConvex)
                {
                    // The 'vertex' is convex. If 'vertexP' was convex, it
                    // cannot become reflex after the ear is clipped.
                    LogAssert(
                        nowConvex,
                        "Unexpected condition.");

                    if (vertexP.record->value.type != RPWeight::Type::unmodifiable)
                    {
                        weight = WeightFunction(iPrev);
                        earHeap.Update(vertexP.record, weight);
                    }
                }
                else // 'vertexP' was reflex
                {
                    if (nowConvex)
                    {
                        if (vertexP.record->value.type != RPWeight::Type::unmodifiable)
                        {
                            weight = WeightFunction(iPrev);
                            earHeap.Update(vertexP.record, weight);
                        }
                    }
                }

                RPVertex& vertexN = rpPolygon.Vertex(iNext);
                wasConvex = vertexN.isConvex;
                rpPolygon.Classify(iNext, mToLineWrapper);
                nowConvex = vertexN.isConvex;
                if (wasConvex)
                {
                    // The 'vertex' is convex. If 'vertexN' was convex, it
                    // cannot become reflex after the ear is clipped.
                    LogAssert(
                        nowConvex,
                        "Unexpected condition.");

                    if (vertexN.record->value.type != RPWeight::Type::unmodifiable)
                    {
                        weight = WeightFunction(iNext);
                        earHeap.Update(vertexN.record, weight);
                    }
                }
                else // 'vertexN' was reflex
                {
                    if (nowConvex)
                    {
                        if (vertexN.record->value.type != RPWeight::Type::unmodifiable)
                        {
                            weight = WeightFunction(iNext);
                            earHeap.Update(vertexN.record, weight);
                        }
                    }
                }
            }
        }

        void DeleteRemovalPolygon(int32_t vRemovalIndex,
            std::unordered_set<std::shared_ptr<Triangle>> const& adjacents,
            std::vector<int32_t>& polygon)
        {
            // Get the edges of the removal polygon. The polygon is star
            // shaped relative to the removal position.
            std::map<int32_t, int32_t> edges;
            for (auto const& adj : adjacents)
            {
                size_t i;
                for (i = 0; i < 3; ++i)
                {
                    if (vRemovalIndex == adj->V[i])
                    {
                        break;
                    }
                }
                LogAssert(
                    i < 3,
                    "Unexpected condition.");

                int32_t opposite1 = adj->V[(i + 1) % 3];
                int32_t opposite2 = adj->V[(i + 2) % 3];
                edges.insert(std::make_pair(opposite1, opposite2));
            }

            // Remove the triangles.
            for (auto const& edge : edges)
            {
                bool removed = mGraph.Remove(vRemovalIndex, edge.first, edge.second);
                LogAssert(
                    removed,
                    "Unexpected removal failure.");
            }

            // Create the removal polygon; its vertices are counterclockwise
            // ordered.
            polygon.reserve(edges.size());
            polygon.clear();
            int32_t vStart = edges.begin()->first;
            int32_t vCurr = edges.begin()->second;
            polygon.push_back(vStart);
            while (vCurr != vStart)
            {
                polygon.push_back(vCurr);
                auto eIter = edges.find(vCurr);
                LogAssert(
                    eIter != edges.end(),
                    "Unexpected condition.");

                vCurr = eIter->second;
            }
        }

        void RetriangulateInteriorRemovalPolygon(int32_t vRemovalIndex,
            std::vector<int32_t> const& polygon)
        {
            // Create a representation of 'polygon' that can be processed
            // using a priority queue.
            RPPolygon rpPolygon(polygon, mToLineWrapper);

            auto WeightFunction = [this, &rpPolygon, vRemovalIndex](size_t i)
            {
                return ComputeWeight(i, vRemovalIndex, rpPolygon);
            };

            // Create a priority queue of vertices. Convex vertices have a
            // finite and positive weight. Reflex vertices have a weight of
            // +infinity.
            MinHeap<size_t, RPWeight> earHeap(static_cast<int32_t>(polygon.size()));
            RPWeight const posInfinity(RPWeight::Type::infinite);
            RPWeight weight{};
            for (size_t i = 0; i < polygon.size(); ++i)
            {
                RPVertex& vertex = rpPolygon.Vertex(i);
                if (vertex.isConvex)
                {
                    weight = WeightFunction(i);
                }
                else
                {
                    weight = posInfinity;
                }
                vertex.record = earHeap.Insert(i, weight);
            }

            // Remove the finite-weight vertices from the priority queue,
            // one at a time.
            DoEarClipping(earHeap, WeightFunction, rpPolygon);
            LogAssert(
                earHeap.GetNumElements() == 0,
                "Expecting the hole to be completely filled.");
        }

        void RetriangulateBoundaryRemovalPolygon(int32_t vRemovalIndex,
            std::vector<int32_t> const& polygon)
        {
            size_t const numPolygon = polygon.size();
            if (numPolygon >= 3)
            {
                // Create a representation of 'polygon' that can be processed
                // using a priority queue.
                RPPolygon rpPolygon(polygon, mToLineWrapper);

                auto WeightFunction = [this, &rpPolygon, vRemovalIndex](size_t i)
                {
                    return ComputeWeight(i, vRemovalIndex, rpPolygon);
                };

                auto ZeroWeightFunction = [](size_t)
                {
                    return RPWeight(RPWeight::Type::finite);
                };

                // Create a priority queue of vertices. The removal index
                // (polygon[0] = vRemovalIndex) and its two vertex neighbors
                // have a weight of +infinity. Of the other vertices, convex
                // vertices have a finite and positive weight and reflex
                // vertices have a weight of +infinity.
                MinHeap<size_t, RPWeight> earHeap(static_cast<int32_t>(polygon.size()));
                RPWeight const rigid(RPWeight::Type::unmodifiable);
                RPWeight const posInfinity(RPWeight::Type::infinite);

                size_t iPrev = numPolygon - 2, iCurr = iPrev + 1, iNext = 0;
                for (; iNext < numPolygon; iPrev = iCurr, iCurr = iNext, ++iNext)
                {
                    RPVertex& vertexPrev = rpPolygon.Vertex(iPrev);
                    RPVertex& vertexCurr = rpPolygon.Vertex(iCurr);
                    RPVertex& vertexNext = rpPolygon.Vertex(iNext);
                    if (IsSupervertex(vertexPrev.vIndex) ||
                        IsSupervertex(vertexCurr.vIndex) ||
                        IsSupervertex(vertexNext.vIndex))
                    {
                        vertexCurr.record = earHeap.Insert(iCurr, rigid);
                    }
                    else
                    {
                        if (vertexCurr.isConvex)
                        {
                            vertexCurr.record = earHeap.Insert(iCurr,
                                WeightFunction(iCurr));
                        }
                        else
                        {
                            vertexCurr.record = earHeap.Insert(iCurr, posInfinity);
                        }
                    }
                }

                // Remove the finite-weight vertices from the priority queue,
                // one at a time. This process fills in the subpolygon of the
                // removal polygon that is contained by the Delaunay
                // triangulation.
                DoEarClipping(earHeap, WeightFunction, rpPolygon);

                // Get the subpolygon of the removal polygon that is external
                // to the Delaunay triangulation.
                size_t numExternal = rpPolygon.GetNumActive();
                std::vector<size_t> external(numExternal);
                iCurr = rpPolygon.GetActive();
                for (size_t i = 0; i < numExternal; ++i)
                {
                    external[i] = iCurr;
                    rpPolygon.Classify(iCurr, mToLineWrapper);
                    iCurr = rpPolygon.Vertex(iCurr).iNext;
                }

                earHeap.Reset(static_cast<int32_t>(numExternal));
                for (size_t i = 0; i < numExternal; ++i)
                {
                    size_t index = external[i];
                    RPVertex& vertex = rpPolygon.Vertex(index);
                    if (IsSupervertex(vertex.vIndex))
                    {
                        vertex.record = earHeap.Insert(index, rigid);
                    }
                    else
                    {
                        if (vertex.isConvex)
                        {
                            vertex.record = earHeap.Insert(index,
                                ZeroWeightFunction(index));
                        }
                        else
                        {
                            vertex.record = earHeap.Insert(index, posInfinity);
                        }
                    }
                }

                // Remove the finite-weight vertices from the priority queue,
                // one at a time. This process fills in a portion or all of
                // the subpolygon of the removal polygon that is external to
                // the Delaunay triangulation.
                DoEarClipping(earHeap, ZeroWeightFunction, rpPolygon);
                if (earHeap.GetNumElements() == 0)
                {
                    // The external polygon contained only 1 supervertex.
                    return;
                }

                // The remaining external polygon is a triangle fan with
                // 2 or 3 supervertices.
                numExternal = rpPolygon.GetNumActive();
                external.resize(numExternal);
                iCurr = rpPolygon.GetActive();
                for (size_t i = 0; i < numExternal; ++i)
                {
                    external[i] = iCurr;
                    rpPolygon.Classify(iCurr, mToLineWrapper);
                    iCurr = rpPolygon.Vertex(iCurr).iNext;
                }

                earHeap.Reset(static_cast<int32_t>(numExternal));
                iPrev = numExternal - 2;
                iCurr = iPrev + 1;
                iNext = 0;
                for (; iNext < numExternal; iPrev = iCurr, iCurr = iNext, ++iNext)
                {
                    size_t index = external[iCurr];
                    RPVertex& vertexPrev = rpPolygon.Vertex(external[iPrev]);
                    RPVertex& vertexCurr = rpPolygon.Vertex(index);
                    RPVertex& vertexNext = rpPolygon.Vertex(external[iNext]);
                    if (IsSupervertex(vertexCurr.vIndex))
                    {
                        if (IsDelaunayVertex(vertexPrev.vIndex) ||
                            IsDelaunayVertex(vertexNext.vIndex))
                        {
                            LogAssert(
                                vertexCurr.isConvex,
                                "Unexpected condition.");

                            vertexCurr.record = earHeap.Insert(index,
                                ZeroWeightFunction(index));
                        }
                        else
                        {
                            vertexCurr.record = earHeap.Insert(index, rigid);
                        }
                    }
                    else
                    {
                        vertexCurr.record = earHeap.Insert(index, posInfinity);
                    }
                }

                // Remove the finite-weight vertices from the priority queue,
                // one at a time. This process fills in the triangle fan of
                // the subpolygon of the removal polygon that is external to
                // the Delaunay triangulation.
                DoEarClipping(earHeap, ZeroWeightFunction, rpPolygon);
                LogAssert(
                    earHeap.GetNumElements() == 0,
                    "Expecting the hole to be completely filled.");
            }
            else // numPolygon == 2
            {
                int32_t vOtherIndex;
                if (polygon[0] == vRemovalIndex)
                {
                    vOtherIndex = polygon[1];
                }
                else
                {
                    vOtherIndex = polygon[0];
                }

                mGraph.Clear();
                for (int32_t i0 = 2, i1 = 0; i1 < 3; i0 = i1++)
                {
                    auto inserted = mGraph.Insert(vOtherIndex, i0, i1);
                    LogAssert(
                        inserted != nullptr,
                        "Unexpected insertion failure.");
                }
            }
        }
    };
}
