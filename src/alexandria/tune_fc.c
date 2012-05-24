/* -*- mode: c; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; c-file-style: "stroustrup"; -*-
 * $Id: tune_dip.c,v 1.39 2009/04/12 21:24:26 spoel Exp $
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 4.0.99
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2008, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * Groningen Machine for Chemical Simulation
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <assert.h>
#ifdef GMX_LIB_MPI
#include <mpi.h>
#endif
#ifdef GMX_THREADS
#include "tmpi.h"
#endif
#include "maths.h"
#include "macros.h"
#include "copyrite.h"
#include "bondf.h"
#include "string2.h"
#include "smalloc.h"
#include "strdb.h"
#include "sysstuff.h"
#include "confio.h"
#include "physics.h"
#include "futil.h"
#include "statutil.h"
#include "vec.h"
#include "3dview.h"
#include "txtdump.h"
#include "readinp.h"
#include "names.h"
#include "vec.h"
#include "atomprop.h"
#include "xvgr.h"
#include "mdatoms.h"
#include "force.h"
#include "vsite.h"
#include "shellfc.h"
#include "network.h"
#include "viewit.h"
#include "gmx_random.h"
#include "gmx_wallcycle.h"
#include "gmx_statistics.h"
#include "gmx_matrix.h"
#include "convparm.h"
#include "gpp_atomtype.h"
#include "grompp.h"
#include "gen_ad.h"
#include "slater_integrals.h"
#include "gentop_qgen.h"
#include "gentop_core.h"
#include "poldata.h"
#include "poldata_xml.h"
#include "molprop.h"
#include "molprop_xml.h"
#include "molprop_util.h"
#include "molselect.h"
#include "mtop_util.h"
#include "gentop_comm.h"
#include "nmsimplex.h"
#include "mymol.h"

typedef struct {
    int nb,*ngtb;
    int na,*ngta;
    int nd,*ngtd;
} opt_mask_t;

typedef struct {
    int gt_index;
    int natom;
    char **ai;
    int nparam;
    double *param;
} opt_bad_t;

enum { ebadBOND, ebadANGLE, ebadDIH, ebadNR };

typedef struct {
    t_moldip *md;
    gmx_bool bBonds,bAngles,bDihs;
    int nbad[ebadNR];
    opt_bad_t *bad[ebadNR];
    int *inv_gt[ebadNR];
    int nparam;
    double *param,*orig,*best,*lower,*upper;
} opt_param_t;

static void add_obt(opt_bad_t *obt,int natom,char **ai,
                    int nparam,double *param,int gt)
{
    int i;
    
    obt->gt_index = gt-1;
    obt->natom = natom;
    snew(obt->ai,natom);
    for(i=0; (i<natom); i++) 
        obt->ai[i] = strdup(ai[i]);
    obt->nparam = nparam;
    snew(obt->param,nparam);
    for(i=0; (i<nparam); i++)
        obt->param[i] = param[i];
}

static void add_opt(opt_param_t *opt,int otype,int natom,char **ai,
                    int nparam,double *param,int gt)
{
    assert(otype < ebadNR);
    assert(otype >= 0);
    srenew(opt->bad[otype],++opt->nbad[otype]);
    add_obt(&opt->bad[otype][opt->nbad[otype]-1],natom,ai,nparam,param,gt);
}

static void opt2list(opt_param_t *opt)
{
    int i,j,p,n,nparam;
    
    for(i=n=0; (i<ebadNR); i++) {
        for(j=0; (j<opt->nbad[i]); j++) {
            nparam = n+opt->bad[i][j].nparam;
            if (nparam > opt->nparam) {
                srenew(opt->param,nparam);
                opt->nparam = nparam;
            }
            for(p=0; (p<opt->bad[i][j].nparam); p++) {
                opt->param[n++] = opt->bad[i][j].param[p];
            }
        }
    }
}

static void list2opt(opt_param_t *opt)
{
    gmx_poldata_t pd = opt->md->pd;
    int i,j,p,n,nparam;
    char buf[STRLEN];
    
    for(i=n=0; (i<ebadNR); i++) {
        for(j=0; (j<opt->nbad[i]); j++) {
            buf[0] = '\0';
            for(p=0; (p<opt->bad[i][j].nparam); p++) {
                opt->bad[i][j].param[p] = opt->param[n++];
                strcat(buf," ");
                strcat(buf,gmx_ftoa(opt->bad[i][j].param[p]));
            }
            switch (i) {
            case ebadBOND:
                gmx_poldata_set_gt_bond_params(pd,opt->bad[i][j].ai[0],
                                               opt->bad[i][j].ai[1],
                                               buf);
                break;
            case ebadANGLE:
                gmx_poldata_set_gt_angle_params(pd,opt->bad[i][j].ai[0],
                                                opt->bad[i][j].ai[1],
                                                opt->bad[i][j].ai[2],
                                                buf);
                break;
            case ebadDIH:
                gmx_poldata_set_gt_dihedral_params(pd,opt->bad[i][j].ai[0],
                                                   opt->bad[i][j].ai[1],
                                                   opt->bad[i][j].ai[2],
                                                   opt->bad[i][j].ai[3],
                                                   buf);
                break;
            }
        }
    }
}

static void mk_inv_gt(opt_param_t *opt)
{
    int i,j,gt_max;

    /* Make in index to look up gt */    
    for(i=0; (i<ebadNR); i++) {
        gt_max = -1;
        for(j=0; (j<opt->nbad[i]); j++) {
            if (opt->bad[i][j].gt_index > gt_max)
                gt_max = opt->bad[i][j].gt_index;
        }
        snew(opt->inv_gt[i],gt_max+1);
        for(j=0; (j<=gt_max); j++)
            opt->inv_gt[i][j] = NOTSET;
        for(j=0; (j<opt->nbad[i]); j++) {
            opt->inv_gt[i][opt->bad[i][j].gt_index] = j;
        }
    }
}

static void get_dissociation_energy(FILE *fplog,opt_param_t *opt)
{
    FILE     *csv;
    int      nD,nMol,ftb,gt,gti,i,j,ai,aj,niter,row;
    char     *aai,*aaj,**ctest;
    char     buf[STRLEN];
    t_mymol  *mymol;
    double   **a,**at,**ata;
    double   *x,*atx,*fpp;
    double   a0,da0,ax,chi2;
    int      *test;
    gmx_bool bZero = FALSE;
    
    nD     = opt->nbad[ebadBOND];
    nMol   = opt->md->nmol;
    a      = alloc_matrix(nMol,nD);
    at     = alloc_matrix(nD,nMol);
    ata    = alloc_matrix(nD,nD);
    snew(x,nMol+1);
    snew(atx,nD+1);
    snew(fpp,nD+1);
    snew(test,nD+1);
    snew(ctest,nD+1);
    
    fprintf(fplog,"There are %d different bondtypes to optimize the heat of formation\n",
            nD);
    fprintf(fplog,"There are %d (experimental) reference heat of formation.\n",nMol);
    
    ftb = gmx_poldata_get_gt_bond_ftype(opt->md->pd);
    for(j=0; (j<nMol); j++) 
    {
        mymol = &(opt->md->mymol[j]);
        for(i=0; (i<mymol->ltop->idef.il[ftb].nr); i+=interaction_function[ftb].nratoms+1) {
            ai = mymol->ltop->idef.il[ftb].iatoms[i+1];
            aj = mymol->ltop->idef.il[ftb].iatoms[i+2];
            aai = *mymol->atoms->atomtype[ai];
            aaj = *mymol->atoms->atomtype[aj];
            if ((gt = gmx_poldata_search_gt_bond(opt->md->pd,aai,aaj,
                                                 NULL,NULL,NULL)) != 0) {
                gti = opt->inv_gt[ebadBOND][gt-1];
                at[gti][j]++;
                a[j][gti]++; 
                test[gti]++;
                if (NULL == ctest[gti]) {
                    sprintf(buf,"%s-%s",aai,aaj);
                    ctest[gti] = strdup(buf);
                }
            }
            else {
                gmx_fatal(FARGS,"There are no parameters for bond %s-%s in the force field",aai,aaj);
            }
        }
        x[j] = mymol->Emol;
    }
    csv = ffopen("out.csv","w");
    for(i=0; (i<nMol); i++) 
    {
        mymol = &(opt->md->mymol[i]);
        fprintf(csv,"\"%s\",",mymol->molname);
        for(j=0; (j<nD); j++) {
            fprintf(csv,"\"%g\",",a[i][j]);
        }
        fprintf(csv,"\"%.3f\"\n",x[i]);
    }
    fclose(csv);
    
    matrix_multiply(NULL,nMol,nD,a,at,ata);
    if ((row = matrix_invert(NULL,nD,ata)) != 0) {
        gmx_fatal(FARGS,"Matrix inversion failed. Incorrect row = %d.\nThis probably indicates that you do not have sufficient data points, or that some parameters are linearly dependent.",
                  row);
    }
    snew(atx,nD);
    snew(fpp,nD);
    a0 = 0;
    do {
        for(i=0; (i<nD); i++)  
        {
            atx[i] = 0;
            for(j=0; (j<nMol); j++)
                atx[i] += at[i][j]*(x[j]-a0);
        }
        for(i=0; (i<nD); i++) 
        {
            fpp[i] = 0;
            for(j=0; (j<nD); j++)
                fpp[i] += ata[i][j]*atx[j];
        }
        da0 = 0;
        chi2 = 0;
        if (bZero)
        {
            for(j=0; (j<nMol); j++)
            {
                ax = a0;
                for(i=0; (i<nD); i++)  
                    ax += fpp[i]*a[j][i];
                da0 += (x[j]-ax);
                chi2 += sqr(x[j]-ax);
            }
            da0 = da0 / nMol;
            a0 += da0;
            niter++;
            printf("iter: %d, a0 = %g, chi2 = %g\n",
                   niter,a0,chi2/nMol);
        }
    } while ((fabs(da0) > 1e-5) && (niter < 1000));
    
    for(i=0; (i<nD); i++) 
    {
        opt->bad[ebadBOND][i].param[0] = -fpp[i];
        if (fplog)
            fprintf(fplog,"Optimized dissociation energy for %8s with %4d copies to %g\n",
                    ctest[i],test[i],fpp[i]);
    }

    sfree(test);
    sfree(fpp);
}

static opt_param_t *init_opt(FILE *fplog,t_moldip *md,int *nparam,
                             gmx_bool bBonds,gmx_bool bAngles,gmx_bool bDihs,
                             real D0,real beta0,real D0_min,real beta_min,
                             opt_mask_t *omt,real factor)
{
    opt_param_t *opt;
    char   *ai[4];
    char   *params,**ptr;
    int    gt,i,n,maxfc = 0;
    double *fc=NULL;
    
    snew(opt,1);
    opt->md = md;
    opt->bBonds = bBonds;
    opt->bAngles = bAngles;
    opt->bDihs = bDihs;
    *nparam = 0;
    if (bBonds) {
        while((gt = gmx_poldata_get_gt_bond(md->pd,&(ai[0]),&(ai[1]),NULL,NULL,NULL,&params)) > 0) {
            if (omt->ngtb[gt-1] > 0) {
                ptr = split(' ',params);
                for(n = 0; (NULL != ptr[n]); n++) {
                    if (n>=maxfc) {
                        srenew(fc,++maxfc);
                    }
                    fc[n] = atof(ptr[n]);
                }
                if (D0 > 0)
                    fc[0] = D0;
                if (beta0 > 0)
                    fc[1] = beta0;
                add_opt(opt,ebadBOND,2,ai,n,fc,gt);
                *nparam += n;
            }
        }
    }
    if (bAngles) {
        while((gt = gmx_poldata_get_gt_angle(md->pd,&(ai[0]),&(ai[1]),&(ai[2]),NULL,NULL,&params)) > 0) {
            if (omt->ngta[gt-1] > 0) {
                ptr = split(' ',params);
                for(n = 0; (NULL != ptr[n]); n++) {
                    if (n>maxfc) {
                        srenew(fc,++maxfc);
                    }
                    fc[n] = atof(ptr[n]);
                }
                add_opt(opt,ebadANGLE,3,ai,n,fc,gt);
                *nparam += n;
            }
        }
    }
    if (bDihs) {
        while((gt = gmx_poldata_get_gt_dihedral(md->pd,&(ai[0]),&(ai[1]),&(ai[2]),&(ai[3]),
                                                NULL,NULL,&params)) > 0) {
            if (omt->ngtd[gt-1] > 0) {
                ptr = split(' ',params);
                for(n = 0; (NULL != ptr[n]); n++) {
                    if (n>maxfc) {
                        srenew(fc,++maxfc);
                    }
                    fc[n] = atof(ptr[n]);
                }
                add_opt(opt,ebadDIH,4,ai,n,fc,gt);
                *nparam += n;
            }
        }
    }
    sfree(fc);
    
    mk_inv_gt(opt);
    opt2list(opt);
    get_dissociation_energy(fplog,opt);
    opt2list(opt);
    list2opt(opt);
    snew(opt->best,opt->nparam);
    snew(opt->orig,opt->nparam);
    snew(opt->lower,opt->nparam);
    snew(opt->upper,opt->nparam);
    if (factor < 1)
        factor = 1/factor;
    for(i=0; (i<opt->nparam); i++) {
        opt->best[i] = opt->orig[i] = opt->param[i];
        opt->lower[i] = opt->orig[i]/factor;
        opt->upper[i] = opt->orig[i]*factor;
    }
    return opt;
}

static void print_opt(FILE *fp,opt_param_t *opt)
{
    int k;
    
    fprintf(fp,"Param        Orig        Best\n");
    for(k=0; (k<opt->nparam); k++)
        fprintf(fp,"%-5d  %10g  %10g\n",k,opt->orig[k],opt->best[k]);
}

static void print_stats(FILE *fp,const char *prop,gmx_stats_t lsq,gmx_bool bHeader,
                        char *xaxis,char *yaxis)
{
    real a,da,b,db,chi2,rmsd,Rfit;
    int  n;
    
    if (bHeader)
    {
        fprintf(fp,"Fitting data to y = ax+b, where x = %s and y = %s\n",
                xaxis,yaxis);
        fprintf(fp,"%-12s %5s %13s %13s %8s %8s\n",
                "Property","N","a","b","R","RMSD");
        fprintf(fp,"---------------------------------------------------------------\n");
    }
    gmx_stats_get_ab(lsq,elsqWEIGHT_NONE,&a,&b,&da,&db,&chi2,&Rfit);
    gmx_stats_get_rmsd(lsq,&rmsd);
    gmx_stats_get_npoints(lsq,&n);
    fprintf(fp,"%-12s %5d %6.3f(%5.3f) %6.3f(%5.3f) %7.2f%% %8.4f\n",
            prop,n,a,da,b,db,Rfit*100,rmsd);
}

static void print_lsq_set(FILE *fp,gmx_stats_t lsq)
{
    real   x,y;
    
    fprintf(fp,"@type xy\n");
    while (gmx_stats_get_point(lsq,&x,&y,NULL,NULL,0) == estatsOK)
    {
        fprintf(fp,"%10g  %10g\n",x,y);
    }
    fprintf(fp,"&\n");
}

static void xvgr_symbolize(FILE *xvgf,int nsym,const char *leg[],
                           const output_env_t oenv)
{
    int i;

    xvgr_legend(xvgf,nsym,leg,oenv);
    for(i=0; (i<nsym); i++)
    {
        xvgr_line_props(xvgf,i,elNone,ecBlack+i,oenv);
        fprintf(xvgf,"@ s%d symbol %d\n",i,i+1);
    }        
}

static opt_mask_t *analyze_idef(FILE *fp,int nmol,t_mymol mm[],gmx_poldata_t pd,
                                gmx_bool bBonds,gmx_bool bAngles,gmx_bool bDihs)
{
    int  gt,i,n,tp,ai,aj,ak,al,ftb,fta,ftd;
    char *aai,*aaj,*aak,*aal,*params,**ptr;
    t_mymol *mymol;
    int  NGTB,NGTA,NGTD,nbtot,natot,ndtot;
    char **cgtb=NULL,**cgta=NULL,**cgtd=NULL;
    opt_mask_t *omt;
    
    snew(omt,1);
    if (bBonds) {
        omt->nb = gmx_poldata_get_ngt_bond(pd);
        snew(omt->ngtb,omt->nb);
        snew(cgtb,omt->nb);
        nbtot = 0;
        ftb = gmx_poldata_get_gt_bond_ftype(pd);
        for(n=0; (n<nmol); n++) { 
            mymol = &(mm[n]);
            
            for(i=0; (i<mymol->ltop->idef.il[ftb].nr); i+=interaction_function[ftb].nratoms+1) {
                tp = mymol->ltop->idef.il[ftb].iatoms[i];
                ai = mymol->ltop->idef.il[ftb].iatoms[i+1];
                aj = mymol->ltop->idef.il[ftb].iatoms[i+2];
                aai = *mymol->atoms->atomtype[ai];
                aaj = *mymol->atoms->atomtype[aj];
                if ((gt = gmx_poldata_search_gt_bond(pd,aai,aaj,NULL,NULL,&params)) != 0) {
                    omt->ngtb[gt-1]++;
                    if (NULL == cgtb[gt-1]) {
                        snew(cgtb[gt-1],strlen(aai)+strlen(aaj)+2);
                        sprintf(cgtb[gt-1],"%s-%s",aai,aaj);
                    }
                    nbtot++;
                }
            }
        }
        for(i=0; (i<omt->nb); i++) {
            if (omt->ngtb[i] > 0) {
                fprintf(fp,"BOND     %6d  %-20s  %d\n",i,cgtb[i],omt->ngtb[i]);
                sfree(cgtb[i]);
            }
        }
        sfree(cgtb);
    }
    if (bAngles) {
        omt->na = gmx_poldata_get_ngt_angle(pd);
        snew(omt->ngta,omt->na);
        snew(cgta,omt->na);
        natot = 0;
        fta = gmx_poldata_get_gt_angle_ftype(pd);
        for(n=0; (n<nmol); n++) { 
            mymol = &(mm[n]);
            for(i=0; (i<mymol->ltop->idef.il[fta].nr); i+=interaction_function[fta].nratoms+1) {
                tp = mymol->ltop->idef.il[fta].iatoms[i];
                ai = mymol->ltop->idef.il[fta].iatoms[i+1];
                aj = mymol->ltop->idef.il[fta].iatoms[i+2];
                ak = mymol->ltop->idef.il[fta].iatoms[i+3];
                aai = *mymol->atoms->atomtype[ai];
                aaj = *mymol->atoms->atomtype[aj];
                aak = *mymol->atoms->atomtype[ak];
                if ((gt = gmx_poldata_search_gt_angle(pd,aai,aaj,aak,NULL,NULL,&params)) != 0) {
                    omt->ngta[gt-1]++;
                    if (NULL == cgta[gt-1]) {
                        snew(cgta[gt-1],strlen(aai)+strlen(aaj)+strlen(aak)+3);
                        sprintf(cgta[gt-1],"%s-%s-%s",aai,aaj,aak);
                    }
                    natot++;
                }
            }
        }
        for(i=0; (i<omt->na); i++) {
            if (omt->ngta[i] > 0) {
                fprintf(fp,"ANGLE    %6d  %-20s  %d\n",i,cgta[i],omt->ngta[i]);
                sfree(cgta[i]);
            }
        }
        sfree(cgta);
    }
    if (bDihs) {
        omt->nd = gmx_poldata_get_ngt_dihedral(pd);
        snew(omt->ngtd,omt->nd);
        snew(cgtd,omt->nd);
        ndtot = 0;
        ftd = gmx_poldata_get_gt_dihedral_ftype(pd);
        for(n=0; (n<nmol); n++) { 
            mymol = &(mm[n]);
            for(i=0; (i<mymol->ltop->idef.il[ftd].nr); i+=interaction_function[ftd].nratoms+1) {
                tp = mymol->ltop->idef.il[ftd].iatoms[i];
                ai = mymol->ltop->idef.il[ftd].iatoms[i+1];
                aj = mymol->ltop->idef.il[ftd].iatoms[i+2];
                ak = mymol->ltop->idef.il[ftd].iatoms[i+3];
                al = mymol->ltop->idef.il[ftd].iatoms[i+4];
                aai = *mymol->atoms->atomtype[ai];
                aaj = *mymol->atoms->atomtype[aj];
                aak = *mymol->atoms->atomtype[ak];
                aal = *mymol->atoms->atomtype[al];
                if ((gt = gmx_poldata_search_gt_dihedral(pd,aai,aaj,aak,aal,
                                                         NULL,NULL,&params)) != 0) {
                    omt->ngtd[gt-1]++;
                    if (NULL == cgtd[gt-1]) {
                        snew(cgtd[gt-1],strlen(aai)+strlen(aaj)+strlen(aak)+strlen(aal)+4);
                        sprintf(cgtd[gt-1],"%s-%s-%s-%s",aai,aaj,aak,aal);
                    }
                }
                ndtot++;
            }
        }
        for(i=0; (i<omt->nd); i++) {
            if (omt->ngtd[i] > 0) {
                fprintf(fp,"DIHEDRAL %6d  %-20s  %d\n",i,cgtd[i],omt->ngtd[i]);
                sfree(cgtd[i]);
            }
        }
        sfree(cgtd);
    }
    fprintf(fp,"In the total data set of %d molecules we have:\n",nmol);
    if (bBonds)
        fprintf(fp,"%6d bonds     of %4d types\n",nbtot,omt->nb);
    if (bAngles)
        fprintf(fp,"%6d angles    of %4d types\n",natot,omt->na);
    if (bDihs)
        fprintf(fp,"%6d dihedrals of %4d types.\n",ndtot,omt->nd);
    return omt;
}

static void update_idef(t_mymol *mymol,gmx_poldata_t pd,
                        gmx_bool bBonds,gmx_bool bAngles,gmx_bool bDihs)
{
    int  gt,i,tp,ai,aj,ak,al;
    int  ftb,fta,ftd;
    char *aai,*aaj,*aak,*aal,*params,**ptr;
    int  lu;
    double value;
    
    lu = string2unit(gmx_poldata_get_length_unit(pd));
    if (bBonds) {
        ftb = gmx_poldata_get_gt_bond_ftype(pd);
        for(i=0; (i<mymol->ltop->idef.il[ftb].nr); i+=interaction_function[ftb].nratoms+1) {
            tp = mymol->ltop->idef.il[ftb].iatoms[i];
            ai = mymol->ltop->idef.il[ftb].iatoms[i+1];
            aj = mymol->ltop->idef.il[ftb].iatoms[i+2];
            aai = *mymol->atoms->atomtype[ai];
            aaj = *mymol->atoms->atomtype[aj];
            /* Here unfortunately we need a case statement for the types */
            if ((gt = gmx_poldata_search_gt_bond(pd,aai,aaj,&value,NULL,&params)) != 0) {
                mymol->mtop.ffparams.iparams[tp].morse.b0 = convert2gmx(value,lu);
                  
                ptr = split(' ',params);
                if (NULL != ptr[0]) {
                    mymol->mtop.ffparams.iparams[tp].morse.cb = atof(ptr[0]);
                    sfree(ptr[0]);
                }
                if (NULL != ptr[1]) {
                    mymol->mtop.ffparams.iparams[tp].morse.beta = atof(ptr[1]);
                    sfree(ptr[1]);
                }
                sfree(ptr);
            }
            else {
                gmx_fatal(FARGS,"There are no parameters for bond %s-%s in the force field",aai,aaj);
            }
        }
    }
    if (bAngles) {
        fta = gmx_poldata_get_gt_angle_ftype(pd);
        for(i=0; (i<mymol->ltop->idef.il[fta].nr); i+=interaction_function[fta].nratoms+1) {
            tp = mymol->ltop->idef.il[fta].iatoms[i];
            ai = mymol->ltop->idef.il[fta].iatoms[i+1];
            aj = mymol->ltop->idef.il[fta].iatoms[i+2];
            ak = mymol->ltop->idef.il[fta].iatoms[i+3];
            aai = *mymol->atoms->atomtype[ai];
            aaj = *mymol->atoms->atomtype[aj];
            aak = *mymol->atoms->atomtype[ak];
            if ((gt = gmx_poldata_search_gt_angle(pd,aai,aaj,aak,NULL,NULL,&params)) != 0) {
                ptr = split(' ',params);
                if (NULL != ptr[0]) {
                    mymol->mtop.ffparams.iparams[tp].harmonic.rA = atof(ptr[0]);
                    sfree(ptr[0]);
                }
                if (NULL != ptr[1]) {
                    mymol->mtop.ffparams.iparams[tp].harmonic.krA = atof(ptr[1]);
                    sfree(ptr[1]);
                }
                sfree(ptr);
            }
            else {
                gmx_fatal(FARGS,"There are no parameters for angle %s-%s-%s in the force field",aai,aaj,aak);
            }
        }
    }
    if (bDihs) {
        ftd = gmx_poldata_get_gt_dihedral_ftype(pd);
        
        for(i=0; (i<mymol->ltop->idef.il[ftd].nr); i+=interaction_function[ftd].nratoms+1) {
            tp = mymol->ltop->idef.il[ftd].iatoms[i];
            ai = mymol->ltop->idef.il[ftd].iatoms[i+1];
            aj = mymol->ltop->idef.il[ftd].iatoms[i+2];
            ak = mymol->ltop->idef.il[ftd].iatoms[i+3];
            al = mymol->ltop->idef.il[ftd].iatoms[i+4];
            aai = *mymol->atoms->atomtype[ai];
            aaj = *mymol->atoms->atomtype[aj];
            aak = *mymol->atoms->atomtype[ak];
            aal = *mymol->atoms->atomtype[al];
            if ((gt = gmx_poldata_search_gt_dihedral(pd,aai,aaj,aak,aal,
                                                     NULL,NULL,&params)) != 0) {
                ptr = split(' ',params);
                if (NULL != ptr[0]) {
                    mymol->mtop.ffparams.iparams[tp].pdihs.phiA = atof(ptr[0]);
                    sfree(ptr[0]);
                }
                if (NULL != ptr[1]) {
                    mymol->mtop.ffparams.iparams[tp].pdihs.cpA = atof(ptr[1]);
                    sfree(ptr[1]);
                }
                if (NULL != ptr[2]) {
                    mymol->mtop.ffparams.iparams[tp].pdihs.mult = atof(ptr[2]);
                    sfree(ptr[2]);
                }
                sfree(ptr);
            }
            else {
                gmx_fatal(FARGS,"There are no parameters for angle %s-%s-%s in the force field",aai,aaj,aak);
            }
        }
    }
}

static double calc_opt_deviation(opt_param_t *opt)
{
    int    i,j,count,atomnr;
    double qq,qtot,rr2,ener;
    real   t = 0;
    rvec   mu_tot = {0,0,0};
    tensor force_vir={{0,0,0},{0,0,0},{0,0,0}};
    t_nrnb   my_nrnb;
    gmx_wallcycle_t wcycle;
    gmx_bool        bConverged;
    t_mymol *mymol;
    int     eQ;
    gmx_mtop_atomloop_all_t aloop;
    gmx_enerdata_t gmd;
    t_atom *atom; 
    int    at_global,resnr;
    
    if (PAR(opt->md->cr)) 
    {
        gmx_bcast(sizeof(opt->md->bDone),&opt->md->bDone,opt->md->cr);
        gmx_bcast(sizeof(opt->md->bFinal),&opt->md->bFinal,opt->md->cr);
    }
    if (opt->md->bDone)
        return opt->md->ener[ermsTOT];
    if (PAR(opt->md->cr)) 
    {
        gmx_poldata_comm_eemprops(opt->md->pd,opt->md->cr);
    }
    init_nrnb(&my_nrnb);
  
    wcycle  = wallcycle_init(stdout,0,opt->md->cr,1);
    for(j=0; (j<ermsNR); j++)
    {
        opt->md->ener[j] = 0;
    }
    for(i=0; (i<opt->md->nmol); i++) {
        mymol = &(opt->md->mymol[i]);
        if ((mymol->eSupport == eSupportLocal) ||
            (opt->md->bFinal && (mymol->eSupport == eSupportRemote)))
        {
            /* Update topology for this molecule */
            update_idef(mymol,opt->md->pd,opt->bBonds,opt->bAngles,opt->bDihs);
            
            /* Now compute energy */
            atoms2md(&mymol->mtop,&(mymol->ir),0,NULL,0,
                     mymol->mtop.natoms,mymol->md);
            
            /* Now optimize the shell positions */
            if (mymol->shell) { 
                count = 
                    relax_shell_flexcon(debug,opt->md->cr,FALSE,0,
                                        &(mymol->ir),TRUE,
                                        GMX_FORCE_ALLFORCES,FALSE,
                                        mymol->ltop,NULL,NULL,&(mymol->enerd),
                                        NULL,&(mymol->state),
                                        mymol->f,force_vir,mymol->md,
                                        &my_nrnb,wcycle,NULL,
                                        &(mymol->mtop.groups),
                                        mymol->shell,mymol->fr,FALSE,t,mu_tot,
                                        mymol->mtop.natoms,&bConverged,NULL,NULL);
            }
            else {
                do_force(debug,opt->md->cr,&(mymol->ir),0,
                         &my_nrnb,wcycle,mymol->ltop,
                         &mymol->mtop,&(mymol->mtop.groups),
                         mymol->box,mymol->x,NULL,/*history_t *hist,*/
                         mymol->f,force_vir,mymol->md,
                         &mymol->enerd,NULL,/*mymol->fcd,*/
                         0.0,NULL,/*mymol->graph,*/
                         mymol->fr,
                         NULL,mu_tot,t,NULL,NULL,FALSE,GMX_FORCE_ALLFORCES | GMX_FORCE_STATECHANGED);

            }
            mymol->Ecalc = mymol->enerd.term[F_EPOT];
            ener = sqr(mymol->Ecalc-mymol->Emol);
            opt->md->ener[ermsEPOT] += opt->md->fc[ermsEPOT]*ener/opt->md->nmol_support;
        }
    }
    /* Compute E-bounds */
    for(j=0; (j<opt->nparam); j++) {
        if (opt->param[j] < opt->lower[j])
            opt->md->ener[ermsBOUNDS] += opt->md->fc[ermsBOUNDS]*sqr(opt->param[j]-opt->lower[j]);
        else if (opt->param[j] > opt->upper[j])
            opt->md->ener[ermsBOUNDS] += opt->md->fc[ermsBOUNDS]*sqr(opt->param[j]-opt->upper[j]);
    }
    
    for(j=0; (j<ermsTOT); j++) 
        opt->md->ener[ermsTOT] += opt->md->ener[j];
    
    if (debug)
    {
        fprintf(debug,"ENER:");
        for(j=0; (j<ermsNR); j++)
            fprintf(debug,"  %8.3f",opt->md->ener[j]);
        fprintf(debug,"\n");
    }
    /* Global sum energies */
    if (PAR(opt->md->cr)) 
    {
#ifdef GMX_DOUBLE
        gmx_sumd(ermsNR,opt->md->ener,opt->md->cr);
#else
        gmx_sumf(ermsNR,opt->md->ener,opt->md->cr);
#endif
    }
    return opt->md->ener[ermsTOT];
}

static double energy_function(void *params,double v[])
{
    opt_param_t *opt = (opt_param_t *)params;
    t_moldip *md = opt->md;
    int      i;
    
    /* Copy parameters to topologies */
    for(i=0; (i<opt->nparam); i++) {
        opt->param[i] = v[i];
    }
    list2opt(opt);
    
    return calc_opt_deviation(opt);
}

static real guess_new_param(real x,real step,real x0,real x1,gmx_rng_t rng,
                            gmx_bool bRandom)
{
    real r = gmx_rng_uniform_real(rng);
  
    if (bRandom) 
        x = x0+(x1-x0)*r;
    else
        x = x*(1-step+2*step*r);

    if (x < x0)
        return x0;
    else if (x > x1)
        return x1;
    else
        return x;
}

static void guess_all_param(FILE *fplog,opt_param_t *opt,int iter,real stepsize,
                            gmx_bool bRandom,gmx_rng_t rng)
{
    double   ppp,xxx;
    gmx_bool bStart = (iter == 0);
    gmx_bool bRand  = bRandom && (iter == 0);
    int      n;
    
    for(n=0; (n<opt->nparam); n++) 
    {
        if (bStart)
        {
            ppp = opt->param[n];
            xxx = guess_new_param(ppp,stepsize,opt->lower[n],opt->upper[n],rng,bRand);
            if (bRand)
                opt->orig[n] = xxx;
            else
                opt->orig[n] = ppp;
            ppp = xxx;
        }
        else 
            ppp = guess_new_param(opt->orig[n],stepsize,opt->lower[n],
                                  opt->upper[n],rng,bRand);
        opt->param[n] = ppp;
    }
}

void bayes(char *xvg,
           void *data,
           nm_target_func func,
           double start[],
           int n,
           int nprint,
           double step,
           unsigned int seed,
           int    maxiter,
           double *chi2,
           output_env_t oenv)
{
    int iter,j,k,cur=0;
    double ds,sorig,DE,E[2],beta=100;
#define prev (1-cur)
    gmx_rng_t rng;
    real r;
    FILE *fp;
  
    if (NULL != xvg) {
        fp = xvgropen(xvg,"Parameter convergence","iteration","",oenv);
    }
    rng = gmx_rng_init(seed);
    
    E[prev] = func(data,start);
    *chi2 = E[prev];
    
    for(j=iter=0; (iter<maxiter); iter++) {
        if ((NULL != xvg) && ((j % nprint) == 0)) {
          fprintf(fp,"%5d",iter);
          for(k=0; (k<n); k++) 
              fprintf(fp,"  %10g",start[k]);
          fprintf(fp,"\n");
        }
        ds = (2*gmx_rng_uniform_real(rng)-1)*step*fabs(start[j]);
        sorig = start[j];
        start[j] += ds;
        E[cur] = func(data,start);
        DE = E[cur]-E[prev];
        if ((DE < 0) || (exp(beta*DE) < gmx_rng_uniform_real(rng))) {
            cur = prev;
            if (NULL != debug) {
                fprintf(debug,"Changing parameter %3d from %.3f to %.3f. DE = %.3f 'kT'\n",
                        j,sorig,start[j],beta*DE);
            }
            *chi2 = E[cur];
        }
        else 
            start[j] = sorig;
        j = (j+1) % n;
    }
    gmx_rng_destroy(rng);
    if (NULL != xvg)
        xvgrclose(fp);
}

static void optimize_moldip(FILE *fp,FILE *fplog,
                            t_moldip *md,int maxiter,real tol,
                            int nrun,int reinit,real stepsize,int seed,
                            gmx_bool bRandom,real stol,output_env_t oenv,
                            gmx_bool bBonds,gmx_bool bAngles,gmx_bool bDihs,
                            real D0,real beta0,real D0_min,real beta_min,
                            opt_mask_t *omt,real factor,int nprint,
                            char *xvgconv)
{
    double chi2,chi2_min,wj,rms_nw;
    int    status = 0;
    int    i,k,index,n,nparam;
    gmx_bool bMinimum=FALSE;
    char   *name,*qstr,*rowstr;
    char   buf[STRLEN];
    gmx_rng_t rng;
    opt_param_t *opt;
    
    opt = init_opt(fplog,md,&nparam,bBonds,bAngles,bDihs,
                   D0,beta0,D0_min,beta_min,omt,factor);
    
    if (MASTER(md->cr)) 
    {
        rng = gmx_rng_init(seed);
  
        chi2 = chi2_min = GMX_REAL_MAX; 
        for(n=0; (n<nrun); n++) {
            if ((NULL != fp) && (0 == n)) {
                fprintf(fp,"\nStarting run %d out of %d\n",n+1,nrun);
            }
                    
            guess_all_param(fplog,opt,n,stepsize,bRandom,rng);
            
            bayes(xvgconv,(void *)opt,energy_function,opt->param,nparam,nprint,
                  stepsize,seed,maxiter,&chi2,oenv);
                
            if (chi2 < chi2_min) {
                bMinimum = TRUE;
                /* Print convergence if needed */
                for(k=0; (k<opt->nparam); k++)
                {
                    opt->best[k] = opt->param[k];
                }
                chi2_min   = chi2;
            }
            
            if (fp) 
                fprintf(fp,"%5d  %8.3f  %8.3f  %8.3f\n",n,chi2,md->ener[ermsTOT],md->ener[ermsBOUNDS]);
            if (fplog) {
                fprintf(fplog,"%5d  %8.3f  %8.3f  %8.3f\n",n,chi2,md->ener[ermsTOT],md->ener[ermsBOUNDS]);
                fflush(fplog);
            }
        }
        
        if (bMinimum) {
            for(k=0; (k<nparam); k++)
                opt->param[k] = opt->best[k];
            
            energy_function(opt,opt->best);
            if (fplog)
            {
                fprintf(fplog,"\nMinimum chi^2 value during optimization: %.3f.\n",
                        chi2_min);
                fprintf(fplog,"\nMinimum RMSD value during optimization: %.3f.\n",
                        opt->md->ener[ermsTOT]);
                print_opt(fplog,opt);
            }
        }
        calc_opt_deviation(opt);
        md->bDone = TRUE;
        
    }
    else 
    {
        /* Slave calculators */
        do {
            calc_opt_deviation(opt);
        } while (!md->bDone);
    }
    calc_opt_deviation(opt);
}

static real quality_of_fit(real chi2,int N)
{
    return -1;
}

static void print_moldip_specs(FILE *fp,t_moldip *md,char *title,
                               const char *xvg,output_env_t oenv)
{
    FILE *xfp;
    int i;
    double msd;
    
    if (NULL != xvg) 
        xfp = xvgropen(xvg,"Entalpy of Formation","Experiment (kJ/mol)","Calculated (kJ/mol)",
                      oenv);
    
    fprintf(fp,"%s\n",title);
    fprintf(fp,"Nr.   %-40s %10s %10s %10s\n","Molecule","DHf@298K","Emol@0K","Diff");
    msd = 0;
    for(i=0; (i<md->nmol); i++) {
        fprintf(fp,"%-5d %-40s %10g %10g %10g\n",i,md->mymol[i].molname,
                md->mymol[i].Hform,md->mymol[i].Emol,
                md->mymol[i].Emol-md->mymol[i].Ecalc);
        msd += sqr(md->mymol[i].Emol-md->mymol[i].Ecalc);
        if (NULL != xvg)
            fprintf(xfp,"%10g  %10g\n",md->mymol[i].Hform,
                    md->mymol[i].Ecalc+md->mymol[i].Hform-md->mymol[i].Emol);
    }
    fprintf(fp,"\n");
    fprintf(fp,"RMSD is %g kJ/mol for %d molecules.\n\n",sqrt(msd/md->nmol),md->nmol);
    if (NULL != xvg) {
        xvgrclose(xfp);
        do_view(oenv,xvg,NULL);
    }
}

int main(int argc, char *argv[])
{
    static const char *desc[] = {
        "tune_fc read a series of molecules and corresponding experimental",
        "heats of formation from a file, and tunes parameters in an algorithm",
        "until the experimental energies are reproduced by the force field.[PAR]",
        "Minima and maxima for the parameters can be set, these are however",
        "not strictly enforced, but rather they are penalized with a harmonic",
        "function, for which the force constant can be set explicitly.[PAR]",
        "At every reinit step parameters are changed by a random amount within",
        "the fraction set by step size, and within the boundaries given",
        "by the minima and maxima. If the [TT]-random[tt] flag is",
        "given a completely random set of parameters is generated at the start",
        "of each run. At reinit steps however, the parameters are only changed",
        "slightly, in order to speed-up local search but not global search."
        "In other words, complete random starts are done only at the beginning of each",
        "run, and only when explicitly requested.[PAR]",
        "The absolut dipole moment of a molecule remains unchanged if all the",
        "atoms swap the sign of the charge. To prevent this kind of mirror",
        "effects a penalty is added to the square deviation ",
        "if hydrogen atoms have a negative charge. Similarly a penalty is",
        "added if atoms from row VI or VII in the periodic table have a positive",
        "charge. The penalty is equal to the force constant given on the command line",
        "time the square of the charge.[PAR]",
        "One of the electronegativities (chi) is redundant in the optimization,",
        "only the relative values are meaningful.",
        "Therefore by default we fix the value for hydrogen to what is written",
        "in the eemprops.dat file (or whatever is given with the [tt]-d[TT] flag).",
        "A suitable value would be 2.3, the original, value due to Pauling,",
        "this can by overridden by setting the [tt]-fixchi[TT] flag to something else (e.g. a non-existing atom).[PAR]",
        "A selection of molecules into a training set and a test set (or ignore set)",
        "can be made using option [TT]-sel[tt]. The format of this file is:[BR]",
        "iupac|Train[BR]",
        "iupac|Test[BR]",
        "iupac|Ignore[BR]",
        "and you should ideally have a line for each molecule in the molecule database",
        "([TT]-f[tt] option). Missing molecules will be ignored."
    };
  
    t_filenm fnm[] = {
        { efDAT, "-f", "allmols",    ffREAD  },
        { efDAT, "-d", "gentop",     ffOPTRD },
        { efDAT, "-o", "tune_fc",    ffWRITE },
        { efDAT, "-sel", "molselect",ffREAD },
        { efLOG, "-g", "tune_fc",    ffWRITE },
        { efXVG, "-x", "hform-corr", ffWRITE },
        { efXVG, "-conv", "param-conv", ffWRITE }
    };
#define NFILE asize(fnm)
    static int  nrun=1,maxiter=100,reinit=0,seed=1993;
    static int  minimum_data=3,compress=0;
    static real tol=1e-3,stol=1e-6,watoms=1;
    static gmx_bool bRandom=FALSE,bZero=TRUE,bWeighted=TRUE,bOptHfac=FALSE,bQM=FALSE,bCharged=TRUE,bGaussianBug=TRUE,bPol=FALSE,bFitZeta=TRUE;
    static real J0_0=5,Chi0_0=1,w_0=5,step=0.01,hfac=0,rDecrZeta=-1;
    static real J0_1=30,Chi0_1=30,w_1=50,epsr=1;
    static real fc_mu=1,fc_bound=1,fc_quad=1,fc_charge=0,fc_esp=0;
    static real factor=0.8;
    static real th_toler=170,ph_toler=5,dip_toler=0.5,quad_toler=5,q_toler=0.25;
    static char *opt_elem = NULL,*const_elem=NULL,*fixchi=(char *)"H";
    static char *lot = (char *)"B3LYP/aug-cc-pVTZ";
    static char *qgen[] = { NULL,(char *)"AXp", (char *)"AXs", (char *)"AXg", NULL };
    static gmx_bool bBonds=TRUE,bAngles=FALSE,bDihs=FALSE;
    static real beta0=0,D0=0,beta_min=10,D0_min=50;
    static int nprint=10;
    static int  nthreads=0; /* set to determine # of threads automatically */
    t_pargs pa[] = {
        { "-tol",   FALSE, etREAL, {&tol},
          "Tolerance for convergence in optimization" },
        { "-maxiter",FALSE, etINT, {&maxiter},
          "Max number of iterations for optimization" },
        { "-nprint",FALSE, etINT, {&nprint},
          "How often to print the parameters during the simulation" },
        { "-reinit", FALSE, etINT, {&reinit},
          "After this many iterations the search vectors are randomized again. A vlue of 0 means this is never done at all." },
        { "-stol",   FALSE, etREAL, {&stol},
          "If reinit is -1 then a reinit will be done as soon as the simplex size is below this treshold." },
        { "-nrun",   FALSE, etINT,  {&nrun},
          "This many runs will be done, before each run a complete randomization will be done" },
        { "-bonds",  FALSE, etBOOL, {&bBonds},
          "Optimize bond parameters" },
        { "-angles",  FALSE, etBOOL, {&bAngles},
          "Optimize angle parameters" },
        { "-dihedrals",  FALSE, etBOOL, {&bDihs},
          "Optimize dihedral parameters" },
        { "-beta0", FALSE, etREAL, {&beta0},
          "Reset the initial beta for Morse potentials to this value, independent of gentop.dat. If value is <= 0 gentop.dat value is used." },
        { "-D0", FALSE, etREAL, {&D0},
          "Reset the initial D for Morse potentials to this value, independent of gentop.dat. If value is <= 0 gentop.dat value is used." },
        { "-beta_min", FALSE, etREAL, {&beta_min},
          "Minimum value for beta in Morse potential" },
        { "-DO_min", FALSE, etREAL, {&D0_min},
          "Minimum value for D0 in Morse potential" },
        { "-qm",     FALSE, etBOOL, {&bQM},
          "Use only quantum chemistry results (from the levels of theory below) in order to fit the parameters. If not set, experimental values will be used as reference with optional quantum chemistry results, in case no experimental results are available" },
        { "-lot",    FALSE, etSTR,  {&lot},
      "Use this method and level of theory when selecting coordinates and charges. Multiple levels can be specified which will be used in the order given, e.g.  B3LYP/aug-cc-pVTZ:HF/6-311G**" },
        { "-fc_bound",    FALSE, etREAL, {&fc_bound},
          "Force constant in the penalty function for going outside the borders given with the above six options." },
        { "-step",  FALSE, etREAL, {&step},
          "Step size in parameter optimization. Is used as a fraction of the starting value, should be less than 10%. At each reinit step the step size is updated." },
        { "-min_data",  FALSE, etINT, {&minimum_data},
          "Minimum number of data points in order to be able to optimize the parameters for a given atomtype" },
        { "-opt_elem",  FALSE, etSTR, {&opt_elem},
          "Space-separated list of elements to optimize, e.g. \"H C Br\". The other available elements in gentop.dat are left unmodified. If this variable is not set, all elements will be optimized." },
        { "-const_elem",  FALSE, etSTR, {&const_elem},
          "Space-separated list of elements to include but keep constant, e.g. \"O N\". These elements from gentop.dat are left unmodified" },
        { "-seed", FALSE, etINT, {&seed},
          "Random number seed for reinit" },
        { "-factor", FALSE, etREAL, {&factor},
          "Factor for generating random parameters. Parameters will be taken within the limit factor*x - x/factor" },
        { "-random", FALSE, etBOOL, {&bRandom},
          "Generate completely random starting parameters within the limits set by the options. This will be done at the very first step and before each subsequent run." },
        { "-weight", FALSE, etBOOL, {&bWeighted},
          "Perform a weighted fit, by using the errors in the dipoles presented in the input file. This may or may not improve convergence." },
        { "-th_toler", FALSE, etREAL, {&th_toler},
          "Minimum angle to be considered a linear A-B-C bond" },
        { "-ph_toler", FALSE, etREAL, {&ph_toler},
          "Maximum angle to be considered a planar A-B-C/B-C-D torsion" },
#ifdef GMX_THREAD_MPI
    { "-nt",      FALSE, etINT, {&nthreads},
      "Number of threads to start (0 is guess)" },
#endif
        { "-compress", FALSE, etBOOL, {&compress},
          "Compress output XML file" }
    };
    t_moldip  *md;
    FILE      *fp,*out;
    int       iModel;
    t_commrec *cr;
    output_env_t oenv;
    gmx_molselect_t gms;
    time_t    my_t;
    char      pukestr[STRLEN];
    opt_mask_t *omt;
    cr = init_par(&argc,&argv);

    if (MASTER(cr))
        CopyRight(stdout,argv[0]);

    parse_common_args(&argc,argv,PCA_CAN_VIEW | (MASTER(cr) ? 0 : PCA_QUIET),
                      NFILE,fnm,asize(pa),pa,asize(desc),desc,0,NULL,&oenv);

#ifndef GMX_THREAD_MPI
    nthreads=1;
#endif
    if (qgen[0]) 
        iModel = name2eemtype(qgen[0]);
    else
        iModel = eqgAXg;
    
    if (MASTER(cr)) {
        fp = ffopen(opt2fn("-g",NFILE,fnm),"w");

        time(&my_t);
        fprintf(fp,"# This file was created %s",ctime(&my_t));
        fprintf(fp,"# by the following command:\n# %s\n#\n",command_line());
        fprintf(fp,"# %s is part of G R O M A C S:\n#\n",ShortProgram());
        bromacs(pukestr,99);
        fprintf(fp,"# %s\n#\n",pukestr);
    }
    else
        fp = NULL;
        

    if (MASTER(cr))
        gms = gmx_molselect_init(opt2fn_null("-sel",NFILE,fnm));
    else
        gms = NULL;
    md = init_moldip(cr,bQM,bGaussianBug,iModel,rDecrZeta,epsr,
                     J0_0,Chi0_0,w_0,J0_1,Chi0_1,w_1,
                     fc_bound,fc_mu,fc_quad,fc_charge,
                     fc_esp,fixchi,bOptHfac,hfac,bPol,bFitZeta);
    read_moldip(md,fp ? fp : (debug ? debug : NULL),
                opt2fn("-f",NFILE,fnm),
                opt2fn_null("-d",NFILE,fnm),
                minimum_data,bZero,bWeighted,
                opt_elem,const_elem,
                lot,bCharged,oenv,gms,th_toler,ph_toler,dip_toler,
                TRUE,TRUE,TRUE,2,watoms,FALSE);
    print_moldip_specs(fp,md,"Before optimization",NULL,oenv);
    omt = analyze_idef(fp,md->nmol,md->mymol,md->pd,bBonds,bAngles,bDihs);

    optimize_moldip(MASTER(cr) ? stderr : NULL,fp,
                    md,maxiter,tol,nrun,reinit,step,seed,
                    bRandom,stol,oenv,bBonds,bAngles,bDihs,D0,beta0,
                    D0_min,beta_min,omt,factor,nprint,
                    opt2fn("-conv",NFILE,fnm));
    print_moldip_specs(fp,md,"After optimization",opt2fn("-x",NFILE,fnm),oenv);
    gmx_poldata_write(opt2fn("-o",NFILE,fnm),md->pd,md->atomprop,compress);
    
    if (MASTER(cr)) 
    {
        ffclose(fp);
  
        thanx(stdout);
    }
    
#ifdef GMX_MPI
    if (gmx_parallel_env_initialized())
        gmx_finalize();
#endif

    return 0;
}