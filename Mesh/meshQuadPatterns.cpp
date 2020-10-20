// Gmsh - Copyright (C) 1997-2020 C. Geuzaine, J.-F. Remacle
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/gmsh/gmsh/issues.

#include <map>
#include <iostream>
#include "meshQuadQuasiStructured.h"
#include "meshGFace.h"
#include "GmshMessage.h"
#include "GFace.h"
#include "GModel.h"
#include "MVertex.h"
#include "MTriangle.h"
#include "MQuadrangle.h"
#include "MLine.h"
#include "GmshConfig.h"
#include "Context.h"
#include "Options.h"
#include "fastScaledCrossField.h"

#include "meshRefine.h"
#include "Generator.h"
#include "PView.h"
#include "PViewOptions.h"
#include "Field.h"
#include "geolog.h"
#include "meshWinslow2d.h"
#include "gmsh.h"
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include "qmt_utils.hpp" // For debug printing
#include "row_echelon_integer.hpp"
#include "meshQuadData.hpp"
#include "StringUtils.h"


#if defined(_OPENMP)
#include <omp.h>
#endif

using std::vector;
using std::array;

namespace QuadPatternMatching {
  using id = uint32_t;
  using id2 = std::array<id,2>;
  using id4 = std::array<id,4>;


  /* Quad meshes of patterns, known at compile time
   * These patterns must be CONVEX
   * WARNING: orientation of quads must be coherent ! */
  const std::vector<std::vector<id4> > quad_meshes = {
    /* regular quad patch */
    {{0,1,2,3}},

    /* triangular patch with one val3 singularity */
    {{0,1,6,5},{1,2,3,6},{3,4,5,6}}, 

    /* pentagonal patch with one val5 singularity */
    {{0, 1, 2, 3}, {0, 5, 4, 1}, {0, 7, 6, 5}, {0, 9, 8, 7}, {0, 3, 10, 9}},

    /* quad patch with one val3 and one val5 singularities (on diagonal) */
    {{0, 1, 2, 3}, {0, 5, 4, 1}, {0, 7, 6, 5}, {0, 9, 8, 7}, {0, 3, 10, 9}, {8, 9, 10, 11}},

    /* quad patch with one val3, one val5 singularities and two regular inside (3-5 pair for size transition) */
    {{0, 1, 2, 3}, {0, 5, 4, 1}, {0, 7, 6, 5}, {0, 9, 8, 7}, {0, 3, 10, 9}, {9, 10, 12, 11}, {3, 13, 12, 10}, {8, 9, 11, 14}, {2, 15, 13, 3}},

    /* quad patch with two val3, two val5 inside, size transition by having a chord making a U-turn */
    {{0, 1, 2, 3}, {0, 5, 4, 1}, {0, 7, 6, 5}, {0, 9, 8, 7}, {0, 3, 10, 9}, {9, 10, 12, 11}, {9, 11, 14, 13}, {8, 9, 13, 15}, {6, 7, 8, 15}},

    /* patch with two corners, two val3 singularities inside (good for eye shape) */
    {{0, 1, 2, 3}, {0, 5, 4, 1}, {0, 3, 6, 5}, {4, 5, 6, 7}},

  };


  constexpr id NO_ID = (id) -1;
  inline id2 sorted(id v1, id v2) { if (v1 < v2) { return {v1,v2}; } else { return {v2,v1}; } }
  inline id2 sorted(id2 e) { if (e[0] < e[1]) { return {e[0],e[1]}; } else { return {e[1],e[0]}; } }
  struct id2Hash {
    size_t operator()(id2 p) const noexcept {
      return size_t(p[0]) << 32 | p[1];
    }
  };

  struct vidHash {
    size_t operator()(const std::vector<id>& p) const noexcept {
      uint32_t hash = 0;
      for (size_t i = 0; i < p.size(); ++i) {
        hash += p[i];
        hash += hash << 10;
        hash ^= hash >> 6;
      }
      hash += hash << 3;
      hash ^= hash >> 11;
      hash += hash << 15;
      return hash;
    }
  };


  using Quadrangulation = std::vector< std::array<id,4> >;
  /* Global variable, filled by load_disk_quadrangulations() with the data 
   * in Mesh/meshQuadData.hpp */
  std::vector< std::vector<Quadrangulation> > B_disk_quadrangulations;
  /* Hash mapping from boundary valence loop (BVL) to disk_quadrangulations
   * Useful for fast queries */
  std::vector< std::unordered_map< std::vector<id>, std::vector<id>, vidHash > > B_BVL_ids;

  /* Keep track of pattern usage, just for statistics */
  std::unordered_map<id2,id,id2Hash> usage_count;

  template<class T> 
    void sort_unique(std::vector<T>& vec) {
      std::sort( vec.begin(), vec.end() );
      vec.erase( std::unique( vec.begin(), vec.end() ), vec.end() );
    }

  bool build_quad_chord(const vector<id4>& quadEdges, const vector<vector<id> >& e2f, id eStart, std::vector<id>& chordEdges) {
    chordEdges.size();

    vector<bool> visited(e2f.size(),false);

    /* Init */
    visited[eStart] = true;
    std::queue<id> qq;
    qq.push(eStart);

    /* Propagation */
    while (qq.size() > 0) {
      id e = qq.front();
      qq.pop();
      chordEdges.push_back(e);

      for (size_t lf = 0; lf < e2f[e].size(); ++lf) {
        id f = e2f[e][lf];
        id oe = NO_ID;
        for (size_t le = 0; le < 4; ++le) {
          if (quadEdges[f][le] == e) {
            oe = quadEdges[f][(le+2)%4];
            break;
          }
        }
        if (oe == NO_ID) return false;
        if (visited[oe]) continue;
        visited[oe] = true;
        qq.push(oe);
      }
    }

    sort_unique(chordEdges);

    return true;
  }

  struct QuadMesh {
    id n = 0;
    vector<id2> edges;
    vector<id4> quads; /* contains edge id's, not vertex ! */
    vector<id4> qvertices;
    vector<bool> vOnBdr;
    vector<id> eChordId;
    vector<vector<id> > v2e;
    vector<vector<id> > e2f;
    vector<vector<id> > chords;
    vector<vector<id> > sides;

    bool load(const std::vector<id4>& quadVertices) {
      edges.reserve(2*quadVertices.size());
      quads.reserve(quadVertices.size());
      std::unordered_map<id2,id,id2Hash> vpair2e;
      n = 0;
      for (size_t f = 0; f < quadVertices.size(); ++f) {
        id4 quad;
        for (size_t le = 0; le < 4; ++le) {
          id v1 = quadVertices[f][le];
          id v2 = quadVertices[f][(le+1)%4];
          n = std::max(n,v1+1);
          id2 oedge = {v1,v2};
          id2 vpair = sorted(v1,v2);
          auto it = vpair2e.find(vpair);
          id e = 0; 
          if (it == vpair2e.end()) {
            e = edges.size();
            edges.push_back(oedge);
            vpair2e[vpair] = e;
          } else {
            e = it->second;
          }
          quad[le] = e;
        }
        quads.push_back(quad);
        qvertices.push_back(quadVertices[f]);
      }
      v2e.resize(n);
      for (size_t e = 0; e < edges.size(); ++e) {
        v2e[edges[e][0]].push_back(e);
        v2e[edges[e][1]].push_back(e);
      }
      e2f.resize(edges.size());
      for (size_t f = 0; f < quads.size(); ++f) {
        for (size_t le = 0; le < 4; ++le) {
          e2f[quads[f][le]].push_back(f);
        }
      }
      vOnBdr.resize(n,false);
      for (size_t e = 0; e < e2f.size(); ++e) if (e2f[e].size() == 1) {
        vOnBdr[edges[e][0]] = true;
        vOnBdr[edges[e][1]] = true;
      }

      { /* Build chords of the quad mesh */
        chords.clear();
        eChordId.resize(edges.size(),NO_ID);
        vector<id> chordEdges;
        for (size_t e = 0; e < edges.size(); ++e) if (eChordId[e] == NO_ID) {
          chordEdges.clear();
          bool ok = build_quad_chord(quads, e2f, e, chordEdges);
          if (!ok) return false;
          for (id e: chordEdges) {
            eChordId[e] = chords.size();
          }
          chords.push_back(chordEdges);
        }
      }

      { /* Build the boundary sides (seperated by convex corners) */
        sides.clear();
        vector<id>* cur_side = NULL;
        for (id v0 = 0; v0 < n; ++v0) if (v2e[v0].size() == 2) {
          id v = v0;
          id e = NO_ID;
          do {
            if (v2e[v].size() == 2) { /* concave corner */
              sides.resize(sides.size()+1);
              cur_side = &sides.back();
            }
            for (id ee: v2e[v]) if (ee != e && e2f[ee].size() == 1) {
              if (edges[ee][0] == v) { /* assume edges are ordered on bdr */
                e = ee; break;
              }
            }
            if (e == NO_ID) {
              Msg::Error("load, edge not found !");
              return false;
            }
            cur_side->push_back(e);
            id v2 = (edges[e][0] != v) ? edges[e][0] : edges[e][1];
            v = v2;
          } while (v != v0);
          break;
        }
      }

      return true;
    }
  };


  /* Patterns are initialized at runtime, by the call to load_patterns() */
  std::vector<QuadMesh> patterns;

  bool load_patterns() {
#if defined(_OPENMP)
#pragma omp critical
#endif
    {
      if (patterns.size() != 0) patterns.clear();
      Msg::Info("loading %li quad patterns", quad_meshes.size());
      patterns.resize(quad_meshes.size());
      for (size_t i = 0; i < quad_meshes.size(); ++i) {
        bool ok = patterns[i].load(quad_meshes[i]);
        if (!ok) {
          Msg::Error("mesh quad patterns, failed to init pattern no %i", i);
        }
      }
    }
    return true;
  }

  double sum_sqrt(const vector<int>& values) {
    double s = 0.;
    for (const auto& v: values) s += sqrt(v);
    return s;
  }

  bool all_strictly_positive(const vector<int>& values) {
    for (const auto& v: values) if (v <= 0) return false;
    return true;
  }

  /* Struct to interface with row_echelon_integer.hpp */
  struct IMatrix {
    /* Data */
    int m;
    int n;
    std::vector<int> a;

    /* Methods */
    IMatrix(int m_, int n_): m(m_), n(n_) {
      a.resize(m*n,0);
    }

    void set(int i, int j, int value) { a[i+j*m] = value; }
    int get(int i, int j) const {return a[i+j*m]; }

    int tansform_to_row_reduced_echelon() {
      return i4mat_ref(m,n,a.data());
    }

    void print(const std::string& title = "IMatrix") {
      i4mat_print(m, n, a.data(), title);
    }

    bool get_positive_solution(std::vector<int>& x) {
      x.clear();
      x.resize(n-1,0);
      vector<array<int,2> > indices_weight;
      indices_weight.reserve(n);
      for (int i = m-1; i >= 0; --i) {
        indices_weight.clear();
        int sum = 0;
        int total = -1 * get(i,n-1);
        // DBG(i,total);
        for (int j = 0; j < n-1; ++j) {
          int v = get(i,j);
          // DBG(v,x[j]);
          if (v != 0) {
            if (x[j] == 0) {
              indices_weight.push_back({j,v});
              sum += v;
            } else {
              total -= v * x[j];
            }
          }
        }
        if (indices_weight.size() == 0) {
          if (total == 0) {
            continue;
          } else {
            return false;
          }
        }
        if (indices_weight.size() == 0) continue;
        // DBG(sum,indices,total);
        for (size_t k = 0; k < indices_weight.size(); ++k) {
          int j = indices_weight[k][0];
          int w = indices_weight[k][1];
          if (k == indices_weight.size() - 1) {
            if (x[j] % w == 0) {
              x[j] = total / w;
            } else {
              Msg::Error("w not a multiple of total ... wrong approach");
              return false;
            }
            // DBG(j,"<---", x[j]);
            if (x[j] <= 0) return false;
          } else {
            int value = int(double(w) / double(sum) * double(total));
            if (value > total) {
              Msg::Error("get_positive_solution | bad ...");
              return false;
            }
            // DBG(j,"<---", value);
            if (value <= 0) return false;
            x[j] = value;
            total -= w * value;
          }
        }
      }

      /* Verify solution */
      for (int i = 0; i < m; ++i) {
        int sum = 0;
        for (int j = 0; j < n-1; ++j) {
          sum += x[j] * get(i,j);
        }
        sum += get(i,n-1);
        if (sum != 0) {
          Msg::Error("! solution is not solution ! i=%i, A_i x=%i", i, sum);
          print("bad matrix");
          DBG("bad slt", x);
          return false;
        }
      }

      return true;
    }

    double solution_score(const std::vector<int>& x) {
      if (x.size() != (size_t) n-1) return 0.;
      for (int j = 0; j < n-1; ++j) {
        if (x[j] == 0) return 0.;
      }

      /* Verify x is solution */
      for (int i = 0; i < m; ++i) {
        int sum = 0;
        for (int j = 0; j < n-1; ++j) {
          sum += x[j] * get(i,j);
        }
        sum += get(i,n-1);
        if (sum != 0) { /* Not a solution */
          return 0.;
        }
      }

      return sum_sqrt(x);
    }

    double get_positive_solution_DFS(std::vector<int>& x, int& count, int count_limit) {
      if (x.size() == 0) {
        x.resize(n-1,0);
      }

      if (all_strictly_positive(x)) count += 1;
      if (count > count_limit) return 0.;

      /* Stop condition: x is solution and has positive score */
      double current_score = solution_score(x);
      if (current_score > 0.) return current_score;


      double best_score = 0.;
      vector<int> best_x;
      std::vector<int> undetermined;
      /* Loop from last line to first line */
      for (int i = m-1; i >= 0; --i) { 
        undetermined.clear();
        /* Check line */
        int total = -1 * get(i,n-1);
        for (int j = 0; j < n-1; ++j) {
          int w = get(i,j);
          if (w != 0) {
            if (x[j] == 0) {
              undetermined.push_back(j);
            } else {
              total -= w * x[j];
            }
          }
        }
        if (undetermined.size() == 0) {
          if (total == 0) {
            continue;
          } else {
            return 0.;
          }
        }

        /* Fix one value and make recursive call */
        double sum = 0.;
        for (size_t k = 0; k < undetermined.size(); ++k) {
          int j = undetermined[k];
          int w = get(i,j);
          sum += std::abs(w);
        }
        for (size_t k = 0; k < undetermined.size(); ++k) {
          int j = undetermined[k];
          int w = get(i,j);
          double ideal_repartition = 1./sum * double(total);
          int xmin = 1;
          int xmax = int(double(total) / double(w));
          if (xmax < 1) return 0.;

          vector<std::pair<double,int> > prio_candidate;
          for (int candidate = xmin; candidate < xmax+1; ++candidate) {
            double dist = std::pow(ideal_repartition-candidate,2);
            prio_candidate.push_back({dist,candidate});
          }
          std::sort(prio_candidate.begin(),prio_candidate.end());
          vector<int> x2;
          for (size_t l = 0; l < prio_candidate.size(); ++l) {
            int candidate = prio_candidate[k].second;
            x2 = x;
            x2[j] = candidate;
            double sub_score = get_positive_solution_DFS(x2, count, count_limit);
            if (sub_score > 0.) { /* Found a solution ! Return this one */
              x = x2;
              return sub_score;
            }
            if (sub_score > best_score) {
              best_score = sub_score;
              best_x = x2;
            }
          }
          for (int candidate = xmin; candidate < xmax+1; ++candidate) {
            // double candidate_sum_sqrt = cur_sum_sqrt + std::sqrt(candidate);
            // if (candidate_sum_sqrt <= score_filter) continue;
          }
        }
      }
      if (best_score > 0.) {
        x = best_x;
        return best_score;
      }
      return 0.;
    }

    double get_positive_solution_recursive(std::vector<int>& x, double& best_score) {
      if (x.size() == 0) {
        x.resize(n-1,0);
        best_score = 0.;
      }
      static int count = 0;
      count += 1;

      /* Stop condition: x is solution and has positive score */
      double current_score = solution_score(x);
      DBG("   ", count, current_score, x);
      if (current_score > best_score) {
        best_score = current_score;
        DBG("-> stop recursion, unroll", current_score, "->", best_score);
        return current_score;
      }

      std::vector<int> best_x;
      double best_score_b = best_score;
      std::vector<int> undetermined;
      /* Loop from last line to first line */
      for (int i = m-1; i >= 0; --i) { 
        undetermined.clear();
        /* Check line */
        int total = -1 * get(i,n-1);
        for (int j = 0; j < n-1; ++j) {
          int w = get(i,j);
          if (w != 0) {
            if (x[j] == 0) {
              undetermined.push_back(j);
            } else {
              total -= w * x[j];
            }
          }
        }
        if (undetermined.size() == 0) {
          if (total == 0) {
            continue;
          } else {
            return 0.;
          }
        }

        /* Fix one value and make recursive call */
        for (size_t k = 0; k < undetermined.size(); ++k) {
          int j = undetermined[k];
          int w = get(i,j);
          int xmin = 1;
          int xmax = int(double(total) / double(w));
          if (xmax < 1) return 0.;
          for (int candidate = xmin; candidate < xmax+1; ++candidate) {
            std::vector<int> x2 = x;
            x2[j] = candidate;
            double score = get_positive_solution_recursive(x2, best_score);
            if (score == best_score && score > best_score_b) {
              best_x = x2;
              best_score_b = score;
            }
          }
        }
      }
      if (best_x.size() > 0) {
        x = best_x;
      }
      return best_score_b;
    }

  };


  std::vector<MVertex*> createVertices (GFace* gf, MVertex *v1, MVertex *v2, int n) {
    std::vector<MVertex*> r;
    r.push_back(v1);
    for (int i=1;i<n;i++){
      double xi = (double)i/n;
      SPoint3 p ((1.-xi)*v1->x()+xi*v2->x(),(1.-xi)*v1->y()+xi*v2->y(),(1.-xi)*v1->z()+xi*v2->z());
      double uv[2] = {0.,0.};
      MVertex *vNew = new MFaceVertex(p.x(),p.y(),p.z(),gf,uv[0],uv[1]);
      gf->mesh_vertices.push_back(vNew);
      r.push_back(vNew);
    }
    r.push_back(v2);
    return r;
  }

  std::vector<MVertex*> reverseVector (const std::vector<MVertex*> &v){
    std::vector<MVertex*> r;
    for (size_t i=0;i<v.size();i++)r.push_back(v[v.size() - 1 - i]);
    return r;
  }

  void createQuadPatch (GFace* gf,
      const std::vector<MVertex*> &s0,
      const std::vector<MVertex*> &s1,
      const std::vector<MVertex*> &s2,
      const std::vector<MVertex*> &s3,
      std::vector<MElement*> &newQuads){
    std::vector< std::vector<MVertex*> > grid;
    grid.push_back(s0);
    std::vector<MVertex*> s3r = reverseVector(s3);
    for (size_t i=1;i<s3r.size()-1;i++){
      grid.push_back(createVertices(gf,s3r[i],s1[i],s0.size()-1));
    }
    grid.push_back(reverseVector(s2));


    for (size_t i=0;i<grid.size()-1;i++){
      for (size_t j=0;j<grid[i].size()-1;j++){
        MQuadrangle *q = new MQuadrangle (grid[i][j],grid[i+1][j],grid[i+1][j+1],grid[i][j+1]);
        newQuads.push_back(q);
        gf->quadrangles.push_back(q);
      }
    }    
  }

  bool addQuadsAccordingToPattern(
      const QuadMesh& P,
      const std::vector<int>& quantization,
      GFace* gf, 
      const std::vector<std::vector<MVertex*> >& sides, /* vertices on the boundary, not changed */
      std::vector<MVertex*>& newVertices,               /* new vertices inside the cavity */
      std::vector<bool>& vertexIsIrregular,             /* for each new vertex, true if irregular */
      std::vector<MElement*>& newElements               /* new quads inside the cavity */
      ) {
    if (P.sides.size() != sides.size()) {
      Msg::Error("wrong input sizes ... pattern: %li sides, input: %li sides", P.sides.size(),sides.size());
    }

    std::unordered_map<id2, std::vector<MVertex*>, id2Hash> vpair2vertices;
    std::vector<MVertex*> v2mv(P.n,NULL);
    std::vector<MVertex*> vert;
    /* Associate exising vertices to pattern sides */

    for (size_t s = 0; s < sides.size(); ++s) {
      for (size_t k = 0; k < sides[s].size()-1; ++k) {
        vector<array<double,3>> pts = {SVector3(sides[s][k]->point()),SVector3(sides[s][k+1]->point())};
        vector<double> values = {double(k),double(k+1)};
        GeoLog::add(pts,values,"side_input_vert_"+std::to_string(s));
      }
    }

    DBG("--------");
    for (size_t s = 0; s < P.sides.size(); ++s) {
      size_t pos = 0;
      DBG(s);
      DBG("- input:", sides[s].size());
      size_t sp = 0;
      for (size_t j = 0; j < P.sides[s].size(); ++j) {
        sp += quantization[P.eChordId[P.sides[s][j]]];
      }
      DBG("- pattern:", sp+1);
      if (sp + 1 != sides[s].size()) {
        DBG("bad");
        return false;
      }
      for (size_t j = 0; j < P.sides[s].size(); ++j) {
        id e = P.sides[s][j];
        int n_e = quantization[P.eChordId[e]];
        DBG(" ", j, P.sides[s].size(), e, n_e);
        vert.resize(n_e+1);
        for (size_t k = 0; k < (size_t) n_e+1; ++k) {
          DBG("   ", k, n_e+1, pos, pos+k, sides[s].size());
          if (pos+k>=sides[s].size()) {
            Msg::Error("issue, pos=%li + k=%li = %li >= sides[s].size()=%li", pos, k, pos+k, sides[s].size());
            GeoLog::flush();
            return false;
          }
          vert[k] = sides[s][pos+k];
        }
        pos += n_e;
        id v1 = P.edges[e][0];
        id v2 = P.edges[e][1];
        if (v2mv[v1] == NULL) v2mv[v1] = vert.front();
        if (v2mv[v2] == NULL) v2mv[v2] = vert.back();

        {
          vector<array<double,3>> pts = {SVector3(v2mv[v1]->point()),SVector3(v2mv[v2]->point())};
          vector<double> values = {0.,1.};
          GeoLog::add(pts,values,"side_"+std::to_string(s)+"_"+std::to_string(j));
        }
        {
          for (size_t k = 0; k < vert.size()-1; ++k) {
            vector<array<double,3>> pts = {SVector3(vert[k]->point()),SVector3(vert[k+1]->point())};
            vector<double> values = {double(k),double(k+1)};
            GeoLog::add(pts,values,"side_vert_"+std::to_string(s)+"_"+std::to_string(j));
          }
        }
        // // DBG(s,j,e,n_e,pos-n_e,pos,v1,v2,vert,vert.size());
        // DBG("  ", sides[s].size());

        id2 vpair = sorted(v1,v2);
        if (v1 < v2) {
          vpair2vertices[vpair] = vert;
        } else {
          std::reverse(vert.begin(),vert.end());
          vpair2vertices[vpair] = vert;
        }
      }
    }
    GeoLog::flush();

    /* Create vertices on internal points */
    for (size_t v = 0; v < P.n; ++v) if (!P.vOnBdr[v]) {
      GPoint pp(0,0,0);
      MVertex *sing = new MFaceVertex(pp.x(),pp.y(),pp.z(),gf,pp.u(),pp.v());
      gf->mesh_vertices.push_back(sing);
      newVertices.push_back(sing);
      bool irregular = true; // TODO !
      vertexIsIrregular.push_back(irregular);
      v2mv[v] = sing;
    }

    /* Create vertices on internal curves */
    for (size_t e = 0; e < P.edges.size(); ++e) if (P.e2f[e].size() == 2) {
      id v1 = P.edges[e][0];
      id v2 = P.edges[e][1];
      MVertex* mv1 = v2mv[v1];
      MVertex* mv2 = v2mv[v2];
      if (mv1 == NULL || mv2 == NULL) {
        Msg::Error("MVertex* not found ?");
        return false;
      }
      int n_e = quantization[P.eChordId[e]];
      id2 vpair = sorted(v1,v2);
      if (v1 < v2) {
        vpair2vertices[vpair] = createVertices (gf, mv1, mv2, n_e);
      } else {
        vpair2vertices[vpair] = createVertices (gf, mv2, mv1, n_e);
      }
    }

    /* Create vertices inside the quad patches */
    for (size_t f = 0; f < P.quads.size(); ++f) {
      std::vector<std::vector<MVertex*> > quadCurves(4);
      for (size_t le = 0; le < 4; ++le) {
        id v0 = P.qvertices[f][le];
        id v1 = P.qvertices[f][(le+1)%4];
        id2 vpair = sorted(v0,v1);
        auto it = vpair2vertices.find(vpair);
        if (it == vpair2vertices.end()) {
          Msg::Error("MVertex* vector not found for vertex pair (edge in pattern)");
          return false;
        }
        quadCurves[le] = it->second;
        if (v1 < v0) {
          std::reverse(quadCurves[le].begin(),quadCurves[le].end());
        }
        {
          for (size_t k = 0; k < quadCurves[le].size()-1; ++k) {
            vector<array<double,3>> pts = {SVector3(quadCurves[le][k]->point()),SVector3(quadCurves[le][k+1]->point())};
            vector<double> values = {double(k),double(k+1)};
            GeoLog::add(pts,values,"quad_"+std::to_string(f));

          }
        }
      }
      GeoLog::flush();
      createQuadPatch(gf, quadCurves[0], quadCurves[1], quadCurves[2], quadCurves[3], newElements);
    }

    return true;
  }

  bool load_disk_quadrangulations() {
#if defined(_OPENMP)
#pragma omp critical
#endif
    {
      if (B_disk_quadrangulations.size() != 0) {
        B_disk_quadrangulations.clear();
        B_BVL_ids.clear();
      }
      Msg::Info("loading disk quadrangulations ...");
      B_disk_quadrangulations.reserve(20);
      B_BVL_ids.reserve(20);
      std::string data(disk_quadrangulations);
      vector<std::string> lines = SplitString(data,'\n');
      Quadrangulation qdrl;
      vector<std::string> numbers;
      vector<id> bdrValLoop;
      for (size_t i = 0; i < lines.size(); ++i) {
        numbers = SplitString(lines[i],' ');
        if (numbers.size() < 7) continue;
        size_t B = std::stoi(numbers[0]);
        size_t I = std::stoi(numbers[1]);
        size_t Q = std::stoi(numbers[2]);
        if (numbers.size() != 3 + 4 * Q) {
          Msg::Warning("load_disk_quadrangulations | wrong sizes: B=%li, I=%li, Q=%li and numbers.size = %li",
              B, I, Q, numbers.size());
          continue;
        }
        qdrl.resize(Q);
        for (size_t j = 0; j < Q; ++j) {
          for (size_t lv = 0; lv < 4; ++lv) {
            qdrl[j][lv] = std::stoi(numbers[3 + 4 * j + lv]);
          }
        }

        if (B >= B_disk_quadrangulations.size()) {
          B_disk_quadrangulations.resize(B+1);
          B_disk_quadrangulations[B].reserve(1000);
          B_BVL_ids.resize(B+1);
        }

        id qId = B_disk_quadrangulations[B].size();
        B_disk_quadrangulations[B].push_back(qdrl);

        /* Assumes:
         * - first B vertices are on the boundary 
         * - canonical valence ordering according to boundary valence loop 
         *   (should be compatible with the generator) */
        bdrValLoop.clear();
        bdrValLoop.resize(B,0);
        for (size_t j = 0; j < Q; ++j) for (size_t lv = 0; lv < 4; ++lv){
          id v = qdrl[j][lv];
          if (v < B) bdrValLoop[v] += 1;
        }
        B_BVL_ids[B][bdrValLoop].push_back(qId);
      }
      Msg::Info("%li disk quadrangulations loaded", lines.size());
    }
    return true;
  }



  bool computeQuadMeshValences(const vector<id4>& quads, vector<int>& valence) {
    valence.clear();
    valence.reserve(quads.size()*4);
    for (size_t f = 0; f < quads.size(); ++f) for (size_t lv = 0; lv < 4; ++lv) {
      size_t v = quads[f][lv];
      if (v >= valence.size()) valence.resize(v+1,0);
      valence[v] += 1;
    }
    return true;
  }

  double computeIrregularity(
      const vector<id4>& quads, 
      const vector<int>& valence,
      const std::vector<int>& bndIdealValence,
      const std::vector<std::pair<int,int> >& bndAllowedValenceRange) 
  {
    double irregularity = 0.;
    for (size_t bv = 0; bv < bndIdealValence.size(); ++bv) {
      if (valence[bv] < bndAllowedValenceRange[bv].first) return DBL_MAX;
      if (valence[bv] > bndAllowedValenceRange[bv].second) return DBL_MAX;
      if (bndIdealValence[bv] <= 1 && valence[bv] >= 2) { /* probably making a 6+ ... */
        irregularity += 1000;
      } else {
        irregularity += 10*std::pow(bndIdealValence[bv]-valence[bv],2);
      }
    }
    for (size_t iv = bndIdealValence.size(); iv < valence.size(); ++iv) {
      irregularity += std::pow(4-valence[iv],2);
    }
    return irregularity;
  }

  bool computeBestMatchingConfiguration(
      const vector<id4>& quads, 
      const vector<int>& valence,
      const vector<int>& bndIdealValence,
      const vector<std::pair<int,int> >& bndAllowedValenceRange,
      int& rotation,
      double& irregularity)
  {
    double best = DBL_MAX;
    int rot = 0;
    vector<int> biv = bndIdealValence;
    vector<std::pair<int,int>>  bav = bndAllowedValenceRange;

    /* Initial config */
    {
      double irreg = computeIrregularity(quads, valence, biv, bav);
      if (irreg < best) {
        best = irreg;
        rotation = rot;
      }
    }

    /* Forward rotation */
    for (size_t r = 1; r < biv.size(); ++r) {
      rot += 1;
      std::rotate(biv.begin(),biv.begin()+1,biv.end());
      std::rotate(bav.begin(),bav.begin()+1,bav.end());
      double irreg = computeIrregularity(quads, valence, biv, bav);
      if (irreg < best) {
        best = irreg;
        rotation = rot;
      }
    }

    /* Try in reverse order */
    rot = 0;
    biv = bndIdealValence;
    bav = bndAllowedValenceRange;
    std::reverse(biv.begin(),biv.end());
    std::reverse(bav.begin(),bav.end());
    for (size_t r = 1; r < biv.size(); ++r) {
      rot -= 1;
      std::rotate(biv.begin(),biv.begin()+1,biv.end());
      std::rotate(bav.begin(),bav.begin()+1,bav.end());
      double irreg = computeIrregularity(quads, valence, biv, bav);
      if (irreg < best) {
        best = irreg;
        rotation = rot;
      }
    }

    irregularity = best;
    return (best != DBL_MAX);
  }

  /* WARNING: GFace is not modified, just the "floating" MVertex
   * and MQuadrangle are created, they must be inserted in the GFace
   * later is the pattern is kept */
  bool applyPatternToRemeshFewQuads(
      GFace* gf,
      const std::vector<MVertex*>& bnd,
      const std::vector<int>& bndIdealValence,
      const std::vector<std::pair<int,int> >& bndAllowedValenceRange,
      int rotation,                                      /* rotation to apply to input */
      const std::vector<id4>& quads,                     /* pattern */
      const vector<int>& valence,                        /* valence in pattern */
      std::vector<MVertex*> & newVertices,               /* new vertices inside the cavity */
      std::vector<bool> & vertexIsIrregular,             /* for each new vertex, true if irregular */
      std::vector<MElement*> & newElements               /* new quads inside the cavity */
      ) {

    std::vector<MVertex*> bndr = bnd;
    if (rotation > 0) {
      std::rotate(bndr.begin(),bndr.begin()+(size_t)rotation,bndr.end());
    } else if (rotation < 0) {
      std::reverse(bndr.begin(),bndr.end());
      std::rotate(bndr.begin(),bndr.begin()+(size_t) std::abs(rotation),bndr.end());
    }

    std::unordered_map<id,MVertex*> pv2mv;
    for (size_t f = 0; f < quads.size(); ++f) {
      std::array<MVertex*,4> vert;
      for (size_t lv = 0; lv < 4; ++lv) {
        size_t pv = quads[f][lv];
        if (pv < bndr.size()) {
          vert[lv] = bndr[pv];
        } else {
          auto it = pv2mv.find(pv);
          if (it == pv2mv.end()) {
            SVector3 p = bndr[0]->point();
            double uv[2] = {0.,0.};
            MVertex *mv = new MFaceVertex(p.x(),p.y(),p.z(),gf,uv[0],uv[1]);
            pv2mv[pv] = mv;
            vert[lv] = mv;
            newVertices.push_back(mv);
            vertexIsIrregular.push_back(valence[pv]!=4);
          } else {
            vert[lv] = it->second;
          }
        }
      }
      if (rotation < 0) { /* revert quad to keep coherent orientation */
        std::reverse(vert.begin(),vert.end());
      }
      MQuadrangle *q = new MQuadrangle (vert[0],vert[1],vert[2],vert[3]);
      newElements.push_back(q);
    }

    return true;
  }

  bool laplacianSmoothing(
    const std::vector<MVertex*>& newVertices,
    const std::vector<MElement*>& newElements,
    size_t iter = 10) {

    std::unordered_map<MVertex*,std::set<MVertex*>> v2v;
    for (MElement* f: newElements) {
      for (size_t le = 0; le < 4; ++le) {
        MVertex* v1 = f->getVertex(le);
        MVertex* v2 = f->getVertex((le+1)%4);
        v2v[v1].insert(v2);
        v2v[v2].insert(v1);
      }
    }
    for (size_t i = 0; i < iter; ++i) {
      for (MVertex* v: newVertices) {
        SVector3 avg(0.,0.,0.);
        double sum = 0.;
        for (MVertex* v2: v2v[v]) {
          avg += v2->point();
          sum += 1.;
        }
        if (sum == 0) continue;
        v->x() = 1./sum * avg.x();
        v->y() = 1./sum * avg.y();
        v->z() = 1./sum * avg.z();
      }
    }

    return true;
  }

  double checkPatternMatching(const QuadMesh& P, const std::vector<size_t>& sideSizes, vector<int>& slt) {
    slt.clear();
    size_t N = sideSizes.size();
    bool possible = true;
    for (size_t s = 0; s < N; ++s) {
      if (sideSizes[s] < P.sides[s].size() + 1) {
        /* on given side, less edges in the cavity than edges in the pattern */
        possible = false; break;
      }
    }
    if (!possible) return 0.;

    int nvars = 0;
    for (id v: P.eChordId) nvars = std::max(nvars,(int)v+1);
    IMatrix mat(N,nvars+1);
    for (size_t s = 0; s < N; ++s) {
      for (size_t j = 0; j < P.sides[s].size(); ++j) {
        id e = P.sides[s][j];
        id var = P.eChordId[e];
        mat.set(s,var,mat.get(s,var)+1);
      }
      mat.set(s,nvars,-1. * (sideSizes[s]-1));
    }
    mat.tansform_to_row_reduced_echelon();
    bool use_recursive = true;
    bool ok = false;
    double score = 0.;
    if (use_recursive) {
      slt.clear();
      int count = 0;
      int count_limit = 1000; /* limit on the number of solution tried in the DFS */
      score = mat.get_positive_solution_DFS(slt, count, count_limit);
      DBG(score);
      ok = (score > 0.);
    } else {
      ok = mat.get_positive_solution(slt);
      score = 0.;
      for (int x_i: slt) score += std::sqrt(x_i);
    }
    if (ok) {
      // Msg::Info("solution found !");
      // DBG(slt);
    } else {
      // Msg::Info("solution not found !");
      // DBG(slt);
      return 0.;
    }
    if (ok) {
      DBG("matched pattern", score, slt);
    }

    return score;
  }

  double checkPatternMatchingWithRotations(const QuadMesh& P, const std::vector<size_t>& sideSizes, int& rotation) {
    size_t N = sideSizes.size();
    if (P.sides.size() != N) return 0.;

    vector<size_t> ssr = sideSizes;
    vector<int> slt;

    double best = 0.;
    int rot = 0;

    /* Initial config */
    {
      double match = checkPatternMatching(P, ssr, slt);
      DBG(match);
      if (match > best) {
        best = match;
        rotation = rot;
      }
    }

    /* Forward rotation */
    for (size_t r = 1; r < ssr.size(); ++r) {
      rot += 1;
      std::rotate(ssr.begin(),ssr.begin()+1,ssr.end());
      double match = checkPatternMatching(P, ssr, slt);
      DBG(match);
      if (match > best) {
        best = match;
        rotation = rot;
      }
    }

    /* Try in reverse order */
    rot = 0;
    ssr = sideSizes;
    std::reverse(ssr.begin(),ssr.end());
    for (size_t r = 1; r < ssr.size(); ++r) {
      rot -= 1;
      std::rotate(ssr.begin(),ssr.begin()+1,ssr.end());
      double match = checkPatternMatching(P, ssr, slt);
      DBG(match);
      if (match > best) {
        best = match;
        rotation = rot;
      }
    }

    DBG("-", rotation, best);

    return best;
  }

}

using namespace QuadPatternMatching;

bool patchIsRemeshableWithQuadPattern(const std::vector<size_t>& sideSizes, 
    std::pair<size_t,int>& patternNoAndRot) {
  if (patterns.size() == 0) load_patterns();
  DBG("---");
  DBG("isRemeshable ?");
  DBG(sideSizes);

  double best = 0.;
  for (size_t i = 0; i < patterns.size(); ++i) {
    DBG(" ", i);
    const QuadMesh& P = patterns[i];
    if (sideSizes.size() != P.sides.size()) continue;

    int rot = 0;
    double score = checkPatternMatchingWithRotations(P, sideSizes, rot);
    if (score > best) {
      patternNoAndRot.first = i;
      patternNoAndRot.second = rot;
      best = score;
    }
    DBG("-",i,score);
  }
  DBG("isRemeshable?", best, patternNoAndRot);
  return (best > 0.);
}


int remeshPatchWithQuadPattern(
    GFace* gf, 
    const std::vector<std::vector<MVertex*> >& sides, /* vertices on the boundary, not changed */
    const std::pair<size_t,int>& patternNoAndRot,     /* pattern to use, from patchIsRemeshableWithQuadPattern */
    std::vector<MVertex*>& newVertices,               /* new vertices inside the cavity */
    std::vector<bool>& vertexIsIrregular,             /* for each new vertex, true if irregular */
    std::vector<MElement*>& newElements               /* new quads inside the cavity */
    ) {

  size_t N = sides.size();
  const QuadMesh& P = patterns[patternNoAndRot.first];
  int rot = patternNoAndRot.second;
  if (P.sides.size() != N) {
    Msg::Error("sides not matching, shoud not happen (pattern has %li sides, but %li sides in input) ...", P.sides.size(), N);
    return -1;
  }

  std::vector<std::vector<MVertex*> > sidesr = sides;
  if (rot > 0) {
    std::rotate(sidesr.begin(),sidesr.begin()+(size_t)rot,sidesr.end());
  } else if (rot < 0) {
    std::reverse(sidesr.begin(),sidesr.end());
    std::rotate(sidesr.begin(),sidesr.begin()+(size_t) std::abs(rot),sidesr.end());
    for (size_t i = 0; i <sidesr.size(); ++i) {
      std::reverse(sidesr[i].begin(),sidesr[i].end());
    }
  }

  vector<size_t> ssr(sidesr.size());
  for (size_t i = 0; i < sidesr.size(); ++i) ssr[i] = sidesr[i].size();
  vector<int> slt;
  double match = checkPatternMatching(P, ssr, slt);
  if (match <= 0.) {
    Msg::Error("given pattern not marching sides, weird... N=%li", N);
    DBG(patternNoAndRot);
    DBG(match);
    DBG(ssr);
    return -1;
  }

  DBG("+++++ addQuadsAccordingToPattern ++++++");
  DBG(ssr);
  DBG(slt);
  bool oka = addQuadsAccordingToPattern(P, slt, gf, sidesr, newVertices, vertexIsIrregular, newElements);
  if (!oka) {
    Msg::Error("failed to add quads according to pattern, weird");
    return 1;
  }
  return 0; /* ok ! */
}

void printPatternUsage() {
  Msg::Info("disk quadrangulation remeshing stats: %li distinct patterns used", usage_count.size());
}

int remeshFewQuads(GFace* gf, 
    const std::vector<MVertex*>& bnd,
    const std::vector<int>& bndIdealValence,
    const std::vector<std::pair<int,int> >& bndAllowedValenceRange,
    std::vector<MVertex*> & newVertices,               /* new vertices inside the cavity */
    std::vector<bool> & vertexIsIrregular,             /* for each new vertex, true if irregular */
    std::vector<MElement*> & newElements               /* new quads inside the cavity */
    ) {
  if (B_disk_quadrangulations.size() == 0) {
    load_disk_quadrangulations();
  }

  const vector<vector<id4> >* small_patterns = NULL;
  if (bnd.size() < B_disk_quadrangulations.size() && B_disk_quadrangulations[bnd.size()].size() > 0) {
    small_patterns = &(B_disk_quadrangulations[bnd.size()]);
  } else {
    Msg::Error("no pattern for input size (%li bnd vertices)", bnd.size());
    return 1;
  }

  const vector<vector<id4> >& qmeshes = *small_patterns;

  vector<int> valence;

  std::vector<std::pair<double,std::pair<size_t,int> > > irregularity_pattern_rotation;
  for (size_t i = 0; i < qmeshes.size(); ++i) {
    const vector<id4>& quads = qmeshes[i];
    computeQuadMeshValences(quads, valence);
    double irregularity = DBL_MAX;
    int rotation = 0;
    bool found = computeBestMatchingConfiguration(quads, valence, bndIdealValence, bndAllowedValenceRange, rotation, irregularity);
    if (found) {
      // DBG("  ", i, rotation, irregularity);
      irregularity_pattern_rotation.push_back({irregularity,{i,rotation}});
    }
  }
  if (irregularity_pattern_rotation.size() == 0) {
    Msg::Debug("remeshFewQuads: no pattern matching input allowed valence range");
    // DBG("  ", bnd.size());
    // DBG("  ", bndIdealValence);
    // DBG("  ", bndAllowedValenceRange);
    return 1; /* no pattern matching allowed valence range */
  }

  /* Apply best pattern */
  std::sort(irregularity_pattern_rotation.begin(),irregularity_pattern_rotation.end());
  size_t no = irregularity_pattern_rotation[0].second.first;
  int rotation = irregularity_pattern_rotation[0].second.second;

  const vector<id4>& quads = qmeshes[no];
  computeQuadMeshValences(quads, valence);
  bool ok = applyPatternToRemeshFewQuads(gf, bnd, bndIdealValence, bndAllowedValenceRange, rotation,
      quads, valence, newVertices, vertexIsIrregular, newElements);
  if (ok) {
    Msg::Debug("successfully remesh small cavity (%li bnd vertices) with %li quads", bnd.size(), newElements.size());
    laplacianSmoothing(newVertices, newElements,10);
    id2 B_i = {(id)bnd.size(),(id)no};
    usage_count[B_i] += 1;
    return 0;
  } else {
    Msg::Error("failed to remesh small cavity (%li bnd vertices) with %li quads", bnd.size(), newElements.size());
  }

  Msg::Info("failed to remesh small cavity (%li bnd vertices)", bnd.size());
  DBG(bnd);
  DBG(bndIdealValence);
  DBG(bndAllowedValenceRange);

  return 1;
}
