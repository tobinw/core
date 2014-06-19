/****************************************************************************** 

  Copyright 2013 Scientific Computation Research Center, 
      Rensselaer Polytechnic Institute. All rights reserved.
  
  The LICENSE file included with this distribution describes the terms
  of the SCOREC Non-Commercial License this program is distributed under.
 
*******************************************************************************/
#include "maSnap.h"
#include "maAdapt.h"
#include <maOperator.h>
#include <maDigger.h>
#include <PCU.h>

namespace ma {

/* this is the logic to deal with discontinuous
   periodic parametric coordinates.
   We assume that if the difference between
   the vertex coordinates of a mesh edge is more than half
   the periodic range, then the edge
   crosses over the discontinuity and we need
   to interpolate differently. */
static double interpolateParametricCoordinate(
    double t,
    double a,
    double b,
    double range[2],
    bool isPeriodic)
{
  if ( ! isPeriodic)
    return (1-t)*a + t*b;
  if (range[0] > range[1])
    std::swap(range[0],range[1]);
  if (a > b)
  {
    std::swap(a,b);
    t = 1-t;
  }
  double period = range[1]-range[0];
  double span = b-a;
  if (span < (period/2))
    return (1-t)*a + t*b;
  a += period;
  double result = (1-t)*b + t*a;
  if (result > range[1])
    result -= period;
  assert(result > range[0]);
  assert(result < range[1]);
  return result;
}

static void interpolateParametricCoordinates(
    Mesh* m,
    Model* g,
    double t,
    Vector const& a,
    Vector const& b,
    Vector& p)
{
  double range[2];
  int dim = m->getModelType(g);
  for (int d=0; d < dim; ++d)
  {
    bool isPeriodic = m->getPeriodicRange(g,d,range);
    p[d] = interpolateParametricCoordinate(t,a[d],b[d],range,isPeriodic);
  }
}

void transferParametricOnEdgeSplit(
    Mesh* m,
    Entity* e,
    double t,
    Vector& p)
{
  Model* g = m->toModel(e);
  int modelDimension = m->getModelType(g);
  if (modelDimension==m->getDimension()) return;
  Entity* ev[2];
  m->getDownward(e,0,ev);
  Vector ep[2];
  for (int i=0; i < 2; ++i)
    m->getParamOn(g,ev[i],ep[i]);
  interpolateParametricCoordinates(m,g,t,ep[0],ep[1],p);
}

static void getSnapPoint(Mesh* m, Entity* v, Vector& x)
{
  m->getPoint(v,0,x);
  Vector p;
  m->getParam(v,p);
  Model* g = m->toModel(v);
  m->snapToModel(g,p,x);
}

static bool trySnapping(Adapt* adapter, Tag* tag, Entity* vert)
{
  Mesh* mesh = adapter->mesh;
  Vector x = getPosition(mesh, vert);
  Vector s;
  mesh->getDoubleTag(vert, tag, &s[0]);
  mesh->setPoint(vert, 0, s);
  Upward elements;
  mesh->getAdjacent(vert, mesh->getDimension(), elements);
  bool success = true;
  for (size_t i=0; i < elements.getSize(); ++i)
    if ( ! isElementValid(adapter, elements[i])) {
      mesh->setPoint(vert, 0, x);
      success = false;
      break;
    }
  if (success) {
    mesh->removeTag(vert, tag);
    return true;
  }
  return false;
}

class Snapper : public Operator
{
  public:
    Snapper(Adapt* a, Tag* t, bool d):
      digger(a, t)
    {
      adapter = a;
      tag = t;
      shouldDig = d;
      successCount = 0;
      didAnything = false;
    }
    int getTargetDimension() {return 0;}
    bool shouldApply(Entity* e)
    {
      if ( ! getFlag(adapter, e, SNAP))
        return false;
      vert = e;
      return true;
    }
    bool requestLocality(apf::CavityOp* o)
    {
      if (shouldDig)
        return digger.setVert(vert, o);
      return o->requestLocality(&vert, 1);
    }
    void apply()
    {
      bool snapped = false;
      if (shouldDig) {
        bool dug = digger.run();
        if (dug) {
          didAnything = true;
          snapped = trySnapping(adapter, tag, vert);
          fprintf(stderr, "digging succeeded\n");
        } else {
          fprintf(stderr, "digging failed\n");
        }
      } else {
        snapped = trySnapping(adapter, tag, vert);
      }
      if (snapped) {
        didAnything = true;
        ++successCount;
      }
      clearFlag(adapter, vert, SNAP);
    }
    int successCount;
    bool didAnything;
  private:
    Adapt* adapter;
    Tag* tag;
    Entity* vert;
    Digger digger;
    bool shouldDig;
};

static bool areExactlyEqual(Vector& a, Vector& b)
{
  return a[0] == b[0] &&
         a[1] == b[1] &&
         a[2] == b[2];
}

long tagVertsToSnap(Adapt* a, Tag*& t)
{
  Mesh* m = a->mesh;
  int dim = m->getDimension();
  t = m->createDoubleTag("ma_snap", 3);
  Entity* v;
  long n = 0;
  Iterator* it = m->begin(0);
  while ((v = m->iterate(it))) {
    if (getFlag(a, v, LAYER))
      continue;
    int md = m->getModelType(m->toModel(v));
    if (md == dim) continue;
    Vector s;
    getSnapPoint(m, v, s);
    Vector x = getPosition(m, v);
    if (areExactlyEqual(s, x))
      continue;
    m->setDoubleTag(v, t, &s[0]);
    if (m->isOwned(v))
      ++n;
  }
  PCU_Add_Longs(&n, 1);
  return n;
}

static void markVertsToSnap(Adapt* a, Tag* t)
{
  Mesh* m = a->mesh;
  Iterator* it = m->begin(0);
  Entity* v;
  while ((v = m->iterate(it)))
    if (m->hasTag(v, t))
      setFlag(a, v, SNAP);
  m->end(it);
}

bool snapOneRound(Adapt* a, Tag* t, bool shouldDig, long& successCount)
{
  markVertsToSnap(a, t);
  Snapper snapper(a, t, shouldDig);
  applyOperator(a, &snapper);
  long n = snapper.successCount;
  PCU_Add_Longs(&n, 1);
  successCount += n;
  return snapper.didAnything;
}

void snap(Adapt* a)
{
  if ( ! a->input->shouldSnap)
    return;
  Tag* tag;
  long targetCount = tagVertsToSnap(a, tag);
  long successCount = 0;
  long roundSuccess;
  /* first snap all the vertices we can without digging.
     This is fast because it uses just the elements around
     the vertex and doesn't think much, it should also handle
     the vast majority of vertices */
  while (snapOneRound(a, tag, false, successCount));
  /* all the remaining vertices now need some kind of modification
     in order to snap.
     Here we turn on the "try digging before snapping" flag,
     which requires two-layer cavities so hopefully fewer vertices
     are involved here */
  while (snapOneRound(a, tag, true, successCount));
  a->mesh->destroyTag(tag);
  print("snapped %li of %li vertices", successCount, targetCount);
}

void visualizeGeometricInfo(Mesh* m, const char* name)
{
  Tag* dimensionTag = m->createIntTag("ma_geom_dim",1);
  Tag* idTag = m->createIntTag("ma_geom_id",1);
  apf::Field* field = apf::createLagrangeField(m,"ma_param",apf::VECTOR,1);
  Iterator* it = m->begin(0);
  Entity* v;
  while ((v = m->iterate(it)))
  {
    Model* c = m->toModel(v);
    int dimension = m->getModelType(c);
    m->setIntTag(v,dimensionTag,&dimension);
    int id = m->getModelTag(c);
    m->setIntTag(v,idTag,&id);
    Vector p;
    m->getParam(v,p);
    apf::setVector(field,v,0,p);
  }
  m->end(it);
  apf::writeVtkFiles(name,m);
  it = m->begin(0);
  while ((v = m->iterate(it)))
  {
    m->removeTag(v,dimensionTag);
    m->removeTag(v,idTag);
  }
  m->end(it);
  m->destroyTag(dimensionTag);
  m->destroyTag(idTag);
  apf::destroyField(field);
}

}
