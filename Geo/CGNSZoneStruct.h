// Gmsh - Copyright (C) 1997-2019 C. Geuzaine, J.-F. Remacle
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/gmsh/gmsh/issues.

#ifndef CGNS_CGNSZONESTRUCT_H
#define CGNS_CGNSZONESTRUCT_H

#include "CGNSZone.h"

#if defined(HAVE_LIBCGNS)


template<int DIM>
class CGNSZoneStruct : public CGNSZone
{
public:
  CGNSZoneStruct(int fileIndex, int baseIndex, int zoneIndex, int meshDim,
                 cgsize_t startNode,
                 const Family2EltNodeTransfo &allEltNodeTransfo, int &err);
  
  cgsize_t nbNodeIJK(int d) const { return size_[d]; }
  cgsize_t nbEltIJK(int d) const { return size_[DIM+d]; }
  const cgsize_t *nbNodeIJK() const { return size_; }
  const cgsize_t *nbEltIJK() const { return size_ + DIM; }
  cgsize_t nbNodeInRange(const cgsize_t *range) const;
  cgsize_t nbEltInRange(const cgsize_t *range) const;

  virtual cgsize_t indexDataSize(cgsize_t nbVal) { return DIM * nbVal; }

  virtual void eltFromRange(const cgsize_t *range,
                            std::vector<cgsize_t> &elt) const;
  virtual void eltFromList(const std::vector<cgsize_t> &list,
                           std::vector<cgsize_t> &elt) const;
  virtual void nodeFromRange(const cgsize_t *range,
                             std::vector<cgsize_t> &node) const;
  virtual void nodeFromList(const std::vector<cgsize_t> &range,
                            std::vector<cgsize_t> &node) const;

  virtual int readElements(std::vector<MVertex *> &allVert,
                           std::map<int, std::vector<MElement *> > *allElt,
                           std::vector<std::string> &allBCName);

  virtual int readConnectivities(const std::map<std::string, int> &name2Zone,
                                 std::vector<CGNSZone *> &allZones);

protected:
  int readOneInterface(int iConnect,
                       const std::map<std::string, int> &name2Zone,
                       std::vector<CGNSZone *> &allZones);
  void makeBndElement(const cgsize_t *ijk, const int *dir, int order,
                      int defaultEntity, std::vector<MVertex *> &allVert,
                      std::map<int, std::vector<MElement *> > *allElt);
};


template<int DIM>
inline cgsize_t CGNSZoneStruct<DIM>::nbNodeInRange(const cgsize_t *range) const
{
  cgsize_t nb = 1;
  for(int d = 0; d < DIM; d++) {
    const cgsize_t diff = range[DIM+d] - range[d];
    nb *= (diff >= 0) ? diff + 1 : -diff + 1;
  }
  return nb;
}


template<int DIM>
inline cgsize_t CGNSZoneStruct<DIM>::nbEltInRange(const cgsize_t *range) const
{
  return nbNodeInRange(range);
}


#endif // HAVE_LIBCGNS

#endif // CGNS_CGNSZONESTRUCT_H
