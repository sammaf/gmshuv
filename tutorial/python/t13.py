# ------------------------------------------------------------------------------
#
#  Gmsh Python tutorial 13
#
#  Remeshing an STL file without an underlying CAD model
#
# ------------------------------------------------------------------------------

import gmsh
import math
import os
import sys

gmsh.initialize()

def createGeometryAndMesh():
    # Clear all models and merge an STL mesh that we would like to remesh (from
    # the parent directory):
    gmsh.clear()
    path = os.path.dirname(os.path.abspath(__file__))
    gmsh.merge(os.path.join(path, os.pardir, 't13_data.stl'))

    # We first classify ("color") the surfaces by splitting the original surface
    # along sharp geometrical features. This will create new discrete surfaces,
    # curves and points.

    # Angle between two triangles above which an edge is considered as sharp:
    angle = gmsh.onelab.getNumber('Parameters/Angle for surface detection')[0]

    # For complex geometries, patches can be too complex, too elongated or too
    # large to be parametrized; setting the following option will force the
    # creation of patches that are amenable to reparametrization:
    forceParametrizablePatches = gmsh.onelab.getNumber(
        'Parameters/Create surfaces guaranteed to be parametrizable')[0]

    # For open surfaces include the boundary edges in the classification
    # process:
    includeBoundary = True

    # Force curves to be split on given angle:
    curveAngle = 180

    gmsh.model.mesh.classifySurfaces(angle * math.pi / 180., includeBoundary,
                                     forceParametrizablePatches,
                                     curveAngle * math.pi / 180.)

    # Create a geometry for all the discrete curves and surfaces in the mesh, by
    # computing a parametrization for each one
    gmsh.model.mesh.createGeometry()

    # Note that if a CAD model (e.g. as a STEP file, see `t20.py') is available
    # instead of an STL mesh, it is usually better to use that CAD model instead
    # of the geometry created by reparametrizing the mesh. Indeed, CAD
    # geometries will in general be more accurate, with smoother
    # parametrizations, and will lead to more efficient and higher quality
    # meshing. Discrete surface remeshing in Gmsh is optimized to handle dense
    # STL meshes coming from e.g. imaging systems / where no CAD is available;
    # it is less well suited for the poor quality STL triangulations (optimized
    # for size, with e.g. very elongated triangles) that are usually generated
    # by CAD tools for e.g. 3D printing.

    # Create a volume from all the surfaces
    s = gmsh.model.getEntities(2)
    l = gmsh.model.geo.addSurfaceLoop([s[i][1] for i in range(len(s))])
    gmsh.model.geo.addVolume([l])

    gmsh.model.geo.synchronize()

    # We specify element sizes imposed by a size field, just because we can :-)
    f = gmsh.model.mesh.field.add("MathEval")
    if gmsh.onelab.getNumber('Parameters/Apply funny mesh size field?')[0]:
        gmsh.model.mesh.field.setString(f, "F", "2*Sin((x+y)/5) + 3")
    else:
        gmsh.model.mesh.field.setString(f, "F", "4")
    gmsh.model.mesh.field.setAsBackgroundMesh(f)

    gmsh.model.mesh.generate(3)
    gmsh.write('t13.msh')

# Create ONELAB parameters with remeshing options:
gmsh.onelab.set("""[
  {
    "type":"number",
    "name":"Parameters/Angle for surface detection",
    "values":[40],
    "min":20,
    "max":120,
    "step":1
  },
  {
    "type":"number",
    "name":"Parameters/Create surfaces guaranteed to be parametrizable",
    "values":[0],
    "choices":[0, 1]
  },
  {
    "type":"number",
    "name":"Parameters/Apply funny mesh size field?",
    "values":[0],
    "choices":[0, 1]
  }
]""")

# Create the geometry and mesh it:
createGeometryAndMesh()

# Launch the GUI and handle the "check" event to recreate the geometry and mesh
# with new parameters if necessary:
def checkForEvent():
    action = gmsh.onelab.getString("ONELAB/Action")
    if len(action) and action[0] == "check":
        gmsh.onelab.setString("ONELAB/Action", [""])
        createGeometryAndMesh()
        gmsh.graphics.draw()
    return True

if "-nopopup" not in sys.argv:
    gmsh.fltk.initialize()
    while gmsh.fltk.isAvailable() and checkForEvent():
        gmsh.fltk.wait()

gmsh.finalize()
