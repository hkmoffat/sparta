/* ----------------------------------------------------------------------
   SPARTA - Stochastic PArallel Rarefied-gas Time-accurate Analyzer
   http://sparta.sandia.gov
   Steve Plimpton, sjplimp@sandia.gov, Michael Gallis, magalli@sandia.gov
   Sandia National Laboratories

   Copyright (2014) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level SPARTA directory.
------------------------------------------------------------------------- */

#ifdef SURF_COLLIDE_CLASS

SurfCollideStyle(specular,SurfCollideSpecular)

#else

#ifndef SPARTA_SURF_COLLIDE_SPECULAR_H
#define SPARTA_SURF_COLLIDE_SPECULAR_H

#include "surf_collide.h"
#include "particle.h"

namespace SPARTA_NS {

class SurfCollideSpecular : public SurfCollide {
 public:
  SurfCollideSpecular(class SPARTA *, int, char **);
  SurfCollideSpecular(class SPARTA *sparta) : SurfCollide(sparta) {}
  virtual ~SurfCollideSpecular() {}
  virtual Particle::OnePart *collide(Particle::OnePart *&, double *, double &, int,
                                     SurfState* surfState) override;
};

}

#endif
#endif

/* ERROR/WARNING messages:

E: Illegal ... command

Self-explanatory.  Check the input script syntax and compare to the
documentation for the command.  You can use -echo screen as a
command-line option when running SPARTA to see the offending line.

*/
