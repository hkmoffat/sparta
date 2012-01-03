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

#ifndef DSMC_FIX_H
#define DSMC_FIX_H

#include "pointers.h"

namespace DSMC_NS {

class Fix : protected Pointers {
 public:
  char *id,*style;

  int scalar_flag;               // 0/1 if compute_scalar() function exists
  int vector_flag;               // 0/1 if compute_vector() function exists
  int array_flag;                // 0/1 if compute_array() function exists
  int size_vector;               // length of global vector
  int size_array_rows;           // rows in global array
  int size_array_cols;           // columns in global array
  int global_freq;               // frequency s/v data is available at

  int peratom_flag;              // 0/1 if per-atom data is stored
  int size_peratom_cols;         // 0 = vector, N = columns in peratom array
  int peratom_freq;              // frequency per-atom data is available at

  double *vector_atom;           // computed per-atom vector
  double **array_atom;           // computed per-atom array

  int START_OF_STEP,END_OF_STEP;    // mask settings

  Fix(class DSMC *, int, char **);
  virtual ~Fix();

  virtual int setmask() = 0;

  virtual void init() {}
  virtual void setup(int) {}

  virtual void start_of_step() {}
  virtual void end_of_step() {}

  virtual double compute_scalar() {return 0.0;}
  virtual double compute_vector(int) {return 0.0;}
  virtual double compute_array(int,int) {return 0.0;}

  virtual double memory_usage() {return 0.0;}
};

}

#endif
