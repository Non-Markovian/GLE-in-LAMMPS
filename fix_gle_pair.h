/* -*- c++ -*- ----------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#ifdef FIX_CLASS

FixStyle(gle/pair,FixGLEPair)

#else

#ifndef LMP_FIX_GLE_PAIR_H
#define LMP_FIX_GLE_PAIR_H

#include "fix.h"
#include "thr_omp.h"

#define USE_CHEBYSHEV

namespace LAMMPS_NS {

class FixGLEPair : public Fix {
 public:
  FixGLEPair(class LAMMPS *, int, char **);
  virtual ~FixGLEPair();
  int setmask();
  virtual void init();
  virtual void initial_integrate(int);
  virtual void final_integrate();
  virtual double compute_vector(int);

  double memory_usage();
  void grow_arrays(int);
  void write_restart(FILE *fp);
  void restart(char *buf);

 protected:
  int me;
  double t_target;

  // read in
  FILE * input;
  char* keyword;
  double dStart,dStep,dStop;
  int Nd;
  double tStart,tStep,tStop;
  int Nt;
  double *self_data;
  double *cross_data;
  double *self_data_dist;
  double *self_data_ft;
  double *cross_data_ft;
  double *self_data_dist_ft;
  
  // system constants and data
  int d;
  double dtf, int_a,int_b;
  double **ran;
  double **fd;
  double **fr;
  double **x_save;
  int lastindexN,lastindexn;
  double **fc;
  double **array;

  class RanMars *random;
  
  // neighbor list
  int irequest;
  NeighList *list;
  
  // timing 
  double t1,t2;
  double time_read;
  double time_init;
  double time_int_rel1;
  double time_noise;
  double time_matrix_create;
  double time_forwardft;
  double time_forwardft_prep;
  double time_sqrt;
  double time_backwardft;
  double time_dist_update;
  double time_int_rel2;
  int k_tot;
  
  // sqrt_matrix
  int mLanczos;
  double tolLanczos;
  
  void read_input();
  void update_noise();
  void compute_step(int w, int* dist_pair_list, double **dr_pair_list, double* input, double* output);
};

}

#endif
#endif

/* ERROR/WARNING messages:

E: Illegal ... command

Self-explanatory.  Check the input script syntax and compare to the
documentation for the command.  You can use -echo screen as a
command-line option when running LAMMPS to see the offending line.

E: Fix gld series type must be pprony for now

Self-explanatory.

E: Fix gld prony terms must be > 0

Self-explanatory.

E: Fix gld start temperature must be >= 0

Self-explanatory.

E: Fix gld stop temperature must be >= 0

Self-explanatory.

E: Fix gld needs more prony series coefficients

Self-explanatory.

E: Fix gld c coefficients must be >= 0

Self-explanatory.

E: Fix gld tau coefficients must be > 0

Self-explanatory.

E: Cannot zero gld force for zero atoms

There are no atoms currently in the group.

*/
