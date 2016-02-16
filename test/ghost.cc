#include <gmi_mesh.h>
#include <apf.h>
#include <apfMesh2.h>
#include <apfMDS.h>
#include <PCU.h>
#include <parma.h>
#include <cassert>

namespace {
  const char* modelFile = 0;
  const char* meshFile = 0;

  void freeMesh(apf::Mesh* m)
  {
    m->destroyNative();
    apf::destroyMesh(m);
  }

  void getConfig(int argc, char** argv)
  {
    assert(argc==4);
    modelFile = argv[1];
    meshFile = argv[2];
  }

  double getFun3dW(int type) {
    assert( type >= apf::Mesh::VERTEX && type <= apf::Mesh::PYRAMID );
    const double vtxw = 1.0;
    const double edgew = 1.0;
    const double triw = 1.0;
    const double quadw = 1.0;
    const double tetw = 1.0;
    const double pyrw = 6.8;
    const double przw = 7.5;
    const double hexw = 13.8;
    const double weights[8] =
      {vtxw, edgew, triw, quadw, tetw, hexw, przw, pyrw};
    return weights[type];
  }

  apf::MeshTag* applyFun3dWeight(apf::Mesh* m) {
    apf::MeshTag* wtag = m->createDoubleTag("ghostWeight",1);
    apf::MeshEntity* e;
    for(int d=0; d <= m->getDimension(); d++) {
      apf::MeshIterator* itr = m->begin(d);
      while( (e = m->iterate(itr)) ) {
        double w = getFun3dW(m->getType(e));
        m->setDoubleTag(e, wtag, &w);
      }
      m->end(itr);
    }
    return wtag;
  }

  void runParma(apf::Mesh* m, apf::MeshTag* weights) {
    const int layers = 1;
    const double stepFactor = 0.5;
    const int verbosity = 2;
    apf::Balancer* ghost =
      Parma_MakeGhostDiffuser(m, layers, stepFactor, verbosity);
    ghost->balance(weights, 1.05);
    delete ghost;
  }
}

int main(int argc, char** argv)
{
  MPI_Init(&argc,&argv);
  PCU_Comm_Init();
  PCU_Debug_Open();
  gmi_register_mesh();
  getConfig(argc,argv);
  apf::Mesh2* m = apf::loadMdsMesh(modelFile,meshFile);
  apf::MeshTag* weights = applyFun3dWeight(m);
  runParma(m,weights);
  m->destroyTag(weights);
  apf::writeVtkFiles(argv[3],m);
  freeMesh(m);
  PCU_Comm_Free();
  MPI_Finalize();
}
