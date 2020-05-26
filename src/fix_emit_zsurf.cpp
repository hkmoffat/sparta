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

#include "stdlib.h"
#include "string.h"
#include "fix_emit_zsurf.h"
#include "update.h"
#include "domain.h"
#include "region.h"
#include "particle.h"
#include "mixture.h"
#include "surf.h"
#include "modify.h"
#include "cut2d.h"
#include "cut3d.h"
#include "input.h"
#include "comm.h"
#include "random_park.h"
#include "math_extra.h"
#include "math_const.h"
#include "memory.h"
#include "error.h"

#include "surf_state.h"
#include "zuzax_setup.h"


using namespace SPARTA_NS;
using namespace MathConst;

#ifdef USE_ZSURF

enum {PKEEP,PINSERT,PDONE,PDISCARD,PENTRY,PEXIT,PSURF};  // several files
enum {NOSUBSONIC,PTBOTH,PONLY};

#define DELTATASK 256
#define TEMPLIMIT 1.0e5

/* ---------------------------------------------------------------------- */

FixEmitZSurf::FixEmitZSurf(SPARTA* sparta, int narg, char** arg) :
  FixEmit(sparta, narg, arg)
{
  if (narg < 4) {
    error->all(FLERR,"Illegal fix emit/surf command");
  }

  imix = particle->find_mixture(arg[2]);
  if (imix < 0) {
    error->all(FLERR,"Fix emit/surf mixture ID does not exist");
  }

  // Too complicated to figure out mixtures, Keep the sparta species
  // vectors simple.
  if (strcmp(arg[2], "all") != 0) {
    error->all(FLERR,"Fix emit/Zsurf mixture ID must equal \"all\"");
  }

  int igroup = surf->find_group(arg[3]);
  if (igroup < 0) {
    error->all(FLERR,"Fix emit/surf group ID does not exist");
  }
  groupbit = surf->bitmask[igroup];

  // Always perspecies defined
  perspecies = 1;
  // optional args

  np = 0;
  normalflag = 0;

  int iarg = 4;
  options(narg-iarg,&arg[iarg]);

  // error checks

  if (!surf->exist) {
    error->all(FLERR,"Fix emit/surf requires surface elements");
  }
  if (surf->distributed)
    error->all(FLERR,
               "Cannot yet use fix emit/surf with distributed surf elements");
  if (np > 0 && perspecies) {
    error->all(FLERR,"Cannot use fix emit/face n > 0 with perspecies yes");
  }

  // task list and subsonic data structs

  tasks = NULL;
  ntask = ntaskmax = 0;

  maxactive = 0;
  activecell = NULL;
}

/* ---------------------------------------------------------------------- */

FixEmitZSurf::~FixEmitZSurf()
{
  for (int i = 0; i < ntaskmax; i++) {
    delete [] tasks[i].path;
    delete [] tasks[i].fracarea;
  }
  memory->sfree(tasks);
  memory->destroy(activecell);

  delete cut3d;
  delete cut2d;
}

/* ---------------------------------------------------------------------- */

void FixEmitZSurf::init()
{
  // copies of class data before invoking parent init() and count_task()

  dimension = domain->dimension;
  fnum = update->fnum;
  dt = update->dt;

  nspecies = particle->mixture[imix]->nspecies;
  fraction = particle->mixture[imix]->fraction;
  cummulative = particle->mixture[imix]->cummulative;

  lines = surf->lines;
  tris = surf->tris;

  // subsonic prefactor

  tprefactor = update->mvv2e / (3.0*update->boltz);

  // mixture soundspeed, used by subsonic PONLY as default cell property

  double avegamma = 0.0;
  double avemass = 0.0;

  for (int m = 0; m < nspecies; m++) {
    int ispecies = particle->mixture[imix]->species[m];
    avemass += fraction[m] * particle->species[ispecies].mass;
    avegamma += fraction[m] * (1.0 + 2.0 /
                               (3.0 + particle->species[ispecies].rotdof));
  }

  soundspeed_mixture = sqrt(avegamma * update->boltz *
                            particle->mixture[imix]->temp_thermal / avemass);

  // create instance of Cut2d,Cut3d for geometry calculations

  if (dimension == 3) {
    cut3d = new Cut3d(sparta);
  } else {
    cut2d = new Cut2d(sparta,domain->axisymmetric);
  }

  // magvstream = magnitude of mixture vstream vector
  // norm_vstream = unit vector in stream direction

  double* vstream = particle->mixture[imix]->vstream;
  magvstream = MathExtra::len3(vstream);

  norm_vstream[0] = vstream[0];
  norm_vstream[1] = vstream[1];
  norm_vstream[2] = vstream[2];
  if (norm_vstream[0] != 0.0 || norm_vstream[1] != 0.0 ||
      norm_vstream[2] != 0.0) {
    MathExtra::norm3(norm_vstream);
  }

  // if used, reallocate ntargetsp and vscale for each task
  // b/c nspecies count of mixture may have changed

  //if (perspecies) {
  //  for (int i = 0; i < ntask; i++) {
  //delete [] tasks[i].ntargetsp;
  //tasks[i].ntargetsp = new double[nspecies];
  //  }
  //}

  // invoke FixEmit::init() to populate task list
  // it calls create_task() for each grid cell

  ntask = 0;
  FixEmit::init();

  // if Np > 0, nper = # of insertions per task
  // set nthresh so as to achieve exactly Np insertions
  // tasks > tasks_with_no_extra need to insert 1 extra particle
  // NOTE: setting same # of insertions per task
  //       should weight by overlap area of cell/surf

  if (np > 0) {
    int all,nupto,tasks_with_no_extra;
    MPI_Allreduce(&ntask,&all,1,MPI_INT,MPI_SUM,world);
    if (all) {
      npertask = np / all;
      tasks_with_no_extra = all - (np % all);
    } else {
      npertask = tasks_with_no_extra = 0;
    }

    MPI_Scan(&ntask,&nupto,1,MPI_INT,MPI_SUM,world);
    if (tasks_with_no_extra < nupto-ntask) {
      nthresh = 0;
    } else if (tasks_with_no_extra >= nupto) {
      nthresh = ntask;
    } else {
      nthresh = tasks_with_no_extra - (nupto-ntask);
    }
  }

  // deallocate Cut2d,Cut3d

  if (dimension == 3) {
    delete cut3d;
  } else {
    delete cut2d;
  }
}

/* ---------------------------------------------------------------------- */

void FixEmitZSurf::setup()
{
  // needed for Kokkos because pointers are changed in UpdateKokkos::setup()

  lines = surf->lines;
  tris = surf->tris;
}

/* ---------------------------------------------------------------------- */

void FixEmitZSurf::create_task(int icell)
{
  int i,m,isurf,isp,npoint,isplit,subcell;
  double indot,area,areaone,ntargetsp;
  double* normal,*p1,*p2,*p3,*path;
  double cpath[36],delta[3],e1[3],e2[3];

  Grid::ChildCell* cells = grid->cells;
  Grid::ChildInfo* cinfo = grid->cinfo;
  Grid::SplitInfo* sinfo = grid->sinfo;

  double nrho = particle->mixture[imix]->nrho;
  double* vstream = particle->mixture[imix]->vstream;
  double* vscale = particle->mixture[imix]->vscale;

  // no tasks if no surfs in cell

  if (cells[icell].nsurf == 0) {
    return;
  }

  // loop over surfs in cell
  // use Cut2d/Cut3d to find overlap area and geoemtry of overlap

  int ntaskorig = ntask;

  double* lo = cells[icell].lo;
  double* hi = cells[icell].hi;
  surfint* csurfs = cells[icell].csurfs;
  int nsurf = cells[icell].nsurf;

  for (i = 0; i < nsurf; i++) {
    isurf = csurfs[i];

    if (dimension == 2) {
      if (!(lines[isurf].mask & groupbit)) {
        continue;
      }
    } else {
      if (!(tris[isurf].mask & groupbit)) {
        continue;
      }
    }

    // set cell parameters of task
    // pcell = sub cell for particles if a split cell

    if (ntask == ntaskmax) {
      grow_task();
    }

    tasks[ntask].icell = icell;
    tasks[ntask].isurf = isurf;
    if (cells[icell].nsplit == 1) {
      tasks[ntask].pcell = icell;
    } else {
      isplit = cells[icell].isplit;
      subcell = sinfo[isplit].csplits[i];
      tasks[ntask].pcell = sinfo[isplit].csubs[subcell];
    }

    // set geometry-dependent params of task
    // indot = vstream magnitude for normalflag = 1
    // indot = vstream dotted with surface normal for normalflag = 0
    // area = area for insertion = extent of line/triangle inside grid cell

    if (dimension == 2) {
      normal = lines[isurf].norm;
      if (normalflag) {
        indot = magvstream;
      } else {
        indot = vstream[0]*normal[0] + vstream[1]*normal[1];
      }

      p1 = lines[isurf].p1;
      p2 = lines[isurf].p2;
      npoint = cut2d->clip_external(p1,p2,lo,hi,cpath);
      if (npoint < 2) {
        continue;
      }

      tasks[ntask].npoint = 2;
      delete [] tasks[ntask].path;
      tasks[ntask].path = new double[6];
      path = tasks[ntask].path;
      path[0] = cpath[0];
      path[1] = cpath[1];
      path[2] = 0.0;
      path[3] = cpath[2];
      path[4] = cpath[3];
      path[5] = 0.0;

      // axisymmetric "area" of line segment = surf area of truncated cone
      // PI (y1+y2) sqrt( (y1-y2)^2 + (x1-x2)^2) )

      if (domain->axisymmetric) {
        double sqrtarg = (path[1]-path[4])*(path[1]-path[4]) +
                         (path[0]-path[3])*(path[0]-path[3]);
        area = MY_PI * (path[1]+path[4]) * sqrt(sqrtarg);
      } else {
        MathExtra::sub3(&path[0],&path[3],delta);
        area = MathExtra::len3(delta);
      }
      tasks[ntask].area = area;

      // set 2 tangent vectors to surf normal
      // tan1 is in xy plane, 90 degrees from normal
      // tan2 is unit +z vector

      tasks[ntask].tan1[0] = normal[1];
      tasks[ntask].tan1[1] = -normal[0];
      tasks[ntask].tan1[2] = 0.0;
      tasks[ntask].tan2[0] = 0.0;
      tasks[ntask].tan2[1] = 0.0;
      tasks[ntask].tan2[2] = 1.0;

    } else {
      normal = tris[isurf].norm;
      if (normalflag) {
        indot = magvstream;
      } else indot = vstream[0]*normal[0] + vstream[1]*normal[1] +
                       vstream[2]*normal[2];

      p1 = tris[isurf].p1;
      p2 = tris[isurf].p2;
      p3 = tris[isurf].p3;
      npoint = cut3d->clip_external(p1,p2,p3,lo,hi,cpath);
      if (npoint < 3) {
        continue;
      }

      tasks[ntask].npoint = npoint;
      delete [] tasks[ntask].path;
      tasks[ntask].path = new double[npoint*3];
      path = tasks[ntask].path;
      memcpy(path,cpath,npoint*3*sizeof(double));
      delete [] tasks[ntask].fracarea;
      tasks[ntask].fracarea = new double[npoint-2];

      area = 0.0;
      p1 = &path[0];
      for (m = 0; m < npoint-2; m++) {
        p2 = &path[3*(m+1)];
        p3 = &path[3*(m+2)];
        MathExtra::sub3(p2,p1,e1);
        MathExtra::sub3(p3,p1,e2);
        MathExtra::cross3(e1,e2,delta);
        areaone = fabs(0.5*MathExtra::len3(delta));
        area += areaone;
        tasks[ntask].fracarea[m] = area;
      }
      tasks[ntask].area = area;
      for (m = 0; m < npoint-2; m++) {
        tasks[ntask].fracarea[m] /= area;
      }

      // set 2 random tangent vectors to surf normal
      // tangent vectors are also normal to each other

      delta[0] = random->uniform();
      delta[1] = random->uniform();
      delta[2] = random->uniform();
      MathExtra::cross3(tris[isurf].norm,delta,tasks[ntask].tan1);
      MathExtra::norm3(tasks[ntask].tan1);
      MathExtra::cross3(tris[isurf].norm,tasks[ntask].tan1,tasks[ntask].tan2);
      MathExtra::norm3(tasks[ntask].tan2);

      SurfState* sState = tris[isurf].surfaceState;
      if (sState == nullptr) {
        error->allf(FLERR,"Surface %d doesn't have state malloced on it ", isurf);
      }
      if (!net) {
        net = sState->net;
      } else {
        if (net != sState->net) {
          error->allf(FLERR,"Surface State net on %d is different. May indicate different chem", isurf);
        }
      }
      sState->Area = area;
    }

    // set ntarget and ntargetsp via mol_inflow()
    // skip task if final ntarget = 0.0, due to large outbound vstream
    // do not skip for subsonic since it resets ntarget every step

    //tasks[ntask].ntarget = 0.0;
    for (isp = 0; isp < nspecies; isp++) {
      ntargetsp = mol_inflow(indot,vscale[isp],fraction[isp]);
      ntargetsp *= nrho*area*dt / fnum;
      ntargetsp /= cinfo[icell].weight;
      //tasks[ntask].ntarget += ntargetsp;
      //if (perspecies) tasks[ntask].ntargetsp[isp] = ntargetsp;
    }

    // Initialize other task values with mixture properties
    // HKM Need vstream because a set of surfaces may be moving wrt the gas phase

    tasks[ntask].nrho = particle->mixture[imix]->nrho;
    tasks[ntask].temp_thermal = particle->mixture[imix]->temp_thermal;
    tasks[ntask].temp_rot = particle->mixture[imix]->temp_rot;
    tasks[ntask].temp_vib = particle->mixture[imix]->temp_vib;
    tasks[ntask].vstream[0] = particle->mixture[imix]->vstream[0];
    tasks[ntask].vstream[1] = particle->mixture[imix]->vstream[1];
    tasks[ntask].vstream[2] = particle->mixture[imix]->vstream[2];

    // increment task counter

    ntask++;
  }

  return;
}

/* ----------------------------------------------------------------------
   insert particles in grid cells with emitting surface elements
------------------------------------------------------------------------- */

void FixEmitZSurf::perform_task()
{
  int i,m,n,pcell,isurf,ninsert,nactual,isp,ispecies,ntri,id;
  double indot,scosine,rn,ntarget,vr,alpha,beta;
  double beta_un,normalized_distbn_fn,theta,erot,evib;
  double vnmag,vamag,vbmag;
  double* normal,*p1,*p2,*p3,*atan,*btan,*vstream,*vscale;
  double x[3],v[3],e1[3],e2[3];
  Particle::OnePart* p;

  double dt = update->dt;
  int* species = particle->mixture[imix]->species;


  // insert particles for each task = cell/surf pair
  // ntarget/ninsert is either perspecies or for all species
  // for one particle:
  //   x = random position with overlap of surf with cell
  //   v = randomized thermal velocity + vstream
  //       if normalflag, mag of vstream is applied to surf normal dir
  //       first stage: normal dimension (normal)
  //       second stage: parallel dimensions (tan1,tan2)

  // double while loop until randomized particle velocity meets 2 criteria
  // inner do-while loop:
  //   v = vstream-component + vthermal is into simulation box
  //   see Bird 1994, p 425
  // outer do-while loop:
  //   shift Maxwellian distribution by stream velocity component
  //   see Bird 1994, p 259, eq 12.5

  // Locate the  face and the created surf_state  object


  SurfState* surfState = nullptr;

  int nfix_add_particle = modify->n_add_particle;
  indot = magvstream;

  for (i = 0; i < ntask; i++) {
    pcell = tasks[i].pcell;
    isurf = tasks[i].isurf;
    if (dimension == 2) {
      normal = lines[isurf].norm;
    } else {
      normal = tris[isurf].norm;
    }
    atan = tasks[i].tan1;
    btan = tasks[i].tan2;

    temp_thermal = tasks[i].temp_thermal;
    temp_rot = tasks[i].temp_rot;
    temp_vib = tasks[i].temp_vib;

    vstream = tasks[i].vstream;

    if (!normalflag) indot = vstream[0]*normal[0] + vstream[1]*normal[1] +
                               vstream[2]*normal[2];

    // Restore the net object to the state of this face
    if (dimension == 2) {
      surfState = lines[isurf].surfaceState;
    } else {
      surfState = tris[isurf].surfaceState;
    }
    surfState->setState(update->ntimestep, dt);

    std::vector< struct Zuzax::PartToSurf >& surfInitPSTaskList = surfState->m_surfInitPSTaskList;

    std::vector< struct Zuzax::ZTask >& surfInitTaskList = surfState->surfInitTaskList;

    // Indexing in the next loop is ok because we are using mix "all" by design
    for (int k = 0; k < nspecies; k++) {
      vscale[k] = sqrt(2.0 * update->boltz * temp_thermal / particle->species[k].mass);
    }

    // Zero counters for the events that actually occured.
    for (struct Zuzax::ZTask& zt : surfInitTaskList) {
      zt.nintervalActual = 0;
    }

    // Loop over reactions that create gas phase particle creation events
    for (struct Zuzax::ZTask& zt : surfInitTaskList) {

      // Calculate the area-corrected # of events to occur on that surface
      double neventsCorrect = zt.nCAvgEvents * tasks[i].area / zt.area;
      // Add the random factor before doing the int cutoff
      ntarget = neventsCorrect + random->uniform();
      // This is the number of discrete reaction events that will be inserted
      ninsert = static_cast<int>(ntarget);

      if (ninsert > 0) {

        // Do the reaction within the surface tracker.
        int iPos; // not used
        size_t kGasOut[3];  // not used
        for (int itimes = 0; itimes < ninsert; ++itimes) {

          bool ok = net->doExplicitReaction(true, Zuzax::npos, zt.irxn, zt.rxn_dir,
                                            update->fnum, temp_thermal, iPos, kGasOut);
          // If the reaction doesn't occur then don't register it, do nothing
          if (!ok) {
            printf("REACTION DID NOT OCCUR\n");
          }
          if (ok) {
            // Now carry out the particle creation events
            for (Zuzax::SpeciesAmount& sa : zt.ntargetsp) {
              size_t kGas = sa.first;
              double stoich = sa.second;

              int ispecies = zuzax_setup->ZutoSp_speciesMap[kGas];

              int nCreate = stoich;

              // Details of particle creation

              scosine = indot / vscale[isp];

              for (int m = 0; m < nCreate; m++) {
                if (dimension == 2) {
                  rn = random->uniform();
                  p1 = &tasks[i].path[0];
                  p2 = &tasks[i].path[3];
                  x[0] = p1[0] + rn * (p2[0]-p1[0]);
                  x[1] = p1[1] + rn * (p2[1]-p1[1]);
                  x[2] = 0.0;
                } else {
                  rn = random->uniform();
                  ntri = tasks[i].npoint - 2;
                  for (n = 0; n < ntri; n++)
                    if (rn < tasks[i].fracarea[n]) {
                      break;
                    }
                  p1 = &tasks[i].path[0];
                  p2 = &tasks[i].path[3*(n+1)];
                  p3 = &tasks[i].path[3*(n+2)];
                  MathExtra::sub3(p2,p1,e1);
                  MathExtra::sub3(p3,p1,e2);
                  alpha = random->uniform();
                  beta = random->uniform();
                  if (alpha+beta > 1.0) {
                    alpha = 1.0 - alpha;
                    beta = 1.0 - beta;
                  }
                  x[0] = p1[0] + alpha*e1[0] + beta*e2[0];
                  x[1] = p1[1] + alpha*e1[1] + beta*e2[1];
                  x[2] = p1[2] + alpha*e1[2] + beta*e2[2];
                }

                if (region && !region->match(x)) {
                  continue;
                }

                do {
                  do {
                    beta_un = (6.0*random->uniform() - 3.0);
                  } while (beta_un + scosine < 0.0);
                  normalized_distbn_fn = 2.0 * (beta_un + scosine) /
                                         (scosine + sqrt(scosine*scosine + 2.0)) *
                                         exp(0.5 + (0.5*scosine)*(scosine-sqrt(scosine*scosine + 2.0)) -
                                             beta_un*beta_un);
                } while (normalized_distbn_fn < random->uniform());


                if (normalflag) {
                  vnmag = beta_un*vscale[isp] + magvstream;
                } else {
                  vnmag = beta_un*vscale[isp] + indot;
                }

                theta = MY_2PI * random->uniform();
                vr = vscale[isp] * sqrt(-log(random->uniform()));
                if (normalflag) {
                  vamag = vr * sin(theta);
                  vbmag = vr * cos(theta);
                } else {
                  vamag = vr * sin(theta) + MathExtra::dot3(vstream,atan);
                  vbmag = vr * cos(theta) + MathExtra::dot3(vstream,btan);
                }

                v[0] = vnmag*normal[0] + vamag*atan[0] + vbmag*btan[0];
                v[1] = vnmag*normal[1] + vamag*atan[1] + vbmag*btan[1];
                v[2] = vnmag*normal[2] + vamag*atan[2] + vbmag*btan[2];

                erot = particle->erot(ispecies,temp_rot,random);
                evib = particle->evib(ispecies,temp_vib,random);
                id = MAXSMALLINT*random->uniform();

                // add the particle
                particle->add_particle(id,ispecies,pcell,x,v,erot,evib);

                p = &particle->particles[particle->nlocal-1];
                p->flag = PINSERT;
                p->dtremain = dt * random->uniform();

                if (nfix_add_particle)
                  modify->add_particle(particle->nlocal-1,temp_thermal,
                                       temp_rot,temp_vib,vstream);
              } // loop over single particle insertion of a single type of species

              nsingle += nCreate;
            } // loop over gas phase species that are products of the reaction
          } // ok
        } // for loop over ninsert
      } // if (ninsert > 0)
    } // Loop over reactions that create gas phase particle creation events

    // Save the state of the surface, we've changed it by calling doExplicitReaction
    // and we are now going to another surface but using the same function
    surfState->saveState();

  } // Loop over surfaces

}

/* ---------------------------------------------------------------------- */

int FixEmitZSurf::setmask()
{
  int mask = 0;
  mask |= START_OF_STEP;
  mask |= END_OF_STEP;
  return mask;
}


/* ----------------------------------------------------------------------
   end_of_step() hook
------------------------------------------------------------------------- */
void FixEmitZSurf::end_of_step()
{
  int pcell, isurf;
  // printf("FixEmitZSurf::end_of_step() we are here\n");
  double deltaT = update->dt;
  double time = deltaT * update->ntimestep;
  int n = update->ntimestep;
  SurfState* surfState;

  for (int i = 0; i < ntask; i++) {
    pcell = tasks[i].pcell;
    isurf = tasks[i].isurf;

    // Restore the net object to the state of this face
    if (dimension == 2) {
      surfState = lines[isurf].surfaceState;
    } else {
      surfState = tris[isurf].surfaceState;
    }
    int nrxn = net->nReactions();
    int nRxnEvents = 0;
    for (size_t irxn = 0; irxn < nrxn; irxn++) {
      nRxnEvents += surfState->GlobalReactionEventsF[irxn] + surfState->GlobalReactionEventsR[irxn]; 
    }

    // Reset the surface into net
    surfState->setState(n, dt);

    // Zero final counters
    net->finalizeTimeStepArrays(deltaT);

    // Writing of special CSV file that dumps the surface state at each time step
    net->write_step_results(time, deltaT, comm->me, nRxnEvents);
  }
}

/* ----------------------------------------------------------------------
   pack one task into buf
   return # of bytes packed
   if not memflag, only return count, do not fill buf
------------------------------------------------------------------------- */

int FixEmitZSurf::pack_task(int itask, char* buf, int memflag)
{
  char* ptr = buf;
  if (memflag) {
    memcpy(ptr,&tasks[itask],sizeof(Task));
  }
  ptr += sizeof(Task);
  ptr = ROUNDUP(ptr);

  // pack task vectors
  // vscale is allocated, but not communicated, since updated every step


  int npoint = tasks[itask].npoint;
  if (memflag) {
    memcpy(ptr,tasks[itask].path,npoint*3*sizeof(double));
  }
  ptr += npoint*3*sizeof(double);
  if (memflag) {
    memcpy(ptr,tasks[itask].fracarea,(npoint-2)*sizeof(double));
  }
  ptr += (npoint-2)*sizeof(double);

  return ptr-buf;
}

/* ----------------------------------------------------------------------
   unpack one task from buf
------------------------------------------------------------------------- */

int FixEmitZSurf::unpack_task(char* buf, int icell)
{
  char* ptr = buf;

  if (ntask == ntaskmax) {
    grow_task();
  }
  //double *ntargetsp = tasks[ntask].ntargetsp;
  //double *vscale = tasks[ntask].vscale;
  double* path = tasks[ntask].path;
  double* fracarea = tasks[ntask].fracarea;

  memcpy(&tasks[ntask],ptr,sizeof(Task));
  ptr += sizeof(Task);
  ptr = ROUNDUP(ptr);

  // unpack task vectors
  // vscale is allocated, but not communicated, since updated every step

  //if (perspecies) {
  //memcpy(ntargetsp,ptr,nspecies*sizeof(double));
  //  ptr += nspecies*sizeof(double);
  //}

  int npoint = tasks[ntask].npoint;
  delete [] path;
  path = new double[npoint*3];
  memcpy(path,ptr,npoint*3*sizeof(double));
  ptr += npoint*3*sizeof(double);

  delete [] fracarea;
  fracarea = new double[npoint-2];
  memcpy(fracarea,ptr,(npoint-2)*sizeof(double));
  ptr += (npoint-2)*sizeof(double);

  //tasks[ntask].ntargetsp = ntargetsp;
  //tasks[ntask].vscale = vscale;
  tasks[ntask].path = path;
  tasks[ntask].fracarea = fracarea;

  // reset task icell and pcell
  // if a split cell, set pcell via scan of icell csurfs, extract from sinfo

  Grid::ChildCell* cells = grid->cells;
  Grid::SplitInfo* sinfo = grid->sinfo;

  tasks[ntask].icell = icell;
  if (cells[icell].nsplit == 1) {
    tasks[ntask].pcell = icell;
  } else {
    int isurf = tasks[ntask].isurf;
    int nsurf = cells[icell].nsurf;
    surfint* csurfs = cells[icell].csurfs;
    int i;
    for (i = 0; i < nsurf; i++)
      if (csurfs[i] == isurf) {
        break;
      }
    int isplit = cells[icell].isplit;
    int subcell = sinfo[isplit].csplits[i];
    tasks[ntask].pcell = sinfo[isplit].csubs[subcell];
  }

  ntask++;
  return ptr-buf;
}

/* ----------------------------------------------------------------------
   copy N tasks starting at index oldfirst to index first
------------------------------------------------------------------------- */

void FixEmitZSurf::copy_task(int icell, int n, int first, int oldfirst)
{
  // reset icell in each copied task
  // copy task vectors
  // vscale is allocated, but not copied, since updated every step

  if (first == oldfirst) {
    for (int i = 0; i < n; i++) {
      tasks[first].icell = icell;
      first++;
    }

  } else {
    int npoint;
    for (int i = 0; i < n; i++) {
      // double *ntargetsp = tasks[first].ntargetsp;
      //double *vscale = tasks[first].vscale;
      double* path = tasks[first].path;
      double* fracarea = tasks[first].fracarea;

      memcpy(&tasks[first],&tasks[oldfirst],sizeof(Task));

      // if (perspecies)
      //   memcpy(ntargetsp,tasks[oldfirst].ntargetsp,nspecies*sizeof(double));

      npoint = tasks[first].npoint;
      delete [] path;
      path = new double[npoint*3];
      memcpy(path,tasks[oldfirst].path,npoint*3*sizeof(double));

      delete [] fracarea;
      fracarea = new double[npoint-2];
      memcpy(fracarea,tasks[oldfirst].fracarea,(npoint-2)*sizeof(double));

      //tasks[first].ntargetsp = ntargetsp;
      //tasks[first].vscale = vscale;
      tasks[first].path = path;
      tasks[first].fracarea = fracarea;

      tasks[first].icell = icell;
      first++;
      oldfirst++;
    }
  }

  ntask += n;
}

/* ----------------------------------------------------------------------
   grow task list
------------------------------------------------------------------------- */

void FixEmitZSurf::grow_task()
{
  int oldmax = ntaskmax;
  ntaskmax += DELTATASK;
  tasks = (Task*) memory->srealloc(tasks,ntaskmax*sizeof(Task),
                                   "emit/face:tasks");

  // set all new task bytes to 0 so valgrind won't complain
  // if bytes between fields are uninitialized

  memset(&tasks[oldmax],0,(ntaskmax-oldmax)*sizeof(Task));

  // allocate vectors in each new task or set to NULL
  // path and fracarea are allocated later to specific sizes

  //if (perspecies) {
  //   for (int i = oldmax; i < ntaskmax; i++)
  //    tasks[i].ntargetsp = new double[nspecies];
  //} else {
  //  for (int i = oldmax; i < ntaskmax; i++)
  //   tasks[i].ntargetsp = NULL;
  // }

  //for (int i = oldmax; i < ntaskmax; i++)
  //  tasks[i].vscale = NULL;

  for (int i = oldmax; i < ntaskmax; i++) {
    tasks[i].path = NULL;
    tasks[i].fracarea = NULL;
  }
}

/* ----------------------------------------------------------------------
   reset pcell for all compress task entries
   called from Grid::compress() after grid cells have been compressed
   wait to do this until now b/c split cells and their sinfo
     are setup in Grid::compress() between compress_grid()
     and post_compress_grid()
------------------------------------------------------------------------- */

void FixEmitZSurf::post_compress_grid()
{
  Grid::ChildCell* cells = grid->cells;
  Grid::SplitInfo* sinfo = grid->sinfo;

  for (int i = 0; i < ntask; i++) {
    int icell = tasks[i].icell;
    if (cells[icell].nsplit == 1) {
      tasks[i].pcell = icell;
    } else {
      int isurf = tasks[i].isurf;
      int nsurf = cells[icell].nsurf;
      surfint* csurfs = cells[icell].csurfs;
      int j;
      for (j = 0; j < nsurf; j++)
        if (csurfs[j] == isurf) {
          break;
        }
      int isplit = cells[icell].isplit;
      int subcell = sinfo[isplit].csplits[j];
      tasks[i].pcell = sinfo[isplit].csubs[subcell];
    }
  }
}

/* ----------------------------------------------------------------------
   process keywords specific to this class
------------------------------------------------------------------------- */

int FixEmitZSurf::option(int narg, char** arg)
{
  if (strcmp(arg[0],"zuzaxReact") == 0) {
    if (2 > narg) {
      error->all(FLERR,"fix emit/zsurf keyword requires an additional react ID arg");
    }

    isrZuzax = surf->find_react(arg[1]);
    if (isrZuzax < 0) {
      error->allf(FLERR,"Could not find surfReact sr-ID, %s", arg[1]);
    }

    return 2;
  }

  error->allf(FLERR,"Illegal fix emit/zsurf keyword: %s", arg[0]);
  return 0;
}

#endif
