/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing authors: Stephen Bond (SNL) and
                         Andrew Baczewski (Michigan State/SNL)
------------------------------------------------------------------------- */


/*
Careful:
-fix changes neighbor skin!
-> neighbor update (int Neighbor::check_distance()) does not make sence due to wrong skin
-> update every step necessary (not such a big problem since building the neighbour list is not the bottleneck in this kind of simulations)
*/

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "fix_gle_pair.h"
#include "math_extra.h"
#include "atom.h"
#include "force.h"
#include "pair.h"
#include "update.h"
#include "comm.h"
#include "input.h"
#include "variable.h"
#include "random_mars.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "neigh_request.h"
#include "memory.h"
#include "error.h"
#include "group.h"
#include "domain.h"
#include "kiss_fft.h"
#include "kiss_fftr.h"

using namespace LAMMPS_NS;
using namespace FixConst;
using namespace std;
using namespace Eigen;
typedef ConjugateGradient<SparseMatrix<double>,Lower, IncompleteCholesky<double> > ICCG;
typedef Eigen::Triplet<double> Td;
typedef Eigen::Triplet<int> Ti;

#define MAXLINE 1024
#define PI 3.14159265359


/* ----------------------------------------------------------------------
   Parses parameters passed to the method, allocates some memory
------------------------------------------------------------------------- */

FixGLEPair::FixGLEPair(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg)
{
  
  global_freq = 1;
  nevery = 1;
  peratom_freq = 1;
  vector_flag = 1;
  
  MPI_Comm_rank(world,&me);

  int narg_min = 5;
  if (narg < narg_min) error->all(FLERR,"Illegal fix gle/pair/jung command");

  t_target = force->numeric(FLERR,arg[3]);
  
  int seed = force->inumeric(FLERR,arg[4]);
  
  // read input file
  input = fopen(arg[5],"r");
  if (input == NULL) {
    char str[128];
    sprintf(str,"Cannot open fix gle/pair/jung file %s",arg[5]);
    error->one(FLERR,str);
  }
  keyword = arg[6];
  
  // Error checking for the first set of required input arguments
  if (seed <= 0) error->all(FLERR,"Illegal fix gle/pair/jung command");
  if (t_target < 0)
    error->all(FLERR,"Fix gle/pair/jung temperature must be >= 0");
  
  // Set number of dimensions
  d=3;
  d2=d*d;
  
  // Timing
  time_read = 0.0;
  time_init = 0.0;
  time_int_rel1 = 0.0;
  time_noise = 0.0;
  time_matrix_create = 0.0;
  time_forwardft = 0.0;
  time_eigenvalues = 0.0;
  time_chebyshev = 0.0;
  time_final_noise = 0.0;
  time_dist_update = 0.0;
  time_int_rel2 = 0.0;
  
  // read input file
  t1 = MPI_Wtime();
  read_input();
  t2 = MPI_Wtime();
  time_read += t2 -t1;
  
  // initialize Marsaglia RNG with processor-unique seed
  random = new RanMars(lmp,seed + comm->me);
  
  t1 = MPI_Wtime();
  memory->create(x_save, Nt, 3*atom->nlocal, "gle/pair/aux:x_save");
  memory->create(x_save_update,atom->nlocal,3, "gle/pair/aux:x_save_update");
  memory->create(fc, atom->nlocal, 3, "gle/pair/aux:fc");
  
  int *type = atom->type;
  double *mass = atom->mass;
  dtv = update->dt;
  dtf = 0.5 * update->dt * force->ftm2v;
  int_b = 1.0/(1.0+self_data[0]*update->dt/4.0/mass[type[0]]); // 4.0 because K_0 = 0.5*K(0)
  int_a = (1.0-self_data[0]*update->dt/4.0/mass[type[0]])*int_b; // 4.0 because K_0 = 0.5*K(0)
  printf("integration: int_a %f, int_b %f mem %f\n",int_a,int_b,self_data[0]);
  lastindexN = 0,lastindexn=0;
  Nupdate = 0;
  
    
  int nlocal = atom->nlocal;
  double **x = atom->x;
  double **f = atom->f;
  int k,i,j,n,t;
  
  // allocate memory
  int N = 2*Nt-2;
  memory->create(ran, N, atom->nlocal*d, "gle/pair/jung:ran");
  memory->create(fd, atom->nlocal,3, "gle/pair/jung:fd");
  memory->create(fr, atom->nlocal*d,3, "gle/pair/jung:fr");
  size_vector = atom->nlocal;
  
  // initialize forces
  for ( i=0; i< nlocal; i++) {
    for (int dim1=0; dim1<d; dim1++) { 
      fc[i][dim1] = 0.0;
      fd[i][dim1] = 0.0;
      fr[i][dim1] = 0.0;
    }
  }
  
  imageint *image = atom->image;
  tagint *tag = atom->tag;
  double unwrap[3];
  for (int t = 0; t < Nt; t++) {
    for (int i = 0; i < nlocal; i++) {
      domain->unmap(x[i],image[i],unwrap);
      for (int dim1=0; dim1<d; dim1++) { 
	x_save[t][3*i+dim1] = unwrap[dim1];
	x_save_update[i][dim1] = x[i][dim1];
      }
    }
  }
  
  for (int t = 0; t < N; t++) {
    for (int i = 0; i < nlocal; i++) {
      for (int dim1=0; dim1<d; dim1++) { 
	ran[t][d*i+dim1] = random->gaussian();
      }
    }
  }
  
  t2 = MPI_Wtime();
  time_init += t2 -t1;

}

/* ----------------------------------------------------------------------
   Destroys memory allocated by the method
------------------------------------------------------------------------- */

FixGLEPair::~FixGLEPair()
{

  delete random;
  memory->destroy(ran);

  memory->destroy(x_save);
  memory->destroy(x_save_update);
  
  memory->destroy(fc);
  memory->destroy(fd);
  memory->destroy(fr);
  
  delete [] cross_data;
  delete [] self_data;

}

/* ----------------------------------------------------------------------
   Specifies when the fix is called during the timestep
------------------------------------------------------------------------- */

int FixGLEPair::setmask()
{
  int mask = 0;
  mask |= INITIAL_INTEGRATE;
  mask |= FINAL_INTEGRATE;
  return mask;
}

/* ----------------------------------------------------------------------
   Initialize the method parameters before a run
------------------------------------------------------------------------- */

void FixGLEPair::init()
{
  // need a full neighbor list, built whenever re-neighboring occurs
  irequest = neighbor->request(this);
  neighbor->requests[irequest]->pair = 0;
  neighbor->requests[irequest]->fix = 1;
  neighbor->requests[irequest]->half = 0;
  neighbor->requests[irequest]->full = 1;
  
  //increase pair/cutoff to the target values
  if (!force->pair) {
    error->all(FLERR,"We need a pair potential to build neighbor-list! TODO: If this error appears, just create pair potential with zero amplitude\n");
  } else {
    int my_type = atom->type[0];
    double cutsq = dStop*dStop - force->pair->cutsq[my_type][my_type];
    if (cutsq > 0) {
      //increase skin
      neighbor->skin = dStop - sqrt(force->pair->cutsq[my_type][my_type]) + 0.3;
    }
    // since skin is increased neighbor needs to be updated every step
    char **c = (char**)&*(const char* const []){ "delay", "0", "every","1", "check", "no" };
    neighbor->modify_params(6,c);
  }
  isInitialized = 0;
  
  // FFT memory kernel for later processing
  int N = 2*Nt -2;
  self_data_ft = new double[Nt];
  cross_data_ft = new double[Nt*Nd];
  kiss_fft_scalar * buf;
  kiss_fft_cpx * bufout;
  buf=(kiss_fft_scalar*)KISS_FFT_MALLOC(sizeof(kiss_fft_scalar)*N*(1+Nd));
  bufout=(kiss_fft_cpx*)KISS_FFT_MALLOC(sizeof(kiss_fft_cpx)*N*(1+Nd));
  memset(bufout,0,sizeof(kiss_fft_cpx)*N*(1+Nd));
  for (int t=0; t<Nt; t++) {
    buf[t] = self_data[t];
    if (t==0 || t==Nt-1){ }
    else {
      buf[N-t] = self_data[t];
    }
  }
  for (int d=0; d< Nd; d++) {
    for (int t=0; t<Nt; t++) {
      buf[N*(d+1)+t] = cross_data[Nt*d+t];
      if (t==0 || t==Nt-1){ }
      else {
	buf[N*(d+1)+N-t] = cross_data[Nt*d+t];
      }
    }
  }
  
  kiss_fftr_cfg st = kiss_fftr_alloc( N ,0 ,0,0);
  for (int i=0; i<1+Nd;i++) {
    kiss_fftr( st ,&buf[i*N],&bufout[i*N] );
  }
  
  
  for (int t=0; t<Nt; t++) {
    self_data_ft[t] = bufout[t].r;
    //printf("%f\n",self_data_ft[t]);
  }
  for (int d=0; d< Nd; d++) {
    for (int t=0; t<Nt; t++) {
      cross_data_ft[Nt*d+t] = bufout[N*(d+1)+t].r;
    }
  }
  free(st);
}

/* ----------------------------------------------------------------------
   First half of a timestep (V^{n} -> V^{n+1/2}; X^{n} -> X^{n+1})
------------------------------------------------------------------------- */

void FixGLEPair::initial_integrate(int vflag)
{
  double dtfm;
  double ftm2v = force->ftm2v;

  double meff;
  double theta_qss, theta_qsc, theta_qcs, theta_qcc11, theta_qcc12;
  double theta_qsps, theta_qspc;
  int ind_coef, ind_q=0;
  int s,c,m;
  int indi,indd,indj;

  // update v and x of atoms in group
  double **x = atom->x;
  double **v = atom->v;
  double **f = atom->f;
  double *mass = atom->mass;
  int *type = atom->type;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;
  tagint *tag = atom->tag;
  if (igroup == atom->firstgroup) nlocal = atom->nfirst;
  
  
  // update noise
  for (int i = 0; i < nlocal; i++) {
    for (int dim1=0; dim1<d; dim1++) { 
      ran[lastindexN][d*i+dim1] = random->gaussian();
      fr[i][dim1] = 0.0;
      fd[i][dim1] = 0.0;
    }
  }
  t1 = MPI_Wtime();
  // initilize in the first step
  if (!isInitialized) {
    list = neighbor->lists[irequest];
    update_cholesky();
    isInitialized = 1;
  }
  t2 = MPI_Wtime();
  time_noise += t2 -t1;
  
  // determine random contribution
  int n = lastindexN;
  int N = 2*Nt -2;
  double sqrt_dt=sqrt(update->dt);
  /*for (int t = 0; t < N; t++) {
    for (int k=0; k<a[t].outerSize(); ++k) {
      int dim = k%d;
      int i = (k-dim)/d;
      for (SparseMatrix<double>::InnerIterator it(a[t],k); it; ++it) {
	fr[i][dim] += it.value()*ran[n][it.row()]*sqrt_dt;
      }   
    }
    n--;
    if (n==-1) n=2*Nt-3;
  }*/
  
  // determine dissipative contribution
    t1 = MPI_Wtime();
  /*n = lastindexn;
  m = lastindexn-1;
  if (m==-1) m=Nt-1;
  for (int t = 1; t < Nt; t++) {
    for (int k=0; k<A[t].outerSize(); ++k) {
      int dim = k%d;
      int i = (k-dim)/d;
      for (SparseMatrix<double>::InnerIterator it(A[t],k); it; ++it) {
	int dimj = it.row()%d;
	int j = (it.row()-dimj)/d;
	fd[i][dim] += it.value()* (x_save[n][3*j+dimj]-x_save[m][3*j+dimj]);
      }   
    }
    n--;
    m--;
    if (n==-1) n=Nt-1;
    if (m==-1) m=Nt-1;
  }*/
  t2 = MPI_Wtime();
  time_int_rel1 += t2 -t1;
  
  // Advance X by dt
  for (int i = 0; i < nlocal; i++) {
    if (mask[i] & groupbit) {
      meff = mass[type[i]];   
      //printf("x: %f f: %f fd: %f fr: %f\n",x[i][0],f_step[i][0],fd[i],fr[i]);
      for (int dim1=0; dim1<d; dim1++) { 
	/*x[i][dim1] += int_b * update->dt * v[i][dim1] 
	  + int_b * update->dt * update->dt / 2.0 / meff * fc[i][dim1] 
	  - int_b * update->dt / meff/ 2.0 * fd[i][dim1]
	  + int_b*update->dt/ 2.0 / meff * fr[i][dim1]; // convection, conservative, dissipative, random*/
      }
    }
  }
  
  lastindexN++;
  if (lastindexN == 2*Nt-2) lastindexN = 0;
  lastindexn++;
  if (lastindexn == Nt) lastindexn = 0;
  t1 = MPI_Wtime();
  // Check whether Cholesky Update is necessary
  double dr_max = 0.0;
  double dx,dy,dz,rsq;
  for (int i = 0; i < nlocal; i++) {
    dx = x_save_update[i][0] - x[i][0];
    dy = x_save_update[i][1] - x[i][1];
    dz = x_save_update[i][2] - x[i][2];
    rsq = dx*dx+dy*dy+dz*dz;
    if (rsq > dr_max) dr_max = rsq;
  }
  //printf("dr_max %f\n",dr_max);
  //if (dr_max > dStep*dStep/4.0) 
  {
    Nupdate++;
    for (int i = 0; i < nlocal; i++) {
      x_save_update[i][0] = x[i][0];
      x_save_update[i][1] = x[i][1];
      x_save_update[i][2] = x[i][2];
    }
    update_cholesky();
  }
  t2 = MPI_Wtime();
  time_noise += t2 -t1;
  
  t1 = MPI_Wtime();
  
  // Update positions
  imageint *image = atom->image;
  double unwrap[3];
  for (int i = 0; i < nlocal; i++) {
    domain->unmap(x[i],image[i],unwrap);
    x_save[lastindexn][3*i] = unwrap[0];
    x_save[lastindexn][3*i+1] = unwrap[1];
    x_save[lastindexn][3*i+2] = unwrap[2];
  }
  
  t2 = MPI_Wtime();
  time_dist_update += t2 -t1;
  
  
}

/* ----------------------------------------------------------------------
   Second half of a timestep (V^{n+1/2} -> V^{n+1})
------------------------------------------------------------------------- */

void FixGLEPair::final_integrate()
{

  double dtfm;
  double ftm2v = force->ftm2v;

  double meff;
  double theta_vs, alpha_vs, theta_vc, alpha_vc;
  int ind_coef, ind_q;

  t1 = MPI_Wtime();
  // update v and x of atoms in group
  double **x = atom->x;
  double **v = atom->v;
  double **f = atom->f;
  double *mass = atom->mass;
  int *type = atom->type;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;
  if (igroup == atom->firstgroup) nlocal = atom->nfirst;

  // Advance V by dt
  for (int i = 0; i < nlocal; i++) {
    if (mask[i] & groupbit) {
      // Calculate integration constants
      meff = mass[type[i]];   
      dtfm = dtf / meff;
      for (int dim1=0; dim1<d; dim1++) { 
	v[i][dim1] = int_a * v[i][dim1] 
	  + update->dt/2.0/meff * (int_a*fc[i][dim1] + f[i][dim1]) 
	  - int_b * fd[i][dim1]/meff 
	  + int_b*fr[i][dim1]/meff;
      }
    }
  }
  
  // save conservative force for integration
  for ( int i=0; i< nlocal; i++) {
    fc[i][0] = f[i][0];
    fc[i][1] = f[i][1];
    fc[i][2] = f[i][2];
  }

  // force equals .... (not yet implemented)
  for ( int i=0; i< nlocal; i++) {
    f[i][0] = fr[i][0];
    f[i][1] = fr[i][1];
    f[i][2] = fr[i][2];
  }

  t2 = MPI_Wtime();
  time_int_rel2 += t2 -t1;
  
      // print timing
  if (update->nsteps == update->ntimestep || update->ntimestep % 10000 == 0) {
    printf("Update %d times\n",Nupdate);
    printf("processor %d: time(read) = %f\n",me,time_read);
    printf("processor %d: time(init) = %f\n",me,time_init);
    printf("processor %d: time(int_rel1) = %f\n",me,time_int_rel1);
    printf("processor %d: time(noise) = %f\n",me,time_noise);
    printf("processor %d: time(matrix_create) = %f\n",me,time_matrix_create);
    printf("processor %d: time(forwardft) = %f\n",me,time_forwardft);
    printf("processor %d: time(eigenvalues) = %f\n",me,time_eigenvalues);
    printf("processor %d: time(chebyshev) = %f\n",me,time_chebyshev);
    printf("processor %d: time(final_noise) = %f\n",me,time_final_noise);
    printf("processor %d: time(int_rel2) = %f\n",me,time_int_rel2);
  }
  

}

/* ----------------------------------------------------------------------
   memory usage of local atom-based arrays
------------------------------------------------------------------------- */

double FixGLEPair::compute_vector(int n)
{
  tagint *tag = atom->tag;
  
  //printf("%d %d\n",t,i);
  
  return fr[n][0];
}


/* ----------------------------------------------------------------------
   memory usage of local atom-based arrays
------------------------------------------------------------------------- */

double FixGLEPair::memory_usage()
{
  double bytes = atom->nlocal*atom->nlocal*Nt*Nt*sizeof(double);
  return bytes;
}

/* ----------------------------------------------------------------------
   allocate local atom-based arrays
------------------------------------------------------------------------- */

void FixGLEPair::grow_arrays(int nmax)
{
  
}

/* ----------------------------------------------------------------------
   read input coefficients
------------------------------------------------------------------------- */
void FixGLEPair::read_input()
{
  char line[MAXLINE];
  
  // loop until section found with matching keyword

  while (1) {
    if (fgets(line,MAXLINE,input) == NULL)
      error->one(FLERR,"Did not find keyword in table file");
    if (strspn(line," \t\n\r") == strlen(line)) continue;  // blank line
    if (line[0] == '#') continue;                          // comment
    char *word = strtok(line," \t\n\r");
    if (strcmp(word,keyword) == 0) break;           // matching keyword
  }
  
  fgets(line,MAXLINE,input);
  char *word = strtok(line," \t\n\r\f");
  
  // default values
  Niter = 500;
  tStart= 0.0;
  tStep = 0.05;
  tStop = 5.0;
  
  while (word) {
    if (strcmp(word,"dStart") == 0) {
      word = strtok(NULL," \t\n\r\f");
      dStart = atof(word);
      printf("dStart %f\n",dStart);
    } else if (strcmp(word,"dStep") == 0) {
      word = strtok(NULL," \t\n\r\f");
      dStep = atof(word);
      printf("dStep %f\n",dStep);
    } else if (strcmp(word,"dStop") == 0) {
      word = strtok(NULL," \t\n\r\f");
      dStop = atof(word);
      printf("dStop %f\n",dStop);
    } else if (strcmp(word,"tStart") == 0) {
      word = strtok(NULL," \t\n\r\f");
      tStart = atof(word);
      printf("tStart %f\n",tStart);
    } else if (strcmp(word,"tStep") == 0) {
      word = strtok(NULL," \t\n\r\f");
      tStep = atof(word);
      printf("tStep %f\n",tStep);
    } else if (strcmp(word,"tStop") == 0) {
      word = strtok(NULL," \t\n\r\f");
      tStop = atof(word);
      printf("tStop %f\n",tStop);
    } else if (strcmp(word,"Niter") == 0) {
      word = strtok(NULL," \t\n\r\f");
      Niter = atoi(word);
      printf("Niter %d\n",Niter);
    } else {
      printf("WORD: %s\n",word);
      error->one(FLERR,"Invalid keyword in pair table parameters");
    }
    word = strtok(NULL," \t\n\r\f");
  }
  
  if (dStop < dStart)
    error->all(FLERR,"Fix gle/pair/aux dStop must be > dStart");
  Nd = (dStop - dStart) / dStep + 1.5;
  
  if (tStop < tStart)
    error->all(FLERR,"Fix gle/pair/aux tStop must be > tStart");
  Nt = (tStop - tStart) / tStep + 1.5;
  
    
  // initilize simulations for either TIME input (fitting necessary) or FIT input (only reading necessary)
  int d,t,i,n;
  double dummy_d, dummy_t,mem;
  
  self_data = new double[Nt];
  cross_data = new double[Nt*Nd];
  double *time = new double[Nt];
  
  //read self_memory
  for (t=0; t<Nt; t++) {
    fscanf(input,"%lf %lf %lf\n",&dummy_d, &dummy_t, &mem);
    self_data[t] = mem*update->dt;
    time[t] = dummy_t;
    //printf("%f\n",mem);
  }
  
  // read cross_memory
  for (int d=0; d< Nd; d++) {
    for (t=0; t<Nt; t++) {
      fscanf(input,"%lf %lf %lf\n",&dummy_d, &dummy_t, &mem);
      cross_data[Nt*d+t] = mem*update->dt;
      //printf("%f %f %f\n",dummy_d,dummy_t,mem);
    }
  }
  delete [] time;
  fclose(input);
  
}

/* ----------------------------------------------------------------------
   updates the interaction amplitudes by cholesky decomposition
------------------------------------------------------------------------- */
void FixGLEPair::update_cholesky() 
{
  // initialize input matrix
  int dist,t;
  int *type = atom->type;
  int nlocal = atom->nlocal;
  int *tag = atom->tag;
  double **x = atom->x;
  int i,j,ii,jj,inum,jnum,itype,jtype,itag,jtag;
  double xtmp,ytmp,ztmp,rsq,rsqi;
  double *dr = new double[3];
  int *ilist,*jlist,*numneigh,**firstneigh;
  neighbor->build_one(list);
  inum = list->inum;
  ilist = list->ilist;
  numneigh = list->numneigh;
  firstneigh = list->firstneigh;
  int non_zero=0;
  double t1 = MPI_Wtime();
  std::vector<Td> tripletListd;
  std::vector<Ti> tripletListi;
  Eigen::SparseMatrix<double > A_FT(d*nlocal,d*nlocal);
  Eigen::SparseMatrix<int > A_dist(d*nlocal,d*nlocal);

  // step 1: create matrix A_FT (for t=0)
  int counter = 0;
  int N = 2*Nt-2;
  int size = d*nlocal;
  for (int ii = 0; ii < inum; ii++) {
    i = ilist[ii];
    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];
    itype = type[i];
    itag = tag[i]-1;
    jlist = firstneigh[i];
    jnum = numneigh[i];
      
    // set self-correlation
    for (int dim1=0; dim1<d;dim1++) {
      tripletListd.push_back(Td(itag*d+dim1,itag*d+dim1,self_data_ft[0]));
      tripletListi.push_back(Ti(itag*d+dim1,itag*d+dim1,-1));
    }
      
    //set cross-correlation
    for (jj = 0; jj < jnum; jj++) {
      j = jlist[jj];
      j &= NEIGHMASK;
      jtype = type[j];
      jtag = tag[j]-1;
	
      dr[0] = xtmp - x[j][0];
      dr[1] = ytmp - x[j][1];
      dr[2] = ztmp - x[j][2];

      rsq = dr[0]*dr[0] + dr[1]*dr[1] + dr[2]*dr[2];
      rsqi = 1/rsq;
      dist = (sqrt(rsq) - dStart)/dStep;
	    
      if (dist < 0) {
	error->all(FLERR,"Particles closer than lower cutoff in fix/pair/jung\n");
      } else if (dist < Nd) {
	for (int dim1=0; dim1<d;dim1++) {
	  for (int dim2=0; dim2<d;dim2++) {
	    double proj = dr[dim1]*dr[dim2]*rsqi;
	    tripletListd.push_back(Td(itag*d+dim1,jtag*d+dim2,cross_data_ft[dist*Nt]*proj));
	    tripletListi.push_back(Ti(itag*d+dim1,jtag*d+dim2,dist));
	  }
	}
      }
    }
  }
  A_FT.setFromTriplets(tripletListd.begin(), tripletListd.end());
  A_dist.setFromTriplets(tripletListi.begin(), tripletListi.end());
  delete [] dr;
  double t2 = MPI_Wtime();
  time_matrix_create += t2-t1;

  // step 2: determine FT noise vector
  t1 = MPI_Wtime();
  kiss_fft_scalar * buf;
  kiss_fft_cpx * bufout;
  buf=(kiss_fft_scalar*)KISS_FFT_MALLOC(sizeof(kiss_fft_scalar)*size*N);
  bufout=(kiss_fft_cpx*)KISS_FFT_MALLOC(sizeof(kiss_fft_cpx)*size*N);
  memset(bufout,0,sizeof(kiss_fft_cpx)*size*N);
  int nt0 = lastindexN;
  for (int t = 0; t < N; t++) {
    int ind = Nt-1+t;
    if (ind >= N) ind -= N;
    for (int i=0; i<size;i++) {
      buf[i*N+ind]=ran[nt0][i];
    }
    nt0--;
    if (nt0==-1) nt0=2*Nt-3;
  }  
  // FFT evaluation
  kiss_fftr_cfg st = kiss_fftr_alloc( N ,0 ,0,0);
  for (int i=0; i<size;i++) {
    kiss_fftr( st ,&buf[i*N],&bufout[i*N] );
  }
  
  t2 = MPI_Wtime();
  time_forwardft += t2-t1;
  //printf("-------------------------------\n");
  
  // step 3: use lanczos method to compute sqrt-Matrix
  t1 = MPI_Wtime();
  int mLanczos = 50;
  double tolLanczos = 0.00001;

  // main Lanczos loop, determine eigenvalue bounds
  // choose twice the size to include complex values
  std::vector<VectorXd> FT_w;
  
  for (int t=0; t<Nt; t++) {
    double t1 = MPI_Wtime();
    if (t>0) {
      int counter = 0;
      for (int k=0; k<A_FT.outerSize(); ++k) {
	for (SparseMatrix<double>::InnerIterator it(A_FT,k); it; ++it) {
	  int dist = A_dist.valuePtr()[counter];
	  //printf("%d\n",dist);
	  if (dist == -1) {
	    it.valueRef() *= self_data_ft[t]/self_data_ft[t-1];
	    //printf("%f\n",it.valueRef());
	  }
	  else it.valueRef() *= cross_data_ft[dist*Nt+t]/cross_data_ft[dist*Nt+t-1];
	  counter++;
	}
      }
    }
    double t2 = MPI_Wtime();
    time_chebyshev += t2-t1;
    //cout << A_FT << endl;
    for (int s = 0; s<2; s++) { 
      Eigen::MatrixXd Vn;
      //printf("test0\n");
      Vn.resize(size,1);
      //printf("test1\n");
      VectorXd rk = VectorXd::Zero(size);
      double *alpha = new double[mLanczos+1];
      double *beta = new double[mLanczos+1];
      for (int k=0; k<mLanczos+1; k++) {
	alpha[k] = 0.0;
	beta[k] = 0.0;
      }
      // generate random vector v and normalize
      double norm = 0.0, random=0.0;
      Vn.col(0) = VectorXd::Zero(size);
      srand (time(NULL));
      for (int i=0; i< size; i++) {
	if (s==0) Vn(i,0) = bufout[i*N+t].r;
	else Vn(i,0) = bufout[i*N+t].i;
      }
      norm = Vn.col(0).norm();
      Vn.col(0) = Vn.col(0).normalized();
      rk = A_FT * Vn.col(0);
      alpha[1] = (Vn.col(0).adjoint()*rk).value();
      //printf("\n");
      for (int k=2; k<=mLanczos; k++) {
	rk = rk - alpha[k-1]*Vn.col(k-2);
	if (k>2) rk -= beta[k-2]*Vn.col(k-3);
	beta[k-1] = rk.norm();
	// set new v
	//printf("test2\n");
	Vn.conservativeResize(size,k);
	//cout << Vn.col(0) << endl;
	//printf("test3\n");
	Vn.col(k-1) = rk.normalized();
      
	rk = A_FT * Vn.col(k-1);
	alpha[k] = (Vn.col(k-1).adjoint()*rk).value();

	//printf("Lanczos Step %d: alpha %f, beta %f evMin %f, evMax %f\n",k,alpha[k-1],beta[k],evMin,evMax);
	if (k>=2) {
	  //generate eigenvalues/eigenvectors and check whether they fullfill the tolerance
	  Eigen::MatrixXd Hk = Eigen::MatrixXd::Zero(k,k);
	  for (int i=0; i<k; i++) {
	    for (int j=0; j<k; j++) {
	      if (i==j) Hk(i,j) = alpha[i+1];
	      if (i==j+1||i==j-1) {
		int kprime = j;
		if (i>j) kprime = i;
		Hk(i,j) = beta[kprime];
	      }
	    }
	  }
	  //printf("Hk-Matrix\n");
	  //cout << Hk << endl;
	  //Eigen::LLT<MatrixXd > Hk_comp(Hk);
	  //MatrixXd f_Hk = Hk_comp.matrixL();
	  // determine sqrt-matrix
	  Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> Hk_eigen(Hk);
	  Eigen::MatrixXd Hk_eigenvector = Hk_eigen.eigenvectors().real();
	  if (Hk_eigen.info()!=0) {
	    cout << Hk_eigen.eigenvalues() << endl;
	    printf("A not positive-definite!\n");
	  }  
	  // create square root matrix
	  Eigen::MatrixXd Hk_diag = Eigen::MatrixXd::Zero(k,k);
	  for (int i=0; i<k; i++) {
	    Hk_diag(i,i) = sqrt(Hk_eigen.eigenvalues().real()(i));
	  }
	  MatrixXd f_Hk = Hk_eigenvector * Hk_diag * Hk_eigenvector.transpose();
	  VectorXd e1 = VectorXd::Zero(k);
	  e1(0) = 1.0;
	  VectorXd f_Hk1 = f_Hk * e1;
	  VectorXd xk = Vn*f_Hk1*norm; 
	  if (k==2) FT_w.push_back(xk);
	  else {
	    VectorXd diff = (FT_w[2*t+s] - xk);
	    double diff_norm = diff.norm();
	    FT_w[2*t+s] = xk;
	    if (diff_norm < tolLanczos) {
	      printf("%d\n",k);
	      break;
	    }
	    //printf("%d %f\n",k,diff_norm);
	  }
	  //printf("sol %d\n",k);
	  //cout << xk << endl;
	}
      }
      //cout << A_FT[t] << endl;
      //printf("Lanczos EV-bounds %d: evMin = %f, evMax = %f\n",k,evMin[t],evMax[t]);
      delete [] alpha;
      delete [] beta;
    
      // compare results
      //Eigen::LLT<MatrixXd > A_chol(MatrixXd(A_FT[t]));
      //MatrixXd f_A = MatrixXd(A_FT[t]).sqrt();
      //cout << f_A << endl;
      //cout << Vn.col(0) << endl;
      //VectorXd xn = f_A * Vn.col(0);
      //printf("sol exact\n");
      //cout << xn << endl;
    }
  }

  //printf("done approx\n");
  
  t2 = MPI_Wtime();
  time_eigenvalues += t2-t1;
  
    t1 = MPI_Wtime();
  // transform back
  memset(bufout,0,sizeof(kiss_fft_cpx)*size*N);
  for (int t = 0; t < Nt; t++) {
    for (int i=0; i<size;i++) {
      bufout[i*N+t].r = FT_w[2*t](i);
      bufout[i*N+t].i = FT_w[2*t+1](i);
    }
  }
  
  kiss_fftr_cfg sti = kiss_fftr_alloc( N ,1 ,0,0);
  for (int i=0; i<size;i++) {
    kiss_fftri( sti ,&bufout[i*N],&buf[i*N] );
  }
  
  free(st); free(sti);
  kiss_fft_cleanup();

  //printf("FT result\n");
  for (int i=0; i<nlocal;i++) {
    fr[i][0]=buf[(3*i+0)*N]/N*sqrt(update->dt);
    fr[i][1]=buf[(3*i+1)*N]/N*sqrt(update->dt);
    fr[i][2]=buf[(3*i+2)*N]/N*sqrt(update->dt);
    //printf("lanc %f %f %f\n",fr[i][0],fr[i][1],fr[i][2]);
  }
  t2 = MPI_Wtime();
  time_final_noise += t2-t1;
  free(buf); free(bufout);
  
 
}


