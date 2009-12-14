/***************************************************************************
 *   Copyright (C) 2004 by Rudolph Pienaar / Christian Haselgrove          *
 *   {ch|rudolph}@nmr.mgh.harvard.edu                                      *
 *                                                                         *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
/// \file mris_pmake.cpp
///
/// \brief Brief description
/// Determine the shortest path on a freesurfer brain curvature map.
///
/// \b NAME
///
/// mris_pmake (dijkstra prototype 1)
///
/// \b SYNPOSIS
///
/// mris_pmake <--optionsFile [fileName]> <--dir [workingDir]> [--listen] [--listenOnPort <port>]
///
/// \b DESCRIPTION
///
/// Determine the shortest path along cost function based on the curvature,
/// sulcal height, and distance using a surface map generated by freesurfer.
/// It is hoped that such a shortest path will accurate trace the sulcal
/// fundus between a specified start and end vertex.
///
/// \b HISTORY
///
///  Week of 20 September 2004 - kdevelop integration / cvs setup
///
///  27 October 2009
///  o Resurrection!
///    Changed setting of default cost function to occur during system
///    init. Previously this happened prior to each call to dijkstra(...).
///    This allows for dijkstra RUN to use arbitrary cost (old behaviour
///    was to only allow arbitrary cost function selection for ply searching).
///
///  November - December 2009
///  o Modification to allow running without dsh.
///  

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>

using namespace std;

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "help.h"
#include "scanopt.h"
#include "dijkstra.h"
#include "C_mpmProg.h"
#include "c_SSocket.h"
#include "pstream.h"
#include "general.h"
#include "asynch.h"
#include "c_vertex.h"
#include "c_label.h"
#include "c_surface.h"

extern  const option longopts[];

char*   Gpch_Progname;
char*   Progname        = Gpch_Progname;
bool    Gb_stdout       = true;         // Global flag controlling output to
                                        //+stdout
string  G_SELF          = "";           // "My" name
string  G_VERSION       =               // version
  "$Id: mris_pmake.cpp,v 1.10 2009/12/14 16:21:51 rudolph Exp $";
stringstream            Gsout("");
int     G_lw            = 40;           // print column
int     G_rw            = 20;           // widths (left and right)

int
main(
    int         argc,
    char**      ppch_argv) {

  /* ----- initializations ----- */
  Gpch_Progname  = strrchr(ppch_argv[0], '/');
  Gpch_Progname  = (Gpch_Progname == NULL ? ppch_argv[0] : Gpch_Progname+1);
  string                        str_progname(Gpch_Progname);
  G_SELF                                                = str_progname;

  string                        str_asynchComms         = "HUP";
  C_scanopt*                    pcso_options            = NULL;
  s_env                         st_env;
  s_env_nullify(st_env);

  s_weights                     st_costWeight;
  s_Dweights                    st_DcostWeight;
  c_SSocket_UDP_receive*        pCSSocketReceive        = NULL;
  bool                          b_socketCreated         = false;
  float                         f_cost                  = 0.0;

  // Prior to completely populating the enter st_env structure, we fill in
  // some defaults to "boot strap" the process.
  st_env.str_workingDir         = "./";
  st_env.str_optionsFileName    = "options.txt";
  st_env.b_surfacesKeepInSync   = true;         // This allows us to
                                                //+ propogate changes
                                                //+ in the working
                                                //+ surface to the
                                                //+ auxillary surface.

  // Set the default cost function in the enviroment
  s_env_costFctSet(&st_env, costFunc_defaultDetermine, e_default);
  string        str_optionsFQName       = "";
  string        str_patchFQName         = "";

  // Process command line options
  str_asynchComms       = commandLineOptions_process(argc, ppch_argv, st_env);

  // The main functional and event processing loop
  while (str_asynchComms != "TERM") {

    if ( str_asynchComms == "HUP"               || \
         str_asynchComms == "LISTEN"            || \
         str_asynchComms == "LISTENPORT"        || \
         str_asynchComms == "RUN"               || \
         str_asynchComms == "INITMPMPROG"       || \
         str_asynchComms == "RUNPROG") {

      system("echo > lock");            // signal a "lock"
                                        //+ semaphore on
                                        //+ the file system

      if (str_asynchComms != "RUN") {
        // Create scanopt objects to parse the (possibly changed)
        // options file
        str_optionsFQName = st_env.str_workingDir + st_env.str_optionsFileName;
        pcso_options      = new C_scanopt(str_optionsFQName, e_EquLink);

        if (str_asynchComms != "LISTENPORT") {
          // Parse the options file
          s_env_scan(       st_env,         *pcso_options);
          s_weights_scan(   st_costWeight,  *pcso_options);
          s_Dweights_scan(  st_DcostWeight, *pcso_options);
          st_env.pSTw   =       &st_costWeight;
          st_env.pSTDw  =       &st_DcostWeight;
          G_lw          =       st_env.lw;
          G_rw          =       st_env.rw;
        }

        if (st_env.port && !b_socketCreated) {
          // Create a UDP socket
          pCSSocketReceive  = new c_SSocket_UDP_receive(
                                st_env.port, st_env.timeoutSec);
          b_socketCreated   = true;
        }
        if (!pCSSocketReceive)
          str_asynchComms = "TERM";
      }

      if (str_asynchComms       == "INITMPMPROG") {
        s_env_mpmProgSetIndex(&st_env, st_env.empm_current);
        str_asynchComms         = "RUNPROG";
      }
      
      if( (str_asynchComms      == "RUNPROG")                           &&
           st_env.pSTw          != NULL) {
        Gsout.str("");
        Gsout << "Running embedded program '";
        Gsout << st_env.pstr_mpmProgName[st_env.empm_current];
        Gsout << "'"    << endl;
        ULOUT(Gsout.str());
        if(st_env.pCmpmProg) st_env.pCmpmProg->run();
        else {
            fprintf(stderr, "Warning -- mpmProg has not been created!\n");
            fprintf(stderr, "Have you run 'ENV mpmProg set <X>'?\n");
        }
        if(st_env.b_exitOnDone) str_asynchComms = "TERM";
      }

      if ( (str_asynchComms  == "HUP" || str_asynchComms  == "RUN")     &&
           st_env.pSTw      != NULL) {
        Gsout.str("");
        Gsout << "Determining path from vertex " << st_env.startVertex;
        Gsout << " to vertex " << st_env.endVertex << "..." << flush;
        ULOUT(Gsout.str());
        SLOUT("PROCESSING: path");

        //----------------------------------
        // The "heart" of this entire system
        if (!dijkstra(st_env)) exit(1);
        //----------------------------------

        nULOUT("\t\t[ ok ]\n");
        nSLOUT("\t\t\t\t\t\t[ ok ]\n");

        ULOUT("Marking (rip) path along vertices...");
        s_env_activeSurfaceSetIndex(&st_env, (int) e_workingCurvature);
        f_cost = surface_ripMark(st_env);
        colprintf(G_lw, G_rw, "Total path cost:", "%f\n", f_cost);
        Gsout.str(""); Gsout << f_cost;
        nRLOUT(Gsout.str());
        nULOUT("\t\t\t\t[ ok ]\n");

        if (st_env.b_patchFile_save) {
          ULOUT("Saving patch file...");
          str_patchFQName =  st_env.str_workingDir +
                             st_env.str_patchFileName;
          if (MRISwritePatch(  st_env.pMS_curvature,
                               (char*) str_patchFQName.c_str()) != NO_ERROR)
            exit(1);
          nULOUT("\t\t\t\t\t\t[ ok ]\n");
        }

        if (st_env.b_labelFile_save) {
          ULOUT("Labeling and saving all target vertices... ");
          //label_save(st_env);
          void* pv_void = NULL;
          label_workingSurface_saveTo(st_env, vertex_ripFlagIsTrue, pv_void);
          if (st_env.b_surfacesKeepInSync) {
            surface_workingToAux_ripTrueCopy(st_env);
            label_auxSurface_saveTo(st_env, vertex_ripFlagIsTrue, pv_void);
          }
          nULOUT("\t\t\t[ ok ]\n")
        }

        if (st_env.b_surfacesClear) {
          ULOUT("Clearing (rip) path along vertices...");
          s_env_activeSurfaceSetIndex(&st_env, (int) e_workingCurvature);
          surface_ripClear(st_env, true);
          if (st_env.b_surfacesKeepInSync) {
            s_env_activeSurfaceSetIndex(&st_env, (int) e_auxillary);
            surface_ripClear(st_env, true);
            // NB!! Remember to set the "active" surface back
            // to the "working" surface. The dijkstra()
            // function operates on this "active" surface.
            s_env_activeSurfaceSetIndex(&st_env, (int) e_workingCurvature);
          }
          nULOUT("\t\t\t\t[ ok ]\n");
        }

      }
      /* ----- clean up and exit ----- */
      if (str_asynchComms != "RUN") delete pcso_options;
    }

    // Listen on the socket for asynchronous user evernts
    if (pCSSocketReceive && !st_env.b_exitOnDone) {
      SLOUT("Ready\n");
      ULOUT("Listening for socket comms...\n");
      str_asynchComms  = asynchEvent_poll(pCSSocketReceive, 5);
      Gsout.str("");
      Gsout << "COMMS: Received \t\t\t\t\t\t[ " << str_asynchComms << " ]" << endl;
      SLOUT(Gsout.str());
      Gsout.str("");
      Gsout << "\tReceived \t\t\t\t\t\t[ " << str_asynchComms << " ]" << endl;
      ULOUT(Gsout.str());
      asynchEvent_process(st_env, str_asynchComms);
      Gsout.str("");
      Gsout << "PROCESSED: " << str_asynchComms << endl;
      SLOUT(Gsout.str());
    }
  }

  delete pCSSocketReceive;
  if (st_env.pcsm_syslog) {
    st_env.pcsm_syslog->timer(eSM_stop);
    SLOUT("Ready\n");
  }
  if (st_env.pcsm_userlog) {
    st_env.pcsm_userlog->timer(eSM_stop);
  }
  system("rm -f lock 2>/dev/null");  // "unlock" semaphore
  return EXIT_SUCCESS;

} /* end main() */

/* eof */

