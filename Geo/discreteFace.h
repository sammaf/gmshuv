// Gmsh - Copyright (C) 1997-2019 C. Geuzaine, J.-F. Remacle
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/gmsh/gmsh/issues.

#ifndef DISCRETE_FACE_H
#define DISCRETE_FACE_H

#include <algorithm>
#include "GmshConfig.h"
#include "GModel.h"
#include "GFace.h"
#include "discreteVertex.h"
#include "discreteEdge.h"
#include "MTriangle.h"
#include "MElementCut.h"
#include "MEdge.h"
#include "MLine.h"
#include "rtree.h"

class MElementOctree;

class discreteFace : public GFace {
private:
  bool _checkAndFixOrientation();
  int _currentParametrization;
  class param {
  public:
    MElementOctree *oct;
    mutable RTree<std::pair<MTriangle *, MTriangle *> *, double, 3> rtree3d;
    std::vector<MVertex> v2d;
    std::vector<MVertex> v3d;
    std::vector<MTriangle> t2d;
    std::vector<MTriangle> t3d;
    std::vector<SVector3> CURV;
    std::vector<GEdge *> bnd;
    std::vector<GEdge *> emb;
    param() : oct(NULL) {}
    ~param();
    bool checkPlanar();
  };
  std::vector<param> _parametrizations;
public:
  discreteFace(GModel *model, int num);
  virtual ~discreteFace() {}
  using GFace::point;
  GPoint point(double par1, double par2) const;
  SPoint2 parFromPoint(const SPoint3 &p, bool onSurface = true) const;
  GPoint closestPoint(const SPoint3 &queryPoint, double maxDistance,
                      SVector3 *normal = NULL) const;
  GPoint closestPoint(const SPoint3 &queryPoint,
                      const double initialGuess[2]) const;
  SVector3 normal(const SPoint2 &param) const;
  double curvatureMax(const SPoint2 &param) const;
  double curvatures(const SPoint2 &param, SVector3 &dirMax, SVector3 &dirMin,
                    double &curvMax, double &curvMin) const;
  GEntity::GeomType geomType() const { return DiscreteSurface; }
  virtual Pair<SVector3, SVector3> firstDer(const SPoint2 &param) const;
  virtual void secondDer(const SPoint2 &param, SVector3 &dudu, SVector3 &dvdv,
                         SVector3 &dudv) const;
  int createGeometry(std::map<MVertex *,
                     std::pair<SVector3, SVector3> > *curvatures);
  void createGeometryFromSTL();
  virtual bool haveParametrization()
  {
    return !_parametrizations.empty();
  }
  virtual void mesh(bool verbose);
  void setBoundEdges(const std::vector<int> &tagEdges);
  void setBoundEdges(const std::vector<int> &tagEdges,
                     const std::vector<int> &signEdges);
  int trianglePosition(double par1, double par2, double &u, double &v) const;
  GPoint intersectionWithCircle(const SVector3 &n1, const SVector3 &n2,
                                const SVector3 &p, const double &R,
                                double uv[2]);
};

#endif
