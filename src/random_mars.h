/* ----------------------------------------------------------------------
   DSMC - Sandia parallel DSMC code
   www.sandia.gov/~sjplimp/dsmc.html
   Steve Plimpton, sjplimp@sandia.gov, Michael Gallis, magalli@sandia.gov
   Sandia National Laboratories

   Copyright (2011) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level DSMC directory.
------------------------------------------------------------------------- */

#ifndef DSMC_RAN_MARS_H
#define DSMC_RAN_MARS_H

#include "pointers.h"

namespace DSMC_NS {

class RanMars : protected Pointers {
 public:
  RanMars(class DSMC *);
  ~RanMars();
  void init(int);
  double uniform();
  double gaussian();

 private:
  int initflag,save;
  int i97,j97;
  double c,cd,cm;
  double second;
  double *u;
};

}

#endif

/* ERROR/WARNING messages:

E: Invalid seed for Marsaglia random # generator

The initial seed for this random number generator must be a positive
integer less than or equal to 900 million.

*/
